// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2RtPipelineStateALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2ShaderBindingABIVulkan.h"
#include "SpirvReflectVulkan.h"
#include "UtilitiesVulkan.h"
#include "../ALLog.h"

#include <algorithm>
#include <deque>

namespace
{
const VkShaderStageFlags k_rayTracingStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;

// SPIR-V execution models of the ray tracing stages (SPV_KHR_ray_tracing).
VkShaderStageFlagBits StageForExecutionModel( uint32_t executionModel )
{
	switch( executionModel )
	{
	case 5313:
		return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
	case 5314:
		return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
	case 5315:
		return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
	case 5316:
		return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
	case 5317:
		return VK_SHADER_STAGE_MISS_BIT_KHR;
	case 5318:
		return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
	default:
		return VkShaderStageFlagBits( 0 );
	}
}

// The engine's names are wide (DXR's are); a SPIR-V entry point name is UTF-8.
std::string ToUtf8( const std::wstring& wide )
{
	std::string result;
	result.reserve( wide.size() );
	for( auto c : wide )
	{
		const uint32_t cp = uint32_t( c );
		if( cp < 0x80 )
		{
			result += char( cp );
		}
		else if( cp < 0x800 )
		{
			result += char( 0xc0 | ( cp >> 6 ) );
			result += char( 0x80 | ( cp & 0x3f ) );
		}
		else if( cp < 0x10000 )
		{
			result += char( 0xe0 | ( cp >> 12 ) );
			result += char( 0x80 | ( ( cp >> 6 ) & 0x3f ) );
			result += char( 0x80 | ( cp & 0x3f ) );
		}
		else
		{
			result += char( 0xf0 | ( cp >> 18 ) );
			result += char( 0x80 | ( ( cp >> 12 ) & 0x3f ) );
			result += char( 0x80 | ( ( cp >> 6 ) & 0x3f ) );
			result += char( 0x80 | ( cp & 0x3f ) );
		}
	}
	return result;
}
}


