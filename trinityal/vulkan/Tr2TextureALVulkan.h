// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN


#include <map>

#include "../include/Tr2TextureAL.h"
#include "../Tr2HalHelperStructures.h"

namespace TrinityALImpl
{
	class Tr2ResourceSetAL;
}

struct Tr2DisplayModeInfo;


namespace TrinityALImpl
{
	class Tr2TextureAL : public Tr2DeviceResourceAL<Tr2TextureAL>
	{
	public:
		Tr2TextureAL();
		~Tr2TextureAL();

		ALResult Create( const Tr2BitmapDimensions& desc, const Tr2MsaaDesc& msaa, Tr2GpuUsage::Type gpuUsage, Tr2CpuUsage::Type cpuUsage, Tr2SubresourceData* initialData, Tr2PrimaryRenderContextAL& renderContext );

		ALResult OpenShared( uintptr_t handle, Tr2GpuUsage::Type gpuUsage, Tr2PrimaryRenderContextAL& renderContext )
		{
			return E_NOTIMPL;
		}
		void Destroy();

		bool IsValid() const;
		Tr2ALMemoryType GetMemoryClass() const;
		const Tr2BitmapDimensions& GetDesc() const;
		const Tr2MsaaDesc& GetMsaaDesc() const;
		Tr2GpuUsage::Type GetGpuUsage() const;
		Tr2CpuUsage::Type GetCpuUsage() const;

		ALResult MapForReading( const Tr2TextureSubresource& region, const void*& data, uint32_t& pitch, Tr2RenderContextAL& renderContext )
		{
			return MapForReading( region, true, data, pitch, renderContext );
		}
		// Both directions go through a host-visible staging buffer, because a device-local
		// optimally-tiled image cannot be mapped at all and the AL's contract is a pointer
		// plus a row pitch. Reading copies image to buffer and stalls; writing fills the
		// buffer and copies it back at Unmap.
		//
		// The pitch reported is the tightly packed one for the mapped region, which is what
		// the staging buffer is laid out as -- not the image's own row pitch, which for an
		// optimally-tiled image is not a thing the application may know.
		ALResult MapForReading( const Tr2TextureSubresource& region, bool synchronize, const void*& data, uint32_t& pitch, Tr2RenderContextAL& renderContext );
		void UnmapForReading( Tr2RenderContextAL& renderContext );
		ALResult MapForWriting( const Tr2TextureSubresource& region, void*& data, uint32_t& pitch, Tr2RenderContextAL& renderContext );
		void UnmapForWriting( Tr2RenderContextAL& renderContext );

		ALResult UpdateSubresource( const Tr2TextureSubresource& region, const void* source, uint32_t pitch, uint32_t slicePitch, Tr2RenderContextAL& renderContext )
		{
			return E_NOTIMPL;
		}
		ALResult CopySubresourceRegion( const Tr2TextureSubresource& destSubresource, Tr2TextureAL& source, const Tr2TextureSubresource& sourceSubresource, Tr2RenderContextAL& renderContext );
		ALResult GenerateMipMaps( Tr2RenderContextAL& renderContext );
		ALResult Resolve( Tr2TextureAL& destination, Tr2RenderContextAL& renderContext );
		uintptr_t GetSharedHandle() const
		{
			return 0;
		}

		// Image layout, tracked per image because the back buffer holds one per swapchain
		// entry and they retire independently.
		//
		// Before this existed the backend transitioned images at a handful of hardcoded
		// points and left every other image wherever it happened to be. A texture created
		// without initial data was never transitioned at all and stayed UNDEFINED for
		// life, which is what vkQueueSubmit reported as
		// VUID-vkCmdDraw-None-09600, and a render target sampled after being drawn to sat
		// in TRANSFER_DST_OPTIMAL while its descriptor claimed SHADER_READ_ONLY_OPTIMAL,
		// which is VUID-vkCmdDraw-imageLayout-00344.
		VkImageLayout GetLayoutVulkan() const;

		// Record a transition that something else performed -- a render pass moving the
		// attachment to its finalLayout, for instance. Emits nothing.
		void SetLayoutVulkan( VkImageLayout layout );

		// Emit a barrier if the image is not already in this layout, and record it.
		// Recording a barrier inside a render pass instance is illegal, so the caller is
		// responsible for having closed one -- see EndRenderPassVulkan.
		//
		// srcStageOverride is for the one case the old layout cannot describe: a barrier
		// that has to be ordered after a semaphore wait rather than after earlier work in
		// this command buffer. Leaving it 0 derives the stage from the old layout, which
		// is what every caller but BeginFrame wants.
		void TransitionVulkan( VkCommandBuffer commandBuffer, VkImageLayout newLayout, VkPipelineStageFlags2 srcStageOverride = 0 );

