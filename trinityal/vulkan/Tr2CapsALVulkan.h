// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM==TRINITY_VULKAN

#define TRINITY_PLATFORM_SUPPORTS_BUFFER_SHADER_RESOURCES 1
#define TRINITY_PLATFORM_SUPPORTS_BUFFER_COUNTERS 0
#define TRINITY_PLATFORM_SUPPORTS_UNORDERED_ACCESS 1
#define TRINITY_PLATFORM_SUPPORTS_COMPUTE 1
#define TRINITY_PLATFORM_SUPPORTS_TEXTURE_ARRAYS 1
#define TRINITY_PLATFORM_SUPPORTS_MSAA_SAMPLE 1
#define TRINITY_PLATFORM_SUPPORTS_RENDER_PASS_HINTS 0
#define TRINITY_PLATFORM_IS_LOW_PERFORMACE 0
#define TRINITY_PLATFORM_MAX_CONSTANT_BUFFER_SIZE ( 64 * 1024 )
// 1 says this backend has ray-tracing classes to compile; whether *this* device can use them
// is SupportsRaytracing() below, decided at device creation (dx12 and metal follow the same
// split). TrinityAL_vulkan is one library for Windows, Android and Linux, so the S23 gets the
// classes too and is kept off them by the run-time answer alone.
#define TRINITY_PLATFORM_SUPPORTS_RAY_TRACING 1
#define TRINITY_PLATFORM_SUPPORTS_ASTC_TEXTURES 0


class Tr2CapsAL
{
public:
	bool SupportsFloat16() const
	{
		return true;
	}
	bool SupportsGpuBuffer() const
	{
		return true;
	}
	// false, and this is the honest answer rather than a convenient one. dx11, dx12, metal
	// and the stub all report true; this backend reports false because Tr2SwapChainAL::Create
	// returns E_NOTIMPL, and a capability query that says yes to something the create call
	// refuses is simply wrong. The lie was the true.
	//
	// It is not implemented by decision, not by omission. A standalone swapchain is a second
	// VkSurfaceKHR and a second VkSwapchainKHR with its own acquire/present handshake, and it
	// exists for desktop tools with several top-level windows -- a phone has one surface, one
	// swapchain, and no second window to open. Multiple viewports inside one window need none
	// of it: that is render-to-texture composited into the one back buffer, which works.
	//
	// The six SwapChain tests guard their bodies on this query, so they now do nothing rather
	// than fail. That is the capability contract working as designed, but it does mean those
	// six are no longer coverage of anything -- see section 23.
	bool SupportsStandaloneSwapChain() const
	{
		return false;
	}
	bool SupportsVertexShaderTextures() const
	{
		return true;
	}
	bool SupportsVariableRefreshRate() const
	{
		return false;
	}
	// True only when the device was created with VK_KHR_acceleration_structure,
	// VK_KHR_ray_tracing_pipeline and VK_KHR_deferred_host_operations, the
	// accelerationStructure, rayTracingPipeline and bufferDeviceAddress features, and every
	// ray-tracing entry point resolved -- see Tr2PrimaryRenderContextAL::CreateDevice. When
	// false the device, its buffers and its allocations are created exactly as before ray
	// tracing existed in this backend.
	bool SupportsRaytracing() const
	{
		return m_supportsRaytracing;
	}
	// ASTC sampling, the one M3 asks about. A capability, not a platform (spec D7): the answer
	// differs between two Metal devices, so a compile-time #if would be wrong even here.
	bool SupportsAstcTextures() const
	{
		// Vulkan does define the ASTC formats and mobile Vulkan devices do sample them; this
		// backend simply has no rows for them yet -- UtilitiesVulkan.cpp's table stops at BC.
		// No until that changes, because a yes here would mean uploading blocks the format
		// table cannot name.
		return false;
	}

private:
	Tr2CapsAL() :
		m_supportsRaytracing( false )
	{
	}

	Tr2CapsAL( const Tr2CapsAL& other ) :
		m_supportsRaytracing( other.m_supportsRaytracing )
	{
	}

	Tr2CapsAL& operator=( const Tr2CapsAL& other )
	{
		m_supportsRaytracing = other.m_supportsRaytracing;
		return *this;
	}

	bool m_supportsRaytracing;

	friend class Tr2PrimaryRenderContextAL;
};

#endif
