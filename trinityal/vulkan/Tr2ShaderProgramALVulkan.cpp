// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2ShaderProgramALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2ShaderALVulkan.h"
#include "Tr2ShaderBindingABIVulkan.h"
#include "UtilitiesVulkan.h"
#include "../ALLog.h"

#include <map>


using namespace Tr2RenderContextEnum;


namespace TrinityALImpl
{
	VkDescriptorType GetDescriptorTypeVulkan( Tr2ShaderRegisterAL::RegisterType registerType )
	{
		switch( registerType )
		{
		case Tr2ShaderRegisterAL::CONSTANT_BUFFER:
			return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		case Tr2ShaderRegisterAL::SAMPLER:
			return VK_DESCRIPTOR_TYPE_SAMPLER;
		default:
			// HLSL Buffer<T> compiles to a uniform texel buffer and RWBuffer<T> to a
			// storage texel buffer (verified from dxc SPIR-V: OpTypeImage ... Buffer,
			// Sampled=1 and Sampled=2), so the typed UAV register takes a texel
			// descriptor + VkBufferView, while UAV_STRUCTURED_BUFFER takes a
			// STORAGE_BUFFER + VkDescriptorBufferInfo. Textures take STORAGE_IMAGE.
			if( registerType & Tr2ShaderRegisterAL::UAV_REGISTER_FLAG )
			{
				if( registerType == Tr2ShaderRegisterAL::UAV_BUFFER )
				{
					return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
				}
				if( registerType == Tr2ShaderRegisterAL::UAV_STRUCTURED_BUFFER )
				{
					return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				}
				return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			}
			// The SRV mirror of the above: typed SRV_BUFFER is a uniform texel buffer,
			// SRV_STRUCTURED_BUFFER is a storage buffer, textures are sampled images.
			if( registerType & Tr2ShaderRegisterAL::SRV_REGISTER_FLAG )
			{
				if( registerType == Tr2ShaderRegisterAL::SRV_BUFFER )
				{
					return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
				}
				if( registerType == Tr2ShaderRegisterAL::SRV_STRUCTURED_BUFFER )
				{
					return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				}
				return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			}
			return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		}
	}

	void Tr2DescriptorLayoutsVulkan::Destroy( Tr2PrimaryRenderContextAL& owner )
	{
		owner.DestroyLaterVulkan( pipelineLayout, vkDestroyPipelineLayout );
		if( !sharedResourceLayout )
		{
			owner.DestroyLaterVulkan( resourceLayout, vkDestroyDescriptorSetLayout );
		}
		owner.DestroyLaterVulkan( constantLayout, vkDestroyDescriptorSetLayout );
		owner.DestroyLaterVulkan( emptyLayout, vkDestroyDescriptorSetLayout );
		*this = Tr2DescriptorLayoutsVulkan();
	}

