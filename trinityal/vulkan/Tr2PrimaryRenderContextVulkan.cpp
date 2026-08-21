// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2VideoAdapterInfoALVulkan.h"
#include "ALLog.h"
#include "VkResult.h"
#include "ITr2RenderContextEvents.h"
#include "Tr2AdapterStructures.h"
#include "UtilitiesVulkan.h"
#include "Tr2TextureALVulkan.h"

namespace
{
	// Ten seconds. Long enough that no amount of validation-layer overhead reaches it --
	// the old one-second budget did, which is what BeginFrame's comment is about -- and
	// bounded so that a hung GPU on a platform without a TDR still terminates.
	const uint64_t FRAME_FENCE_TIMEOUT_NS = 10000000000ull;

	bool FindPresentableQueues( VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t& graphicsQueue, uint32_t& presentQueue )
	{
		graphicsQueue = 0xffffffff;
		presentQueue = 0xffffffff;

		std::vector<VkQueueFamilyProperties> queue_family_properties;
		TrinityALImpl::QueryArrayNoFail( &vkGetPhysicalDeviceQueueFamilyProperties, device, queue_family_properties );
		if( queue_family_properties.empty() )
		{
			return false;
		}

		std::vector<VkBool32> presentSupport( queue_family_properties.size() );

		for( uint32_t j = 0; j < uint32_t( queue_family_properties.size() ); ++j )
		{
			vkGetPhysicalDeviceSurfaceSupportKHR( device, j, surface, &presentSupport[j] );

			if( ( queue_family_properties[j].queueCount > 0 ) && ( queue_family_properties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT ) )
			{
				if( graphicsQueue == 0xffffffff )
				{
					graphicsQueue = j;
				}
				if( presentSupport[j] )
				{
					graphicsQueue = j;
					presentQueue = j;
					return true;
				}
			}
		}
		if( graphicsQueue == 0xffffffff )
		{
			return false;
		}
		for( uint32_t i = 0; i < uint32_t( queue_family_properties.size() ); ++i )
		{
			if( presentSupport[i] )
			{
				presentQueue = i;
				return true;
			}
		}
		return false;
	}

	uint32_t GetSwapChainNumImages( VkSurfaceCapabilitiesKHR &surfaceCapabilities ) 
	{
		// Set of images defined in a swap chain may not always be available for application to render to:
		// One may be displayed and one may wait in a queue to be presented
		// If application wants to use more images at the same time it must ask for more images
		uint32_t count = surfaceCapabilities.minImageCount + 1;
		if( ( surfaceCapabilities.maxImageCount > 0 ) && ( count > surfaceCapabilities.maxImageCount ) ) 
		{
			count = surfaceCapabilities.maxImageCount;
		}
		return count;
	}

