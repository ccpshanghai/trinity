// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2ResourceSetALVulkan.h"
#include <cstring>
#include <map>
#include <string>
#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2SamplerStateALVulkan.h"
#include "Tr2TextureALVulkan.h"
#include "Tr2BufferALVulkan.h"
#include "Tr2ShaderProgramALVulkan.h"
#include "Tr2RtPipelineStateALVulkan.h"
#include "../include/Tr2RtPipelineStateAL.h"
#include "UtilitiesVulkan.h"


namespace TrinityALImpl
{
	Tr2ResourceSetAL::Tr2ResourceSetAL()
		:m_owner( nullptr ),
		m_pool( VK_NULL_HANDLE ),
		m_descriptorSet( VK_NULL_HANDLE ),
		m_layout( VK_NULL_HANDLE )
	{
	}

	Tr2ResourceSetAL::~Tr2ResourceSetAL()
	{
		Destroy();
	}

	ALResult Tr2ResourceSetAL::Create( const Tr2ResourceSetDescriptionAL& description, const ::Tr2RtPipelineStateAL& pipeline, Tr2PrimaryRenderContextAL& renderContext )
	{
		Destroy();

		if( !renderContext.IsValid() || !pipeline.IsValid() )
		{
			return E_INVALIDARG;
		}
		const auto& rt = *pipeline.m_pipeline;
		// The register map the engine built from the library's global input on COMPUTE_SHADER
		// (Tr2EffectDescription) against the one the pipeline built from the same signature.
		if( rt.GetRegisterMap() != description.m_registerMap )
		{
			return E_INVALIDARG;
		}
		if( rt.m_resourceLayout )
		{
			FORWARD_HR( WriteSetVulkan( description, rt.m_registerInput, rt.m_poolSizes, rt.m_resourceLayout, renderContext ) );
		}
		m_owner = &renderContext;
		return S_OK;
	}

	// Whether the description holds anything for `input`'s register.
	bool Tr2ResourceSetAL::IsSetVulkan( const Tr2ResourceSetDescriptionAL& description, const Tr2RegisterInputVulkan& input )
	{
		typedef Tr2ResourceSetDescriptionAL::Resource::Type ResourceType;
		if( input.registerIndex >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
		{
			return false;
		}
		if( input.type == Tr2ShaderRegisterAL::SAMPLER )
		{
			const uint8_t slot = description.m_registerMap.samplers[input.stage][input.registerIndex];
			return slot < description.m_registerMap.samplerCount && description.m_samplers[slot].sampler.IsValid();
		}
		const bool uav = ( input.type & Tr2ShaderRegisterAL::UAV_REGISTER_FLAG ) != 0;
		const uint8_t slot = uav ? description.m_registerMap.uavs[input.stage][input.registerIndex] : description.m_registerMap.srvs[input.stage][input.registerIndex];
		if( slot >= ( uav ? description.m_registerMap.uavCount : description.m_registerMap.srvCount ) )
		{
			return false;
		}
		const auto& resource = uav ? description.m_uav[slot] : description.m_srv[slot];
		return ( resource.type == ResourceType::BUFFER && resource.buffer.IsValid() ) || ( resource.type == ResourceType::TEXTURE && resource.texture.IsValid() );
	}

	// Allocates m_descriptorSet against `layout` and writes every resource register of
	// `registers` from `description`. The one write loop for both a shader program's set and a
	// ray tracing pipeline's global one.
	ALResult Tr2ResourceSetAL::WriteSetVulkan(
		const Tr2ResourceSetDescriptionAL& description,
		const std::vector<Tr2RegisterInputVulkan>& registers,
		const std::vector<VkDescriptorPoolSize>& poolSizes,
		VkDescriptorSetLayout layout,
		Tr2PrimaryRenderContextAL& renderContext )
	{
		VkDescriptorPoolCreateInfo poolDesc = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			nullptr,
			0,
			1,
			uint32_t( poolSizes.size() ),
			poolSizes.data()
		};

		CR_RETURN_HR( Vk2Al( vkCreateDescriptorPool( renderContext.m_device, &poolDesc, nullptr, &m_pool ) ) );
		// Owned from here, so a failure below still hands the pool to Destroy.
		m_owner = &renderContext;

		VkDescriptorSetAllocateInfo allocateInfo = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			nullptr,
			m_pool,
			1,
			&layout
		};

		CR_RETURN_HR( Vk2Al( vkAllocateDescriptorSets( renderContext.m_device, &allocateInfo, &m_descriptorSet ) ) );
		m_layout = layout;

