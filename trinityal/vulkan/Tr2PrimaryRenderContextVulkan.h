// Copyright © 2026 CCP ehf.

#pragma once


#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../Tr2MemoryCounterAL.h"
#include "../include/Tr2RenderContextAL.h"
#include "../include/Tr2CapsAL.h"
#include "../include/Tr2SamplerStateAL.h"
#include "../include/Tr2TextureAL.h"
#include "../Tr2AdapterStructures.h"



class Tr2PrimaryRenderContextAL : public Tr2RenderContextAL
{
public:
	Tr2PrimaryRenderContextAL();
	~Tr2PrimaryRenderContextAL();

	ALResult CreateDevice( uint32_t adapter, Tr2WindowHandle focusWindow, const Tr2PresentParametersAL& presentationParameters );
	void Destroy();

	ALResult SetPresentParameters( unsigned adapter, const Tr2PresentParametersAL& pPresentationParameters );

	const Tr2CapsAL& GetCaps() const
	{
		return m_caps;
	}
		
	ALResult Present();

	bool IsValid() const;

	// The swapchain's format, which the default back buffer already carries -- it is set
	// from the presentation mode in AssignFromSwapChainVulkan. Returning UNKNOWN was a
	// placeholder, and one that no caller could distinguish from "there is no device".
	Tr2RenderContextEnum::PixelFormat GetBackBufferFormat() const
	{
		return m_defaultBackBuffer.IsValid()
			? m_defaultBackBuffer.GetFormat()
			: Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN;
	}
	
	static const uint32_t SHADER_TYPE_MASK = 
		( 1 << Tr2RenderContextEnum::VERTEX_SHADER ) |
		( 1 << Tr2RenderContextEnum::PIXEL_SHADER ) |
		( 1 << Tr2RenderContextEnum::COMPUTE_SHADER ) |
		( 1 << Tr2RenderContextEnum::GEOMETRY_SHADER ) |
		( 1 << Tr2RenderContextEnum::HULL_SHADER ) |
		( 1 << Tr2RenderContextEnum::DOMAIN_SHADER );

	template <typename T>
	void DestroyLaterVulkan( T object, void( VKAPI_CALL *destroyFunction )( VkDevice, T, const VkAllocationCallbacks* ) )
	{
		if( !object )
		{
			return;
		}
		PendingDestroy pd = { (VkBuffer)object, (DestroyFunction)destroyFunction };
		m_frameData[m_frameIndex].pendingDestroys.push_back( pd );
	}

	template <typename T>
	void DestroyLaterVulkan( const std::vector<T> objects, void( VKAPI_CALL *destroyFunction )( VkDevice, T, const VkAllocationCallbacks* ) )
	{
		for( auto it = begin( objects ); it != end( objects ); ++it )
		{
			this->DestroyLaterVulkan( *it, destroyFunction );
		}
	}

	VkBuffer GetZeroBufferVulkan() const;
public:
	Tr2TextureAL m_defaultBackBuffer;
		
	ITr2RenderContextEvents* m_events;

public:
	TrinityALImpl::Tr2SamplerStateALFactory m_samplerStateFactory;

	Tr2TextureAL&			GetDefaultBackBuffer()
	{
		return m_defaultBackBuffer;
	}

	// Submit everything recorded so far and wait for the GPU to finish it, then carry on
	// recording into the same command buffer. The dx12 equivalent is FlushAndSyncDx12,
	// and it is called from the same place: a CPU read of something the GPU wrote.
	//
	// Until this existed the only vkQueueSubmit in the backend was the one inside
	// Present, so there was no way to make GPU work observable without also presenting a
	// frame -- and every Map-for-read returned host memory the GPU had not written yet.
	// That is what made all three Compute tests return zeros while every AL call
	// reported S_OK.
	//
	// This is a full stall by design, as dx12's is. It is a readback path.
	ALResult FlushAndSyncVulkan();