	// mode.format is the requested back buffer format, and dx11 honours it:
	// SafeConvertD3DBackBufferFormat feeds it straight into DXGI_SWAP_CHAIN_DESC and only
	// substitutes a default for UNKNOWN and B8G8R8X8.
	//
	// This used to ignore the request entirely and always prefer R8G8B8A8_UNORM, while
	// AssignFromSwapChainVulkan went on building the back buffer image views -- and
	// therefore the render pass attachment descriptions -- from mode.format, which the
	// tests leave at the B8G8R8A8_UNORM that GetAdapterDisplayMode reports. A
	// B8G8R8A8_UNORM view over an R8G8B8A8_UNORM image: 93 of the suite's 117 validation
	// errors were VUID-VkImageViewCreateInfo-image-01762, and every one of them was this
	// line. Undefined behaviour, and the shape it takes on a driver that lets it through is
	// red and blue swapped in every presented frame -- which no test here could catch,
	// because nothing on this backend compares back buffer pixels against a reference.
	VkSurfaceFormatKHR GetSwapChainFormat( const std::vector<VkSurfaceFormatKHR>& surfaceFormats, Tr2RenderContextEnum::PixelFormat requested )
	{
		const VkFormat requestedFormat = TrinityALImpl::GetVulkanFormat( requested );

		// A single VK_FORMAT_UNDEFINED entry means the surface has no preference and
		// anything goes, so the request wins outright.
		if( ( surfaceFormats.size() == 1 ) && ( surfaceFormats[0].format == VK_FORMAT_UNDEFINED ) )
		{
			VkSurfaceFormatKHR fmt = {
				requestedFormat != VK_FORMAT_UNDEFINED ? requestedFormat : VK_FORMAT_B8G8R8A8_UNORM,
				VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
			};
			return fmt;
		}

		// What was asked for, then the two ubiquitous 8-bit orderings, then whatever the
		// surface listed first. Every fallback changes the format the back buffer reports,
		// which is why the caller sets out.mode.format from the answer and not the request.
		//
		// The colour space is pinned as well as the format. This surface lists
		// A2B10G10R10_UNORM_PACK32 twice, once for HDR10_ST2084 and once for
		// SRGB_NONLINEAR, and matching on format alone would take whichever came first.
		const VkFormat preferred[] = { requestedFormat, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
		for( size_t p = 0; p < sizeof( preferred ) / sizeof( preferred[0] ); ++p )
		{
			if( preferred[p] == VK_FORMAT_UNDEFINED )
			{
				continue;
			}
			for( auto surfaceFormat = begin( surfaceFormats ); surfaceFormat != end( surfaceFormats ); ++surfaceFormat )
			{
				if( surfaceFormat->format == preferred[p] && surfaceFormat->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR )
				{
					return *surfaceFormat;
				}
			}
		}

		return surfaceFormats[0];
	}

	VkExtent2D GetSwapChainExtent( VkSurfaceCapabilitiesKHR &surfaceCapabilities ) 
	{
		// Special value of surface extent is width == height == -1
		// If this is so we define the size by ourselves but it must fit within defined confines
		if( surfaceCapabilities.currentExtent.width == -1 ) 
		{
			VkExtent2D swap_chain_extent = { 640, 480 };
			if( swap_chain_extent.width < surfaceCapabilities.minImageExtent.width ) 
			{
				swap_chain_extent.width = surfaceCapabilities.minImageExtent.width;
			}
			if( swap_chain_extent.height < surfaceCapabilities.minImageExtent.height ) 
			{
				swap_chain_extent.height = surfaceCapabilities.minImageExtent.height;
			}
			if( swap_chain_extent.width > surfaceCapabilities.maxImageExtent.width ) 
			{
				swap_chain_extent.width = surfaceCapabilities.maxImageExtent.width;
			}
			if( swap_chain_extent.height > surfaceCapabilities.maxImageExtent.height ) 
			{
				swap_chain_extent.height = surfaceCapabilities.maxImageExtent.height;
			}
			return swap_chain_extent;
		}

		// Most of the cases we define size of the swap_chain images equal to current window's size
		return surfaceCapabilities.currentExtent;
	}

	VkImageUsageFlags GetSwapChainUsageFlags( VkSurfaceCapabilitiesKHR &surface_capabilities ) 
	{
		// Color attachment flag must always be supported
		// We can define other usage flags but we always need to check if they are supported
		if( surface_capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT ) 
		{
			return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}
		return static_cast<VkImageUsageFlags>( -1 );
	}

	VkSurfaceTransformFlagBitsKHR GetSwapChainTransform( VkSurfaceCapabilitiesKHR &surface_capabilities ) 
	{
		// Sometimes images must be transformed before they are presented (i.e. due to device's orienation
		// being other than default orientation)
		// If the specified transform is other than current transform, presentation engine will transform image
		// during presentation operation; this operation may hit performance on some platforms
		// Here we don't want any transformations to occur so if the identity transform is supported use it
		// otherwise just use the same transform as current transform
		if( surface_capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ) 
		{
			return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		}
		else 
		{
			return surface_capabilities.currentTransform;
		}
	}

	VkPresentModeKHR GetSwapChainPresentMode( Tr2RenderContextEnum::PresentInterval interval, std::vector<VkPresentModeKHR> &present_modes ) 
	{
		if( interval == Tr2RenderContextEnum::PRESENT_INTERVAL_IMMEDIATE )
		{
			if( std::find( begin( present_modes ), end( present_modes ), VK_PRESENT_MODE_IMMEDIATE_KHR ) != end( present_modes ) )
			{
				return VK_PRESENT_MODE_IMMEDIATE_KHR;
			}
			if( std::find( begin( present_modes ), end( present_modes ), VK_PRESENT_MODE_MAILBOX_KHR ) != end( present_modes ) )
			{
				return VK_PRESENT_MODE_MAILBOX_KHR;
			}
		}
		if( std::find( begin( present_modes ), end( present_modes ), VK_PRESENT_MODE_FIFO_KHR ) != end( present_modes ) )
		{
			return VK_PRESENT_MODE_FIFO_KHR;
		}
		return static_cast<VkPresentModeKHR>( -1 );
	}

	struct SwapChainObjects
	{
		VkSwapchainKHR swapChain;
		std::vector<VkImage> backBuffers;
		std::vector<VkSemaphore> finishedRenderingSemaphores;

		// What was actually created, which is not necessarily what was asked for -- see the
		// extent note in BuildSwapChain.
		Tr2DisplayModeInfo mode;

		SwapChainObjects() : swapChain( VK_NULL_HANDLE ) {}
	};

	// The single place that decides swapchain parameters. CreateDevice and every rebuild go
	// through it, so the two cannot drift on format, image count or present mode.
	ALResult BuildSwapChain(
		VkDevice device,
		VkPhysicalDevice physicalDevice,
		VkSurfaceKHR surface,
		const Tr2PresentParametersAL& parameters,
		VkSwapchainKHR oldSwapChain,
		bool mutableFormat,
		SwapChainObjects& out )
	{
		VkSurfaceCapabilitiesKHR surfaceCapabilities;
		CR_RETURN_HR( Vk2Al( vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physicalDevice, surface, &surfaceCapabilities ) ) );

		std::vector<VkSurfaceFormatKHR> surfaceFormats;
		FORWARD_HR( TrinityALImpl::QueryArrayNotEmpty( &vkGetPhysicalDeviceSurfaceFormatsKHR, physicalDevice, surface, surfaceFormats ) );

		std::vector<VkPresentModeKHR> presentModes;
		FORWARD_HR( TrinityALImpl::QueryArrayNotEmpty( &vkGetPhysicalDeviceSurfacePresentModesKHR, physicalDevice, surface, presentModes ) );

		// The extent comes from the surface, not from the parameters.
		//
		// When currentExtent is not the "application chooses" sentinel it is the *only*
		// legal imageExtent, and on Win32 and Android it is always the real client area.
		// This used to pass parameters.mode straight through, which asked for 1920x1080
		// against a 640x480 window: eighteen VUID-VkSwapchainCreateInfoKHR-imageExtent
		// violations across eight tests, two of which were green because the back buffer
		// reported the *request* rather than what exists.
		//
		// So the AL's mode is a request, not a guarantee. DXGI can honour it because a DXGI
		// swapchain's buffers are independent of the window and scale at present time;
		// Vulkan has no equivalent without VK_KHR_swapchain_maintenance1, and emulating it
		// means rendering offscreen and blitting. That is a separate decision, and until it
		// is taken the back buffer reports what was actually created.
		VkExtent2D desiredExtent = { parameters.mode.width, parameters.mode.height };
		if( surfaceCapabilities.currentExtent.width != 0xFFFFFFFF )
		{
			desiredExtent = surfaceCapabilities.currentExtent;
		}
		else
		{
			if( desiredExtent.width < surfaceCapabilities.minImageExtent.width )   desiredExtent.width = surfaceCapabilities.minImageExtent.width;
			if( desiredExtent.height < surfaceCapabilities.minImageExtent.height ) desiredExtent.height = surfaceCapabilities.minImageExtent.height;
			if( desiredExtent.width > surfaceCapabilities.maxImageExtent.width )   desiredExtent.width = surfaceCapabilities.maxImageExtent.width;
			if( desiredExtent.height > surfaceCapabilities.maxImageExtent.height ) desiredExtent.height = surfaceCapabilities.maxImageExtent.height;
		}

		// A zero extent means the window is minimised. There is nothing legal to create, and
		// the caller has to try again later rather than treat it as a hard failure.
		if( desiredExtent.width == 0 || desiredExtent.height == 0 )
		{
			return S_FALSE;
		}

		const uint32_t desiredImageCount = GetSwapChainNumImages( surfaceCapabilities );
		const VkSurfaceFormatKHR desiredFormat = GetSwapChainFormat( surfaceFormats, parameters.mode.format );
		const VkImageUsageFlags desiredUsage = GetSwapChainUsageFlags( surfaceCapabilities );
		const VkSurfaceTransformFlagBitsKHR desiredTransform = GetSwapChainTransform( surfaceCapabilities );
		const VkPresentModeKHR desiredPresentMode = GetSwapChainPresentMode( parameters.presentInterval, presentModes );

		if( static_cast<int>( desiredUsage ) == -1 || static_cast<int>( desiredPresentMode ) == -1 )
		{
			return E_FAIL;
		}

		VkSwapchainCreateInfoKHR swapChainCreateInfo = {
			VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			nullptr,
			0,
			surface,
			desiredImageCount,
			desiredFormat.format,
			desiredFormat.colorSpace,
			desiredExtent,
			1,
			desiredUsage,
			VK_SHARING_MODE_EXCLUSIVE,
			0,
			nullptr,
			desiredTransform,
			VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			desiredPresentMode,
			VK_TRUE,
			// Handing the old swapchain over lets the driver reuse what it can and avoids a
			// blank window on some platforms. It also means the old one must be destroyed
			// after this call, and never presented to again.
			oldSwapChain
		};

		// RS_SRGBWRITEENABLE's mechanism (section 24e): the swapchain images get an sRGB
		// sibling view, so writes through it encode on the way out while presentation
		// still sees the UNORM format it chose. MUTABLE_FORMAT_BIT requires the complete
		// list of view formats up front, which is also what lets the driver keep its
		// framebuffer compression. Skipped when the format has no same-class sibling;
		// AssignFromSwapChainVulkan makes the same two-part check when it builds views.
		VkFormat viewFormats[2] = { desiredFormat.format, TrinityALImpl::GetSrgbCounterpartVulkan( desiredFormat.format ) };
		VkImageFormatListCreateInfo formatList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
		formatList.viewFormatCount = 2;
		formatList.pViewFormats = viewFormats;
		if( mutableFormat && viewFormats[1] != VK_FORMAT_UNDEFINED )
		{
			swapChainCreateInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
			swapChainCreateInfo.pNext = &formatList;
		}

		CR_RETURN_HR( Vk2Al( vkCreateSwapchainKHR( device, &swapChainCreateInfo, nullptr, &out.swapChain ) ) );

		CR_RETURN_HR( TrinityALImpl::QueryArray( &vkGetSwapchainImagesKHR, device, out.swapChain, out.backBuffers ) );

		VkSemaphoreCreateInfo semaphoreInfo = {
			VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			nullptr,
			0
		};

		// One per image, not one per virtual frame -- see the member's comment. The count is
		// whatever the driver actually handed back, which need not be the number we asked for
		// and need not equal VIRTUAL_FRAMES.
		out.finishedRenderingSemaphores.resize( out.backBuffers.size(), VK_NULL_HANDLE );
		for( size_t i = 0; i < out.finishedRenderingSemaphores.size(); ++i )
		{
			CR_RETURN_HR( Vk2Al( vkCreateSemaphore( device, &semaphoreInfo, nullptr, &out.finishedRenderingSemaphores[i] ) ) );
		}

		out.mode = parameters.mode;
		out.mode.width = desiredExtent.width;
		out.mode.height = desiredExtent.height;

		// The back buffer image views, and every render pass attachment description that
		// references them, are built from out.mode.format -- so it has to name the format
		// the swapchain was actually created with rather than the one that was asked for.
		out.mode.format = TrinityALImpl::GetAlPixelFormat( desiredFormat.format );
		if( out.mode.format == Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN )
		{
			// A presentable format the AL has no name for. Carrying on would build views
			// from PIXEL_FORMAT_UNKNOWN and put back exactly the mismatch this replaced,
			// so fail here instead. The preference list above chose the format, so this is
			// a gap in GetAlPixelFormat and wants fixing there.
			return E_FAIL;
		}
		return S_OK;
	}

