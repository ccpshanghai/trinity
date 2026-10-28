// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include <string>

#include "../include/Tr2RtShaderTableAL.h"
#include "../include/Tr2RtPipelineStateAL.h"
#include "Tr2RtBottomLevelAccelerationStructureALVulkan.h"

namespace TrinityALImpl
{
// The shader binding table for one dispatch (the engine rebuilds it every frame).
//
// Records are a group handle followed by the bytes of the group's one local constant buffer,
// copied from the buffer's current contents when the table is created -- the snapshot DX12
// takes with UploadConstants, inline rather than as an address because a Vulkan shader record
// is the data itself ([[vk::shader_record_ext]]; the compiler verifies the record layout equals
// the D3D cbuffer layout at offset 0).
//
// Layout (VkPhysicalDeviceRayTracingPipelinePropertiesKHR): every region starts at a multiple
// of shaderGroupBaseAlignment; a region's stride is handle + its largest record, rounded up to
// shaderGroupHandleAlignment. Ray generation records each start on a base-aligned boundary,
// because DispatchRays picks one by name and the raygen region's size must equal its stride.
// One host-visible buffer from the primary context's table buffer pool
// (Tr2PrimaryRenderContextAL::m_rtTableBuffers), returned to it on Destroy.
class Tr2RtShaderTableAL : public Tr2DeviceResourceAL<Tr2RtShaderTableAL>
{
public:
	Tr2RtShaderTableAL();
	~Tr2RtShaderTableAL();

	ALResult Create( const Tr2RtShaderTableDescriptionAL& desc, const ::Tr2RtPipelineStateAL& pipeline, Tr2PrimaryRenderContextAL& renderContext );
	void Destroy() override;
	bool IsValid() const;

	Tr2ALMemoryType GetMemoryClass() const;
	void Describe( Tr2DeviceResourceDescriptionAL& description ) const override;

	// The region for the ray generation record added under `name`; false if there is none.
	bool GetRayGenRegionVulkan( const wchar_t* name, VkStridedDeviceAddressRegionKHR& region ) const;
	const VkStridedDeviceAddressRegionKHR& GetMissRegionVulkan() const
	{
		return m_missRegion;
	}
	const VkStridedDeviceAddressRegionKHR& GetHitRegionVulkan() const
	{
		return m_hitRegion;
	}

private:
	Tr2PrimaryRenderContextAL* m_owner;
	// The pooled buffer (see the .cpp), and the base-aligned address the regions start from.
	VkBuffer m_buffer;
	VkDeviceMemory m_memory;
	VkDeviceSize m_bufferSize;
	VkDeviceAddress m_bufferAddress;
	uint8_t* m_bufferMapped;
	VkDeviceAddress m_address;
	std::vector<std::wstring> m_rayGenNames;
	VkDeviceSize m_rayGenSpacing;
	VkDeviceSize m_rayGenStride;
	VkStridedDeviceAddressRegionKHR m_missRegion;
	VkStridedDeviceAddressRegionKHR m_hitRegion;
};
}

#endif
