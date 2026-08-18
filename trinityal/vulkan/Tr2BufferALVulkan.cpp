// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2BufferALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "UtilitiesVulkan.h"


namespace TrinityALImpl
{
	Tr2BufferAL::Tr2BufferAL()
		:m_buffer( VK_NULL_HANDLE ),
		m_memory( VK_NULL_HANDLE ),
		m_bufferView( VK_NULL_HANDLE ),
		m_owner( nullptr ),
		m_mapped( nullptr )
	{

	}

	Tr2BufferAL::~Tr2BufferAL()
	{
		Destroy();
	}

	ALResult Tr2BufferAL::Create(
		const Tr2BufferDescriptionAL& desc,
		const void* initialData,
		Tr2PrimaryRenderContextAL& renderContext )
	{
		Destroy();

		if( desc.count == 0 )
		{
			return E_INVALIDARG;
		}

		if( !renderContext.IsValid() )
		{
			return E_INVALIDCALL;
		}

		bool isImmutable = !HasFlag( desc.cpuUsage, Tr2CpuUsage::WRITE ) && !HasFlag( desc.gpuUsage, Tr2GpuUsage::UNORDERED_ACCESS );
		if( isImmutable && !initialData )
		{
			return E_INVALIDARG;
		}

		if( HasFlag( desc.cpuUsage, Tr2CpuUsage::READ ) && HasFlag( desc.cpuUsage, Tr2CpuUsage::WRITE ) )
		{
			return E_INVALIDARG;
		}

		auto stride = desc.stride;
		if( desc.format != Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN )
		{
			stride = GetBytesPerPixel( desc.format );
		}

		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::INDEX_BUFFER ) && stride != 2 && stride != 4 )
		{
			return E_INVALIDARG;
		}

		auto size = desc.count * stride;

		// TRANSFER_DST unconditionally: UpdateBuffer accepts any buffer regardless of
		// cpuUsage (the dx12 backend imposes no cpuUsage requirement there), and for a
		// device-local buffer the only way to service that is a staging copy into it.
		// Deciding the bit from initialData/WRITE here left exactly those buffers
		// without it. The bit is always legal on a VkBuffer and costs nothing.
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::INDEX_BUFFER ) )
		{
			usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		}
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::VERTEX_BUFFER ) )
		{
			usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		}
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::DRAW_INDIRECT_ARGS ) )
		{
			usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		}
		const bool isTypedBuffer = desc.format != Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN;
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::SHADER_RESOURCE ) )
		{
			usage |= isTypedBuffer ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		}
		if( HasFlag( desc.gpuUsage, Tr2GpuUsage::UNORDERED_ACCESS ) )
		{
			usage |= isTypedBuffer ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		}

		// CreateBuffer takes VkMemoryPropertyFlagBits, not VkMemoryPropertyFlags, so a
		// combined mask needs the explicit conversion -- C++ will not convert an integer
		// to an enumeration implicitly. Tr2ConstantBufferALVulkan.cpp does the same.
		//
		// HOST_COHERENT is requested alongside HOST_VISIBLE deliberately: it is what lets
		// every write path below skip vkFlushMappedMemoryRanges, and flush ranges are the
		// one thing here with a nonCoherentAtomSize alignment rule that a caller-supplied
		// offset/size cannot be trusted to satisfy.
		const bool isHostVisible = HasFlag( desc.cpuUsage, Tr2CpuUsage::READ ) || HasFlag( desc.cpuUsage, Tr2CpuUsage::WRITE );
		const VkMemoryPropertyFlagBits memoryProperty = isHostVisible
			? VkMemoryPropertyFlagBits( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT )
			: VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		FORWARD_HR( CreateBuffer( buffer, memory, size, usage, memoryProperty, renderContext ) );
		ON_BLOCK_EXIT( [&] {
			
			if( buffer ) renderContext.DestroyLaterVulkan( buffer, &vkDestroyBuffer );
			if( memory ) renderContext.DestroyLaterVulkan( memory, &vkFreeMemory );
		} );

		if( initialData )
		{
			if( isHostVisible )
			{
				// No flush: this allocation is HOST_COHERENT by construction above.
				void* mapped = nullptr;
				CR_RETURN_HR( Vk2Al( vkMapMemory( renderContext.m_device, memory, 0, size, 0, &mapped ) ) );
				memcpy( mapped, initialData, size );
				vkUnmapMemory( renderContext.m_device, memory );
			}
			else
			{
			VkBuffer stagingBuffer = VK_NULL_HANDLE;
			VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
			FORWARD_HR( CreateBuffer( stagingBuffer, stagingMemory, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, renderContext ) );

			ON_BLOCK_EXIT( [&] {
				if( stagingBuffer ) renderContext.DestroyLaterVulkan( stagingBuffer, &vkDestroyBuffer );
				if( stagingMemory ) renderContext.DestroyLaterVulkan( stagingMemory, &vkFreeMemory );
			} );


			void *mapped;
			CR_RETURN_HR( Vk2Al( vkMapMemory( renderContext.m_device, stagingMemory, 0, size, 0, &mapped ) ) );
			memcpy( mapped, initialData, size );

			VkMappedMemoryRange flushRange = {
				VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
				nullptr,
				stagingMemory,
				0,
				VK_WHOLE_SIZE
			};
			vkFlushMappedMemoryRanges( renderContext.m_device, 1, &flushRange );
			vkUnmapMemory( renderContext.m_device, stagingMemory );

			VkBufferCopy copyInfo = { 0, 0, size };
			vkCmdCopyBuffer( renderContext.m_commandBuffer, stagingBuffer, buffer, 1, &copyInfo );

			VkBufferMemoryBarrier barrier = {
				VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				nullptr,
				VK_ACCESS_MEMORY_WRITE_BIT,
				VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
				VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED,
				buffer,
				0,
				VK_WHOLE_SIZE
			};
			vkCmdPipelineBarrier( renderContext.m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr );
			}
		}

		VkBufferView bufferView = VK_NULL_HANDLE;
		if( isTypedBuffer && ( HasFlag( desc.gpuUsage, Tr2GpuUsage::SHADER_RESOURCE ) || HasFlag( desc.gpuUsage, Tr2GpuUsage::UNORDERED_ACCESS ) ) )
		{
			VkBufferViewCreateInfo viewInfo = {
				VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
				nullptr,
				0,
				buffer,
				GetVulkanFormat( desc.format ),
				0,
				VK_WHOLE_SIZE
			};
			CR_RETURN_HR( Vk2Al( vkCreateBufferView( renderContext.m_device, &viewInfo, nullptr, &bufferView ) ) );
		}

		m_buffer = buffer;
		buffer = VK_NULL_HANDLE;
		m_memory = memory;
		memory = VK_NULL_HANDLE;
		m_bufferView = bufferView;
		bufferView = VK_NULL_HANDLE;

		m_owner = &renderContext;
		m_desc = desc;

		return S_OK;
	}

	void Tr2BufferAL::Destroy()
	{
		if( m_buffer )
		{
			// Freeing implicitly unmaps, but m_mapped has to be cleared with it or a
			// reused Tr2BufferAL would carry a stale "still mapped" into its next Create.
			if( m_mapped )
			{
				vkUnmapMemory( m_owner->m_device, m_memory );
				m_mapped = nullptr;
			}

			if( m_bufferView )
			{
				m_owner->DestroyLaterVulkan( m_bufferView, &vkDestroyBufferView );
				m_bufferView = VK_NULL_HANDLE;
			}

			m_owner->DestroyLaterVulkan( m_buffer, &vkDestroyBuffer );
			m_buffer = VK_NULL_HANDLE;

			m_owner->DestroyLaterVulkan( m_memory, &vkFreeMemory );
			m_memory = VK_NULL_HANDLE;

			m_owner = nullptr;
			m_desc = Tr2BufferDescriptionAL();
		}
	}

	bool Tr2BufferAL::IsValid() const
	{
		return m_buffer != VK_NULL_HANDLE;
	}

	Tr2ALMemoryType Tr2BufferAL::GetMemoryClass() const
	{
		return AL_MEMORY_VIDEO;
	}

	const Tr2BufferDescriptionAL& Tr2BufferAL::GetDesc() const
	{
		return m_desc;
	}

	// The buffer's own extent in bytes, by the same stride rule Create uses.
	static uint32_t ByteSize( const Tr2BufferDescriptionAL& desc )
	{
		auto stride = desc.stride;
		if( desc.format != Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN )
		{
			stride = GetBytesPerPixel( desc.format );
		}
		return desc.count * stride;
	}

	// Two things are true of every map path below and neither is obvious.
	//
	// The device comes from m_owner, never from the caller's context. Tr2RenderContextAL
	// -- the base class these overrides take by reference -- has no m_device member at
	// all; only Tr2PrimaryRenderContextAL does. The memory belongs to the context that
	// created the buffer regardless, which is why dx12 also routes through *m_owner.
	//
	// m_mapped, not IsValid(), is what guards the unmaps. The AL's own tests call
	// UnmapForReading immediately after a MapForReading they expect to fail --
	// Buffer.LockingNonReadableBufferForReadingFails and its writing twin -- and
	// vkUnmapMemory on memory that is not currently mapped is undefined behaviour, not a
	// tolerated no-op. Keying off IsValid() sent both of those straight into it, and in
	// the reading case the allocation is DEVICE_LOCAL and was never mappable to begin
	// with. m_mapped also rejects a second Map while one is outstanding.
	ALResult Tr2BufferAL::MapForReading( const void*& data, Tr2RenderContextAL& renderContext )
	{
		return MapForReading( data, 0, ByteSize( m_desc ), renderContext );
	}
	ALResult Tr2BufferAL::MapForReading( const void*& data, uint32_t offset, uint32_t size, Tr2RenderContextAL& renderContext )
	{
		if( !IsValid() )
		{
			return E_INVALIDCALL;
		}
		if( !HasFlag( m_desc.cpuUsage, Tr2CpuUsage::READ ) )
		{
			return E_INVALIDCALL;
		}
		if( size == 0 || uint64_t( offset ) + size > ByteSize( m_desc ) )
		{
			return E_INVALIDARG;
		}
		if( m_mapped )
		{
			return E_INVALIDCALL;
		}

		// The GPU may still owe us this buffer's contents -- a compute shader that wrote
		// it through a UAV, or Create's own staging copy, is only recorded, not run.
		// Make those writes available to the host, then submit and wait. Without the
		// wait the map returns memory the GPU has not touched yet, which is what made
		// every Compute test read back zeros while reporting S_OK.
		VkBufferMemoryBarrier barrier = {
			VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			nullptr,
			VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_HOST_READ_BIT,
			VK_QUEUE_FAMILY_IGNORED,
			VK_QUEUE_FAMILY_IGNORED,
			m_buffer,
			0,
			VK_WHOLE_SIZE
		};
		vkCmdPipelineBarrier(
			m_owner->m_commandBuffer,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_HOST_BIT,
			0, 0, nullptr, 1, &barrier, 0, nullptr );

		FORWARD_HR( m_owner->FlushAndSyncVulkan() );

		// No vkInvalidateMappedMemoryRanges: the allocation is HOST_COHERENT.
		void* mapped = nullptr;
		CR_RETURN_HR( Vk2Al( vkMapMemory( m_owner->m_device, m_memory, offset, size, 0, &mapped ) ) );
		m_mapped = mapped;
		data = mapped;
		return S_OK;
	}
	void Tr2BufferAL::UnmapForReading( Tr2RenderContextAL& renderContext )
	{
		if( m_mapped )
		{
			vkUnmapMemory( m_owner->m_device, m_memory );
			m_mapped = nullptr;
		}
	}
	ALResult Tr2BufferAL::MapForWriting( void*& data, Tr2RenderContextAL& renderContext )
	{
		if( !IsValid() )
		{
			return E_INVALIDCALL;
		}
		if( !HasFlag( m_desc.cpuUsage, Tr2CpuUsage::WRITE ) )
		{
			return E_INVALIDCALL;
		}
		if( m_mapped )
		{
			return E_INVALIDCALL;
		}
		void* mapped = nullptr;
		CR_RETURN_HR( Vk2Al( vkMapMemory( m_owner->m_device, m_memory, 0, VK_WHOLE_SIZE, 0, &mapped ) ) );
		m_mapped = mapped;
		data = mapped;
		return S_OK;
	}
	void Tr2BufferAL::UnmapForWriting( Tr2RenderContextAL& renderContext )
	{
		// No vkFlushMappedMemoryRanges: the allocation is HOST_COHERENT. Flushing a
		// caller-shaped range here would also have to satisfy nonCoherentAtomSize, which
		// an arbitrary offset/size does not.
		if( m_mapped )
		{
			vkUnmapMemory( m_owner->m_device, m_memory );
			m_mapped = nullptr;
		}
	}

	ALResult Tr2BufferAL::UpdateBuffer( uint32_t offset, uint32_t size, const void* data, Tr2RenderContextAL & renderContext )
	{
		if( !IsValid() )
		{
			return E_INVALIDCALL;
		}
		const uint32_t byteSize = ByteSize( m_desc );
		if( size == 0 )
		{
			// dx12's convention, matched deliberately: zero means the whole buffer.
			size = byteSize;
		}
		if( uint64_t( offset ) + size > byteSize )
		{
			return E_INVALIDARG;
		}
		if( m_mapped )
		{
			return E_INVALIDCALL;
		}

		if( HasFlag( m_desc.cpuUsage, Tr2CpuUsage::READ ) || HasFlag( m_desc.cpuUsage, Tr2CpuUsage::WRITE ) )
		{
			void* mapped = nullptr;
			CR_RETURN_HR( Vk2Al( vkMapMemory( m_owner->m_device, m_memory, offset, size, 0, &mapped ) ) );
			memcpy( mapped, data, size );
			vkUnmapMemory( m_owner->m_device, m_memory );
			return S_OK;
		}

		// Device-local memory cannot be mapped at all, so this is a staging copy -- the
		// same shape Create takes for initialData, and the reason TRANSFER_DST is set
		// unconditionally there. Mapping m_memory directly here was a VUID-vkMapMemory-
		// memory-00682 violation for every buffer created without READ or WRITE.
		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
		FORWARD_HR( CreateBuffer( stagingBuffer, stagingMemory, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, *m_owner ) );
		ON_BLOCK_EXIT( [&] {
			if( stagingBuffer ) m_owner->DestroyLaterVulkan( stagingBuffer, &vkDestroyBuffer );
			if( stagingMemory ) m_owner->DestroyLaterVulkan( stagingMemory, &vkFreeMemory );
		} );

		void* mapped = nullptr;
		CR_RETURN_HR( Vk2Al( vkMapMemory( m_owner->m_device, stagingMemory, 0, size, 0, &mapped ) ) );
		memcpy( mapped, data, size );

		// This one does need a flush: the staging allocation asked for HOST_VISIBLE only,
		// so it may be non-coherent. VK_WHOLE_SIZE is the range shape that is exempt from
		// the nonCoherentAtomSize rule, which is why it is spelled that way rather than
		// as 0/size.
		VkMappedMemoryRange flushRange = {
			VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
			nullptr,
			stagingMemory,
			0,
			VK_WHOLE_SIZE
		};
		vkFlushMappedMemoryRanges( m_owner->m_device, 1, &flushRange );
		vkUnmapMemory( m_owner->m_device, stagingMemory );

		VkBufferCopy copyInfo = { 0, offset, size };
		vkCmdCopyBuffer( m_owner->m_commandBuffer, stagingBuffer, m_buffer, 1, &copyInfo );

		VkBufferMemoryBarrier barrier = {
			VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			nullptr,
			VK_ACCESS_MEMORY_WRITE_BIT,
			VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
			VK_QUEUE_FAMILY_IGNORED,
			VK_QUEUE_FAMILY_IGNORED,
			m_buffer,
			0,
			VK_WHOLE_SIZE
		};
		vkCmdPipelineBarrier( m_owner->m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr );
		return S_OK;
	}

	uint32_t Tr2BufferAL::GetSrvIndexInHeap() const
	{
		return 0xffffffff;
	}

	uint32_t Tr2BufferAL::GetUavIndexInHeap() const
	{
		return 0xffffffff;
	}

	void Tr2BufferAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
	{
		description["type"] = "Tr2BufferAL";
		description["size"] = std::to_string( GetDesc().count * GetDesc().stride );
	}

	ALResult Tr2BufferAL::SetName( const char* )
	{
		return E_NOTIMPL;
	}
}


#endif