// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2RtTopLevelAccelerationStructureAL.h"
#include "../include/Tr2BufferAL.h"
#include "Tr2RtBottomLevelAccelerationStructureALVulkan.h"

namespace TrinityALImpl
{
// The scene's TLAS. Its storage is a ::Tr2BufferAL with Tr2GpuUsage::ACCELERATION_STRUCTURE,
// as on DX12, because the engine binds the TLAS to effects as that buffer (an SRV,
// Tr2RaytracingGeometry::GetGpuBuffer); the VkAccelerationStructureKHR built over it is handed
// to the buffer (SetAccelerationStructureVulkan), where the resource set finds it.
class Tr2RtTopLevelAccelerationStructureAL : public Tr2DeviceResourceAL<Tr2RtTopLevelAccelerationStructureAL>
{
public:
	Tr2RtTopLevelAccelerationStructureAL();
	~Tr2RtTopLevelAccelerationStructureAL();

	// Capacity Align( count, 128 ), as DX12; built on the primary context's command buffer.
	ALResult Create( const size_t count, const Tr2RtInstanceAL* instances, Tr2RtBuildFlags::Type buildFlags, Tr2PrimaryRenderContextAL& renderContext );
	// Up to the capacity: new instance data from a per-frame ring, then a refit (ALLOW_UPDATE
	// and the same count as the last build) or a rebuild into the same structure.
	ALResult Update( const size_t count, const Tr2RtInstanceAL* instances, Tr2RenderContextAL& renderContext );
	void Destroy() override;
	bool IsValid() const;

	Tr2ALMemoryType GetMemoryClass() const;
	void Describe( Tr2DeviceResourceDescriptionAL& description ) const override;

	const ::Tr2BufferAL& GetBuffer() const;
	size_t GetCapacity() const;

private:
	// The instance descriptors for one build, host-visible, reused once the frame that read
	// them has rendered.
	struct InstanceBuffer
	{
		RtBufferVulkan buffer;
		uint64_t frame;
	};
	ALResult AcquireInstanceBuffer( RtBufferVulkan*& buffer );
	ALResult Build( size_t count, const Tr2RtInstanceAL* instances, Tr2RenderContextAL& renderContext );

	Tr2PrimaryRenderContextAL* m_owner;
	::Tr2BufferAL m_buffer;
	// For the capacity; each build takes its scratch from the frame's arena
	// (Tr2PrimaryRenderContextAL::AllocateRtScratchVulkan).
	VkDeviceSize m_buildScratchSize;
	VkDeviceSize m_updateScratchSize;
	std::vector<InstanceBuffer> m_instanceBuffers;
	size_t m_capacity;
	size_t m_builtCount;
	Tr2RtBuildFlags::Type m_buildFlags;
};
}

#endif
