// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2ResourceBindingsVulkan.h"

#include <algorithm>
#include <chrono>

#include "Tr2BufferALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2SamplerStateALVulkan.h"
#include "Tr2ShaderProgramALVulkan.h"
#include "Tr2TextureALVulkan.h"
#include "UtilitiesVulkan.h"
#include "VkResult.h"
#include "ALLog.h"

namespace
{
// A fresh pool's budget. A frame that needs more gets another pool, so these only decide how
// often that happens; the EVE scene commits a few hundred sets a frame.
const uint32_t SETS_PER_POOL = 256;
const VkDescriptorPoolSize POOL_SIZES[] = {
	{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SETS_PER_POOL * 16 },
	{ VK_DESCRIPTOR_TYPE_SAMPLER, SETS_PER_POOL * 8 },
	{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS_PER_POOL * 4 },
	{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SETS_PER_POOL * 4 },
	{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, SETS_PER_POOL * 4 },
	{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, SETS_PER_POOL * 4 },
};

bool IsPoolExhausted( VkResult result )
{
	return result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL;
}
}

// --------------------------------------------------------------------------------------
Tr2ResourceBindings::Tr2ResourceBindings() :
	m_program( nullptr ),
	m_committed( false ),
	m_sealed( false ),
	m_set( VK_NULL_HANDLE ),
	m_setLayout( VK_NULL_HANDLE ),
	m_setFrame( 0 ),
	m_activeGroup( SIZE_MAX )
{
}

// --------------------------------------------------------------------------------------
Tr2ResourceBindings::~Tr2ResourceBindings()
{
	// The pools have to go through the owner's deferred destruction, which only the render
	// context can reach; it calls Release before it lets go of us. Nothing to do here.
}

// --------------------------------------------------------------------------------------
void Tr2ResourceBindings::Clear() throw()
{
	if( !m_program )
	{
		return;
	}
	const auto& registerMap = m_program->GetRegisterMap();
	std::fill( m_srvs, m_srvs + registerMap.srvCount, Resource() );
	std::fill( m_uavs, m_uavs + registerMap.uavCount, Resource() );
	std::fill( m_samplers, m_samplers + registerMap.samplerCount, Sampler() );
}

// --------------------------------------------------------------------------------------
void Tr2ResourceBindings::SetProgram( const TrinityALImpl::Tr2ShaderProgramAL* program ) throw()
{
	if( program == m_program )
	{
		return;
	}
	Clear();
	m_program = program;
	m_committed = false;
}

// --------------------------------------------------------------------------------------
void Tr2ResourceBindings::BeginBatch() throw()
{
	if( !m_sealed )
	{
		return;
	}
	m_sealed = false;
	m_committed = false;
	Clear();
}

// --------------------------------------------------------------------------------------
ALResult Tr2ResourceBindings::Reset() throw()
{
	Clear();
	m_committed = false;
	m_sealed = false;
	return S_OK;
}

// --------------------------------------------------------------------------------------
void Tr2ResourceBindings::Discard() throw()
{
	// Every slot, not Clear()'s current-program range: Discard runs at teardown, when the
	// program may already be gone (or null after SetProgram( nullptr )), and a slot left
	// holding a texture keeps its image alive past vkDestroyDevice
	// (VUID-vkDestroyDevice-device-05137, seen on the first run of the suite).
	std::fill( std::begin( m_srvs ), std::end( m_srvs ), Resource() );
	std::fill( std::begin( m_uavs ), std::end( m_uavs ), Resource() );
	std::fill( std::begin( m_samplers ), std::end( m_samplers ), Sampler() );
	m_program = nullptr;
	m_committed = false;
	m_sealed = false;
	m_set = VK_NULL_HANDLE;
	m_setLayout = VK_NULL_HANDLE;
	m_setFrame = 0;
}

// --------------------------------------------------------------------------------------
Tr2ResourceBindings::Resource* Tr2ResourceBindings::GetSrvSlot( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex ) throw()
{
	if( !m_program )
	{
		return nullptr;
	}
	BeginBatch();
	m_committed = false;
	const auto& registerMap = m_program->GetRegisterMap();
	uint32_t index = registerMap.srvs[stage][registerIndex];
	return index < registerMap.srvCount ? &m_srvs[index] : nullptr;
}