	// Frame numbering. dx12 and the stub backend both expose this pair, and everything in
	// the AL that has to answer "has the GPU finished the work I recorded?" is written
	// against it: Tr2FenceAL, Tr2OcclusionQueryAL, Tr2GpuTimerAL and
	// Tr2PipelineStatsQueryAL each store a recording frame number at record time and
	// compare it against the rendered one at read time. Vulkan had neither number, which
	// is the single reason all four were E_NOTIMPL.
	//
	// GetRenderedFrameNumber is the largest N for which every frame up to and including N
	// has completed on the GPU. It polls the per-frame fences rather than caching, so it
	// needs no callback and cannot go stale. A FlushAndSyncVulkan also advances it,
	// because after that vkQueueWaitIdle everything submitted so far is done.
	uint64_t GetRecordingFrameNumber() const;
	uint64_t GetRenderedFrameNumber() const;

private:
	Tr2PrimaryRenderContextAL( const Tr2PrimaryRenderContextAL& ) /* = delete */;
	Tr2PrimaryRenderContextAL& operator=( const Tr2PrimaryRenderContextAL& ) /* = delete */;

	typedef void( VKAPI_CALL *DestroyFunction )( VkDevice, VkBuffer, const VkAllocationCallbacks* );

	struct PendingDestroy
	{
		VkBuffer object;
		DestroyFunction destroyFunction;
	};

	struct FrameData 
	{
		//VkFramebuffer framebuffer;
		VkCommandBuffer commandBuffer;
		VkSemaphore imageAvailableSemaphore;
		VkFence fence;
		std::vector<PendingDestroy> pendingDestroys;

		// Which absolute frame this slot's fence belongs to, or 0 if the slot has never
		// been submitted. Needed because the fence array cycles and the frame number does
		// not, so a signalled fence on its own does not say *which* frame finished.
		uint64_t submittedFrame;

		// Whether a vkQueueSubmit that signals this slot's fence is outstanding, which is
		// not the same question as whether the slot has ever been used.
		//
		// BeginFrame resets the fence and Present signals it, and those are not the same
		// code path: SetPresentParameters ends the frame it was recording and rebuilds
		// without ever submitting it. That leaves a fence reset by BeginFrame and signalled
		// by nobody, and one lap round the ring later the wait on it can only time out --
		// for the whole budget, every time, deterministically.
		//
		// It was invisible until the timeout stopped being stepped over. So the wait is
		// conditional on this rather than on the slot having been used, and the fences are
		// created *unsignalled*: nothing waits on a fence with no submit behind it, so
		// nothing needs the initial signal that used to paper over the first lap.
		bool fencePending;

		FrameData();
	};

	// The pipeline stage at which a submit waits on imageAvailableSemaphore, and therefore
	// the earliest stage at which anything may touch the acquired image.
	//
	// It has to appear in the srcStageMask of the first barrier of the frame as well, or
	// that barrier's layout transition is not ordered after the wait and races the
	// presentation engine's read -- SYNC-HAZARD-WRITE-AFTER-READ, one per frame, which is
	// what happened the moment the barrier stopped naming this stage by hand. Two places
	// have to agree and neither can see the other, so the constant is the agreement.
	static const VkPipelineStageFlags ACQUIRE_WAIT_STAGE = VK_PIPELINE_STAGE_TRANSFER_BIT;

	static const uint32_t VIRTUAL_FRAMES = 3;
	FrameData m_frameData[VIRTUAL_FRAMES];
	uint32_t m_frameIndex;

	// imageAvailableSemaphore is a binary semaphore signalled by vkAcquireNextImageKHR in
	// BeginFrame, and BeginFrame immediately records a barrier against the acquired image
	// -- so whichever submit carries that barrier has to wait on it. Normally that is
	// Present's. When FlushAndSyncVulkan submits first it must take the wait instead, and
	// Present must then not wait on an already-consumed semaphore, which would never be
	// signalled again and would hang. This flag is which of the two has it.
	bool m_acquireWaited;

	// True exactly while m_commandBuffer is in the recording state.
	//
	// BeginFrame is the only thing that calls vkBeginCommandBuffer, and it has three ways
	// to return before reaching it -- a minimised window, a failed acquire, and a fence
	// wait that did not succeed. On any of them m_commandBuffer still names the *previous*
	// slot's buffer, which is submitted and pending, and the next Present would end it and
	// submit it a second time: VUID-vkQueueSubmit-pCommandBuffers-00071. So Present has to
	// know whether there is a frame to present at all, and this is how.
	bool m_commandBufferRecording;