	ALResult BuildDescriptorLayoutsVulkan( const std::vector<Tr2LayoutRegisterVulkan>& registers,
										   const Tr2BindingSetVulkan* accelerationStructures,
										   const Tr2BindingSetVulkan* usedBindings,
										   bool sharedResourceLayout,
										   Tr2PrimaryRenderContextAL& renderContext,
										   Tr2DescriptorLayoutsVulkan& result )
	{
		result = Tr2DescriptorLayoutsVulkan();
		Tr2DescriptorLayoutsVulkan built;
		built.sharedResourceLayout = sharedResourceLayout;
		// Nothing has used them yet, so a failure destroys at once.
		auto fail = [&]( ALResult hr ) {
			if( built.pipelineLayout )
			{
				vkDestroyPipelineLayout( renderContext.m_device, built.pipelineLayout, nullptr );
			}
			for( auto layout : { sharedResourceLayout ? VK_NULL_HANDLE : built.resourceLayout, built.constantLayout, built.emptyLayout } )
			{
				if( layout )
				{
					vkDestroyDescriptorSetLayout( renderContext.m_device, layout, nullptr );
				}
			}
			return hr;
		};

		std::vector<VkDescriptorSetLayoutBinding> resourceBindings, constantBindings;
		// (set, binding) -> where its layout binding is: { constant set, index }.
		std::map<std::pair<uint32_t, uint32_t>, std::pair<bool, size_t>> declared;
		std::map<VkDescriptorType, uint32_t> poolCounts;

		for( auto& entry : registers )
		{
			const Tr2ShaderRegisterAL& reg = *entry.reg;
			const uint32_t set = Tr2VulkanBindingABI::DescriptorSetIndex( reg.registerType );
			const uint32_t bindingNumber = Tr2VulkanBindingABI::BindingNumber( reg.registerType, reg.registerIndex, entry.bindingStage );
			const auto key = std::make_pair( set, bindingNumber );
			VkDescriptorType type = GetDescriptorTypeVulkan( reg.registerType );
			if( accelerationStructures && accelerationStructures->count( key ) )
			{
				type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
			}
			// descriptorCount is the array size: `Texture2D maps[4] : register( t0 )` is ONE
			// binding with four descriptors, in the SPIR-V dxc emits and therefore here. A layout
			// that said 1 made vkUpdateDescriptorSets write element 0 alone and left three
			// descriptors undefined -- legal to record, wrong to sample (item 12).
			const uint32_t arrayCount = reg.arrayCount ? reg.arrayCount : 1;
			const bool constant = set == Tr2VulkanBindingABI::DESCRIPTOR_SET_CONSTANTS;

			auto existing = declared.find( key );
			if( existing != declared.end() )
			{
				auto& binding = existing->second.first ? constantBindings[existing->second.second] : resourceBindings[existing->second.second];
				if( binding.descriptorType != type )
				{
					CCP_AL_LOGERR( "vulkan: set %u binding %u is declared as two descriptor types", set, bindingNumber );
					return fail( E_INVALIDARG );
				}
				binding.stageFlags |= entry.stageFlags;
				continue;
			}

			VkDescriptorSetLayoutBinding binding = { bindingNumber, type, arrayCount, entry.stageFlags, nullptr };
			auto& bindings = constant ? constantBindings : resourceBindings;
			declared[key] = std::make_pair( constant, bindings.size() );
			bindings.push_back( binding );
			if( constant )
			{
				built.constantBindings.insert( bindingNumber );
			}
			else
			{
				// The pool has to hold every descriptor the layout declares, not every binding.
				poolCounts[type] += arrayCount;
			}
			const bool optional = usedBindings && usedBindings->count( key ) == 0;
			Tr2RegisterInputVulkan input = { bindingNumber, uint32_t( entry.bindingStage ), reg.registerIndex, arrayCount, reg.registerType, type, optional };
			built.registerInput.push_back( input );
		}

		// A binding a module reads that the signature does not declare would fail pipeline
		// creation with a VUID; name it instead.
		if( usedBindings )
		{
			for( auto& used : *usedBindings )
			{
				if( !declared.count( used ) )
				{
					CCP_AL_LOGERR( "Tr2RtPipelineStateAL: a library reads set %u binding %u, which the global input does not declare", used.first, used.second );
					return fail( E_INVALIDARG );
				}
			}
		}

		// One size entry per distinct VkDescriptorType, tallied from each register's real
		// descriptor type, so typed and structured buffers land in distinct entries. Constants are
		// excluded: their sets (set 0) come from the render context's constant pools.
		for( auto& count : poolCounts )
		{
			VkDescriptorPoolSize poolSize = { count.first, count.second };
			built.poolSizes.push_back( poolSize );
		}

		VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		if( !resourceBindings.empty() )
		{
			ALResult hr = S_OK;
			if( sharedResourceLayout )
			{
				hr = renderContext.AcquireRtResourceLayoutVulkan( resourceBindings, built.resourceLayout );
			}
			else
			{
				layoutInfo.bindingCount = uint32_t( resourceBindings.size() );
				layoutInfo.pBindings = resourceBindings.data();
				hr = Vk2Al( vkCreateDescriptorSetLayout( renderContext.m_device, &layoutInfo, nullptr, &built.resourceLayout ) );
			}
			if( FAILED( hr ) )
			{
				built.resourceLayout = VK_NULL_HANDLE;
				return fail( hr );
			}
		}
		if( !constantBindings.empty() )
		{
			layoutInfo.bindingCount = uint32_t( constantBindings.size() );
			layoutInfo.pBindings = constantBindings.data();
			ALResult hr = Vk2Al( vkCreateDescriptorSetLayout( renderContext.m_device, &layoutInfo, nullptr, &built.constantLayout ) );
			if( FAILED( hr ) )
			{
				built.constantLayout = VK_NULL_HANDLE;
				return fail( hr );
			}
		}

		VkDescriptorSetLayout layouts[2];
		uint32_t layoutCount = 0;
		if( built.constantLayout || built.resourceLayout )
		{
			if( built.constantLayout )
			{
				layouts[layoutCount++] = built.constantLayout;
			}
			else
			{
				VkDescriptorSetLayoutCreateInfo emptyInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
				ALResult hr = Vk2Al( vkCreateDescriptorSetLayout( renderContext.m_device, &emptyInfo, nullptr, &built.emptyLayout ) );
				if( FAILED( hr ) )
				{
					built.emptyLayout = VK_NULL_HANDLE;
					return fail( hr );
				}
				layouts[layoutCount++] = built.emptyLayout;
			}
			if( built.resourceLayout )
			{
				layouts[layoutCount++] = built.resourceLayout;
			}
		}
		VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		pipelineLayoutInfo.setLayoutCount = layoutCount;
		pipelineLayoutInfo.pSetLayouts = layouts;
		ALResult hr = Vk2Al( vkCreatePipelineLayout( renderContext.m_device, &pipelineLayoutInfo, nullptr, &built.pipelineLayout ) );
		if( FAILED( hr ) )
		{
			built.pipelineLayout = VK_NULL_HANDLE;
			return fail( hr );
		}
		result = std::move( built );
		return S_OK;
	}
}