	ALResult CreatePresentationSurface( VkInstance instance, Tr2WindowHandle window, VkSurfaceKHR& surface )
	{
		if( window == 0 )
		{
			surface = VK_NULL_HANDLE;
			return S_OK;
		}
#if defined( VK_USE_PLATFORM_WIN32_KHR )
		VkWin32SurfaceCreateInfoKHR surfacecreateInfo = {
			VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
			nullptr,
			0,
			GetModuleHandle( nullptr ),
			window
		};
		CR_RETURN_HR( Vk2Al( vkCreateWin32SurfaceKHR( instance, &surfacecreateInfo, nullptr, &surface ) ) );
#elif defined( VK_USE_PLATFORM_ANDROID_KHR )
		VkAndroidSurfaceCreateInfoKHR surfacecreateInfo = {
			VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
			nullptr,
			0,
			reinterpret_cast<ANativeWindow*>( window )
		};
		CR_RETURN_HR( Vk2Al( vkCreateAndroidSurfaceKHR( instance, &surfacecreateInfo, nullptr, &surface ) ) );
#else
		static_assert( false, "Define swapchain creation for this platform here" );
#endif
		return S_OK;
	}
}

Tr2PrimaryRenderContextAL::FrameData::FrameData()
	:commandBuffer( VK_NULL_HANDLE ),
	imageAvailableSemaphore( VK_NULL_HANDLE ),
	fence( VK_NULL_HANDLE ),
	submittedFrame( 0 ),
	fencePending( false )
{
}


Tr2PrimaryRenderContextAL::Tr2PrimaryRenderContextAL()
	:m_events( nullptr ),
	m_device( VK_NULL_HANDLE ),
	m_physicalDevice( VK_NULL_HANDLE ),
	m_graphicsQueue( VK_NULL_HANDLE ),
	m_presentQueue( VK_NULL_HANDLE ),
	m_surface( VK_NULL_HANDLE ),
	m_swapChain( VK_NULL_HANDLE ),
	m_commandPool( VK_NULL_HANDLE ),
	m_currentImage( 0 ),
	m_zeroBuffer( VK_NULL_HANDLE ),
	m_zeroBufferMemory( VK_NULL_HANDLE ),
	m_frameIndex( 0 ),
	m_acquireWaited( false ),
	m_commandBufferRecording( false ),
	m_recordingFrame( 0 ),
	m_flushedFrame( 0 ),
	m_needsSwapChainRebuild( false ),
	m_vkCmdBeginRendering( nullptr ),
	m_vkCmdEndRendering( nullptr ),
	m_vkCmdPipelineBarrier2( nullptr ),
	m_vkQueueSubmit2( nullptr ),
	m_swapChainMutableFormat( false ),
	m_dummySampler( VK_NULL_HANDLE )
{
	m_defaultBackBuffer.m_texture = std::make_shared<TrinityALImpl::Tr2TextureAL>();
}

Tr2PrimaryRenderContextAL::~Tr2PrimaryRenderContextAL()
{
	Destroy();
}