	// The frame being recorded right now; 0 before the first BeginFrame. Monotonic for the
	// life of the device, unlike m_frameIndex, which cycles 0..VIRTUAL_FRAMES-1.
	uint64_t m_recordingFrame;

	// Raised by FlushAndSyncVulkan once its vkQueueWaitIdle returns: everything submitted
	// up to and including that frame is complete, whether or not the frame has reached a
	// fence yet. Without this a Tr2FenceAL::Wait would return with IsReached still false.
	uint64_t m_flushedFrame;

	ALResult BeginFrame();

	// Throw the swapchain away and build it again against the surface as it is now. One
	// action for every cause -- a present that reported SUBOPTIMAL or OUT_OF_DATE, a submit
	// that failed, or SetPresentParameters -- because they all leave the same mess and there
	// is only one sound way out of it.
	//
	// The per-frame acquire semaphores are recreated too. That is the point: a failed submit
	// leaves one signalled with nothing to wait on it, Vulkan has no vkResetSemaphore, and
	// the next vkAcquireNextImageKHR on a signalled semaphore is illegal. Waiting the device
	// idle and building new ones is the only way to drain that state.
	ALResult RebuildSwapChainVulkan();

	Tr2CapsAL m_caps;
	VkQueue m_graphicsQueue;
	VkQueue m_presentQueue;

	VkSwapchainKHR m_swapChain;
	VkSurfaceKHR m_surface;

	// Kept so the swapchain can be rebuilt without the caller having to hand them over
	// again -- a rebuild triggered by VK_ERROR_OUT_OF_DATE_KHR has no caller to ask.
	Tr2PresentParametersAL m_presentParameters;

	// Set when the swapchain has to be thrown away and built again: a present that reported
	// SUBOPTIMAL or OUT_OF_DATE, a submit that failed, or SetPresentParameters. Acted on in
	// BeginFrame, which is the one place that is between frames by construction.
	//
	// Recovery is deliberately one action for all of those causes. A failed submit leaves
	// the acquire semaphore signalled with nothing to wait on it, and Vulkan has no
	// vkResetSemaphore -- so the only sound way back is to wait the device idle and rebuild
	// the sync objects along with the swapchain. Trying to un-signal a semaphore, or to
	// carry on from a half-submitted frame, is what makes this look like it needs cleverness.
	bool m_needsSwapChainRebuild;
	uint32_t m_currentImage;

	// One binary semaphore per swapchain image, NOT per virtual frame. It is signalled by
	// Present's submit and waited on by vkQueuePresentKHR for image m_currentImage, so it
	// stays in use until that present completes -- and the only thing that proves a present
	// completed is the image coming back out of vkAcquireNextImageKHR. The frame fence does
	// not prove it: the fence says the submit finished, not the present.
	//
	// Frame index is not that proof either. The presentation engine hands back whichever
	// image is free, so the two indices desync -- observed acquire order on a 3-image
	// swapchain with VIRTUAL_FRAMES == 3 was 2,0,1,2,0,1,2,2. At that last 2 the frame index
	// had come round to the semaphore last used to present image 0, which had not been
	// re-acquired, so signalling it again broke
	// VUID-vkQueueSubmit-pSignalSemaphores-00067 and this driver answered the submit with
	// VK_ERROR_DEVICE_LOST.
	std::vector<VkSemaphore> m_finishedRenderingSemaphores;

	VkCommandPool m_commandPool;

	VkBuffer m_zeroBuffer;
	VkDeviceMemory m_zeroBufferMemory;
public:
	VkDevice m_device;
	VkPhysicalDevice m_physicalDevice;
	VkPhysicalDeviceProperties m_physicalDeviceProperties;

	// What vkCreateDevice was actually given, which is the intersection of what this
	// backend wants and what the device reports -- so a caller that needs a feature asks
	// here rather than assuming it got what it asked for.
	VkPhysicalDeviceFeatures m_enabledFeatures;

	std::map<unsigned, VkRenderPass> m_renderPasses;
	std::map<unsigned, VkPipeline> m_pipelines;
};

#endif
