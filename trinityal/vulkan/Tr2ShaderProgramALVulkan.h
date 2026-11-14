// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include <set>

#include "../include/Tr2ShaderProgramAL.h"
#include "../include/Tr2ShaderAL.h"
#include "../include/Tr2ResourceSetAL.h"

class Tr2RenderContextAL;

namespace TrinityALImpl
{
	// One binding of a set-1 (resource) or set-0 (constant) layout, as the resource set writes
	// it. Shared by Tr2ShaderProgramAL and Tr2RtPipelineStateAL so that one write loop
	// (Tr2ResourceSetAL::WriteSetVulkan) serves both.
	struct Tr2RegisterInputVulkan
	{
		uint32_t binding;
		uint32_t stage;
		uint32_t registerIndex;
		// Descriptors behind the binding: 1 for a plain register, N for `T name[N]`.
		uint32_t arrayCount;

		Tr2ShaderRegisterAL::RegisterType type;

		// What the layout declared. Derived from `type` (GetDescriptorTypeVulkan), except a ray
		// tracing pipeline's acceleration structure, which reflects as SRV_BUFFER.
		VkDescriptorType descriptorType;

		// A binding the layout declares but no shader module of the pipeline reads (a ray
		// tracing library's global input is a superset). Left unwritten when the caller set
		// nothing there, which Vulkan allows for a descriptor that is not statically used.
		bool optional;
	};

	// The descriptor type a register of `registerType` takes in this backend's layouts.
	VkDescriptorType GetDescriptorTypeVulkan( Tr2ShaderRegisterAL::RegisterType registerType );

	// The pipeline layout of a shader program or a ray tracing pipeline -- set 0 the constant
	// buffers, set 1 everything else (Tr2ShaderBindingABIVulkan.h) -- and what writing and
	// binding its sets needs (Tr2ResourceSetAL::WriteSetVulkan, BindConstantBuffers).
	struct Tr2DescriptorLayoutsVulkan
	{
		VkDescriptorSetLayout resourceLayout = VK_NULL_HANDLE; // set 1
		VkDescriptorSetLayout constantLayout = VK_NULL_HANDLE; // set 0
		// Set 0 still has to exist in the pipeline layout when there are resources but no
		// constants; this empty one occupies it.
		VkDescriptorSetLayout emptyLayout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		// resourceLayout is the primary context's (AcquireRtResourceLayoutVulkan), shared with
		// every ray tracing pipeline of the same global input, and not destroyed here.
		bool sharedResourceLayout = false;

		// The binding numbers constantLayout declares. BindConstantBuffers writes one descriptor
		// per constant buffer the caller has set, and a caller sets whatever it has -- writing to
		// a binding the layout does not declare is VUID-VkWriteDescriptorSet-dstBinding-00315, so
		// the writer has to be able to ask.
		std::set<uint32_t> constantBindings;
		std::vector<Tr2RegisterInputVulkan> registerInput;
		// One entry per descriptor type set 1 declares, counting descriptors, not bindings.
		std::vector<VkDescriptorPoolSize> poolSizes;

		// Every handle through DestroyLaterVulkan (the shared resource layout excepted); empty after.
		void Destroy( Tr2PrimaryRenderContextAL& owner );
	};

	// One register of the signature a layout is built from.
	struct Tr2LayoutRegisterVulkan
	{
		const Tr2ShaderRegisterAL* reg; // type, index, arrayCount
		Tr2RenderContextEnum::ShaderType bindingStage; // whose binding numbers it takes (BindingNumber)
		VkShaderStageFlags stageFlags; // the stages that may read it
	};

	typedef std::set<std::pair<uint32_t, uint32_t>> Tr2BindingSetVulkan; // (set, binding)

	// Builds `result` from `registers`, for a shader program and a ray tracing pipeline alike.
	// - One layout binding per (set, binding): a repeat (the reflection reports an explicitly
	//   placed cbuffer twice in a library's global input) is one binding, its stage flags OR-ed,
	//   and fails if the two disagree on the descriptor type.
	// - A binding in `accelerationStructures` takes ACCELERATION_STRUCTURE_KHR, whatever its
	//   register type (an RaytracingAccelerationStructure reflects as SRV_BUFFER).
	// - With `usedBindings`, a binding not in it is optional (left unwritten when the caller set
	//   nothing there), and a used binding no register declares fails; null means every binding
	//   is used (a program's signature is exactly what its modules read).
	// - `sharedResourceLayout` takes set 1 from the primary context's cache, so a ray tracing
	//   pipeline rebuilt over the same global input keeps the layout handle its resource set
	//   was allocated against.
	// On failure nothing is left behind and `result` is empty.
	ALResult BuildDescriptorLayoutsVulkan( const std::vector<Tr2LayoutRegisterVulkan>& registers,
										   const Tr2BindingSetVulkan* accelerationStructures,
										   const Tr2BindingSetVulkan* usedBindings,
										   bool sharedResourceLayout,
										   Tr2PrimaryRenderContextAL& renderContext,
										   Tr2DescriptorLayoutsVulkan& result );

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

		// The set SetPipeline binds when this program declares set-1 resources and the
		// caller bound no resource set: every sampled texture is the primary context's
		// dummy and every sampler its dummy sampler. Built lazily on first need; a
		// program whose unbound slots include anything the dummies cannot stand in for
		// (buffers, UAVs, non-2D textures) gets no default set and behaves as before.
		VkDescriptorSet GetDefaultResourceSetVulkan( Tr2PrimaryRenderContextAL& renderContext );
	private:
		VkDescriptorPool m_defaultPool;
		VkDescriptorSet m_defaultSet;
		bool m_defaultSetTried;

		// Owned, the empty set-0 layout included: keeping that one only in a local leaked one
		// VkDescriptorSetLayout per program with resources and no constants -- 19 of the 57
		// objects VUID-vkDestroyDevice-device-05137 reported.
		Tr2DescriptorLayoutsVulkan m_layouts;

		Tr2PrimaryRenderContextAL* m_owner;
		Tr2RegisterMapAL m_registerMap;

		// ::-qualified: inside namespace TrinityALImpl an unqualified friend declaration
		// names a new TrinityALImpl::Tr2RenderContextAL instead of the global class --
		// MSVC resolved it to the global anyway, clang does not.
		friend class ::Tr2RenderContextAL;
		friend class TrinityALImpl::Tr2ResourceSetAL;
	};
}
#endif