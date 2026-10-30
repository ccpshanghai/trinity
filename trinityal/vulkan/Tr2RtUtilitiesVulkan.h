// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

// What the ray tracing classes (Tr2Rt*ALVulkan) share: alignment, the buffers they own outright,
// and the recording of an acceleration structure build. Per-frame data -- shader binding tables,
// TLAS instance descriptors -- comes from the primary context's upload memory
// (Tr2PrimaryRenderContextAL::AllocateUploadVulkan), build scratch from its scratch arena
// (AllocateRtScratchVulkan); neither is here.

#include "../ALResult.h"

class Tr2RenderContextAL;
class Tr2PrimaryRenderContextAL;

namespace TrinityALImpl
{
// `value` rounded up to a multiple of `alignment` (any alignment; 0 leaves it).
inline VkDeviceSize AlignUpVulkan( VkDeviceSize value, VkDeviceSize alignment )
{
	return alignment ? ( value + alignment - 1 ) / alignment * alignment : value;
}

// A buffer a ray tracing class owns outright -- a BLAS's storage -- with its device address
// aligned up to what the consumer needs. Over-allocated by the alignment, because a VkBuffer's
// memory alignment is the driver's choice and nothing promises it covers it. Host-visible ones
// stay mapped for their life; vkFreeMemory unmaps.
struct RtBufferVulkan
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceAddress address = 0; // aligned
	VkDeviceSize size = 0; // usable bytes from `address`
	uint8_t* mapped = nullptr; // at `address`, host-visible only

	ALResult Create( VkDeviceSize size, VkDeviceSize alignment, VkBufferUsageFlags usage, bool hostVisible, Tr2PrimaryRenderContextAL& renderContext );
	// Queued with DestroyLaterVulkan: the frames in flight may still read it.
	void Destroy( Tr2PrimaryRenderContextAL& renderContext );
};

// One acceleration structure build on `renderContext`'s command buffer, with the
// synchronization the engine leaves to the AL on this backend (it records barriers only for
// DX12, Tr2RaytracingGeometry.cpp). Builds recorded back to back are one batch
// (Tr2RenderContextAL::m_asBatch):
// - the first build of a batch closes an open rendering scope -- a build is an
//   outside-render-pass command -- and records one barrier from everything before to the
//   build stage: the skinning dispatch's writes to the vertex input, a previous trace still
//   reading a structure rebuilt in place, an earlier batch's structures and scratch;
// - a later build records a build -> build barrier only when it reads what the batch built (a
//   TLAS over the batch's BLASes, a refit or rebuild of a structure built in the batch), when
//   it follows a TLAS of the batch (which read BLASes it may rewrite), or when its scratch
//   range overlaps one the batch wrote; BLASes over disjoint scratch from the frame's scratch
//   arena need none;
// - nothing after the last build: whatever reads a structure next records its own barrier
//   from ALL_COMMANDS (DispatchRays, RunComputeShader, a later batch's first build), and
//   every command that is not a build closes the batch.
// `scratchSize` is the bytes of scratch at info.scratchData the build uses. The build's inputs
// (the TLAS's instance descriptors in upload memory, the scratch) must stay valid until this
// frame's fence, which the upload memory and the scratch arena both guarantee.
void RecordAccelerationStructureBuildVulkan( Tr2RenderContextAL& renderContext, const VkAccelerationStructureBuildGeometryInfoKHR& info, const VkAccelerationStructureBuildRangeInfoKHR& range, VkDeviceSize scratchSize );
}

#endif