		std::vector<VkWriteDescriptorSet> descriptorWrites;
		descriptorWrites.reserve( registers.size() );
		// Reserved by descriptors, not registers: an array register contributes arrayCount
		// entries, and every write below keeps a pointer into these vectors, so a reserve
		// that undercounts is a reallocation under a live pointer.
		size_t descriptorTotal = 0;
		for( auto it = begin( registers ); it != end( registers ); ++it )
		{
			descriptorTotal += it->arrayCount ? it->arrayCount : 1;
		}
		std::vector<VkDescriptorImageInfo> imageInfos;
		imageInfos.reserve( descriptorTotal );
		std::vector<VkDescriptorBufferInfo> bufferInfos;
		bufferInfos.reserve( descriptorTotal );
		std::vector<VkBufferView> texelBufferViews;
		texelBufferViews.reserve( descriptorTotal );
		std::vector<VkAccelerationStructureKHR> accelerationStructures;
		accelerationStructures.reserve( descriptorTotal );
		std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelerationStructureWrites;
		accelerationStructureWrites.reserve( descriptorTotal );

		typedef Tr2ResourceSetDescriptionAL::Resource::Type ResourceType;

		for( auto it = begin( registers ); it != end( registers ); ++it )
		{
			if( it->type == Tr2ShaderRegisterAL::CONSTANT_BUFFER )
			{
				// Constant buffers bind at descriptor set 0 (m_constantLayout),
				// written by SetConstants; they are not part of this resource set.
				continue;
			}

			// Arrays are implemented for SRV textures, below. For a buffer, a UAV or a
			// sampler the layout (Tr2ShaderProgramALVulkan) already declares arrayCount
			// descriptors, so writing one of them here would leave the rest undefined --
			// exactly the silent fault item 12 was about. Refuse instead, until a test wants
			// one of those and the write is looped like the texture case.
			const bool isTextureSrv = ( it->type & Tr2ShaderRegisterAL::SRV_REGISTER_FLAG ) &&
				!( it->type & Tr2ShaderRegisterAL::UAV_REGISTER_FLAG ) &&
				it->type >= Tr2ShaderRegisterAL::SRV_TEXTURE1D;
			if( it->arrayCount > 1 && !isTextureSrv )
			{
				return E_FAIL;
			}

			VkWriteDescriptorSet d = {
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				nullptr,
				m_descriptorSet,
				it->binding,
				0,
				1,
			};

			// A binding no module reads, with nothing set for it: left unwritten, which Vulkan
			// allows for a descriptor that is not statically used (VkDescriptorSetLayout of a
			// ray tracing pipeline is the global input's superset; see Tr2RegisterInputVulkan).
			if( it->optional && !IsSetVulkan( description, *it ) )
			{
				continue;
			}

			if( it->descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR )
			{
				// The TLAS's ::Tr2BufferAL, set as an SRV like any buffer; the structure built
				// over it is what the descriptor names.
				const auto& resource = description.m_srv[description.m_registerMap.srvs[it->stage][it->registerIndex]];
				if( resource.type != ResourceType::BUFFER || !resource.buffer.IsValid() || !resource.buffer.m_buffer->GetAccelerationStructureVulkan() )
				{
					return E_FAIL;
				}
				accelerationStructures.push_back( resource.buffer.m_buffer->GetAccelerationStructureVulkan() );
				VkWriteDescriptorSetAccelerationStructureKHR asWrite = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
				asWrite.accelerationStructureCount = 1;
				asWrite.pAccelerationStructures = &accelerationStructures.back();
				accelerationStructureWrites.push_back( asWrite );
				d.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
				d.pNext = &accelerationStructureWrites.back();
			}
			else if( it->type == Tr2ShaderRegisterAL::SAMPLER )
			{
				if( !description.m_samplers[description.m_registerMap.samplers[it->stage][it->registerIndex]].sampler.IsValid() )
				{
					return E_FAIL;
				}
				VkDescriptorImageInfo imageInfo = { description.m_samplers[description.m_registerMap.samplers[it->stage][it->registerIndex]].sampler.m_sampler->m_sampler };
				imageInfos.push_back( imageInfo );
				d.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
				d.pImageInfo = &imageInfos.back();
			}
			else if( it->type & Tr2ShaderRegisterAL::UAV_REGISTER_FLAG )
			{
				const auto& resource = description.m_uav[description.m_registerMap.uavs[it->stage][it->registerIndex]];
				if( resource.type == ResourceType::BUFFER )
				{
					if( it->type == Tr2ShaderRegisterAL::UAV_BUFFER )
					{
						texelBufferViews.push_back( resource.buffer.m_buffer->GetBufferViewVulkan() );
						d.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
						d.pTexelBufferView = &texelBufferViews.back();
					}
					else
					{
						VkDescriptorBufferInfo bufferInfo = { resource.buffer.m_buffer->GetBufferVulkan(), 0, VK_WHOLE_SIZE };
						bufferInfos.push_back( bufferInfo );
						d.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
						d.pBufferInfo = &bufferInfos.back();
					}
				}
				else
				{
					if( !resource.texture.IsValid() )
					{
						return E_FAIL;
					}
					VkDescriptorImageInfo imageInfo = {};
					// The storage view, not the shader-resource one: for a cube texture they
					// differ in dim (2D_ARRAY against CUBE), and the descriptor has to match
					// what the shader declared.
					imageInfo.imageView = resource.texture.m_texture->GetStorageViewVulkan();
					imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
					imageInfos.push_back( imageInfo );
					BoundImageVulkan bound = { resource.texture, VK_IMAGE_LAYOUT_GENERAL };
					m_boundImages.push_back( bound );
					d.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
					d.pImageInfo = &imageInfos.back();
				}
			}
			else
			{
				const auto& resource = description.m_srv[description.m_registerMap.srvs[it->stage][it->registerIndex]];
				if( resource.type == ResourceType::BUFFER )
				{
					if( it->type == Tr2ShaderRegisterAL::SRV_BUFFER )
					{
						texelBufferViews.push_back( resource.buffer.m_buffer->GetBufferViewVulkan() );
						d.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
						d.pTexelBufferView = &texelBufferViews.back();
					}
					else
					{
						VkDescriptorBufferInfo bufferInfo = { resource.buffer.m_buffer->GetBufferVulkan(), 0, VK_WHOLE_SIZE };
						bufferInfos.push_back( bufferInfo );
						d.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
						d.pBufferInfo = &bufferInfos.back();
					}
				}
				else
				{
					// One write, arrayCount descriptors: element k of an array declared at
					// registerIndex is whatever SetSrv( stage, registerIndex + k, ... ) put
					// there -- Tr2RegisterMapAL maps every element (item 12). Element 0 is
					// `resource` itself; a plain register is the loop run once.
					const uint32_t arrayCount = it->arrayCount ? it->arrayCount : 1;
					const size_t first = imageInfos.size();
					for( uint32_t k = 0; k < arrayCount; ++k )
					{
						if( it->registerIndex + k >= Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE )
						{
							return E_FAIL;
						}
						const uint8_t slot = description.m_registerMap.srvs[it->stage][it->registerIndex + k];
						if( slot >= description.m_registerMap.srvCount )
						{
							return E_FAIL;
						}
						const auto& element = description.m_srv[slot];
						if( element.type != ResourceType::TEXTURE || !element.texture.IsValid() )
						{
							// Nothing set for a 2D or 2D-array texture: the primary context's
							// black dummy, as DX12 binds a null SRV and D3D9 samples black. A
							// failed Create leaves the set unbound, and a dispatch that statically
							// uses it then reads whatever the descriptor pool held -- the froxel
							// fog's LightProfileArray, which only a Tr2LightManager registers and
							// SOF Preview has none, lost the device on its first frame
							// (VUID-vkCmdDispatch-None-08600). Other dimensions still fail: no
							// stand-in exists for them yet.
							// Tracked like any bound image: nothing else moves the dummy out of
							// the layout its upload left it in.
							const ::Tr2TextureAL* dummy = it->type == Tr2ShaderRegisterAL::SRV_TEXTURE2D ? &renderContext.m_dummyTexture : it->type == Tr2ShaderRegisterAL::SRV_TEXTURE2DARRAY ? &renderContext.m_dummyTextureArray :
																																																 nullptr;
							if( !dummy || !dummy->IsValid() )
							{
								return E_FAIL;
							}
							VkDescriptorImageInfo imageInfo = {};
							imageInfo.imageView = dummy->m_texture->GetImageView( Tr2RenderContextEnum::COLOR_SPACE_LINEAR );
							imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
							imageInfos.push_back( imageInfo );
							BoundImageVulkan bound = { *dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
							m_boundImages.push_back( bound );
							continue;
						}
						VkDescriptorImageInfo imageInfo = {};
						// element.colorSpace was being dropped on the floor here, so an SRV
						// asked for COLOR_SPACE_SRGB sampled raw UNORM and every sampled
						// texel came back un-decoded. Nothing failed and no test noticed:
						// Rendering.CanSampleSrgbTexture passes either way because it never
						// compares against a reference image.
						//
						// GetImageView also indexes by m_currentIndex rather than 0, which
						// is a second, deliberate change: for a swapchain-backed texture
						// bound as an SRV, view 0 is the wrong image on every frame but the
						// first. Identical for every single-image texture, which is all the
						// suite exercises.
						imageInfo.imageView = element.texture.m_texture->GetImageView( element.colorSpace );
						// A depth-format SRV lives in DEPTH_STENCIL_READ_ONLY_OPTIMAL, not
						// SHADER_READ_ONLY: that is the one layout that is simultaneously
						// legal to sample and legal as a read-only depth attachment, so a
						// depth buffer sampled while still bound (SetReadOnlyDepth) needs
						// no per-draw layout juggling -- the descriptor, the transition
						// target and the attachment all agree on it.
						const VkImageLayout srvLayout =
							( GetAspectMaskVulkan( element.texture.m_texture->m_format ) & VK_IMAGE_ASPECT_DEPTH_BIT ) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
						imageInfo.imageLayout = srvLayout;
						imageInfos.push_back( imageInfo );
						BoundImageVulkan bound = { element.texture, srvLayout };
						m_boundImages.push_back( bound );
					}
					d.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
					d.descriptorCount = arrayCount;
					d.pImageInfo = &imageInfos[first];
				}
			}

			descriptorWrites.push_back( d );
		}
		vkUpdateDescriptorSets( renderContext.m_device, uint32_t( descriptorWrites.size() ), descriptorWrites.data(), 0, nullptr );
		return S_OK;
	}