namespace TrinityALImpl
{
Tr2RtPipelineStateAL::Tr2RtPipelineStateAL() :
	m_owner( nullptr ),
	m_pipeline( VK_NULL_HANDLE ),
	m_handleSize( 0 )
{
}

Tr2RtPipelineStateAL::~Tr2RtPipelineStateAL()
{
	Destroy();
}

ALResult Tr2RtPipelineStateAL::CreateRtPipelineState( const Tr2RtPipelineStateDescriptionAL& desc, Tr2PrimaryRenderContextAL& renderContext )
{
	Destroy();
	if( !renderContext.IsValid() || !renderContext.GetCaps().SupportsRaytracing() )
	{
		return E_INVALIDCALL;
	}
	m_owner = &renderContext;

	// The constant buffer a local signature puts into the shader record, if any. Only constant
	// buffers can be local on this backend (as on DX12, whose table refuses local SRVs, UAVs
	// and samplers), and only one: each group gets one inline record, that buffer's bytes. The
	// engine passes one local signature for a whole library (every export AddShaders adds from
	// it), so the limit is one local cbuffer per library, not per entry point; the shader
	// compiler's CheckShaderRecords refuses a library that reads two, so this only fires on an
	// effect compiled before that rule.
	auto recordRegister = [&desc]( uint32_t signatureIndex, uint32_t& reg ) -> bool {
		reg = NO_RECORD;
		if( signatureIndex == Tr2RtPipelineStateDescriptionAL::NO_SIGNATURE )
		{
			return true;
		}
		if( signatureIndex >= desc.m_localSignatures.size() )
		{
			return false;
		}
		for( auto& r : desc.m_localSignatures[signatureIndex].registers )
		{
			if( r.registerType != Tr2ShaderRegisterAL::CONSTANT_BUFFER )
			{
				CCP_AL_LOGERR( "Tr2RtPipelineStateAL: a local signature with a non-constant-buffer register (type %d, index %u); Vulkan shader records hold one constant buffer only", int( r.registerType ), r.registerIndex );
				return false;
			}
			if( reg != NO_RECORD && reg != r.registerIndex )
			{
				CCP_AL_LOGERR( "Tr2RtPipelineStateAL: a local signature with two constant buffers (b%u, b%u); a Vulkan shader record holds one", reg, r.registerIndex );
				return false;
			}
			reg = r.registerIndex;
		}
		return true;
	};

	std::vector<VkShaderModule> modules;
	ON_BLOCK_EXIT( [&] {
		// Only needed while the pipeline is created.
		for( auto module : modules )
		{
			vkDestroyShaderModule( renderContext.m_device, module, nullptr );
		}
	} );
	std::deque<std::string> entryNames; // stable addresses for pName
	std::vector<VkPipelineShaderStageCreateInfo> stages;
	std::vector<uint32_t> stageRecordSizes; // per stage: its module's shader record size
	std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
	std::map<std::wstring, uint32_t> stageForExport;
	Tr2BindingSetVulkan usedBindings, accelerationStructureBindings;

	auto generalGroup = [&groups]( uint32_t stage ) {
		VkRayTracingShaderGroupCreateInfoKHR group = { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
		group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
		group.generalShader = stage;
		group.closestHitShader = VK_SHADER_UNUSED_KHR;
		group.anyHitShader = VK_SHADER_UNUSED_KHR;
		group.intersectionShader = VK_SHADER_UNUSED_KHR;
		groups.push_back( group );
		return uint32_t( groups.size() - 1 );
	};

	for( auto& shader : desc.m_shaders )
	{
		VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		moduleInfo.codeSize = shader.bytecode.size;
		moduleInfo.pCode = reinterpret_cast<const uint32_t*>( shader.bytecode.bytecode );
		// One walk gives the export stages, the bindings the module reads and its record size.
		SpirvReflectVulkan::SpirvReflection reflection;
		if( !SpirvReflectVulkan::ReflectSpirv( shader.bytecode.bytecode, shader.bytecode.size, reflection ) || reflection.entryPoints.empty() )
		{
			CCP_AL_LOGERR( "Tr2RtPipelineStateAL: a library that is not SPIR-V this AL can read (%u bytes)", uint32_t( shader.bytecode.size ) );
			Destroy();
			return E_INVALIDARG;
		}
		VkShaderModule module = VK_NULL_HANDLE;
		if( FAILED( Vk2Al( vkCreateShaderModule( renderContext.m_device, &moduleInfo, nullptr, &module ) ) ) )
		{
			Destroy();
			return E_FAIL;
		}
		modules.push_back( module );

		for( auto& b : reflection.bindings )
		{
			usedBindings.insert( std::make_pair( b.set, b.binding ) );
			if( b.accelerationStructure )
			{
				accelerationStructureBindings.insert( std::make_pair( b.set, b.binding ) );
			}
		}

		uint32_t record = NO_RECORD;
		if( !recordRegister( shader.localSignature, record ) )
		{
			Destroy();
			return E_INVALIDARG;
		}
		const uint32_t recordSize = record == NO_RECORD ? 0 : reflection.ShaderRecordSize();

		for( auto& name : shader.names )
		{
			entryNames.push_back( ToUtf8( name.name ) );
			const SpirvReflectVulkan::SpirvReflection::EntryPoint* entryPoint = nullptr;
			for( auto& candidate : reflection.entryPoints )
			{
				if( candidate.name == entryNames.back() )
				{
					entryPoint = &candidate;
				}
			}
			const VkShaderStageFlagBits stage = entryPoint ? StageForExecutionModel( entryPoint->executionModel ) : VkShaderStageFlagBits( 0 );
			if( !stage )
			{
				CCP_AL_LOGERR( "Tr2RtPipelineStateAL: the library has no ray tracing entry point '%s'", entryNames.back().c_str() );
				Destroy();
				return E_INVALIDARG;
			}
			VkPipelineShaderStageCreateInfo stageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			stageInfo.stage = stage;
			stageInfo.module = module;
			stageInfo.pName = entryNames.back().c_str();
			stages.push_back( stageInfo );
			stageRecordSizes.push_back( recordSize );
			const uint32_t stageIndex = uint32_t( stages.size() - 1 );
			stageForExport[name.exportName] = stageIndex;

			if( stage == VK_SHADER_STAGE_RAYGEN_BIT_KHR || stage == VK_SHADER_STAGE_MISS_BIT_KHR || stage == VK_SHADER_STAGE_CALLABLE_BIT_KHR )
			{
				GroupInfoVulkan info = { generalGroup( stageIndex ), record, recordSize };
				m_groups[name.exportName] = info;
			}
		}
	}

	for( auto& hitGroup : desc.m_hitGroups )
	{
		if( !hitGroup.intersection.empty() )
		{
			// No EVE effect has one and the engine only builds triangle geometry.
			CCP_AL_LOGERR( "Tr2RtPipelineStateAL: procedural hit groups (an intersection shader) are not implemented on Vulkan" );
			Destroy();
			return E_NOTIMPL;
		}
		auto stageOf = [&stageForExport]( const std::wstring& exportName ) {
			if( exportName.empty() )
			{
				return uint32_t( VK_SHADER_UNUSED_KHR );
			}
			auto found = stageForExport.find( exportName );
			return found == stageForExport.end() ? uint32_t( VK_SHADER_UNUSED_KHR ) : found->second;
		};
		VkRayTracingShaderGroupCreateInfoKHR group = { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
		group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
		group.generalShader = VK_SHADER_UNUSED_KHR;
		group.closestHitShader = stageOf( hitGroup.closestHit );
		group.anyHitShader = stageOf( hitGroup.anyHit );
		group.intersectionShader = VK_SHADER_UNUSED_KHR;
		if( ( !hitGroup.closestHit.empty() && group.closestHitShader == VK_SHADER_UNUSED_KHR ) || ( !hitGroup.anyHit.empty() && group.anyHitShader == VK_SHADER_UNUSED_KHR ) )
		{
			CCP_AL_LOGERR( "Tr2RtPipelineStateAL: a hit group names a shader that was never added" );
			Destroy();
			return E_INVALIDARG;
		}
		uint32_t record = NO_RECORD;
		if( !recordRegister( hitGroup.localSignature, record ) )
		{
			Destroy();
			return E_INVALIDARG;
		}
		uint32_t recordSize = 0;
		if( record != NO_RECORD )
		{
			for( uint32_t stage : { group.closestHitShader, group.anyHitShader } )
			{
				recordSize = std::max( recordSize, stage < stageRecordSizes.size() ? stageRecordSizes[stage] : 0u );
			}
		}
		groups.push_back( group );
		GroupInfoVulkan info = { uint32_t( groups.size() - 1 ), record, recordSize };
		m_groups[hitGroup.exportName] = info;
	}

	if( groups.empty() )
	{
		Destroy();
		return E_INVALIDARG;
	}

	// The global signature: set 0 (constant buffers) and set 1 (everything else) with
	// COMPUTE_SHADER binding numbers, visible to every ray tracing stage. Set 1 comes from the
	// primary context's cache, shared by every pipeline whose global input has the same
	// bindings, so the resource set the engine keeps across a rebuild stays bindable.
	std::vector<Tr2LayoutRegisterVulkan> registers;
	for( auto& reg : desc.m_globalSignature.registers )
	{
		registers.push_back( { &reg, Tr2RenderContextEnum::COMPUTE_SHADER, k_rayTracingStages } );
	}
	ALResult hr = BuildDescriptorLayoutsVulkan( registers, &accelerationStructureBindings, &usedBindings, true, renderContext, m_layouts );
	if( FAILED( hr ) )
	{
		Destroy();
		return hr;
	}

	VkRayTracingPipelineCreateInfoKHR pipelineInfo = { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
	pipelineInfo.stageCount = uint32_t( stages.size() );
	pipelineInfo.pStages = stages.data();
	pipelineInfo.groupCount = uint32_t( groups.size() );
	pipelineInfo.pGroups = groups.data();
	// DX12's MaxTraceRecursionDepth: the shaders trace from raygen only.
	pipelineInfo.maxPipelineRayRecursionDepth = 1;
	pipelineInfo.layout = m_layouts.pipelineLayout;
	hr = Vk2Al( renderContext.m_raytracing.createRayTracingPipelines( renderContext.m_device, VK_NULL_HANDLE, renderContext.m_pipelineCache, 1, &pipelineInfo, nullptr, &m_pipeline ) );
	if( FAILED( hr ) )
	{
		CCP_AL_LOGERR( "Tr2RtPipelineStateAL: vkCreateRayTracingPipelinesKHR failed (%u stages, %u groups)", pipelineInfo.stageCount, pipelineInfo.groupCount );
		m_pipeline = VK_NULL_HANDLE;
		Destroy();
		return hr;
	}

	m_handleSize = renderContext.m_rayTracingPipelineProperties.shaderGroupHandleSize;
	m_handles.resize( size_t( m_handleSize ) * groups.size() );
	hr = Vk2Al( renderContext.m_raytracing.getRayTracingShaderGroupHandles( renderContext.m_device, m_pipeline, 0, uint32_t( groups.size() ), m_handles.size(), m_handles.data() ) );
	if( FAILED( hr ) )
	{
		Destroy();
		return hr;
	}

	// The same map the engine built for the library's resource set description
	// (Tr2EffectDescription: COMPUTE_SHADER over the global input), which Tr2ResourceSetAL
	// compares against.
	const auto computeShader = Tr2RenderContextEnum::COMPUTE_SHADER;
	m_registerMap = Tr2RegisterMapAL( &computeShader, &desc.m_globalSignature, 1 );

	// Rare (the engine rebuilds only when a new hit group library appears) and the one
	// positive sign in a log that the ray-traced path got this far.
	uint32_t accelerationStructures = 0;
	for( auto& input : m_layouts.registerInput )
	{
		accelerationStructures += input.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR ? 1 : 0;
	}
	CCP_AL_LOG( "vulkan: ray tracing pipeline created: %u stages, %u groups, %u global bindings (%u acceleration structure)",
				uint32_t( stages.size() ),
				uint32_t( groups.size() ),
				uint32_t( m_layouts.registerInput.size() ),
				accelerationStructures );
	return S_OK;
}

const Tr2RtPipelineStateAL::GroupInfoVulkan* Tr2RtPipelineStateAL::FindGroupVulkan( const wchar_t* exportName ) const
{
	if( !exportName )
	{
		return nullptr;
	}
	auto found = m_groups.find( exportName );
	return found == m_groups.end() ? nullptr : &found->second;
}

void Tr2RtPipelineStateAL::Destroy()
{
	if( m_owner )
	{
		m_owner->DestroyLaterVulkan( m_pipeline, vkDestroyPipeline );
		// Not the set-1 layout: the primary context's (sharedResourceLayout).
		m_layouts.Destroy( *m_owner );
		m_owner = nullptr;
	}
	m_pipeline = VK_NULL_HANDLE;
	m_layouts = Tr2DescriptorLayoutsVulkan();
	m_registerMap = Tr2RegisterMapAL();
	m_groups.clear();
	m_handles.clear();
	m_handleSize = 0;
}

bool Tr2RtPipelineStateAL::IsValid() const
{
	return m_pipeline != VK_NULL_HANDLE;
}

Tr2ALMemoryType Tr2RtPipelineStateAL::GetMemoryClass() const
{
	return AL_MEMORY_MANAGED;
}

void Tr2RtPipelineStateAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
{
	description["type"] = "Tr2RtPipelineStateAL";
}
}

#endif
