// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2TextureALVulkan.h"
#include "Tr2AdapterStructures.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "VkResult.h"
#include "UtilitiesVulkan.h"


namespace
{

	ALResult CheckCreationFlags( const Tr2BitmapDimensions& desc, const Tr2MsaaDesc& msaa, Tr2GpuUsage::Type gpuUsage, Tr2CpuUsage::Type cpuUsage )
	{
		if( HasBufferFlags( gpuUsage ) )
		{
			return E_INVALIDARG;
		}

		if( msaa.samples > 1 )
		{
			if( HasFlag( gpuUsage, Tr2GpuUsage::UNORDERED_ACCESS ) )
			{
				return E_INVALIDARG;
			}
			if( cpuUsage != Tr2CpuUsage::NONE )
			{
				return E_INVALIDARG;
			}
			if( desc.GetType() != Tr2RenderContextEnum::TEX_TYPE_2D )
			{
				return E_INVALIDARG;
			}
		}
		if( desc.GetType() != Tr2RenderContextEnum::TEX_TYPE_2D )
		{
			if( desc.GetType() == Tr2RenderContextEnum::TEX_TYPE_CUBE )
			{
				if( desc.GetArraySize() != 6 )
				{
					return E_INVALIDARG;
				}
			}
			else if( desc.GetArraySize() > 1 )
			{
				return E_INVALIDARG;
			}
		}
		if( desc.GetType() != Tr2RenderContextEnum::TEX_TYPE_2D && HasFlag( gpuUsage, Tr2GpuUsage::DEPTH_STENCIL ) )
		{
			return E_INVALIDARG;
		}
		if( msaa.samples > 1 && desc.GetTrueMipCount() > 1 )
		{
			return E_INVALIDARG;
		}
		if( HasFlag( gpuUsage, Tr2GpuUsage::RENDER_TARGET ) && HasFlag( cpuUsage, Tr2CpuUsage::WRITE ) )
		{
			return E_INVALIDARG;
		}
		if( HasFlag( gpuUsage, Tr2GpuUsage::DEPTH_STENCIL ) && cpuUsage != Tr2CpuUsage::NONE )
		{
			return E_INVALIDARG;
		}
		if( HasFlag( gpuUsage, Tr2GpuUsage::DEPTH_STENCIL ) && desc.GetTrueMipCount() > 1 )
		{
			return E_INVALIDARG;
		}
		if( desc.GetType() == Tr2RenderContextEnum::TEX_TYPE_3D && cpuUsage != Tr2CpuUsage::NONE )
		{
			return E_INVALIDARG;
		}
		if( HasFlag( cpuUsage, Tr2CpuUsage::READ ) && HasFlag( cpuUsage, Tr2CpuUsage::WRITE_OFTEN ) )
		{
			return E_INVALIDARG;
		}
		return S_OK;
	}
}

namespace TrinityALImpl
{

	Tr2TextureAL::Tr2TextureAL()
		:m_owner( nullptr ),
		m_memory( VK_NULL_HANDLE ),
		m_currentIndex( 0 ),
		m_cpuUsage( Tr2CpuUsage::NONE ),
		m_gpuUsage( Tr2GpuUsage::NONE ),
		m_format( VK_FORMAT_UNDEFINED ),
		m_mapBuffer( VK_NULL_HANDLE ),
		m_mapMemory( VK_NULL_HANDLE ),
		m_mapPitch( 0 ),
		m_mapIsWrite( false )
	{
	}

	Tr2TextureAL::~Tr2TextureAL()
	{
		Destroy();
	}