ALResult Tr2PrimaryRenderContextAL::CreateDevice(
	uint32_t adapter,
	Tr2WindowHandle  focusWindow,
	const Tr2PresentParametersAL& presentationParameters )
{
	Destroy();

	VkInstance instance;
	FORWARD_HR( TrinityALImpl::GetVulkanInstance( instance ) );
	TrinityALImpl::VulkanDeviceInfo physicalDevice;
	FORWARD_HR( TrinityALImpl::GetPhysicalDevice( adapter, physicalDevice ) );

	const bool isWindowless = ( focusWindow == 0 ) && presentationParameters.software;

	VkDevice device = VK_NULL_HANDLE;
	ON_BLOCK_EXIT( [=] { if( device != VK_NULL_HANDLE ) vkDestroyDevice( device, nullptr ); } );
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	ON_BLOCK_EXIT( [=] { if( surface != VK_NULL_HANDLE ) vkDestroySurfaceKHR( instance, surface, nullptr ); } );
	VkSwapchainKHR swapChain = VK_NULL_HANDLE;
	ON_BLOCK_EXIT( [=] { if( swapChain != VK_NULL_HANDLE ) vkDestroySwapchainKHR( device, swapChain, nullptr ); } );
	std::vector<VkImage> backBuffers;
	std::vector<VkSemaphore> finishedRenderingSemaphores;

	uint32_t graphicsQueue = physicalDevice.graphicsQueue;
	uint32_t presentQueue = graphicsQueue;

	if( !isWindowless )
	{
		// Tr2WindowHandle is HWND on Windows and uintptr_t elsewhere (StdAfx.h); on
		// Android the host layer hands the ANativeWindow* it got from the Java Surface
		// through it. Lifetime is the caller's problem exactly as an HWND's is: the
		// window must outlive the surface. Surface loss (background) is a new
		// ANativeWindow, which SetPresentParameters rebuilds via RecreateSurfaceVulkan
		// rather than a new device. Rotation with configChanges keeps the same window.
		CR_RETURN_HR( CreatePresentationSurface( instance, focusWindow, surface ) );
		if( !FindPresentableQueues( physicalDevice.device, surface, graphicsQueue, presentQueue ) )
		{
			CCP_AL_LOGERR( "Could not find graphics queues for the selected device" );
			return E_FAIL;
		}
	}

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::vector<float> queuePriorities;
	queuePriorities.push_back( 1 );

	VkDeviceQueueCreateInfo graphicsQueueInfo = {
		VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		nullptr,
		0,
		graphicsQueue,
		uint32_t( queuePriorities.size() ),
		&queuePriorities[0]
	};
	queueCreateInfos.push_back( graphicsQueueInfo );
	if( graphicsQueue != presentQueue )
	{
		VkDeviceQueueCreateInfo presentQueueInfo = {
			VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			nullptr,
			0,
			presentQueue,
			uint32_t( queuePriorities.size() ),
			&queuePriorities[0]
		};
		queueCreateInfos.push_back( presentQueueInfo );
	}

	std::vector<const char*> extensions;
	extensions.push_back( VK_KHR_SWAPCHAIN_EXTENSION_NAME );
	extensions.push_back( VK_KHR_MAINTENANCE1_EXTENSION_NAME );

	// Dynamic rendering is how every pass is recorded -- there is no VkRenderPass path to
	// fall back to, so a device that cannot do it cannot run this backend, and the honest
	// answer is a clean failure here rather than an invalid begin later. It is core in
	// 1.3; below that it is VK_KHR_dynamic_rendering and has to be named at device
	// creation. Its dependencies are core from 1.2, which is as far down as this reaches:
	// a 1.1 device that has the extension also needs VK_KHR_depth_stencil_resolve and
	// VK_KHR_create_renderpass2 spelled out, and no target of this backend is in that
	// bracket -- Android's floor is 1.3-or-extension-on-1.2 in practice, and desktop
	// drivers this century are 1.3.
	// synchronization2 rides the same policy: every barrier and submit in the backend is
	// the 2 form, so it is required, not preferred. Same version bracket, same clean
	// failure.
	VkPhysicalDeviceSynchronization2Features supportedSynchronization2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
	VkPhysicalDeviceDynamicRenderingFeatures supportedDynamicRendering = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, &supportedSynchronization2 };
	VkPhysicalDeviceFeatures2 supportedFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supportedDynamicRendering };
	vkGetPhysicalDeviceFeatures2( physicalDevice.device, &supportedFeatures2 );
	if( !supportedDynamicRendering.dynamicRendering )
	{
		CCP_AL_LOGERR( "Vulkan device does not support dynamic rendering (core 1.3 or VK_KHR_dynamic_rendering); the Vulkan backend requires it" );
		return E_FAIL;
	}
	if( !supportedSynchronization2.synchronization2 )
	{
		CCP_AL_LOGERR( "Vulkan device does not support synchronization2 (core 1.3 or VK_KHR_synchronization2); the Vulkan backend requires it" );
		return E_FAIL;
	}
	if( physicalDevice.properties.apiVersion < VK_API_VERSION_1_3 )
	{
		extensions.push_back( VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME );
		extensions.push_back( VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME );
	}

	// Optional, unlike the two above: RS_SRGBWRITEENABLE degrades to E_NOTIMPL without
	// it rather than failing device creation. Its dependencies (maintenance2,
	// image_format_list) are core from 1.2, which every device past the two required
	// features has.
	bool swapChainMutableFormat = false;
	{
		uint32_t extensionCount = 0;
		vkEnumerateDeviceExtensionProperties( physicalDevice.device, nullptr, &extensionCount, nullptr );
		std::vector<VkExtensionProperties> availableExtensions( extensionCount );
		if( extensionCount )
		{
			vkEnumerateDeviceExtensionProperties( physicalDevice.device, nullptr, &extensionCount, availableExtensions.data() );
		}
		for( auto it = begin( availableExtensions ); it != end( availableExtensions ); ++it )
		{
			if( strcmp( it->extensionName, VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME ) == 0 )
			{
				swapChainMutableFormat = physicalDevice.properties.apiVersion >= VK_API_VERSION_1_2;
				break;
			}
		}
	}
	if( swapChainMutableFormat )
	{
		extensions.push_back( VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME );
	}

	VkPhysicalDeviceSynchronization2Features enabledSynchronization2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
	enabledSynchronization2.synchronization2 = VK_TRUE;
	VkPhysicalDeviceDynamicRenderingFeatures enabledDynamicRendering = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, &enabledSynchronization2 };
	enabledDynamicRendering.dynamicRendering = VK_TRUE;

	// pEnabledFeatures was null, which enables nothing at all -- and a Vulkan feature that
	// is not enabled is not merely slower, it is illegal to use. Four validation findings
	// trace straight to this line, and one AL class is E_NOTIMPL because of it.
	//
	// The portable form is the intersection of what is wanted and what the device reports,
	// never a fixed list: a feature absent on this device is simply not enabled, and the
	// code that wanted it asks m_enabledFeatures rather than assuming.
	VkPhysicalDeviceFeatures enabledFeatures = {};

	// Occlusion queries answer only "zero or non-zero" without this; with it they return a
	// real sample count. See Tr2OcclusionQueryAL.
	enabledFeatures.occlusionQueryPrecise = physicalDevice.features.occlusionQueryPrecise;

	// A storage image written from a fragment shader needs both of these. HLSL's
	// RWTexture2D<float4> carries no format, so the SPIR-V declares one the image does not
	// have unless writes-without-format is available.
	// VUID-RuntimeSpirv-NonWritable-06340 and the fragmentStoresAndAtomics rule.
	enabledFeatures.shaderStorageImageWriteWithoutFormat = physicalDevice.features.shaderStorageImageWriteWithoutFormat;
	enabledFeatures.fragmentStoresAndAtomics = physicalDevice.features.fragmentStoresAndAtomics;

	// Tr2SamplerStateAL sets anisotropyEnable from the AL description, and without the
	// feature that is VUID-VkSamplerCreateInfo-anisotropyEnable-01070 -- a live error in
	// the inventory, not a hypothetical one.
	enabledFeatures.samplerAnisotropy = physicalDevice.features.samplerAnisotropy;

	// Tr2PipelineStatsQueryAL cannot be implemented at all without this.
	enabledFeatures.pipelineStatisticsQuery = physicalDevice.features.pipelineStatisticsQuery;

	VkDeviceCreateInfo device_create_info = {
		VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		&enabledDynamicRendering,
		0,
		uint32_t( queueCreateInfos.size() ),
		&queueCreateInfos[0],
		0,
		nullptr,
		uint32_t( extensions.size() ),
		extensions.data(),
		&enabledFeatures
	};

	CR_RETURN_HR( Vk2Al( vkCreateDevice( physicalDevice.device, &device_create_info, nullptr, &device ) ) );

	// Core name first, KHR alias second -- see the member's comment for why the loader's
	// exported symbol is not used. Fetched immediately after device creation so nothing
	// below can run without them.
	PFN_vkCmdBeginRendering beginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>( vkGetDeviceProcAddr( device, "vkCmdBeginRendering" ) );
	if( !beginRendering )
	{
		beginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>( vkGetDeviceProcAddr( device, "vkCmdBeginRenderingKHR" ) );
	}
	PFN_vkCmdEndRendering endRendering = reinterpret_cast<PFN_vkCmdEndRendering>( vkGetDeviceProcAddr( device, "vkCmdEndRendering" ) );
	if( !endRendering )
	{
		endRendering = reinterpret_cast<PFN_vkCmdEndRendering>( vkGetDeviceProcAddr( device, "vkCmdEndRenderingKHR" ) );
	}
	PFN_vkCmdPipelineBarrier2 pipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>( vkGetDeviceProcAddr( device, "vkCmdPipelineBarrier2" ) );
	if( !pipelineBarrier2 )
	{
		pipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>( vkGetDeviceProcAddr( device, "vkCmdPipelineBarrier2KHR" ) );
	}
	PFN_vkQueueSubmit2 queueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>( vkGetDeviceProcAddr( device, "vkQueueSubmit2" ) );
	if( !queueSubmit2 )
	{
		queueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>( vkGetDeviceProcAddr( device, "vkQueueSubmit2KHR" ) );
	}
	if( !beginRendering || !endRendering || !pipelineBarrier2 || !queueSubmit2 )
	{
		CCP_AL_LOGERR( "dynamic rendering / synchronization2 entry points not resolvable despite the features" );
		return E_FAIL;
	}

	Tr2DisplayModeInfo actualMode = presentationParameters.mode;

	if( !isWindowless )
	{
		SwapChainObjects created;
		CR_RETURN_HR( BuildSwapChain( device, physicalDevice.device, surface, presentationParameters, VK_NULL_HANDLE, swapChainMutableFormat, created ) );
		swapChain = created.swapChain;
		backBuffers = created.backBuffers;
		finishedRenderingSemaphores = created.finishedRenderingSemaphores;
		actualMode = created.mode;
	}


	VkCommandPoolCreateInfo cmd_pool_create_info = {
		VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		nullptr,
		VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		presentQueue
	};

	VkCommandPool commandPool = VK_NULL_HANDLE;
	ON_BLOCK_EXIT( [=] {if( commandPool != VK_NULL_HANDLE ) vkDestroyCommandPool( device, commandPool, nullptr ); } );
	CR_RETURN_HR( Vk2Al( vkCreateCommandPool( device, &cmd_pool_create_info, nullptr, &commandPool ) ) );

	FrameData frameData[VIRTUAL_FRAMES];

	for( size_t i = 0; i < VIRTUAL_FRAMES; ++i )
	{
		VkCommandBufferAllocateInfo cmd_buffer_allocate_info = {
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			nullptr,
			commandPool,
			VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			1
		};
		CR_RETURN_HR( Vk2Al( vkAllocateCommandBuffers( device, &cmd_buffer_allocate_info, &frameData[i].commandBuffer ) ) );
		if( !isWindowless )
		{
			VkSemaphoreCreateInfo semaphoreInfo = {
				VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
				nullptr,
				0
			};

			CR_RETURN_HR( Vk2Al( vkCreateSemaphore( device, &semaphoreInfo, nullptr, &frameData[i].imageAvailableSemaphore ) ) );
		}
		else
		{
			frameData[i].imageAvailableSemaphore = VK_NULL_HANDLE;
		}

		// Unsignalled. See FrameData::fencePending -- the first lap waits on nothing,
		// because nothing has been submitted yet, so the initial signal is not needed. It
		// was also actively unhelpful: it made an unsignalled-and-unsubmitted fence and a
		// signalled-and-complete one look the same to the only wait in the backend.
		VkFenceCreateInfo fenceCreateInfo = {
			VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			nullptr,
			0
		};

		CR_RETURN_HR( Vk2Al( vkCreateFence( device, &fenceCreateInfo, nullptr, &frameData[i].fence ) ) );
	}

	vkGetDeviceQueue( device, graphicsQueue, 0, &m_graphicsQueue );
	vkGetDeviceQueue( device, presentQueue, 0, &m_presentQueue );

	m_device = device;
	m_physicalDevice = physicalDevice.device;
	m_physicalDeviceProperties = physicalDevice.properties;
	m_enabledFeatures = enabledFeatures;
	m_vkCmdBeginRendering = beginRendering;
	m_vkCmdEndRendering = endRendering;
	m_vkCmdPipelineBarrier2 = pipelineBarrier2;
	m_vkQueueSubmit2 = queueSubmit2;
	// Before AssignFromSwapChainVulkan below, which reads it to decide whether the back
	// buffer gets sRGB views.
	m_swapChainMutableFormat = swapChainMutableFormat;
	m_presentParameters = presentationParameters;
	// CreatePresentationSurface above was built from focusWindow, not from
	// presentationParameters.outputWindow -- verified every current caller passes the
	// same handle for both (trinityal/tests/RenderContextCreation.cpp,
	// WithValidRenderContextFixture.cpp and SwapChainResizing.cpp; trinity/TriDevice.cpp
	// CreateSimpleDevice and ChangeDevice/ResetDevice; trinity/UI/Tr2MainWindow.cpp), so
	// this line changes nothing any of them observe today. It is kept because
	// RecreateSurfaceVulkan and SetPresentParameters's windowChanged check both read
	// m_presentParameters.outputWindow as the record of what the live VkSurfaceKHR was
	// actually built from -- that has to be focusWindow regardless of what a future
	// caller writes into presentationParameters.outputWindow, or the window-change
	// detection and the surface rebuild it drives would work off a value nobody used.
	m_presentParameters.outputWindow = focusWindow;
	m_needsSwapChainRebuild = false;
	m_surface = surface;
	m_swapChain = swapChain;
	for( size_t i = 0; i < VIRTUAL_FRAMES; ++i )
	{
		m_frameData[i] = frameData[i];
	}

	m_finishedRenderingSemaphores = finishedRenderingSemaphores;

	// actualMode, not presentationParameters.mode: the back buffer reports what exists.
	m_defaultBackBuffer.m_texture->AssignFromSwapChainVulkan( backBuffers, actualMode, *this );

	m_commandPool = commandPool;
	m_frameIndex = 0;

	device = VK_NULL_HANDLE;
	surface = VK_NULL_HANDLE;
	swapChain = VK_NULL_HANDLE;
	backBuffers.clear();
	finishedRenderingSemaphores.clear();
	commandPool = VK_NULL_HANDLE;

	m_owner = this;

	BeginFrame();

	{
		TrinityALImpl::CreateBuffer( m_zeroBuffer, m_zeroBufferMemory, 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, *this );

		// See the members' comment. Opaque black, matching what D3D9 samples from an
		// unassigned stage.
		static const uint32_t BLACK = 0xff000000;
		Tr2SubresourceData dummyData = { &BLACK, sizeof( BLACK ), sizeof( BLACK ) };
		m_dummyTexture.Create(
			Tr2BitmapDimensions( 1, 1, 1, Tr2RenderContextEnum::PIXEL_FORMAT_B8G8R8A8_UNORM ),
			Tr2GpuUsage::SHADER_RESOURCE,
			&dummyData,
			*this );

		VkSamplerCreateInfo dummySamplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		dummySamplerInfo.magFilter = VK_FILTER_NEAREST;
		dummySamplerInfo.minFilter = VK_FILTER_NEAREST;
		// LOD_CLAMP_NONE, not 0: a maxLod of zero reads as deliberate lod clamping and
		// draws a best-practices advisory per device; the dummy has one mip either way.
		dummySamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
		vkCreateSampler( m_device, &dummySamplerInfo, nullptr, &m_dummySampler );
	}

	if( m_events )
	{
		m_events->OnContextCreated( *this );
	}

	return S_OK;
}

