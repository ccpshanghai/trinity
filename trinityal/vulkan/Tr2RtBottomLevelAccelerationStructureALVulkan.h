// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2RtBottomLevelAccelerationStructureAL.h"
#include "Tr2RtUtilitiesVulkan.h"

namespace TrinityALImpl
{
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
	// The structure's storage; its size is the structure's (the most a rebuild in place may take).
	RtBufferVulkan m_storage;
	VkAccelerationStructureKHR m_accelerationStructure;
	VkDeviceAddress m_address;
	// For the capacity Create was sized for. A build or refit takes its scratch from the frame's
	// arena (Tr2PrimaryRenderContextAL::AllocateRtScratchVulkan); nothing is kept between builds.
	VkDeviceSize m_buildScratchSize;
	VkDeviceSize m_updateScratchSize;
	GeometryKey m_key;
	Tr2RtBlasGeometryFlags::Type m_geometryFlags;
	Tr2RtBuildFlags::Type m_buildFlags;
};
}

#endif
