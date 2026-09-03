// Copyright © 2026 CCP ehf.

#pragma once

// The Vulkan binding ABI, in one place.
//
// Four artifacts have to agree on these numbers, in four languages:
//
//   1. Tr2ShaderProgramALVulkan.cpp -- builds VkDescriptorSetLayoutBindings from them.
//   2. trinityal/tests/CMakeLists.txt -- computes dxc's -fvk-{b,t,s,u}-shift arguments
//      from the same arithmetic, so the emitted SPIR-V decorations land on the same
//      numbers.
//   3. trinityal/tests/Shaders.vulkan/*.{vsh,psh,csh} -- declare the register spaces
//      (b in space0, t/s/u in space1) that select the descriptor set.
//   4. shadercompiler/EffectCompilerDX11.cpp -- computes -fvk-{b,t,s,u}-shift from
//      this header so the host-side shader compiler stays on the same contract.
//
// Nothing used to tie them together, and all three of this branch's ABI defects were
// the same failure with no assertion behind it: Phase 1's Tasks 4e and 4f moved every
// SRV/UAV binding as a side effect of a descriptor-*type* fix, and
// SampleTextureMipFromTexCoord.psh's DX9-style `register( ps, c0 )` silently escaped
// -fvk-b-shift entirely. trinityal/tests/ShaderBindingABI.cpp now asserts the contract
// by walking the shipped SPIR-V modules -- and it derives its expectations by calling
// straight into this header, so there is one source of truth for the arithmetic rather
// than a table of expected numbers that can drift away from the code.
//
// This header has no includes and mentions no AL or Vulkan type, so both the AL
// test and the host-side shader compiler can include it. The two flag values
// below mirror Tr2ShaderRegisterAL (trinityal/include/Tr2ShaderAL.h) and
// EffectData.h's RegisterInputType, which EffectData.h declares must stay equal
// to the Trinity enums; ShaderBindingABI.cpp static_asserts the AL side.
namespace Tr2VulkanBindingABI
{
	static const uint32_t REGISTER_SIZE = 32;
	static const uint32_t CLASS_BLOCK = 6 * REGISTER_SIZE;

	// Mirrors Tr2ShaderRegisterAL::RegisterType's shape: CONSTANT_BUFFER=0,
	// SAMPLER=1, SRV_* carry 1<<5, UAV_* carry 1<<6.
	static const uint32_t REGISTER_TYPE_CONSTANT_BUFFER = 0;
	static const uint32_t REGISTER_TYPE_SAMPLER = 1;
	static const uint32_t SRV_REGISTER_FLAG = 1u << 5;
	static const uint32_t UAV_REGISTER_FLAG = 1u << 6;

	enum RegisterClass
	{
		REGISTER_CLASS_CONSTANT_BUFFER = 0,   // b
		REGISTER_CLASS_SRV             = 1,   // t
		REGISTER_CLASS_SAMPLER         = 2,   // s
		REGISTER_CLASS_UAV             = 0    // u
	};

	// Descriptor sets. Constants live alone in set 0 so that the b/u block collision
	// above cannot bite; everything else shares set 1. The shader side spells this as
	// the register space (`space1` on every t/s/u declaration).
	enum DescriptorSet
	{
		DESCRIPTOR_SET_CONSTANTS = 0,
		DESCRIPTOR_SET_RESOURCES = 1
	};

	inline uint32_t RegisterClassOffset( uint32_t registerType )
	{
		if( registerType & UAV_REGISTER_FLAG )
		{
			return REGISTER_CLASS_UAV;
		}
		if( registerType & SRV_REGISTER_FLAG )
		{
			return REGISTER_CLASS_SRV;
		}
		return registerType == REGISTER_TYPE_SAMPLER
			? REGISTER_CLASS_SAMPLER : REGISTER_CLASS_CONSTANT_BUFFER;
	}

	inline uint32_t DescriptorSetIndex( uint32_t registerType )
	{
		return registerType == REGISTER_TYPE_CONSTANT_BUFFER
			? DESCRIPTOR_SET_CONSTANTS : DESCRIPTOR_SET_RESOURCES;
	}

	// binding = registerIndex + classOffset * 6 * 32 + shaderType * 32
	inline uint32_t BindingNumber( uint32_t registerType, uint32_t registerIndex, uint32_t shaderType )
	{
		return registerIndex
			+ RegisterClassOffset( registerType ) * CLASS_BLOCK
			+ shaderType * REGISTER_SIZE;
	}
}