// --------------------------------------------------------------------------------------
Tr2ResourceBindings::Resource* Tr2ResourceBindings::GetUavSlot( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex ) throw()
{
	if( !m_program )
	{
		return nullptr;
	}
	BeginBatch();
	m_committed = false;
	const auto& registerMap = m_program->GetRegisterMap();
	uint32_t index = registerMap.uavs[stage][registerIndex];
	return index < registerMap.uavCount ? &m_uavs[index] : nullptr;
}

// --------------------------------------------------------------------------------------
Tr2ResourceBindings::Sampler* Tr2ResourceBindings::GetSamplerSlot( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex ) throw()
{
	if( !m_program )
	{
		return nullptr;
	}
	BeginBatch();
	m_committed = false;
	const auto& registerMap = m_program->GetRegisterMap();
	uint32_t index = registerMap.samplers[stage][registerIndex];
	return index < registerMap.samplerCount ? &m_samplers[index] : nullptr;
}

// --------------------------------------------------------------------------------------
const Tr2ResourceBindings::Resource* Tr2ResourceBindings::FindSrv( uint32_t stage, uint32_t registerIndex ) const throw()
{
	if( !m_program || stage >= Tr2RenderContextEnum::SHADER_TYPE_COUNT || registerIndex >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
	{
		return nullptr;
	}
	const auto& registerMap = m_program->GetRegisterMap();
	uint32_t index = registerMap.srvs[stage][registerIndex];
	return index < registerMap.srvCount ? &m_srvs[index] : nullptr;
}

// --------------------------------------------------------------------------------------
const Tr2ResourceBindings::Resource* Tr2ResourceBindings::FindUav( uint32_t stage, uint32_t registerIndex ) const throw()
{
	if( !m_program || stage >= Tr2RenderContextEnum::SHADER_TYPE_COUNT || registerIndex >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
	{
		return nullptr;
	}
	const auto& registerMap = m_program->GetRegisterMap();
	uint32_t index = registerMap.uavs[stage][registerIndex];
	return index < registerMap.uavCount ? &m_uavs[index] : nullptr;
}

// --------------------------------------------------------------------------------------
const Tr2ResourceBindings::Sampler* Tr2ResourceBindings::FindSampler( uint32_t stage, uint32_t registerIndex ) const throw()
{
	if( !m_program || stage >= Tr2RenderContextEnum::SHADER_TYPE_COUNT || registerIndex >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
	{
		return nullptr;
	}
	const auto& registerMap = m_program->GetRegisterMap();
	uint32_t index = registerMap.samplers[stage][registerIndex];
	return index < registerMap.samplerCount ? &m_samplers[index] : nullptr;
}

// --------------------------------------------------------------------------------------
ALResult Tr2ResourceBindings::SetSrv( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2BufferAL& buffer ) throw()
{
	if( stage >= Tr2RenderContextEnum::SHADER_TYPE_COUNT || registerIndex >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
	{
		return E_INVALIDARG;
	}
	if( auto* slot = GetSrvSlot( stage, registerIndex ) )
	{
		slot->type = Resource::BUFFER;
		slot->buffer = buffer;
		slot->texture = Tr2TextureAL();
	}
	return S_OK;
}

// --------------------------------------------------------------------------------------
ALResult Tr2ResourceBindings::SetSrv( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2TextureAL& texture, Tr2RenderContextEnum::ColorSpace colorSpace ) throw()
{
	if( stage >= Tr2RenderContextEnum::SHADER_TYPE_COUNT || registerIndex >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
	{
		return E_INVALIDARG;
	}
	if( auto* slot = GetSrvSlot( stage, registerIndex ) )
	{
		slot->type = Resource::TEXTURE;
		slot->texture = texture;
		slot->buffer = Tr2BufferAL();
		slot->colorSpace = colorSpace;
	}
	return S_OK;
}

// --------------------------------------------------------------------------------------
ALResult Tr2ResourceBindings::SetUav( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2BufferAL& buffer ) throw()
{
	if( stage >= Tr2RenderContextEnum::SHADER_TYPE_COUNT || registerIndex >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
	{
		return E_INVALIDARG;
	}
	if( auto* slot = GetUavSlot( stage, registerIndex ) )
	{
		slot->type = Resource::BUFFER;
		slot->buffer = buffer;
		slot->texture = Tr2TextureAL();
	}
	return S_OK;
}

// --------------------------------------------------------------------------------------
ALResult Tr2ResourceBindings::SetUav( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2TextureAL& texture, uint32_t mip ) throw()
{
	if( stage >= Tr2RenderContextEnum::SHADER_TYPE_COUNT || registerIndex >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
	{
		return E_INVALIDARG;
	}
	if( auto* slot = GetUavSlot( stage, registerIndex ) )
	{
		slot->type = Resource::TEXTURE;
		slot->texture = texture;
		slot->buffer = Tr2BufferAL();
		slot->mip = mip;
	}
	return S_OK;
}

// --------------------------------------------------------------------------------------
ALResult Tr2ResourceBindings::SetSampler( Tr2RenderContextEnum::ShaderType stage, uint32_t registerIndex, const Tr2SamplerStateAL& sampler ) throw()
{
	if( stage >= Tr2RenderContextEnum::SHADER_TYPE_COUNT || registerIndex >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
	{
		return E_INVALIDARG;
	}
	if( auto* slot = GetSamplerSlot( stage, registerIndex ) )
	{
		slot->sampler = sampler;
		slot->set = true;
	}
	return S_OK;
}

// --------------------------------------------------------------------------------------
VkImageLayout Tr2ResourceBindings::RequiredLayout( const Resource& resource, bool storage )
{
	if( storage )
	{
		return VK_IMAGE_LAYOUT_GENERAL;
	}
	// A depth-format SRV lives in DEPTH_STENCIL_READ_ONLY_OPTIMAL, not SHADER_READ_ONLY: the one
	// layout that is legal to sample and legal as a read-only depth attachment at once, so a
	// depth buffer sampled while still bound (SetReadOnlyDepth) needs no per-draw juggling.
	auto* texture = resource.texture.TrinityALImpl_GetObject();
	return ( TrinityALImpl::GetAspectMaskVulkan( texture->m_format ) & VK_IMAGE_ASPECT_DEPTH_BIT )
			   ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
			   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// --------------------------------------------------------------------------------------
bool Tr2ResourceBindings::NeedsTransition() const
{
	if( !m_program )
	{
		return false;
	}
	for( const auto& input : m_program->m_registerInput )
	{
		const VkDescriptorType type = TrinityALImpl::DescriptorTypeVulkan( input.type );
		if( type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE )
		{
			continue;
		}
		const bool storage = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		const uint32_t count = input.arrayCount ? input.arrayCount : 1;
		for( uint32_t k = 0; k < count; ++k )
		{
			const Resource* r = storage ? FindUav( input.stage, input.registerIndex + k ) : FindSrv( input.stage, input.registerIndex + k );
			if( r && r->type == Resource::TEXTURE && r->texture.IsValid() &&
				r->texture.TrinityALImpl_GetObject()->GetLayoutVulkan() != RequiredLayout( *r, storage ) )
			{
				return true;
			}
		}
	}
	return false;
}

// --------------------------------------------------------------------------------------
void Tr2ResourceBindings::TransitionImages( VkCommandBuffer commandBuffer )
{
	if( !m_program )
	{
		return;
	}
	for( const auto& input : m_program->m_registerInput )
	{
		const VkDescriptorType type = TrinityALImpl::DescriptorTypeVulkan( input.type );
		if( type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE )
		{
			continue;
		}
		const bool storage = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		const uint32_t count = input.arrayCount ? input.arrayCount : 1;
		for( uint32_t k = 0; k < count; ++k )
		{
			const Resource* r = storage ? FindUav( input.stage, input.registerIndex + k ) : FindSrv( input.stage, input.registerIndex + k );
			if( r && r->type == Resource::TEXTURE && r->texture.IsValid() )
			{
				r->texture.TrinityALImpl_GetObject()->TransitionVulkan( commandBuffer, RequiredLayout( *r, storage ) );
			}
		}
	}
}

// --------------------------------------------------------------------------------------
ALResult Tr2ResourceBindings::AllocateSet( Tr2PrimaryRenderContextAL& owner, VkDescriptorSetLayout layout, VkDescriptorSet& set ) throw()
{
	const uint64_t frame = owner.GetRecordingFrameNumber();
	if( m_activeGroup == SIZE_MAX || m_poolGroups[m_activeGroup].frame != frame )
	{
		// A new frame is recording: take a group whose frame the GPU has finished, or start
		// one. "Finished" is GetRenderedFrameNumber's answer -- every frame up to it is done --
		// and 0 means nothing is known to be done yet, so nothing used is reset then.
		const uint64_t rendered = owner.GetRenderedFrameNumber();
		m_activeGroup = SIZE_MAX;
		for( size_t i = 0; i < m_poolGroups.size(); ++i )
		{
			PoolGroup& group = m_poolGroups[i];
			if( group.pools.empty() || ( rendered > 0 && group.frame <= rendered ) )
			{
				for( VkDescriptorPool pool : group.pools )
				{
					vkResetDescriptorPool( owner.m_device, pool, 0 );
				}
				group.current = 0;
				m_activeGroup = i;
				break;
			}
		}
		if( m_activeGroup == SIZE_MAX )
		{
			m_poolGroups.emplace_back();
			m_activeGroup = m_poolGroups.size() - 1;
		}
		m_poolGroups[m_activeGroup].frame = frame;
	}

	PoolGroup& group = m_poolGroups[m_activeGroup];
	bool freshPoolFailed = false;
	for( ;; )
	{
		bool fresh = false;
		if( group.current == group.pools.size() )
		{
			VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			if( freshPoolFailed )
			{
				// One set wanted more of some type than a whole standard pool holds. Give it a
				// pool of its own, sized from the program.
				poolInfo.maxSets = 1;
				poolInfo.poolSizeCount = uint32_t( m_program->m_poolSizes.size() );
				poolInfo.pPoolSizes = m_program->m_poolSizes.data();
			}
			else
			{
				poolInfo.maxSets = SETS_PER_POOL;
				poolInfo.poolSizeCount = uint32_t( sizeof( POOL_SIZES ) / sizeof( POOL_SIZES[0] ) );
				poolInfo.pPoolSizes = POOL_SIZES;
			}
			VkDescriptorPool pool = VK_NULL_HANDLE;
			CR_RETURN_HR( Vk2Al( vkCreateDescriptorPool( owner.m_device, &poolInfo, nullptr, &pool ) ) );
			group.pools.push_back( pool );
			fresh = true;
		}

		VkDescriptorSetAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		allocateInfo.descriptorPool = group.pools[group.current];
		allocateInfo.descriptorSetCount = 1;
		allocateInfo.pSetLayouts = &layout;
		VkResult result = vkAllocateDescriptorSets( owner.m_device, &allocateInfo, &set );
		if( result == VK_SUCCESS )
		{
			return S_OK;
		}
		if( !IsPoolExhausted( result ) )
		{
			return Vk2Al( result );
		}
		if( fresh && freshPoolFailed )
		{
			// Even a pool sized for exactly this program refused. Nothing more to try.
			return Vk2Al( result );
		}
		freshPoolFailed = fresh;
		++group.current;
	}
}

// --------------------------------------------------------------------------------------
ALResult Tr2ResourceBindings::WriteSet( Tr2PrimaryRenderContextAL& owner, VkDescriptorSet set ) throw()
{
	const auto& inputs = m_program->m_registerInput;

	// Every write keeps a pointer into these vectors, so they are reserved for every
	// descriptor up front: a reallocation would move them under a live pointer.
	size_t descriptorTotal = 0;
	for( const auto& input : inputs )
	{
		descriptorTotal += input.arrayCount ? input.arrayCount : 1;
	}
	std::vector<VkWriteDescriptorSet> writes;
	writes.reserve( inputs.size() );
	std::vector<VkDescriptorImageInfo> imageInfos;
	imageInfos.reserve( descriptorTotal );
	std::vector<VkDescriptorBufferInfo> bufferInfos;
	bufferInfos.reserve( descriptorTotal );
	std::vector<VkBufferView> texelViews;
	texelViews.reserve( descriptorTotal );

	const bool haveDummyTexture = owner.m_dummyTexture.IsValid();
	const bool haveDummySampler = owner.m_dummySampler != VK_NULL_HANDLE;

	for( const auto& input : inputs )
	{
		if( input.type == Tr2ShaderRegisterAL::CONSTANT_BUFFER )
		{
			// Constant buffers are descriptor set 0, written by BindConstantBuffers.
			continue;
		}

		const VkDescriptorType type = TrinityALImpl::DescriptorTypeVulkan( input.type );
		const uint32_t count = input.arrayCount ? input.arrayCount : 1;
		const size_t imageFirst = imageInfos.size();
		const size_t bufferFirst = bufferInfos.size();
		const size_t texelFirst = texelViews.size();
		bool complete = true;

		for( uint32_t k = 0; k < count && complete; ++k )
		{
			const uint32_t reg = input.registerIndex + k;
			switch( type )
			{
			case VK_DESCRIPTOR_TYPE_SAMPLER: {
				const Sampler* s = FindSampler( input.stage, reg );
				VkDescriptorImageInfo info = {};
				if( s && s->set && s->sampler.IsValid() )
				{
					info.sampler = s->sampler.TrinityALImpl_GetObject()->m_sampler;
				}
				else if( haveDummySampler )
				{
					info.sampler = owner.m_dummySampler;
				}
				else
				{
					complete = false;
					break;
				}
				imageInfos.push_back( info );
				break;
			}
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {
				const Resource* r = FindSrv( input.stage, reg );
				VkDescriptorImageInfo info = {};
				if( r && r->type == Resource::TEXTURE && r->texture.IsValid() )
				{
					// The colour space chooses the view: an SRV asked for COLOR_SPACE_SRGB has
					// to sample through the sRGB view or every texel comes back un-decoded.
					info.imageView = r->texture.TrinityALImpl_GetObject()->GetImageView( r->colorSpace );
					info.imageLayout = RequiredLayout( *r, false );
				}
				else if( input.type == Tr2ShaderRegisterAL::SRV_TEXTURE2D && haveDummyTexture )
				{
					info.imageView = owner.GetDummyImageViewVulkan();
					info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				}
				else
				{
					complete = false;
					break;
				}
				imageInfos.push_back( info );
				break;
			}
			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
				const Resource* r = FindUav( input.stage, reg );
				if( !r || r->type != Resource::TEXTURE || !r->texture.IsValid() )
				{
					complete = false;
					break;
				}
				if( r->mip != 0 )
				{
					CCP_AL_LOGWARN_LIMITED( "Vulkan: a storage image bound at mip %u is bound at mip 0 (no per-mip storage views yet); stage %u, register %u", r->mip, input.stage, reg );
				}
				VkDescriptorImageInfo info = {};
				// The storage view, not the shader-resource one: for a cube texture they differ
				// in dim (2D_ARRAY against CUBE), and the descriptor has to match the shader.
				info.imageView = r->texture.TrinityALImpl_GetObject()->GetStorageViewVulkan();
				info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
				imageInfos.push_back( info );
				break;
			}
			case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
				const bool storage = type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
				const Resource* r = storage ? FindUav( input.stage, reg ) : FindSrv( input.stage, reg );
				VkBufferView view = VK_NULL_HANDLE;
				if( r && r->type == Resource::BUFFER && r->buffer.IsValid() )
				{
					view = r->buffer.TrinityALImpl_GetObject()->GetBufferViewVulkan();
				}
				if( view == VK_NULL_HANDLE )
				{
					complete = false;
					break;
				}
				texelViews.push_back( view );
				break;
			}
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
				const bool storage = ( input.type & Tr2ShaderRegisterAL::UAV_REGISTER_FLAG ) != 0;
				const Resource* r = storage ? FindUav( input.stage, reg ) : FindSrv( input.stage, reg );
				if( !r || r->type != Resource::BUFFER || !r->buffer.IsValid() )
				{
					complete = false;
					break;
				}
				VkDescriptorBufferInfo info = { r->buffer.TrinityALImpl_GetObject()->GetBufferVulkan(), 0, VK_WHOLE_SIZE };
				bufferInfos.push_back( info );
				break;
			}
			default:
				complete = false;
				break;
			}
		}

		if( !complete )
		{
			// Left unwritten. If the shader really reads it, validation names the binding; this
			// line names the register the engine did not set.
			CCP_AL_LOGWARN_LIMITED( "Vulkan: no resource bound for stage %u register %u (register type %u, binding %u); descriptor left unwritten",
									input.stage, input.registerIndex, uint32_t( input.type ), input.binding );
			imageInfos.resize( imageFirst );
			bufferInfos.resize( bufferFirst );
			texelViews.resize( texelFirst );
			continue;
		}

		VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = set;
		write.dstBinding = input.binding;
		write.dstArrayElement = 0;
		write.descriptorCount = count;
		write.descriptorType = type;
		switch( type )
		{
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			write.pTexelBufferView = &texelViews[texelFirst];
			break;
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			write.pBufferInfo = &bufferInfos[bufferFirst];
			break;
		default:
			write.pImageInfo = &imageInfos[imageFirst];
			break;
		}
		writes.push_back( write );
		m_stats.descriptorsWritten += count;
	}

	if( !writes.empty() )
	{
		vkUpdateDescriptorSets( owner.m_device, uint32_t( writes.size() ), writes.data(), 0, nullptr );
	}
	return S_OK;
}

// --------------------------------------------------------------------------------------
ALResult Tr2ResourceBindings::Commit( Tr2PrimaryRenderContextAL& owner, VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint ) throw()
{
	if( !m_program )
	{
		return E_INVALIDCALL;
	}
	VkDescriptorSetLayout layout = m_program->m_resourceLayout;
	if( layout == VK_NULL_HANDLE )
	{
		// The program reads nothing from set 1.
		m_committed = true;
		m_sealed = true;
		return S_OK;
	}

	const uint64_t frame = owner.GetRecordingFrameNumber();
	const bool reusable = m_committed && m_set != VK_NULL_HANDLE && m_setLayout == layout && m_setFrame == frame;
	if( reusable )
	{
		++m_stats.setsReused;
	}
	else
	{
		const auto start = std::chrono::steady_clock::now();
		VkDescriptorSet set = VK_NULL_HANDLE;
		CR_RETURN_HR( AllocateSet( owner, layout, set ) );
		CR_RETURN_HR( WriteSet( owner, set ) );
		m_set = set;
		m_setLayout = layout;
		m_setFrame = frame;
		++m_stats.setsWritten;
		m_stats.writeNanoseconds += uint64_t(
			std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - start ).count() );
	}

	// Bound on every commit, reused or not: the bind point may differ (a draw after a
	// dispatch), and a pipeline layout change since the last bind disturbs set 1.
	vkCmdBindDescriptorSets( commandBuffer, bindPoint, m_program->m_pipelineLayout, 1, 1, &m_set, 0, nullptr );

	m_committed = true;
	m_sealed = true;
	return S_OK;
}

// --------------------------------------------------------------------------------------
void Tr2ResourceBindings::Release( Tr2PrimaryRenderContextAL& owner ) throw()
{
	for( auto& group : m_poolGroups )
	{
		for( VkDescriptorPool pool : group.pools )
		{
			owner.DestroyLaterVulkan( pool, vkDestroyDescriptorPool );
		}
	}
	m_poolGroups.clear();
	m_activeGroup = SIZE_MAX;
	m_set = VK_NULL_HANDLE;
	m_setLayout = VK_NULL_HANDLE;
	m_setFrame = 0;
	m_committed = false;
}

// --------------------------------------------------------------------------------------
Tr2ResourceBindings::Stats Tr2ResourceBindings::TakeStats() throw()
{
	Stats stats = m_stats;
	m_stats = Stats();
	return stats;
}

#endif