void Tr2PrimaryRenderContextAL::Destroy()
{
	Tr2RenderContextAL::Destroy();

	m_recordingFrame = 0;
	m_flushedFrame = 0;

	m_samplerStateFactory.Clear();

	if( m_device != VK_NULL_HANDLE )
	{
		vkDeviceWaitIdle( m_device );
	}

	for( auto it = begin( m_pipelines ); it != end( m_pipelines ); ++it )
	{
		vkDestroyPipeline( m_device, it->second, nullptr );
	}
	m_pipelines.clear();

	m_dummyTexture = Tr2TextureAL();
	if( m_dummySampler != VK_NULL_HANDLE )
	{
		vkDestroySampler( m_device, m_dummySampler, nullptr );
		m_dummySampler = VK_NULL_HANDLE;
	}

	m_defaultBackBuffer.m_texture->Destroy();

	if( m_zeroBuffer )
	{
		vkDestroyBuffer( m_device, m_zeroBuffer, nullptr );
		m_zeroBuffer = VK_NULL_HANDLE;
		vkFreeMemory( m_device, m_zeroBufferMemory, nullptr );
		m_zeroBufferMemory = VK_NULL_HANDLE;
	}
	for( size_t i = 0; i < VIRTUAL_FRAMES; ++i )
	{
		if( m_frameData[i].commandBuffer != VK_NULL_HANDLE )
		{
			vkFreeCommandBuffers( m_device, m_commandPool, 1, &m_frameData[i].commandBuffer );
			m_frameData[i].commandBuffer = VK_NULL_HANDLE;
		}
		if( m_frameData[i].fence != VK_NULL_HANDLE )
		{
			vkDestroyFence( m_device, m_frameData[i].fence, nullptr );
			m_frameData[i].fence = VK_NULL_HANDLE;
		}
		if( m_frameData[i].imageAvailableSemaphore != VK_NULL_HANDLE )
		{
			vkDestroySemaphore( m_device, m_frameData[i].imageAvailableSemaphore, nullptr );
			m_frameData[i].imageAvailableSemaphore = VK_NULL_HANDLE;
		}

		for( auto it = begin( m_frameData[i].pendingDestroys ); it != end( m_frameData[i].pendingDestroys ); ++it )
		{
			( *it->destroyFunction )( m_device, it->object, nullptr );
		}
		m_frameData[i].pendingDestroys.clear();
	}

	for( size_t i = 0; i < m_finishedRenderingSemaphores.size(); ++i )
	{
		if( m_finishedRenderingSemaphores[i] != VK_NULL_HANDLE )
		{
			vkDestroySemaphore( m_device, m_finishedRenderingSemaphores[i], nullptr );
		}
	}
	m_finishedRenderingSemaphores.clear();

	if( m_commandPool != VK_NULL_HANDLE )
	{
		vkDestroyCommandPool( m_device, m_commandPool, nullptr );
		m_commandPool = VK_NULL_HANDLE;
	}

	if( m_swapChain != VK_NULL_HANDLE )
	{
		vkDestroySwapchainKHR( m_device, m_swapChain, nullptr );
		m_swapChain = VK_NULL_HANDLE;
	}
	if( m_surface != VK_NULL_HANDLE )
	{
		VkInstance instance;
		TrinityALImpl::GetVulkanInstance( instance );
		vkDestroySurfaceKHR( instance, m_surface, nullptr );
		m_surface = VK_NULL_HANDLE;
	}
	if( m_device != VK_NULL_HANDLE )
	{
		vkDestroyDevice( m_device, nullptr );
		m_device = VK_NULL_HANDLE;
	}
	m_graphicsQueue = VK_NULL_HANDLE;
	m_presentQueue = VK_NULL_HANDLE;
	m_physicalDevice = VK_NULL_HANDLE;
}

bool Tr2PrimaryRenderContextAL::IsValid() const
{
	return m_device != VK_NULL_HANDLE;
}