namespace
{
	// The binding-number block a register lives in -- RegisterClassOffset, and the
	// whole binding formula with it -- now lives in Tr2ShaderBindingABIVulkan.h, so
	// that trinityal/tests/ShaderBindingABI.cpp can derive its expected SPIR-V
	// decorations by calling the same code this file calls, instead of tabulating
	// sixteen expected numbers that would keep passing while the formula drifted.
	// That header carries the full rationale (four HLSL classes vs. six descriptor
	// kinds; why b and u can share block 0; what Tasks 4e and 4f got wrong).
}

namespace TrinityALImpl
{
	Tr2ShaderProgramAL::Tr2ShaderProgramAL()
		:m_owner( nullptr ),
		m_defaultPool( VK_NULL_HANDLE ),
		m_defaultSet( VK_NULL_HANDLE ),
		m_defaultSetTried( false )
	{
	}

	Tr2ShaderProgramAL::~Tr2ShaderProgramAL()
	{
		Destroy();
	}

	ALResult Tr2ShaderProgramAL::Create( ::Tr2ShaderAL* shaders, size_t count, Tr2PrimaryRenderContextAL& renderContext )
	{
		Destroy();

		if( !renderContext.IsValid() )
		{
			return E_INVALIDCALL;
		}

		if( count == 0 )
		{
			return E_INVALIDARG;
		}

		uint32_t bitmask = 0;

		for( size_t i = 0; i < count; ++i )
		{
			if( !shaders[i].IsValid() )
			{
				return E_INVALIDARG;
			}
			auto mask = 1 << shaders[i].GetType();
			if( ( mask & bitmask ) != 0 )
			{
				return E_INVALIDARG;
			}
			bitmask |= mask;
		}
		auto csBit = 1 << COMPUTE_SHADER;
		if( ( bitmask & csBit ) != 0 && ( bitmask & ~csBit ) != 0 )
		{
			return E_INVALIDARG;
		}

		m_shaderInfo.reserve( count );
		m_shaders.reserve( count );

		std::vector<Tr2LayoutRegisterVulkan> registers;

		for( size_t i = 0; i < count; ++i )
		{
			VkPipelineShaderStageCreateInfo info = {
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				nullptr,
			};
			switch( shaders[i].GetType() )
			{
			case VERTEX_SHADER:
				info.stage = VK_SHADER_STAGE_VERTEX_BIT;
				m_shaderInputs = shaders[i].m_shader->m_signature.pipelineInputs;
				break;
			case PIXEL_SHADER:
				info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
				break;
			case COMPUTE_SHADER:
				info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
				break;
			case GEOMETRY_SHADER:
				info.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
				break;
			case HULL_SHADER:
				info.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
				break;
			case DOMAIN_SHADER:
				info.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
				break;
			}
			info.module = shaders[i].m_shader->m_shader;
			// The module's own entry point name, read out of the SPIR-V at Tr2ShaderAL::Create.
			// It is not "main" -- dxc names it after the HLSL entry function, so a hardcoded
			// "main" fails vkCreateGraphicsPipelines under
			// VUID-VkPipelineShaderStageCreateInfo-pName-00707 and every draw becomes a no-op.
			// The pointer's lifetime is the shader's, which m_shaders below holds -- the same
			// assumption info.module already makes.
			info.pName = shaders[i].m_shader->m_entryPoint.c_str();
			m_shaderInfo.push_back( info );
			m_shaders.push_back( shaders[i] );

			// Each stage's registers take its own binding numbers (the shader type is part of
			// them, Tr2ShaderBindingABIVulkan.h), so two stages never share a binding.
			auto& inputs = shaders[i].m_shader->m_signature.registers;
			for( auto it = begin( inputs ); it != end( inputs ); ++it )
			{
				registers.push_back( { &*it, shaders[i].GetType(), static_cast<VkShaderStageFlags>( info.stage ) } );
			}
		}

		FORWARD_HR( BuildDescriptorLayoutsVulkan( registers, nullptr, nullptr, false, renderContext, m_layouts ) );
		m_owner = &renderContext;

		m_registerMap = Tr2RegisterMapAL( shaders, count );

		return S_OK;
	}