	ALResult Tr2ResourceSetAL::Create( const Tr2ResourceSetDescriptionAL& description, const ::Tr2ShaderProgramAL& program, Tr2PrimaryRenderContextAL& renderContext )
	{
		Destroy();

		if( !renderContext.IsValid() || !program.IsValid() )
		{
			return E_INVALIDARG;
		}

		if( program.GetRegisterMap() != description.m_registerMap )
		{
			return E_INVALIDARG;
		}

		if( program.m_program->m_resourceLayout )
		{
			FORWARD_HR( WriteSetVulkan( description, program.m_program->m_registerInput, program.m_program->m_poolSizes, program.m_program->m_resourceLayout, renderContext ) );
		}

		m_owner = &renderContext;

		return S_OK;
	}

	void Tr2ResourceSetAL::Destroy()
	{
		// Released here rather than left to the destructor: each entry holds a reference
		// to the texture, which is deliberate -- a descriptor naming a destroyed image is
		// worse than keeping it alive -- but it means the set has to let go explicitly.
		m_boundImages.clear();

		if( m_owner )
		{
			//m_owner->DestroyLaterVulkan( m_descriptorSets, vkFreeDescriptorSets );
			m_owner->DestroyLaterVulkan( m_pool, vkDestroyDescriptorPool );
			m_owner = nullptr;
			m_pool = VK_NULL_HANDLE;
			m_descriptorSet = VK_NULL_HANDLE;
		m_layout = VK_NULL_HANDLE;
		}
	}