ALResult Tr2PrimaryRenderContextAL::RebuildSwapChainVulkan()
{
	if( m_device == VK_NULL_HANDLE )
	{
		return E_INVALIDCALL;
	}
	if( m_surface == VK_NULL_HANDLE )
	{
		// Windowless: there is no swapchain to rebuild and nothing to recover from.
		m_needsSwapChainRebuild = false;
		return S_OK;
	}

	// Everything below assumes nothing is in flight, and that assumption is the reason this
	// is a device-wide wait rather than a fence wait: the frame that failed may have left a
	// submit half-done, and the acquire semaphore signalled with nobody to wait on it.
	vkDeviceWaitIdle( m_device );

	// The queue is idle, so every frame ever submitted is complete. Say so, or
	// GetRenderedFrameNumber keeps reporting the pre-rebuild answer to Tr2FenceAL and the
	// constant-pool recycle.
	m_flushedFrame = m_recordingFrame;
	for( size_t i = 0; i < VIRTUAL_FRAMES; ++i )
	{
		m_frameData[i].submittedFrame = 0;
	}

	// The back buffer's image views are ours and have to go; its images belong to the
	// swapchain and Tr2TextureAL::Destroy already knows not to touch those.
	m_defaultBackBuffer.m_texture->Destroy();

	// The next pass must not begin against image views that have just been destroyed.
	InvalidateAttachmentsVulkan();

	// Build the replacement first, handing the old swapchain over so the driver can reuse
	// what it can. Nothing is destroyed until this succeeds, so a failure here leaves a
	// working swapchain in place rather than a half-torn-down one.
	SwapChainObjects created;
	ALResult built = BuildSwapChain( m_device, m_physicalDevice, m_surface, m_presentParameters, m_swapChain, m_swapChainMutableFormat, created );
	if( FAILED( built ) )
	{
		return built;
	}
	if( built == S_FALSE )
	{
		// A zero-sized surface: the window is minimised. Leave the flag set and try again
		// next frame rather than tearing down what is still there.
		return S_FALSE;
	}

	VkSwapchainKHR oldSwapChain = m_swapChain;
	m_swapChain = created.swapChain;

	// ORDER MATTERS HERE, and getting it wrong cost a device loss in one full-suite run out
	// of twelve while the control lost none.
	//
	// A semaphore waited on by vkQueuePresentKHR is in use until that present completes, and
	// vkDeviceWaitIdle does not prove a present has completed -- it waits on queue work, not
	// on the presentation engine. That is the same invariant 0f0650de recorded for the
	// per-image semaphores in the steady-state path, arrived at from the other direction.
	//
	// Destroying the swapchain is what ends its outstanding presentation operations, so the
	// semaphores those presents waited on can only be destroyed afterwards. Doing it before
	// is a use-after-free inside the driver, which surfaces as VK_ERROR_DEVICE_LOST on some
	// later submit -- intermittently, because it depends on whether the present had already
	// retired.
	if( oldSwapChain != VK_NULL_HANDLE )
	{
		vkDestroySwapchainKHR( m_device, oldSwapChain, nullptr );
	}

	for( size_t i = 0; i < m_finishedRenderingSemaphores.size(); ++i )
	{
		if( m_finishedRenderingSemaphores[i] != VK_NULL_HANDLE )
		{
			vkDestroySemaphore( m_device, m_finishedRenderingSemaphores[i], nullptr );
		}
	}
	m_finishedRenderingSemaphores = created.finishedRenderingSemaphores;

	// The acquire semaphores go and come back, for the reason this whole function exists: a
	// submit that failed leaves one signalled with no pending wait, there is no
	// vkResetSemaphore, and vkAcquireNextImageKHR on a signalled semaphore is illegal.
	// After the swapchain is gone no acquire on it can still be outstanding, so this is also
	// the earliest point at which destroying them is sound.
	VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0 };
	for( size_t i = 0; i < VIRTUAL_FRAMES; ++i )
	{
		if( m_frameData[i].imageAvailableSemaphore != VK_NULL_HANDLE )
		{
			vkDestroySemaphore( m_device, m_frameData[i].imageAvailableSemaphore, nullptr );
			m_frameData[i].imageAvailableSemaphore = VK_NULL_HANDLE;
		}
		CR_RETURN_HR( Vk2Al( vkCreateSemaphore( m_device, &semaphoreInfo, nullptr, &m_frameData[i].imageAvailableSemaphore ) ) );
	}
	m_acquireWaited = false;

	CR_RETURN_HR( m_defaultBackBuffer.m_texture->AssignFromSwapChainVulkan( created.backBuffers, created.mode, *this ) );

	m_currentImage = 0;
	m_needsSwapChainRebuild = false;
	return S_OK;
}

ALResult Tr2PrimaryRenderContextAL::RecreateSurfaceVulkan()
{
	if( m_device == VK_NULL_HANDLE )
	{
		return E_INVALIDCALL;
	}

	// Same idle as RebuildSwapChainVulkan: nothing referencing the old surface may be in
	// flight, including a present the frame fence does not cover.
	vkDeviceWaitIdle( m_device );
	m_flushedFrame = m_recordingFrame;
	for( size_t i = 0; i < VIRTUAL_FRAMES; ++i )
	{
		m_frameData[i].submittedFrame = 0;
	}

	m_defaultBackBuffer.m_texture->Destroy();
	InvalidateAttachmentsVulkan();

	// Swapchain first: destroying it ends outstanding presents, which is what makes the
	// present-waited semaphores safe to destroy afterwards (RebuildSwapChainVulkan).
	if( m_swapChain != VK_NULL_HANDLE )
	{
		vkDestroySwapchainKHR( m_device, m_swapChain, nullptr );
		m_swapChain = VK_NULL_HANDLE;
	}

	VkInstance instance;
	FORWARD_HR( TrinityALImpl::GetVulkanInstance( instance ) );

	if( m_surface != VK_NULL_HANDLE )
	{
		vkDestroySurfaceKHR( instance, m_surface, nullptr );
		m_surface = VK_NULL_HANDLE;
	}

	if( m_presentParameters.outputWindow == 0 )
	{
		m_needsSwapChainRebuild = false;
		return S_OK;
	}

	FORWARD_HR( CreatePresentationSurface( instance, m_presentParameters.outputWindow, m_surface ) );
	return S_OK;
}

ALResult Tr2PrimaryRenderContextAL::SetPresentParameters( unsigned, const Tr2PresentParametersAL& presentationParameters )
{
	if( !IsValid() )
	{
		return E_INVALIDCALL;
	}

	const bool windowChanged = m_presentParameters.outputWindow != presentationParameters.outputWindow;
	m_presentParameters = presentationParameters;

	// Rebuilt now rather than flagged for later: the caller asked for this, and a resize
	// costs a frame whichever way it is spelled. The deferred path exists for failures
	// discovered inside Present, where there is no caller to report to.
	//
	// The command buffer is mid-recording here, and RebuildSwapChainVulkan waits the device
	// idle -- which is legal, but whatever was recorded for this frame is discarded along
	// with the framebuffer it referenced. BeginFrame starts a clean one.
	//
	// Guarded: a soak teardown can arrive after Present already ended the buffer, or after
	// a previous SetPresentParameters dropped the surface. Ending an un-recording buffer
	// is VUID-vkEndCommandBuffer-commandBuffer-00059.
	if( m_commandBufferRecording )
	{
		EndRenderPassVulkan();
		vkEndCommandBuffer( m_commandBuffer );
		m_commandBufferRecording = false;
	}

	if( windowChanged )
	{
		FORWARD_HR( RecreateSurfaceVulkan() );
	}

	ALResult rebuilt = RebuildSwapChainVulkan();
	if( FAILED( rebuilt ) )
	{
		return rebuilt;
	}

	if( m_surface == VK_NULL_HANDLE )
	{
		// Presentation dropped (surface-loss teardown). The next SetPresentParameters
		// with a live window rebuilds surface + swapchain.
		return S_OK;
	}

	FORWARD_HR( BeginFrame() );
	return S_OK;
}