	ALResult Tr2TextureAL::Create( const Tr2BitmapDimensions& desc, const Tr2MsaaDesc& msaa, Tr2GpuUsage::Type gpuUsage, Tr2CpuUsage::Type cpuUsage, Tr2SubresourceData* initialData, Tr2PrimaryRenderContextAL& renderContext )
	{
		Destroy();

		if( !renderContext.IsValid() )
		{
			return E_INVALIDARG;
		}

		FORWARD_HR( CheckCreationFlags( desc, msaa, gpuUsage, cpuUsage ) );

		if( !IsWritable( gpuUsage ) && !HasFlag( cpuUsage, Tr2CpuUsage::WRITE ) && !initialData )
		{
			return E_INVALIDARG;
		}

		// TRANSFER_SRC as well as TRANSFER_DST, and both unconditionally, for the same
		// reason Tr2BufferALVulkan.cpp gives for its TRANSFER_DST: everything that reads an
		// image back needs it and none of them can be predicted from the usage flags the
		// caller passes. MapForReading, CopySubresourceRegion, Resolve and GenerateMipMaps
		// all copy *from* the image; without the bit, vkCmdCopyImageToBuffer is
		// VUID-vkCmdCopyImageToBuffer-srcImage-00186 and the driver loses the device.
		VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if( HasFlag( gpuUsage, Tr2GpuUsage::SHADER_RESOURCE ) )
		{
			usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}
		if( HasFlag( gpuUsage, Tr2GpuUsage::RENDER_TARGET ) )
		{
			usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		}
		if( HasFlag( gpuUsage, Tr2GpuUsage::DEPTH_STENCIL ) )
		{
			usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		if( HasFlag( gpuUsage, Tr2GpuUsage::UNORDERED_ACCESS ) )
		{
			usage |= VK_IMAGE_USAGE_STORAGE_BIT;
		}

		VkImage image;
		VkDeviceMemory memory;

		CR_RETURN_HR( CreateImage( image, memory, desc, msaa, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderContext ) );

		VkImageViewCreateInfo image_view_create_info = {
			VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			nullptr,
			0,
			image,
			desc.GetType() == Tr2RenderContextEnum::TEX_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
			GetVulkanFormat( desc.GetFormat() ),
			{
				VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY
			},
			{
				// Not COLOR unconditionally: a depth format has no colour aspect, and
				// vkCreateImageView rejects the mismatch. This is the first thing that has
				// to be right before a depth buffer can be created at all.
				GetAspectMaskVulkan( GetVulkanFormat( desc.GetFormat() ) ),
				0,
				desc.GetTrueMipCount(),
				0,
				desc.GetArraySize()
			}
		};

		VkImageView imageView;
		CR_RETURN_HR( Vk2Al( vkCreateImageView( renderContext.m_device, &image_view_create_info, nullptr, &imageView ) ) );

		if( initialData )
		{
			std::vector<VkBufferImageCopy> copyInfo;
			size_t index = 0;
			size_t size = 0;

			// bufferRowLength and bufferImageHeight are both counted in TEXELS, and for a
			// block-compressed format they have to be multiples of the block extent. The
			// source pitches are byte counts over block rows, so both conversions go through
			// the block size -- the same split BitmapDimensions::GetMipPitch already makes.
			//
			// GetBytesPerPixel returns 0 for every BC format, so dividing by it was an integer
			// divide by zero. All four Rendering.CanSampleBc* tests died on SEH 0xc0000094,
			// and because an SEH unwind does not run C++ destructors, every object those tests
			// had built leaked through to vkDestroyDevice: 40 of the 57 objects reported by
			// VUID-vkDestroyDevice-device-05137 came from these four tests alone.
			const bool isCompressed = Tr2RenderContextEnum::IsCompressedFormat( desc.GetFormat() );
			const uint32_t blockWidth = isCompressed ? 4u : 1u;
			const uint32_t blockHeight = isCompressed ? 4u : 1u;
			const uint32_t blockBytes = isCompressed
				? Tr2RenderContextEnum::GetBlockByteSize( desc.GetFormat() )
				: Tr2RenderContextEnum::GetBytesPerPixel( desc.GetFormat() );
			for( uint32_t i = 0; i < desc.GetArraySize(); ++i )
			{
				for( uint32_t j = 0; j < desc.GetMipCount(); ++j )
				{
					// bufferImageHeight counts ROWS, so the slice pitch has to be divided by the ROW
					// pitch. Dividing it by the pixel size instead made the GPU believe every slice
					// was width-times taller than it is and read that far past the staging buffer:
					// for the 32x32x32 BGRA8 volume in Rendering.CanSampleVolumeTexture it asked for
					// 4067328 bytes out of a 131072-byte buffer. That is
					// VUID-vkCmdCopyBufferToImage-pRegions-00171, and the page fault it caused lost
					// the device -- which is why a later, unrelated test was the first to see
					// VK_ERROR_DEVICE_LOST. 2D uploads never showed it because sliceExtent only
					// matters once depth or layerCount exceeds 1.
					const uint32_t rowPitch = initialData[index].m_sysMemPitch;
					VkBufferImageCopy buffer_image_copy_info = {
						size,                                  // VkDeviceSize               bufferOffset
						blockBytes ? rowPitch / blockBytes * blockWidth : 0,   // uint32_t                   bufferRowLength
						rowPitch ? initialData[index].m_sysMemSlicePitch / rowPitch * blockHeight : 0,   // uint32_t                   bufferImageHeight
						{ VK_IMAGE_ASPECT_COLOR_BIT, j, i, 1 },
						{ 0, 0, 0 },
						{ desc.GetMipWidth( j ), desc.GetMipHeight( j ), desc.GetMipDepth( j ) }
					};
					copyInfo.push_back( buffer_image_copy_info );
					size += initialData[index].m_sysMemSlicePitch * desc.GetMipDepth( j );
					++index;
				}
			}

			VkBuffer stagingBuffer;
			VkDeviceMemory stagingMemory;
			CreateBuffer( stagingBuffer, stagingMemory, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, renderContext );

			void *data;
			// VK_WHOLE_SIZE, not size: the flush below runs to the end of the mapping, and
			// VUID-VkMappedMemoryRange-size-01389 wants that end either atom-aligned or at the
			// end of the allocation, which vkGetBufferMemoryRequirements rounded up.
			Vk2Al( vkMapMemory( renderContext.m_device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &data ) );

			for( size_t i = 0; i < copyInfo.size(); ++i )
			{
				memcpy( static_cast<uint8_t*>( data ) + copyInfo[i].bufferOffset, initialData[i].m_sysMem, initialData[i].m_sysMemSlicePitch * copyInfo[i].imageExtent.depth );
			}

			VkMappedMemoryRange flush_range = {
				VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
				nullptr,
				stagingMemory,
				0,
				VK_WHOLE_SIZE
			};
			vkFlushMappedMemoryRanges( renderContext.m_device, 1, &flush_range );

			vkUnmapMemory( renderContext.m_device, stagingMemory );


			VkImageMemoryBarrier barrier = {
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				nullptr,
				0,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED,
				image,
				{
					VK_IMAGE_ASPECT_COLOR_BIT,
					0,
					desc.GetTrueMipCount(),
					0,
					desc.GetArraySize()
				}
			};
			// The whole upload -- two barriers and the copy between them -- has to be
			// outside any render pass instance. See the note in Tr2BufferALVulkan.cpp.
			renderContext.EndRenderPassVulkan();
			vkCmdPipelineBarrier( renderContext.m_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier );

			vkCmdCopyBufferToImage( renderContext.m_commandBuffer, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t( copyInfo.size() ), copyInfo.data() );

			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier( renderContext.m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier );

			renderContext.DestroyLaterVulkan( stagingBuffer, vkDestroyBuffer );
			renderContext.DestroyLaterVulkan( stagingMemory, vkFreeMemory );
		}

		m_images.push_back( image );
		m_imageViews.push_back( imageView );

		// The upload above ends with a barrier into SHADER_READ_ONLY_OPTIMAL. Without
		// initial data nothing has touched the image and vkCreateImage left it UNDEFINED
		// -- which is exactly the case that used to reach a draw untransitioned, because
		// the upload path was the only thing in the backend that emitted image barriers.
		m_layouts.push_back( initialData
			? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			: VK_IMAGE_LAYOUT_UNDEFINED );
		m_memory = memory;
		m_cpuUsage = cpuUsage;
		m_gpuUsage = gpuUsage;
		m_desc = desc;
		m_msaa = msaa;
		m_format = GetVulkanFormat( desc.GetFormat() );
		m_owner = &renderContext;

		return S_OK;
	}

	void Tr2TextureAL::Destroy()
	{
		if( m_owner )
		{
			if( m_memory )
			{
				m_owner->DestroyLaterVulkan( m_memory, vkFreeMemory );
			}
			for( auto it = begin( m_imageViews ); it != end( m_imageViews ); ++it )
			{
				m_owner->DestroyLaterVulkan( *it, vkDestroyImageView );
			}
			for( auto it = begin( m_attachmentViews ); it != end( m_attachmentViews ); ++it )
			{
				m_owner->DestroyLaterVulkan( it->second, vkDestroyImageView );
			}
			if( m_images.size() == 1 )
			{
				// don't destroy swap chain images?
				for( auto it = begin( m_images ); it != end( m_images ); ++it )
				{
					m_owner->DestroyLaterVulkan( *it, vkDestroyImage );
				}
			}
			// An open map holds a staging buffer. Leaving it to the destructor would free
			// it against a device that Tr2PrimaryRenderContextAL::Destroy has already torn
			// down, which is the trap the render context's own Destroy documents.
			if( m_mapBuffer != VK_NULL_HANDLE )
			{
				vkUnmapMemory( m_owner->m_device, m_mapMemory );
				m_owner->DestroyLaterVulkan( m_mapBuffer, &vkDestroyBuffer );
				m_owner->DestroyLaterVulkan( m_mapMemory, &vkFreeMemory );
				m_mapBuffer = VK_NULL_HANDLE;
				m_mapMemory = VK_NULL_HANDLE;
				m_mapIsWrite = false;
			}

			m_images.clear();
			m_imageViews.clear();
			m_attachmentViews.clear();
			m_layouts.clear();
		}
		m_currentIndex = 0;
		m_cpuUsage = Tr2CpuUsage::NONE;
		m_gpuUsage = Tr2GpuUsage::NONE;
		m_format = VK_FORMAT_UNDEFINED;
	}

	bool Tr2TextureAL::IsValid() const
	{
		return !m_images.empty();
	}

	Tr2ALMemoryType Tr2TextureAL::GetMemoryClass() const
	{
		return AL_MEMORY_MANAGED;
	}

	const Tr2BitmapDimensions& Tr2TextureAL::GetDesc() const
	{
		return m_desc;
	}

	const Tr2MsaaDesc& Tr2TextureAL::GetMsaaDesc() const
	{
		return m_msaa;
	}

	Tr2GpuUsage::Type Tr2TextureAL::GetGpuUsage() const
	{
		return m_gpuUsage;
	}

	Tr2CpuUsage::Type Tr2TextureAL::GetCpuUsage() const
	{
		return m_cpuUsage;
	}


	ALResult Tr2TextureAL::AssignFromSwapChainVulkan( const std::vector<VkImage>& backBuffers, const Tr2DisplayModeInfo& mode, Tr2PrimaryRenderContextAL& renderContext )
	{
		Destroy();

		std::vector<VkImageView> views;
		for( auto it = begin( backBuffers ); it != end( backBuffers ); ++it )
		{
			VkImageViewCreateInfo imageViewInfo = {
				VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				nullptr,
				0,
				*it,
				VK_IMAGE_VIEW_TYPE_2D,
				GetVulkanFormat( mode.format ),
				{
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY
				},
				{
					VK_IMAGE_ASPECT_COLOR_BIT,
					0,
					1,
					0,
					1
				}
			};

			VkImageView view;

			CR_RETURN_HR( Vk2Al( vkCreateImageView( renderContext.m_device, &imageViewInfo, nullptr, &view ) ) );
			views.push_back( view );
		}

		m_images = backBuffers;
		m_imageViews = views;

		// Acquired swapchain images carry no guarantee about their contents or layout, and
		// BeginFrame transitions each one from UNDEFINED every frame, so UNDEFINED is the
		// truthful starting point rather than a placeholder.
		m_layouts.assign( backBuffers.size(), VK_IMAGE_LAYOUT_UNDEFINED );
		m_desc = Tr2BitmapDimensions( mode.width, mode.height, 1, mode.format );
		m_msaa = Tr2MsaaDesc();
		m_gpuUsage = Tr2GpuUsage::RENDER_TARGET;
		m_format = GetVulkanFormat( mode.format );
		m_owner = &renderContext;
		return S_OK;
	}

	void Tr2TextureAL::SetCurrentImageVulkan( uint32_t index )
	{
		m_currentIndex = index;
	}

	namespace
	{
		// The mapped region in image coordinates, and the size of the tightly packed
		// staging buffer that mirrors it. A subresource with no box means the whole mip.
		void DescribeMappedRegion(
			const Tr2TextureSubresource& region,
			const Tr2BitmapDimensions& desc,
			VkOffset3D& offset,
			VkExtent3D& extent,
			uint32_t& pitch,
			VkDeviceSize& size )
		{
			const uint32_t mip = region.m_startMipLevel;
			if( region.HasBox() )
			{
				offset.x = int32_t( region.m_box.left );
				offset.y = int32_t( region.m_box.top );
				offset.z = int32_t( region.m_box.front );
				extent.width = region.GetWidth();
				extent.height = region.GetHeight();
				extent.depth = region.GetDepth() ? region.GetDepth() : 1;
			}
			else
			{
				offset.x = 0;
				offset.y = 0;
				offset.z = 0;
				extent.width = desc.GetMipWidth( mip );
				extent.height = desc.GetMipHeight( mip );
				extent.depth = desc.GetMipDepth( mip );
			}

			const bool isCompressed = Tr2RenderContextEnum::IsCompressedFormat( desc.GetFormat() );
			if( isCompressed )
			{
				// Rows of blocks, not rows of texels -- the same distinction the upload path
				// in Create has to make.
				pitch = ( ( extent.width + 3 ) / 4 ) * Tr2RenderContextEnum::GetBlockByteSize( desc.GetFormat() );
				size = VkDeviceSize( pitch ) * ( ( extent.height + 3 ) / 4 ) * extent.depth;
			}
			else
			{
				pitch = extent.width * Tr2RenderContextEnum::GetBytesPerPixel( desc.GetFormat() );
				size = VkDeviceSize( pitch ) * extent.height * extent.depth;
			}
		}
	}

	ALResult Tr2TextureAL::MapForReading( const Tr2TextureSubresource& region, bool synchronize, const void*& data, uint32_t& pitch, Tr2RenderContextAL& renderContext )
	{
		data = nullptr;
		pitch = 0;
		if( !IsValid() || !m_owner )
		{
			return E_INVALIDCALL;
		}
		if( m_mapBuffer != VK_NULL_HANDLE )
		{
			return E_INVALIDCALL;
		}
		if( m_currentIndex >= m_images.size() )
		{
			return E_INVALIDCALL;
		}

		VkOffset3D offset;
		VkExtent3D extent;
		VkDeviceSize size = 0;
		DescribeMappedRegion( region, m_desc, offset, extent, pitch, size );
		if( size == 0 )
		{
			return E_INVALIDARG;
		}

		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		CR_RETURN_HR( CreateBuffer(
			buffer,
			memory,
			size_t( size ),
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VkMemoryPropertyFlagBits( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ),
			*m_owner ) );

		// HOST_COHERENT above, so no vkInvalidateMappedMemoryRanges is needed after the
		// copy -- the same reasoning Tr2BufferALVulkan.cpp documents for its write paths.

		renderContext.EndRenderPassVulkan();
		TransitionVulkan( renderContext.m_commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );

		VkBufferImageCopy copy = {
			0,
			0,                                   // tightly packed, so let the extent decide
			0,
			{ GetAspectMaskVulkan( m_format ), region.m_startMipLevel, region.m_startFace, 1 },
			offset,
			extent
		};
		vkCmdCopyImageToBuffer( renderContext.m_commandBuffer, m_images[m_currentIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy );

		if( synchronize )
		{
			// The copy has to have happened before the pointer is handed out. This is a
			// full stall by construction, which is what a CPU read of GPU memory costs.
			ALResult flushed = m_owner->FlushAndSyncVulkan();
			if( FAILED( flushed ) )
			{
				m_owner->DestroyLaterVulkan( buffer, &vkDestroyBuffer );
				m_owner->DestroyLaterVulkan( memory, &vkFreeMemory );
				return flushed;
			}
		}

		void* mapped = nullptr;
		ALResult mappedResult = Vk2Al( vkMapMemory( m_owner->m_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped ) );
		if( FAILED( mappedResult ) )
		{
			m_owner->DestroyLaterVulkan( buffer, &vkDestroyBuffer );
			m_owner->DestroyLaterVulkan( memory, &vkFreeMemory );
			return mappedResult;
		}

		m_mapBuffer = buffer;
		m_mapMemory = memory;
		m_mapPitch = pitch;
		m_mapRegion = region;
		m_mapIsWrite = false;
		data = mapped;
		return S_OK;
	}

	void Tr2TextureAL::UnmapForReading( Tr2RenderContextAL& )
	{
		if( m_mapBuffer == VK_NULL_HANDLE || m_mapIsWrite || !m_owner )
		{
			return;
		}
		vkUnmapMemory( m_owner->m_device, m_mapMemory );
		m_owner->DestroyLaterVulkan( m_mapBuffer, &vkDestroyBuffer );
		m_owner->DestroyLaterVulkan( m_mapMemory, &vkFreeMemory );
		m_mapBuffer = VK_NULL_HANDLE;
		m_mapMemory = VK_NULL_HANDLE;
	}

	ALResult Tr2TextureAL::MapForWriting( const Tr2TextureSubresource& region, void*& data, uint32_t& pitch, Tr2RenderContextAL& renderContext )
	{
		data = nullptr;
		pitch = 0;
		if( !IsValid() || !m_owner )
		{
			return E_INVALIDCALL;
		}
		if( m_mapBuffer != VK_NULL_HANDLE )
		{
			return E_INVALIDCALL;
		}

		VkOffset3D offset;
		VkExtent3D extent;
		VkDeviceSize size = 0;
		DescribeMappedRegion( region, m_desc, offset, extent, pitch, size );
		if( size == 0 )
		{
			return E_INVALIDARG;
		}

		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		CR_RETURN_HR( CreateBuffer(
			buffer,
			memory,
			size_t( size ),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VkMemoryPropertyFlagBits( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ),
			*m_owner ) );

		void* mapped = nullptr;
		ALResult mappedResult = Vk2Al( vkMapMemory( m_owner->m_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped ) );
		if( FAILED( mappedResult ) )
		{
			m_owner->DestroyLaterVulkan( buffer, &vkDestroyBuffer );
			m_owner->DestroyLaterVulkan( memory, &vkFreeMemory );
			return mappedResult;
		}

		// Nothing is copied here: the caller has not written anything yet. UnmapForWriting
		// is where the region reaches the image.
		( void )renderContext;
		m_mapBuffer = buffer;
		m_mapMemory = memory;
		m_mapPitch = pitch;
		m_mapRegion = region;
		m_mapIsWrite = true;
		data = mapped;
		return S_OK;
	}

	void Tr2TextureAL::UnmapForWriting( Tr2RenderContextAL& renderContext )
	{
		if( m_mapBuffer == VK_NULL_HANDLE || !m_mapIsWrite || !m_owner )
		{
			return;
		}
		vkUnmapMemory( m_owner->m_device, m_mapMemory );

		VkOffset3D offset;
		VkExtent3D extent;
		uint32_t pitch = 0;
		VkDeviceSize size = 0;
		DescribeMappedRegion( m_mapRegion, m_desc, offset, extent, pitch, size );

		const VkImageLayout restore = GetLayoutVulkan();
		renderContext.EndRenderPassVulkan();
		TransitionVulkan( renderContext.m_commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

		VkBufferImageCopy copy = {
			0,
			0,
			0,
			{ GetAspectMaskVulkan( m_format ), m_mapRegion.m_startMipLevel, m_mapRegion.m_startFace, 1 },
			offset,
			extent
		};
		vkCmdCopyBufferToImage( renderContext.m_commandBuffer, m_mapBuffer, m_images[m_currentIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );

		// Back to where it was, so that a texture being sampled every frame does not have
		// to wait for SetPass to notice. UNDEFINED means it had never been used, and
		// transitioning back to that would discard what was just written.
		if( restore != VK_IMAGE_LAYOUT_UNDEFINED && restore != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL )
		{
			TransitionVulkan( renderContext.m_commandBuffer, restore );
		}

		m_owner->DestroyLaterVulkan( m_mapBuffer, &vkDestroyBuffer );
		m_owner->DestroyLaterVulkan( m_mapMemory, &vkFreeMemory );
		m_mapBuffer = VK_NULL_HANDLE;
		m_mapMemory = VK_NULL_HANDLE;
		m_mapIsWrite = false;
	}

	ALResult Tr2TextureAL::CopySubresourceRegion( const Tr2TextureSubresource& destSubresource, Tr2TextureAL& source, const Tr2TextureSubresource& sourceSubresource, Tr2RenderContextAL& renderContext )
	{
		if( !IsValid() || !source.IsValid() || !m_owner )
		{
			return E_INVALIDCALL;
		}
		if( m_currentIndex >= m_images.size() || source.m_currentIndex >= source.m_images.size() )
		{
			return E_INVALIDCALL;
		}

		VkOffset3D srcOffset, dstOffset;
		VkExtent3D srcExtent, dstExtent;
		uint32_t ignoredPitch = 0;
		VkDeviceSize ignoredSize = 0;
		DescribeMappedRegion( sourceSubresource, source.m_desc, srcOffset, srcExtent, ignoredPitch, ignoredSize );
		DescribeMappedRegion( destSubresource, m_desc, dstOffset, dstExtent, ignoredPitch, ignoredSize );

		// The source extent wins. vkCmdCopyImage takes one extent for both ends, and the
		// AL's two subresources can legitimately describe different-sized boxes -- the
		// destination box says where to put it, not how much to take.
		VkImageCopy copy = {
			{ GetAspectMaskVulkan( source.m_format ), sourceSubresource.m_startMipLevel, sourceSubresource.m_startFace, 1 },
			srcOffset,
			{ GetAspectMaskVulkan( m_format ), destSubresource.m_startMipLevel, destSubresource.m_startFace, 1 },
			dstOffset,
			srcExtent
		};

		renderContext.EndRenderPassVulkan();
		source.TransitionVulkan( renderContext.m_commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
		TransitionVulkan( renderContext.m_commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

		vkCmdCopyImage(
			renderContext.m_commandBuffer,
			source.m_images[source.m_currentIndex],
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			m_images[m_currentIndex],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&copy );
		return S_OK;
	}

	ALResult Tr2TextureAL::Resolve( Tr2TextureAL& destination, Tr2RenderContextAL& renderContext )
	{
		if( !IsValid() || !destination.IsValid() || !m_owner )
		{
			return E_INVALIDCALL;
		}
		if( m_currentIndex >= m_images.size() || destination.m_currentIndex >= destination.m_images.size() )
		{
			return E_INVALIDCALL;
		}
		if( m_msaa.samples <= 1 )
		{
			// Resolving a single-sample image is not a resolve; vkCmdResolveImage requires
			// the source to be multisampled and the destination not to be.
			return E_INVALIDCALL;
		}

		VkImageResolve resolve = {
			{ GetAspectMaskVulkan( m_format ), 0, 0, 1 },
			{ 0, 0, 0 },
			{ GetAspectMaskVulkan( destination.m_format ), 0, 0, 1 },
			{ 0, 0, 0 },
			{ m_desc.GetWidth(), m_desc.GetHeight(), 1 }
		};

		renderContext.EndRenderPassVulkan();
		TransitionVulkan( renderContext.m_commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
		destination.TransitionVulkan( renderContext.m_commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

		vkCmdResolveImage(
			renderContext.m_commandBuffer,
			m_images[m_currentIndex],
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			destination.m_images[destination.m_currentIndex],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&resolve );
		return S_OK;
	}

	ALResult Tr2TextureAL::GenerateMipMaps( Tr2RenderContextAL& renderContext )
	{
		if( !IsValid() || !m_owner || m_currentIndex >= m_images.size() )
		{
			return E_INVALIDCALL;
		}
		const uint32_t mipCount = m_desc.GetTrueMipCount();
		if( mipCount <= 1 )
		{
			return S_OK;
		}
		if( Tr2RenderContextEnum::IsCompressedFormat( m_desc.GetFormat() ) )
		{
			// vkCmdBlitImage cannot filter a block-compressed format. Generating those
			// means decompress, downsample, recompress, which is a texture-pipeline job
			// and not something the AL should be doing behind a caller's back.
			return E_INVALIDARG;
		}

		// VK_IMAGE_LAYOUT_GENERAL for the whole image rather than the usual
		// TRANSFER_SRC/TRANSFER_DST pair, because the blit chain reads level i-1 while
		// writing level i and the layout tracker is per image, not per level (see
		// TransitionVulkan). GENERAL is legal for both ends of a blit and keeps the tracker
		// truthful; a per-level tracker would allow the tighter layouts, and this is the
		// caller that would justify writing one.
		renderContext.EndRenderPassVulkan();
		const VkImageLayout restore = GetLayoutVulkan();
		TransitionVulkan( renderContext.m_commandBuffer, VK_IMAGE_LAYOUT_GENERAL );

		const VkImageAspectFlags aspect = GetAspectMaskVulkan( m_format );
		for( uint32_t level = 1; level < mipCount; ++level )
		{
			VkImageMemoryBarrier barrier = {
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				nullptr,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_ACCESS_TRANSFER_READ_BIT,
				VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_GENERAL,
				VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED,
				m_images[m_currentIndex],
				{ aspect, level - 1, 1, 0, VK_REMAINING_ARRAY_LAYERS }
			};
			// Level i-1 was written by the previous iteration's blit and is about to be
			// read by this one. No layout change, only the write-then-read ordering.
			vkCmdPipelineBarrier( renderContext.m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier );

			VkImageBlit blit = {
				{ aspect, level - 1, 0, 1 },
				{ { 0, 0, 0 }, { int32_t( m_desc.GetMipWidth( level - 1 ) ), int32_t( m_desc.GetMipHeight( level - 1 ) ), int32_t( m_desc.GetMipDepth( level - 1 ) ) } },
				{ aspect, level, 0, 1 },
				{ { 0, 0, 0 }, { int32_t( m_desc.GetMipWidth( level ) ), int32_t( m_desc.GetMipHeight( level ) ), int32_t( m_desc.GetMipDepth( level ) ) } }
			};
			vkCmdBlitImage(
				renderContext.m_commandBuffer,
				m_images[m_currentIndex],
				VK_IMAGE_LAYOUT_GENERAL,
				m_images[m_currentIndex],
				VK_IMAGE_LAYOUT_GENERAL,
				1,
				&blit,
				VK_FILTER_LINEAR );
		}

		if( restore != VK_IMAGE_LAYOUT_UNDEFINED )
		{
			TransitionVulkan( renderContext.m_commandBuffer, restore );
		}
		return S_OK;
	}

	VkImageLayout Tr2TextureAL::GetLayoutVulkan() const
	{
		if( m_currentIndex >= m_layouts.size() )
		{
			return VK_IMAGE_LAYOUT_UNDEFINED;
		}
		return m_layouts[m_currentIndex];
	}

	void Tr2TextureAL::SetLayoutVulkan( VkImageLayout layout )
	{
		if( m_currentIndex < m_layouts.size() )
		{
			m_layouts[m_currentIndex] = layout;
		}
	}

	void Tr2TextureAL::TransitionVulkan( VkCommandBuffer commandBuffer, VkImageLayout newLayout, VkPipelineStageFlags srcStageOverride )
	{
		if( m_currentIndex >= m_images.size() || m_currentIndex >= m_layouts.size() )
		{
			return;
		}
		const VkImageLayout oldLayout = m_layouts[m_currentIndex];
		if( oldLayout == newLayout )
		{
			return;
		}

		VkPipelineStageFlags srcStage, dstStage;
		VkAccessFlags srcAccess, dstAccess;
		GetLayoutStageAccessVulkan( oldLayout, srcStage, srcAccess );
		GetLayoutStageAccessVulkan( newLayout, dstStage, dstAccess );
		if( srcStageOverride != 0 )
		{
			srcStage = srcStageOverride;
		}

		VkImageMemoryBarrier barrier = {
			VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			nullptr,
			srcAccess,
			dstAccess,
			oldLayout,
			newLayout,
			VK_QUEUE_FAMILY_IGNORED,
			VK_QUEUE_FAMILY_IGNORED,
			m_images[m_currentIndex],
			{
				GetAspectMaskVulkan( m_format ),
				0,
				// Every mip and every layer. A per-mip tracker would let mip generation
				// transition one level at a time, which is what Tr2TextureAL::GenerateMips
				// will want; until that exists, transitioning the whole image keeps the
				// tracker honest, which a partial transition would not.
				VK_REMAINING_MIP_LEVELS,
				0,
				VK_REMAINING_ARRAY_LAYERS
			}
		};
		vkCmdPipelineBarrier( commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier );

		m_layouts[m_currentIndex] = newLayout;
	}

	VkImage Tr2TextureAL::GetImageVulkan() const
	{
		return m_images[m_currentIndex];
	}

	VkImageView Tr2TextureAL::GetImageView() const
	{
		return m_imageViews[m_currentIndex];
	}

	VkImageView Tr2TextureAL::GetAttachmentViewVulkan( uint32_t mip, uint32_t layer )
	{
		if( m_currentIndex >= m_images.size() || !m_owner )
		{
			return VK_NULL_HANDLE;
		}

		// The common case, and the only one the swapchain ever hits: a texture with one
		// level and one layer already has a view of exactly the right shape.
		if( mip == 0 && layer == 0 && m_desc.GetTrueMipCount() <= 1 && m_desc.GetArraySize() <= 1 )
		{
			return m_imageViews[m_currentIndex];
		}

		const uint32_t key = ( m_currentIndex << 20 ) | ( mip << 10 ) | layer;
		auto found = m_attachmentViews.find( key );
		if( found != m_attachmentViews.end() )
		{
			return found->second;
		}

		VkImageViewCreateInfo createInfo = {
			VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			nullptr,
			0,
			m_images[m_currentIndex],
			// Always 2D, even for a 3D texture: a framebuffer attachment is one slice, and
			// a 3D view cannot be one.
			VK_IMAGE_VIEW_TYPE_2D,
			m_format,
			{
				VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY
			},
			{
				GetAspectMaskVulkan( m_format ),
				mip,
				1,
				layer,
				1
			}
		};

		VkImageView view = VK_NULL_HANDLE;
		if( FAILED( Vk2Al( vkCreateImageView( m_owner->m_device, &createInfo, nullptr, &view ) ) ) )
		{
			return VK_NULL_HANDLE;
		}
		m_attachmentViews[key] = view;
		return view;
	}

	uint32_t Tr2TextureAL::GetSrvIndexInHeap( Tr2RenderContextEnum::ColorSpace ) const
	{
		return 0xffffffff;
	}

	uint32_t Tr2TextureAL::GetUavIndexInHeap( uint32_t ) const
	{
		return 0xffffffff;
	}

	void Tr2TextureAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
	{
		description["type"] = "Tr2TextureAL";
	}

	ALResult Tr2TextureAL::SetName( const char* )
	{
		return E_NOTIMPL;
	}

	const char* Tr2TextureAL::GetName() const
	{
		return nullptr;
	}
}

#endif