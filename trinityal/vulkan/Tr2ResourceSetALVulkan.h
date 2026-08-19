// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2ResourceSetAL.h"

namespace TrinityALImpl
{
	class Tr2ResourceSetAL: public Tr2DeviceResourceAL<Tr2ResourceSetAL>
	{
	public:
		Tr2ResourceSetAL();
		~Tr2ResourceSetAL();

		ALResult Create( const Tr2ResourceSetDescriptionAL& description, const ::Tr2ShaderProgramAL& program, Tr2PrimaryRenderContextAL& renderContext );
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

	private:
		Tr2PrimaryRenderContextAL *m_owner;

		VkDescriptorPool m_pool;
		VkDescriptorSet m_descriptorSet;

		friend class ::Tr2RenderContextAL;
	};
}

#endif