VkImageView Tr2PrimaryRenderContextAL::GetDummyImageViewVulkan() const
{
	return m_dummyTexture.IsValid()
		? m_dummyTexture.m_texture->GetImageView( Tr2RenderContextEnum::COLOR_SPACE_LINEAR )
		: VK_NULL_HANDLE;
}

ALResult Tr2PrimaryRenderContextAL::Present()
{
	if( !IsValid() )
	{
		return E_INVALIDCALL;
	}

	if( !m_commandBufferRecording )
	{
		// BeginFrame never got as far as vkBeginCommandBuffer, so m_commandBuffer is the
		// previous slot's buffer and it is already submitted. Ending and submitting it
		// again is VUID-vkQueueSubmit-pCommandBuffers-00071. Try to start a frame instead
		// of presenting one that does not exist.
		FORWARD_HR( BeginFrame() );
		return S_FALSE;
	}

	EndRenderPassVulkan();

	// Whatever the frame left it in -- COLOR_ATTACHMENT_OPTIMAL after a draw,
	// TRANSFER_DST_OPTIMAL after a clear -- into PRESENT_SRC_KHR. The old barrier named
	// TRANSFER_DST_OPTIMAL as the source unconditionally, which was a lie in every frame
	// that ended with a draw.
	m_defaultBackBuffer.m_texture->TransitionVulkan( m_commandBuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );

	CR_RETURN_HR( Vk2Al( vkEndCommandBuffer( m_commandBuffer ) ) );
	m_commandBufferRecording = false;

	// Indexed by the image we acquired, not by the frame -- see the member's comment. The
	// windowless path has no swapchain and so no semaphores; VK_NULL_HANDLE there is what
	// the per-frame code put in this slot as well.
	VkSemaphore renderFinishedSemaphore = m_currentImage < m_finishedRenderingSemaphores.size()
		? m_finishedRenderingSemaphores[m_currentImage]
		: VK_NULL_HANDLE;

	// A FlushAndSyncVulkan earlier in the frame will already have consumed the acquire
	// semaphore; waiting on it a second time would wait for a signal that never comes.
	VkSemaphoreSubmitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
	waitInfo.semaphore = m_frameData[m_frameIndex].imageAvailableSemaphore;
	waitInfo.stageMask = ACQUIRE_WAIT_STAGE;
	VkCommandBufferSubmitInfo commandBufferInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
	commandBufferInfo.commandBuffer = m_commandBuffer;
	// Signalled once everything in the batch completes, which is what VkSubmitInfo
	// meant by pSignalSemaphores; ALL_COMMANDS is that meaning spelled as a stage.
	VkSemaphoreSubmitInfo signalInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
	signalInfo.semaphore = renderFinishedSemaphore;
	signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
	submitInfo.waitSemaphoreInfoCount = m_acquireWaited ? 0u : 1u;
	submitInfo.pWaitSemaphoreInfos = m_acquireWaited ? nullptr : &waitInfo;
	submitInfo.commandBufferInfoCount = 1;
	submitInfo.pCommandBufferInfos = &commandBufferInfo;
	submitInfo.signalSemaphoreInfoCount = 1;
	submitInfo.pSignalSemaphoreInfos = &signalInfo;
	// This fence now stands for this frame. Recorded before the submit so that a submit
	// failure cannot leave the slot claiming a frame that was never sent.
	// Nothing below returns early on failure. Present used to bail through CR_RETURN_HR
	// here and again after vkQueuePresentKHR, having already called vkEndCommandBuffer --
	// so it never reached BeginFrame, the only caller of vkBeginCommandBuffer, and the
	// command buffer stayed ended for the life of the process. Every later vkCmd* was then
	// recorded against a buffer that was not recording, which is the section 13 cascade
	// arrived at from a routine window resize instead of a lost device.
	m_frameData[m_frameIndex].submittedFrame = m_recordingFrame;
	const VkResult submitResult = m_vkQueueSubmit2( m_presentQueue, 1, &submitInfo, m_frameData[m_frameIndex].fence );
	m_acquireWaited = true;

	// Only a submit that was accepted will ever signal the fence. A rejected one leaves it
	// unsignalled, and claiming otherwise is what puts a wait in front of a signal that is
	// never coming.
	if( submitResult == VK_SUCCESS )
	{
		m_frameData[m_frameIndex].fencePending = true;
	}

	// A lost device is the one thing a rebuild cannot fix, so it is the one thing that is
	// still reported as a hard failure.
	if( submitResult == VK_ERROR_DEVICE_LOST )
	{
		return E_FAIL;
	}

	VkPresentInfoKHR presentInfo = {
		VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		nullptr,
		1,
		&renderFinishedSemaphore,
		1,
		&m_swapChain,
		&m_currentImage,
		nullptr
	};
	// Only presented if the submit worked; presenting an image whose rendering was never
	// submitted would wait on a semaphore that will never be signalled.
	VkResult presentResult = VK_SUCCESS;
	if( submitResult == VK_SUCCESS )
	{
		presentResult = vkQueuePresentKHR( m_presentQueue, &presentInfo );
		if( presentResult == VK_ERROR_DEVICE_LOST )
		{
			return E_FAIL;
		}
	}

	// VK_SUBOPTIMAL_KHR is a *success*: the frame was presented, and the swapchain no longer
	// matches the surface. VK_ERROR_OUT_OF_DATE_KHR is a failure that says the same thing
	// more firmly. Both mean rebuild, and so does a submit that failed for any other reason
	// -- one recovery action for every cause, because they leave the same mess.
	const bool needsRebuild =
		submitResult != VK_SUCCESS ||
		presentResult != VK_SUCCESS;
	if( needsRebuild )
	{
		m_needsSwapChainRebuild = true;
	}

	FORWARD_HR( BeginFrame() );

	// S_FALSE, not S_OK: the context is usable again and the caller need not do anything,
	// but a frame was lost. FAILED( S_FALSE ) is false, so a caller that only checks for
	// failure carries on -- which is what a resize should look like from above.
	return needsRebuild ? ALResult( S_FALSE ) : ALResult( S_OK );
}

ALResult Tr2PrimaryRenderContextAL::FlushAndSyncVulkan()
{
	if( !IsValid() )
	{
		return E_INVALIDCALL;
	}

	// Recording has to be outside a render pass instance to end the command buffer. The
	// backend already ends and lazily re-begins render passes mid-frame (see the clear
	// path in Tr2RenderContextVulkan.cpp), so dropping it here is not a new behaviour --
	// but it does mean a caller that flushes mid-pass pays a pass restart, which on a
	// tiler is a resolve and a reload. That is the tile-GPU cost this backend's design
	// notes warn about, and it is why this is a readback path and not a general one.
	EndRenderPassVulkan();

	CR_RETURN_HR( Vk2Al( vkEndCommandBuffer( m_commandBuffer ) ) );
	m_commandBufferRecording = false;

	// No signal semaphore: nothing is being presented and nothing downstream is waiting
	// on this. The wait is the acquire semaphore, and only if Present has not taken it --
	// BeginFrame records a barrier against the acquired image, and that barrier is in the
	// command buffer being submitted here.
	VkSemaphoreSubmitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
	waitInfo.semaphore = m_frameData[m_frameIndex].imageAvailableSemaphore;
	waitInfo.stageMask = ACQUIRE_WAIT_STAGE;
	VkCommandBufferSubmitInfo commandBufferInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
	commandBufferInfo.commandBuffer = m_commandBuffer;
	VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
	submitInfo.waitSemaphoreInfoCount = m_acquireWaited ? 0u : 1u;
	submitInfo.pWaitSemaphoreInfos = m_acquireWaited ? nullptr : &waitInfo;
	submitInfo.commandBufferInfoCount = 1;
	submitInfo.pCommandBufferInfos = &commandBufferInfo;
	CR_RETURN_HR( Vk2Al( m_vkQueueSubmit2( m_presentQueue, 1, &submitInfo, VK_NULL_HANDLE ) ) );
	m_acquireWaited = true;

	// vkQueueWaitIdle rather than a fence: the frame fences belong to Present, and this
	// path is a stall regardless of how the wait is spelled.
	CR_RETURN_HR( Vk2Al( vkQueueWaitIdle( m_presentQueue ) ) );

	// The queue is idle, so everything submitted for this frame and every earlier one has
	// completed. Callers that flushed in order to read something back need this to be
	// visible immediately -- Tr2FenceAL::Wait is exactly that caller.
	m_flushedFrame = m_recordingFrame;

	// Carry on recording into the same command buffer. vkBeginCommandBuffer implicitly
	// resets it because the pool carries VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT.
	//
	// Deliberately NOT re-running BeginFrame's barrier: that one transitions the back
	// buffer from VK_IMAGE_LAYOUT_UNDEFINED, which discards its contents, and everything
	// drawn before this flush is in there. The layout persists across the submit, so
	// Present's barrier still finds TRANSFER_DST_OPTIMAL where it expects it.
	VkCommandBufferBeginInfo cmd_buffer_begin_info = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		nullptr,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		nullptr
	};
	CR_RETURN_HR( Vk2Al( vkBeginCommandBuffer( m_commandBuffer, &cmd_buffer_begin_info ) ) );
	m_commandBufferRecording = true;

	return S_OK;
}

