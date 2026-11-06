// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include <string>

#include "../include/Tr2RtShaderTableAL.h"
#include "../include/Tr2RtPipelineStateAL.h"

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
// The records are in the frame's upload memory (Tr2PrimaryRenderContextAL::AllocateUploadVulkan),
// so a table is good for the frame that created it and no other: DispatchRays refuses a table
// from an earlier frame, whose memory may already hold another frame's data.
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
	// The recording frame the table was created in, the only one it may be traced in.
	uint64_t GetFrameVulkan() const
	{
		return m_frame;
	}

private:
	Tr2PrimaryRenderContextAL* m_owner;
	uint64_t m_frame;
	// The base-aligned address the regions start from, in that frame's upload memory.
	VkDeviceAddress m_address;
	std::vector<std::wstring> m_rayGenNames;
	VkDeviceSize m_rayGenSpacing;
	VkDeviceSize m_rayGenStride;
	VkStridedDeviceAddressRegionKHR m_missRegion;
	VkStridedDeviceAddressRegionKHR m_hitRegion;
};
}

#endif
