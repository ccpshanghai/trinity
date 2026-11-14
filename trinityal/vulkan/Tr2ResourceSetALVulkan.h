// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2ResourceSetAL.h"

namespace TrinityALImpl
{
	struct Tr2RegisterInputVulkan;

	class Tr2ResourceSetAL: public Tr2DeviceResourceAL<Tr2ResourceSetAL>
	{
	public:
		Tr2ResourceSetAL();
		~Tr2ResourceSetAL();

		ALResult Create( const Tr2ResourceSetDescriptionAL& description, const ::Tr2ShaderProgramAL& program, Tr2PrimaryRenderContextAL& renderContext );
		// A set against a ray-tracing pipeline's global signature: the pipeline's set-1 layout,
		// COMPUTE_SHADER register numbering (the engine binds ray tracing globals through the
		// compute slots), and an acceleration structure descriptor where the pipeline's SPIR-V
		// declares one.
		ALResult Create( const Tr2ResourceSetDescriptionAL& description, const ::Tr2RtPipelineStateAL& pipeline, Tr2PrimaryRenderContextAL& renderContext );
		void Destroy();

		bool IsValid() const;
		Tr2ALMemoryType GetMemoryClass() const;
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const;
		ALResult SetName( const char* name );

		// Which images this set binds, and the layout each descriptor claims. The
		// descriptors are written once at Create, but the images they name move between
		// layouts for the life of the set -- a render target is COLOR_ATTACHMENT_OPTIMAL
		// while being drawn to and has to be SHADER_READ_ONLY_OPTIMAL to be sampled -- so
		// the set has to be able to say what it needs, and the render context transitions
		// them before it opens a pass.
		struct BoundImageVulkan
		{
			::Tr2TextureAL texture;
			VkImageLayout layout;
		};
		std::vector<BoundImageVulkan> m_boundImages;

		bool NeedsTransitionVulkan() const;
		void TransitionImagesVulkan( VkCommandBuffer commandBuffer );

		// Whether every binding of `registers` that a module reads (not `optional`) was written.
		// A ray tracing pipeline rebuilt with the same global input shares its set-1 layout with
		// the old one (Tr2PrimaryRenderContextAL::AcquireRtResourceLayoutVulkan), so a set the
		// engine keeps passes the layout check; this is the part a new library could still break.
		// On false, `unwritten` is the first such binding.
		bool CoversVulkan( const std::vector<Tr2RegisterInputVulkan>& registers, uint32_t& unwritten ) const;

	private:
		static bool IsSetVulkan( const Tr2ResourceSetDescriptionAL& description, const Tr2RegisterInputVulkan& input );
		// The description's entry for element `element` of `input`'s register, or null when the
		// register is past the map's table or the map names no slot for it. The one bounds check
		// every lookup in this class goes through.
		static const Tr2ResourceSetDescriptionAL::Resource* FindSrvVulkan( const Tr2ResourceSetDescriptionAL& description, const Tr2RegisterInputVulkan& input, uint32_t element = 0 );
		static const Tr2ResourceSetDescriptionAL::Resource* FindUavVulkan( const Tr2ResourceSetDescriptionAL& description, const Tr2RegisterInputVulkan& input );
		static const Tr2ResourceSetDescriptionAL::Sampler* FindSamplerVulkan( const Tr2ResourceSetDescriptionAL& description, const Tr2RegisterInputVulkan& input );
		ALResult WriteSetVulkan(
			const Tr2ResourceSetDescriptionAL& description,
			const std::vector<Tr2RegisterInputVulkan>& registers,
			const std::vector<VkDescriptorPoolSize>& poolSizes,
			VkDescriptorSetLayout layout,
			Tr2PrimaryRenderContextAL& renderContext );

		Tr2PrimaryRenderContextAL *m_owner;

		VkDescriptorPool m_pool;
		VkDescriptorSet m_descriptorSet;

		// The layout m_descriptorSet was allocated against. A set is only legal with the
		// pipeline layout it was created for, and the AL's state machine happily carries
		// a bound set across a shader program change -- SetPipeline compares this against
		// the current program's layout and falls back to the default set on mismatch.
		VkDescriptorSetLayout m_layout;

		// Bindings WriteSetVulkan skipped because they were optional and nothing was set there.
		std::vector<uint32_t> m_unwrittenBindings;

		friend class ::Tr2RenderContextAL;
	};
}

#endif