	bool Tr2ResourceSetAL::IsValid() const
	{
		return m_owner != nullptr;
	}

	bool Tr2ResourceSetAL::NeedsTransitionVulkan() const
	{
		for( auto it = begin( m_boundImages ); it != end( m_boundImages ); ++it )
		{
			if( it->texture.IsValid() && it->texture.m_texture->GetLayoutVulkan() != it->layout )
			{
				return true;
			}
		}
		return false;
	}

	void Tr2ResourceSetAL::TransitionImagesVulkan( VkCommandBuffer commandBuffer )
	{
		for( auto it = begin( m_boundImages ); it != end( m_boundImages ); ++it )
		{
			if( !it->texture.IsValid() )
			{
				continue;
			}
			// TRINITY_DRAW_LOG=1: the post-process chain's intermediates, sampled here for the
			// first time each, are where a black frame gets decided. Every 60th time one of the
			// watched names is about to be read, its centre is copied out for a deferred readback.
			if( DrawLogEnabledVulkan() && it->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL )
			{
				const char* name = it->texture.m_texture->GetName();
				if( name && ( strcmp( name, "Pre-upscaling Composite" ) == 0 || strcmp( name, "Sharpening Output" ) == 0
					|| strcmp( name, "Final Result" ) == 0 || strcmp( name, "Tonemapping Result" ) == 0 ) )
				{
					// One counter per name, so that two watched textures sampled in the same
					// frame do not lock the ration onto whichever one is odd.
					static std::map<std::string, uint32_t> seen;
					if( ( ++seen[name] % 40 ) == 25 )
					{
						it->texture.m_texture->TransitionVulkan( commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
						RecordProbeVulkan( *it->texture.m_texture->m_owner, commandBuffer, it->texture.m_texture->GetImageVulkan(),
							GetAspectMaskVulkan( it->texture.m_texture->m_format ), it->texture.GetWidth(), it->texture.GetHeight(), name );
					}
				}
			}
			it->texture.m_texture->TransitionVulkan( commandBuffer, it->layout );
		}
	}

	Tr2ALMemoryType Tr2ResourceSetAL::GetMemoryClass() const
	{
		return AL_MEMORY_VIDEO;
	}

	void Tr2ResourceSetAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
	{
		description["type"] = "Tr2ResourceSetAL";
	}

	ALResult Tr2ResourceSetAL::SetName( const char* )
	{
		return E_NOTIMPL;
	}
}


#endif