		// TransitionVulkan for a transfer command about to write this image. The
		// difference is what happens when the image is already in newLayout: a layout
		// transition doubles as the ordering between the last write and the next one, so
		// the early-out that makes TransitionVulkan cheap for reads silently drops that
		// ordering for writes -- a second ClearUav on the same texture, or a copy into a
		// texture that was just cleared, raced its predecessor. In the same-layout case
		// this records a barrier with no layout change instead of nothing.
		void TransitionForTransferWriteVulkan( VkCommandBuffer commandBuffer, VkImageLayout newLayout );

		ALResult AssignFromSwapChainVulkan( const std::vector<VkImage>& backBuffers, const Tr2DisplayModeInfo& mode, Tr2PrimaryRenderContextAL& renderContext );

		// Whether GetAttachmentViewVulkan( 0, 0, true ) would return a genuine sRGB view.
		// SetPass and CreatePipeline both ask, and they must agree: the attachment's view
		// format and the pipeline's declared format are the same decision made twice.
		bool HasSrgbAttachmentViewVulkan() const;
		void SetCurrentImageVulkan( uint32_t index );
		VkImage GetImageVulkan() const;

		// The shader-resource view, in the colour space asked for.
		//
		// COLOR_SPACE_SRGB returns a view whose format is the sRGB sibling of the image's,
		// so the hardware decodes on sample. It falls back to the linear view when there is
		// no sibling, when the driver does not support sampling it, or when creating it
		// failed -- which is what dx11 does, down to the fallback being silent apart from a
		// warning. So this never returns VK_NULL_HANDLE for a valid texture, and a caller
		// asking for sRGB on a format that has no sRGB form gets undecoded data rather than
		// an error, exactly as on dx11.
		VkImageView GetImageView( Tr2RenderContextEnum::ColorSpace colorSpace = Tr2RenderContextEnum::COLOR_SPACE_LINEAR ) const;

		// A view suitable for use as a framebuffer attachment, which must name exactly one
		// mip level and one array layer -- VUID-VkFramebufferCreateInfo-pAttachments-00883.
		// GetImageView returns the shader-resource view, which spans every level, so it
		// cannot be used here: an eight-mip render target was rejected outright.
		//
		// Created on demand and cached, because most textures never become attachments and
		// the ones that do usually need only level 0 of layer 0 -- for which the
		// shader-resource view is already the right shape and is returned unchanged.
		VkImageView GetAttachmentViewVulkan( uint32_t mip, uint32_t layer, bool srgb = false );
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const;
		ALResult SetName( const char* name );
		const char* GetName() const;

		uint32_t GetSrvIndexInHeap( Tr2RenderContextEnum::ColorSpace colorSpace = Tr2RenderContextEnum::COLOR_SPACE_LINEAR ) const;
		uint32_t GetUavIndexInHeap( uint32_t mip ) const;

	private:
		std::vector<VkImage> m_images;
		std::vector<VkImageView> m_imageViews;

		// Parallel to m_imageViews, and VK_NULL_HANDLE wherever an sRGB view could not be
		// made. Empty for every texture whose format has no sRGB sibling, which is most of
		// them, so this costs nothing where it is not used.
		std::vector<VkImageView> m_srgbImageViews;
		std::vector<VkImageLayout> m_layouts;

		// Keyed by image index, mip and layer packed together; see
		// GetAttachmentViewVulkan.
		std::map<uint32_t, VkImageView> m_attachmentViews;

		// The staging buffer behind whichever map is currently open. One at a time: the AL
		// has no handle to distinguish two simultaneous maps of the same texture, so a
		// second Map before the matching Unmap is a caller error rather than something to
		// support.
		VkBuffer m_mapBuffer;
		VkDeviceMemory m_mapMemory;
		uint32_t m_mapPitch;
		Tr2TextureSubresource m_mapRegion;
		bool m_mapIsWrite;
		VkDeviceMemory m_memory;
		Tr2PrimaryRenderContextAL* m_owner;
		uint32_t m_currentIndex;
		VkFormat m_format;

		Tr2BitmapDimensions m_desc;
		Tr2MsaaDesc m_msaa;
		Tr2CpuUsage::Type m_cpuUsage;
		Tr2GpuUsage::Type m_gpuUsage;

		friend class Tr2RenderContextAL;
		friend class TrinityALImpl::Tr2ResourceSetAL;
	};
}

#endif
