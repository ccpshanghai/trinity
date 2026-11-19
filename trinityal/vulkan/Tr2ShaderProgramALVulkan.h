// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include <map>
#include <set>
#include <utility>
#include <vector>

#include "../include/Tr2ShaderProgramAL.h"
#include "../include/Tr2ShaderAL.h"
#include "../include/Tr2RegisterMapAL.h"

class Tr2RenderContextAL;

class Tr2ResourceBindings;

namespace TrinityALImpl
{
	class Tr2RtPipelineStateAL;

	// (set, binding) pairs, as a SPIR-V module decorates its variables.
	typedef std::set<std::pair<uint32_t, uint32_t>> Tr2BindingSetVulkan;

	class Tr2ShaderProgramAL : public Tr2DeviceResourceAL<Tr2ShaderProgramAL>
	{
	public:
		Tr2ShaderProgramAL();
		~Tr2ShaderProgramAL();

		ALResult Create( ::Tr2ShaderAL* shaders, size_t count, Tr2PrimaryRenderContextAL& renderContext );
		// The global input of a ray tracing pipeline (Tr2RtPipelineStateALVulkan), as a program
		// the direct bindings can hold: the engine binds ray tracing globals through the compute
		// slots (Tr2Material::ApplyMaterialDataForRtState), so the signature takes COMPUTE_SHADER
		// binding numbers and register map, visible to every ray tracing stage. Set 0 the
		// constant buffers, set 1 everything else, as Create builds them, except:
		// - a register that appears twice (the reflection reports an explicitly placed cbuffer
		//   twice) is one binding, and fails if the two disagree on the descriptor type;
		// - a binding in `accelerationStructures` takes ACCELERATION_STRUCTURE_KHR whatever its
		//   register type (a RaytracingAccelerationStructure reflects as SRV_BUFFER);
		// - a binding not in `usedBindings` is optional, left unwritten without a word when the
		//   caller set nothing there (a library's global input is a superset of what its modules
		//   read), and a used binding the signature does not declare fails.
		// No shader stages: such a program is never a draw's or a dispatch's.
		ALResult CreateRtGlobalVulkan( const Tr2ShaderSignatureAL& signature,
									   const Tr2BindingSetVulkan& accelerationStructures,
									   const Tr2BindingSetVulkan& usedBindings,
									   Tr2PrimaryRenderContextAL& renderContext );
		void Destroy();
		bool IsValid() const;
		const Tr2RegisterMapAL& GetRegisterMap() const;

		// The compute pipeline of a compute-shader program, created on the first call and kept
		// until Destroy. E_INVALIDCALL for a program without a compute shader.
		ALResult GetComputePipelineVulkan( VkPipeline& pipeline );

		Tr2ALMemoryType GetMemoryClass() const;
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const;
		ALResult SetName( const char* name );

	private:
		std::vector<VkPipelineShaderStageCreateInfo> m_shaderInfo;
		std::vector<::Tr2ShaderAL> m_shaders;
		std::vector<Tr2ShaderPipelineInputAL> m_shaderInputs;

		std::vector<VkDescriptorPoolSize> m_poolSizes;

	private:
		// Set 0 and set 1 layouts from their bindings, and the pipeline layout over them; the
		// common tail of Create and CreateRtGlobalVulkan.
		ALResult CreateLayoutsVulkan( const std::vector<VkDescriptorSetLayoutBinding>& resourceSetBindings,
									  const std::vector<VkDescriptorSetLayoutBinding>& constantBindings,
									  const std::map<VkDescriptorType, uint32_t>& poolCounts,
									  Tr2PrimaryRenderContextAL& renderContext );

		VkPipelineLayout m_pipelineLayout;

		// Built from this program's shader module and pipeline layout and destroyed with them, so
		// it is never found through a layout handle that was freed and reused. One pipeline per
		// render context would be rebuilt at every switch of compute program, and on Adreno each
		// vkCreateComputePipelines is a kernel allocation.
		VkPipeline m_computePipeline;

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
			// Descriptors behind the binding: 1 for a plain register, N for `T name[N]`.
			uint32_t arrayCount;

			Tr2ShaderRegisterAL::RegisterType type;

			// What the layout declared for the binding, which the writer
			// (Tr2ResourceBindingsVulkan) writes, so the two cannot drift: the register type's
			// descriptor type, except a ray tracing global's acceleration structure
			// (CreateRtGlobalVulkan).
			VkDescriptorType descriptorType;
			// Declared but read by no module of the ray tracing pipeline: left unwritten
			// silently when nothing was set, which Vulkan allows for a descriptor no shader
			// statically uses. Always false for a program from Create.
			bool optional;
		};
		std::vector<RegisterInput> m_registerInput;
		Tr2RegisterMapAL m_registerMap;

		// ::-qualified: inside namespace TrinityALImpl an unqualified friend declaration
		// names a new TrinityALImpl::Tr2RenderContextAL instead of the global class --
		// MSVC resolved it to the global anyway, clang does not.
		friend class ::Tr2RenderContextAL;
		friend class ::Tr2ResourceBindings;
		friend class Tr2RtPipelineStateAL;
	};
}
#endif