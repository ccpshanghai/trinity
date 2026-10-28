// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2RtBottomLevelAccelerationStructureAL.h"

namespace TrinityALImpl
{
// A buffer the ray tracing classes own outright -- build scratch, TLAS instance descriptors,
// shader binding tables -- with its device address aligned up to what the consumer needs
// (scratch: minAccelerationStructureScratchOffsetAlignment, a table region:
// shaderGroupBaseAlignment). Over-allocated by the alignment, because a VkBuffer's memory
// alignment is the driver's choice and nothing promises it covers either. Host-visible ones
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
// DX12, Tr2RaytracingGeometry.cpp). Closes an open rendering scope first -- a build is an
// outside-render-pass command -- then: everything before -> this build (the skinning
// dispatch's writes to the vertex input, a previous frame's trace still reading a structure
// rebuilt in place, the shared scratch), and this build -> later builds and ray tracing
// shaders (BLAS -> TLAS, TLAS -> trace).
void RecordAccelerationStructureBuildVulkan( Tr2RenderContextAL& renderContext, const VkAccelerationStructureBuildGeometryInfoKHR& info, const VkAccelerationStructureBuildRangeInfoKHR& range );

class Tr2RtBottomLevelAccelerationStructureAL : public Tr2DeviceResourceAL<Tr2RtBottomLevelAccelerationStructureAL>
{
public:
	Tr2RtBottomLevelAccelerationStructureAL();
	~Tr2RtBottomLevelAccelerationStructureAL();

	// Sized for `capacity`, built from `geometry`, on the primary context's command buffer.
	ALResult Create( const Tr2RtGeometryAL& geometry, const Tr2RtGeometryAL& capacity, Tr2RtBlasGeometryFlags::Type geometryFlags, Tr2RtBuildFlags::Type buildFlags, Tr2PrimaryRenderContextAL& renderContext );
	// Refit in place when the topology is unchanged and ALLOW_UPDATE was asked for; a full
	// rebuild into the same structure when it changed and still fits; E_INVALIDARG when it
	// does not fit, which the engine answers with a new Create (Tr2RaytracingGeometry.cpp).
	ALResult Update( const Tr2RtGeometryAL& geometry, Tr2RenderContextAL& renderContext );
	// Not implemented (no caller; DX12's body is commented out). Fails rather than claiming
	// a compaction that did not happen.
	ALResult CompactBlas( Tr2PrimaryRenderContextAL& renderContext );
	void Destroy() override;
	bool IsValid() const;

	Tr2ALMemoryType GetMemoryClass() const;
	void Describe( Tr2DeviceResourceDescriptionAL& description ) const override;

	// What a TLAS instance references.
	VkDeviceAddress GetDeviceAddressVulkan() const
	{
		return m_address;
	}

private:
	struct GeometryKey
	{
		uint32_t stride;
		uint32_t vertexCount;
		VkFormat vertexFormat;
		VkIndexType indexType;
		uint32_t indexCount;

		bool operator==( const GeometryKey& other ) const
		{
			return stride == other.stride && vertexCount == other.vertexCount && vertexFormat == other.vertexFormat && indexType == other.indexType && indexCount == other.indexCount;
		}
	};

	Tr2PrimaryRenderContextAL* m_owner;
	VkBuffer m_buffer;
	VkDeviceMemory m_memory;
	VkDeviceSize m_size;
	VkAccelerationStructureKHR m_accelerationStructure;
	VkDeviceAddress m_address;
	// Kept only with ALLOW_UPDATE, as DX12 keeps its scratch.
	RtBufferVulkan m_scratch;
	GeometryKey m_key;
	Tr2RtBlasGeometryFlags::Type m_geometryFlags;
	Tr2RtBuildFlags::Type m_buildFlags;
};
}

#endif
