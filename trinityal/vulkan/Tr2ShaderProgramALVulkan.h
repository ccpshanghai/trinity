// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include <set>

#include "../include/Tr2ShaderProgramAL.h"
#include "../include/Tr2ShaderAL.h"
#include "../include/Tr2ResourceSetAL.h"

namespace TrinityALImpl
{
	class Tr2ShaderProgramAL : public Tr2DeviceResourceAL<Tr2ShaderProgramAL>
	{
	public:
		Tr2ShaderProgramAL();
		~Tr2ShaderProgramAL();

		ALResult Create( ::Tr2ShaderAL* shaders, size_t count, Tr2PrimaryRenderContextAL& renderContext );
		void Destroy();
		bool IsValid() const;
		const Tr2RegisterMapAL& GetRegisterMap() const;

		Tr2ALMemoryType GetMemoryClass() const;
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const;
		ALResult SetName( const char* name );

	private:
		std::vector<VkPipelineShaderStageCreateInfo> m_shaderInfo;
		std::vector<::Tr2ShaderAL> m_shaders;
		std::vector<Tr2ShaderPipelineInputAL> m_shaderInputs;

		std::vector<VkDescriptorPoolSize> m_poolSizes;

		VkPipelineLayout m_pipelineLayout;

		VkDescriptorSetLayout m_resourceLayout;
		VkDescriptorSetLayout m_constantLayout;

		// The binding numbers m_constantLayout declares. BindConstantBuffers writes one
		// descriptor per constant buffer the caller has set, and a caller sets whatever it
		// has -- writing to a binding the layout does not declare is
		// VUID-VkWriteDescriptorSet-dstBinding-00315, so the writer has to be able to ask.
		std::set<uint32_t> m_constantBindings;

		// Set 0 still has to exist in the pipeline layout when the program has resources but
		// no constants, so Create makes an empty one to occupy it. It is an ordinary device
		// object and has to be destroyed; keeping it only in a local leaked one
		// VkDescriptorSetLayout per such program -- 19 of the 57 objects
		// VUID-vkDestroyDevice-device-05137 reported.
		VkDescriptorSetLayout m_emptyLayout;

		//std::vector<VkDescriptorSetLayout> m_layouts;
		Tr2PrimaryRenderContextAL* m_owner;

		struct RegisterInput
		{
			uint32_t binding;
			uint32_t stage;
			uint32_t registerIndex;

			Tr2ShaderRegisterAL::RegisterType type;
		};
		std::vector<RegisterInput> m_registerInput;
		Tr2RegisterMapAL m_registerMap;

		friend class Tr2RenderContextAL;
		friend class TrinityALImpl::Tr2ResourceSetAL;
	};
}
#endif