	VkDescriptorSet Tr2ShaderProgramAL::GetDefaultResourceSetVulkan( Tr2PrimaryRenderContextAL& renderContext )
	{
		if( m_defaultSetTried )
		{
			return m_defaultSet;
		}
		m_defaultSetTried = true;

		if( !m_layouts.resourceLayout )
		{
			return VK_NULL_HANDLE;
		}
		if( !renderContext.m_dummyTexture.IsValid() || renderContext.m_dummySampler == VK_NULL_HANDLE )
		{
			return VK_NULL_HANDLE;
		}
		// Only slots the dummies can stand in for. A buffer, UAV or other texture dimension
		// left unbound keeps today's behaviour -- there is no honest 1x1 stand-in for a
		// structured buffer, and inventing one would hide a real binding bug.
		size_t descriptorTotal = 0;
		for( auto it = begin( m_layouts.registerInput ); it != end( m_layouts.registerInput ); ++it )
		{
			if( it->type == Tr2ShaderRegisterAL::SRV_TEXTURE2DARRAY && renderContext.GetDummyArrayImageViewVulkan() == VK_NULL_HANDLE )
			{
				return VK_NULL_HANDLE;
			}
			if( it->type != Tr2ShaderRegisterAL::CONSTANT_BUFFER
				&& it->type != Tr2ShaderRegisterAL::SAMPLER
				&& it->type != Tr2ShaderRegisterAL::SRV_TEXTURE2D
				&& it->type != Tr2ShaderRegisterAL::SRV_TEXTURE2DARRAY )
			{
				return VK_NULL_HANDLE;
			}
			descriptorTotal += it->arrayCount ? it->arrayCount : 1;
		}

		VkDescriptorPoolCreateInfo poolDesc = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			nullptr,
			0,
			1,
			uint32_t( m_layouts.poolSizes.size() ),
			m_layouts.poolSizes.data()
		};
		if( FAILED( Vk2Al( vkCreateDescriptorPool( renderContext.m_device, &poolDesc, nullptr, &m_defaultPool ) ) ) )
		{
			return VK_NULL_HANDLE;
		}

