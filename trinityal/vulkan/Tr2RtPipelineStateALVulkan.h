// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include <map>
#include <set>
#include <string>

#include "../include/Tr2RtPipelineStateAL.h"
#include "../include/Tr2ResourceSetAL.h"
#include "Tr2ShaderProgramALVulkan.h"

class Tr2RenderContextAL;

namespace TrinityALImpl
{
class Tr2RtShaderTableAL;

// A VK_KHR_ray_tracing_pipeline pipeline built from the engine's DXR-shaped description.
//
// - Each library blob is one SPIR-V module with an entry point per export under its HLSL name
//   (the compiler's contract). The description carries no stages, so each export's stage is
//   its OpEntryPoint's execution model. Modules exist only for the vkCreateRayTracingPipelinesKHR
//   call: loading an effect never builds one, so a device without ray tracing (the S23) never
//   sees a module that declares RayTracingKHR.
// - DX12 renames exports to unique names (ExportToRename); Vulkan has no renaming, so the
//   unique name maps to a shader group index here, and the shader table asks by that name.
// - The global signature becomes set 0 (constant buffers) and set 1 (everything else) with
//   COMPUTE_SHADER binding numbers -- the engine binds ray tracing globals through the compute
//   slots (Tr2Material::ApplyMaterialDataForRtState) and the compiler shifts by them -- visible
//   to every ray tracing stage. A register that appears twice (the reflection reports an
//   explicitly placed cbuffer twice) is one binding. A RaytracingAccelerationStructure
//   reflects as SRV_BUFFER; its binding takes ACCELERATION_STRUCTURE_KHR where a module's
//   variable at that binding is OpTypeAccelerationStructureKHR.
// - A local signature may name at most one constant buffer: the shader record holds that
//   buffer's bytes after the group handle (Tr2RtShaderTableALVulkan).
class Tr2RtPipelineStateAL : public Tr2DeviceResourceAL<Tr2RtPipelineStateAL>
{
public:
	Tr2RtPipelineStateAL();
	~Tr2RtPipelineStateAL();

	ALResult CreateRtPipelineState( const Tr2RtPipelineStateDescriptionAL& desc, Tr2PrimaryRenderContextAL& renderContext );
	void Destroy() override;
	bool IsValid() const;

	Tr2ALMemoryType GetMemoryClass() const;
	void Describe( Tr2DeviceResourceDescriptionAL& description ) const override;

	const Tr2RegisterMapAL& GetRegisterMap() const
	{
		return m_registerMap;
	}

	// One shader group, by the unique export name the engine gave it.
	struct GroupInfoVulkan
	{
		uint32_t group;
		// The local constant buffer register whose bytes follow the handle in this group's
		// records, or NO_RECORD.
		uint32_t recordRegister;
		// The shader record block's size in the group's module(s) (0 if none reads it): the
		// zeroed bytes a record gets when the engine leaves that constant buffer unset.
		uint32_t recordSize;
	};
	static const uint32_t NO_RECORD = 0xffffffffu;
	const GroupInfoVulkan* FindGroupVulkan( const wchar_t* exportName ) const;
	const uint8_t* GetGroupHandleVulkan( uint32_t group ) const
	{
		return m_handles.data() + size_t( group ) * m_handleSize;
	}

private:
	Tr2PrimaryRenderContextAL* m_owner;
	VkPipeline m_pipeline;
	// Built by BuildDescriptorLayoutsVulkan, as a shader program's. The set-1 layout is not
	// owned: Tr2PrimaryRenderContextAL::AcquireRtResourceLayoutVulkan's, shared with every
	// pipeline whose global input has the same bindings.
	Tr2DescriptorLayoutsVulkan m_layouts;
	Tr2RegisterMapAL m_registerMap;

	std::map<std::wstring, GroupInfoVulkan> m_groups;
	std::vector<uint8_t> m_handles;
	uint32_t m_handleSize;

	friend class ::Tr2RenderContextAL;
	friend class TrinityALImpl::Tr2ResourceSetAL;
	friend class TrinityALImpl::Tr2RtShaderTableAL;
};
}

#endif