uint64_t Tr2PrimaryRenderContextAL::GetRecordingFrameNumber() const
{
	return m_recordingFrame;
}

uint64_t Tr2PrimaryRenderContextAL::GetRenderedFrameNumber() const
{
	if( m_device == VK_NULL_HANDLE )
	{
		return m_flushedFrame;
	}

	// Start from the newest frame that has actually been submitted -- never from
	// m_recordingFrame, because the frame being recorded has not been sent to the GPU and
	// a frame that was begun and then abandoned never will be.
	uint64_t rendered = 0;
	for( size_t i = 0; i < VIRTUAL_FRAMES; ++i )
	{
		if( m_frameData[i].submittedFrame > rendered )
		{
			rendered = m_frameData[i].submittedFrame;
		}
	}

	// Then pull it back below anything still in flight. The answer has to be "every frame
	// up to N is done", not "some frame N is done", because that is what the callers
	// compare against -- and with VIRTUAL_FRAMES in flight the newest fence can signal
	// while an older one has not.
	for( size_t i = 0; i < VIRTUAL_FRAMES; ++i )
	{
		if( m_frameData[i].submittedFrame != 0 &&
			vkGetFenceStatus( m_device, m_frameData[i].fence ) != VK_SUCCESS &&
			rendered > m_frameData[i].submittedFrame - 1 )
		{
			rendered = m_frameData[i].submittedFrame - 1;
		}
	}

	return rendered > m_flushedFrame ? rendered : m_flushedFrame;
}

ALResult Tr2PrimaryRenderContextAL::BeginFrame()
{
	// The one place that is between frames by construction, so the one place a rebuild can
	// happen. Anything Present discovered too late to act on lands here.
	if( m_needsSwapChainRebuild )
	{
		ALResult rebuilt = RebuildSwapChainVulkan();
		if( FAILED( rebuilt ) )
		{
			return rebuilt;
		}
		if( rebuilt == S_FALSE )
		{
			// Minimised window: nothing to acquire and nothing to record into. The flag
			// stays set, so the next frame tries again.
			return S_FALSE;
		}
	}

	++m_recordingFrame;

	m_frameIndex = ( m_frameIndex + 1 ) % VIRTUAL_FRAMES;

	// Everything below this wait -- resetting the fence, running the slot's pending
	// destroys, acquiring into its semaphore, re-beginning its command buffer -- is
	// illegal while the slot's previous submission is still in flight. So a wait that did
	// not succeed has to stop the frame, not be noted and stepped over.
	//
	// It was noted and stepped over. vkWaitForFences reports VK_TIMEOUT as a Vulkan
	// *success* code, and in release builds CR is `#define CR( x ) x` -- it evaluates its
	// argument and discards the result -- so a timeout fell straight through. Validation
	// named every step of what followed, in order, and one timeout produced all of it:
	//
	//   VUID-vkResetFences-pFences-01123            x1   the fence, still in flight
	//   VUID-vkDestroyBuffer-buffer-00922           x2   pendingDestroys, still in use
	//   VUID-vkAcquireNextImageKHR-semaphore-01779  x1
	//   VUID-vkBeginCommandBuffer-commandBuffer-00049 x1 the buffer, still pending
	//   VUID-vkQueueSubmit-pCommandBuffers-00071    x12  and then it is resubmitted
	//
	// followed by 428 -commandBuffer-recording messages, 13 lost devices and 12 failed
	// tests. Intermittently: 3 full-suite runs in 22, always triggered in the same test
	// and always with those exact counts.
	//
	// The old budget was one second, which a full run under core + sync + thread-safety +
	// best-practices validation exceeds on its own -- the test it always triggered in
	// takes 832ms of wall time by itself. A hung GPU does not need a short timeout here to
	// be caught: on Windows the TDR resets the device after about two seconds and the wait
	// returns VK_ERROR_DEVICE_LOST, which is the honest signal and fails this the same way.
	// The budget is bounded rather than UINT64_MAX only because Android and Linux have no
	// TDR to fall back on.
	if( m_frameData[m_frameIndex].fencePending )
	{
		const VkResult waited = vkWaitForFences( m_device, 1, &m_frameData[m_frameIndex].fence, VK_FALSE, FRAME_FENCE_TIMEOUT_NS );
		if( waited != VK_SUCCESS )
		{
			// m_frameIndex has already moved and m_commandBuffer still names the previous
			// slot's submitted buffer, so this must not look like a frame that can be
			// presented. See m_commandBufferRecording.
			return E_FAIL;
		}

		vkResetFences( m_device, 1, &m_frameData[m_frameIndex].fence );
		m_frameData[m_frameIndex].fencePending = false;
	}

	for( auto it = begin( m_frameData[m_frameIndex].pendingDestroys ); it != end( m_frameData[m_frameIndex].pendingDestroys ); ++it )
	{
		( *it->destroyFunction )( m_device, it->object, nullptr );
	}
	m_frameData[m_frameIndex].pendingDestroys.clear();


	CR_RETURN_HR( Vk2Al( vkAcquireNextImageKHR( m_device, m_swapChain, UINT64_MAX, m_frameData[m_frameIndex].imageAvailableSemaphore, VK_NULL_HANDLE, &m_currentImage ) ) );
	// The fence above has been waited on, so nothing submitted for this slot is still in
	// flight and the sets it used can be handed out again.
	ResetConstantPoolVulkan();

	m_defaultBackBuffer.m_texture->SetCurrentImageVulkan( m_currentImage );

	// vkAcquireNextImageKHR gives no guarantee about the contents or the layout of the
	// image it hands back, so the frame starts by declaring it UNDEFINED and transitioning
	// from there. Carrying PRESENT_SRC_KHR over from the last frame would be a promise the
	// presentation engine never made.
	m_defaultBackBuffer.m_texture->SetLayoutVulkan( VK_IMAGE_LAYOUT_UNDEFINED );
	m_commandBuffer = m_frameData[m_frameIndex].commandBuffer;

	// Freshly signalled, and nothing has waited on it yet. See the member's comment.
	m_acquireWaited = false;

	VkCommandBufferBeginInfo cmd_buffer_begin_info = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		nullptr,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		nullptr
	};

	CR_RETURN_HR( Vk2Al( vkBeginCommandBuffer( m_commandBuffer, &cmd_buffer_begin_info ) ) );
	m_commandBufferRecording = true;

	// Straight to COLOR_ATTACHMENT_OPTIMAL, which is where SetPass wants it and where the
	// render pass now declares both its initial and final layout. It used to land in
	// TRANSFER_DST_OPTIMAL, which only suited the clear path and left every pass claiming
	// an initialLayout it did not have.
	m_defaultBackBuffer.m_texture->TransitionVulkan( m_commandBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, ACQUIRE_WAIT_STAGE );

	SetRenderTarget( m_defaultBackBuffer );

	return S_OK;
}

VkBuffer Tr2PrimaryRenderContextAL::GetZeroBufferVulkan() const
{
	return m_zeroBuffer;
}
#endif