		VkDescriptorSetAllocateInfo allocateInfo = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			nullptr,
			m_defaultPool,
			1,
			&m_layouts.resourceLayout
		};
		if( FAILED( Vk2Al( vkAllocateDescriptorSets( renderContext.m_device, &allocateInfo, &m_defaultSet ) ) ) )
		{
			m_defaultSet = VK_NULL_HANDLE;
			return VK_NULL_HANDLE;
		}

		std::vector<VkWriteDescriptorSet> writes;
		writes.reserve( m_layouts.registerInput.size() );
		// Reserved by descriptors: every write keeps a pointer into this vector.
		std::vector<VkDescriptorImageInfo> imageInfos;
		imageInfos.reserve( descriptorTotal );
		for( auto it = begin( m_layouts.registerInput ); it != end( m_layouts.registerInput ); ++it )
		{
			if( it->type == Tr2ShaderRegisterAL::CONSTANT_BUFFER )
			{
				continue;
			}
			// Every element the layout declares (arrayCount of them), not only the first: the
			// rest would otherwise stay undefined for a shader that indexes the array.
			const uint32_t arrayCount = it->arrayCount ? it->arrayCount : 1;
			VkWriteDescriptorSet d = {
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				nullptr,
				m_defaultSet,
				it->binding,
				0,
				arrayCount,
			};
			VkDescriptorImageInfo imageInfo = {};
			if( it->type == Tr2ShaderRegisterAL::SAMPLER )
			{
				imageInfo.sampler = renderContext.m_dummySampler;
				d.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			}
			else
			{
				imageInfo.imageView = it->type == Tr2ShaderRegisterAL::SRV_TEXTURE2DARRAY ? renderContext.GetDummyArrayImageViewVulkan() : renderContext.GetDummyImageViewVulkan();
				imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				d.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			}
			const size_t first = imageInfos.size();
			imageInfos.insert( end( imageInfos ), arrayCount, imageInfo );
			d.pImageInfo = &imageInfos[first];
			writes.push_back( d );
		}
		vkUpdateDescriptorSets( renderContext.m_device, uint32_t( writes.size() ), writes.data(), 0, nullptr );
		return m_defaultSet;
	}

	void Tr2ShaderProgramAL::Destroy()
	{
		if( m_owner )
		{
			m_owner->DestroyLaterVulkan( m_defaultPool, vkDestroyDescriptorPool );
			m_defaultPool = VK_NULL_HANDLE;
			m_defaultSet = VK_NULL_HANDLE;
			m_defaultSetTried = false;
			m_layouts.Destroy( *m_owner );
			m_owner = nullptr;
		}
		m_layouts = Tr2DescriptorLayoutsVulkan();
		m_shaders.clear();
		m_shaderInfo.clear();
		m_shaderInputs.clear();
		m_registerMap = Tr2RegisterMapAL();
	}

	bool Tr2ShaderProgramAL::IsValid() const
	{
		return !m_shaderInfo.empty();
	}

	Tr2ALMemoryType Tr2ShaderProgramAL::GetMemoryClass() const
	{
		return AL_MEMORY_MANAGED;
	}

	void Tr2ShaderProgramAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
	{
		description["type"] = "Tr2ShaderProgramAL";
	}

	const Tr2RegisterMapAL& Tr2ShaderProgramAL::GetRegisterMap() const
	{
		return m_registerMap;
	}

	ALResult Tr2ShaderProgramAL::SetName( const char* )
	{
		return E_NOTIMPL;
	}
}

#endif