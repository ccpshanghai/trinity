// Copyright © 2026 CCP ehf.

#pragma once


#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2ShaderAL.h"


namespace TrinityALImpl
{
	class Tr2ShaderProgramAL;

	// What a SPIR-V module says about itself, read by walking its words (no SPIRV-Tools). A
	// pass shader's module has one entry point and Tr2ShaderAL::Create names its stage from
	// it; a ray tracing library's has one per export, and Tr2RtPipelineStateAL takes each
	// export's stage from its execution model (RayGenerationKHR 5313 ... CallableKHR 5318),
	// because Tr2RtPipelineStateDescriptionAL carries names and no stages.
	struct SpirvEntryPointVulkan
	{
		uint32_t executionModel;
		std::string name;
	};
	std::vector<SpirvEntryPointVulkan> EnumerateSpirvEntryPointsVulkan( const void* bytecode, size_t size );

	// Every variable decorated with both DescriptorSet and Binding, and whether its type (under
	// any arrays) is OpTypeAccelerationStructureKHR. The effect's reflection reports a
	// RaytracingAccelerationStructure as a plain SRV_BUFFER register, which on Vulkan would be a
	// uniform texel buffer; the module is the one place that still knows it is not.
	struct SpirvBindingVulkan
	{
		uint32_t set;
		uint32_t binding;
		bool accelerationStructure;
	};
	std::vector<SpirvBindingVulkan> EnumerateSpirvBindingsVulkan( const void* bytecode, size_t size );

	// The size in bytes of the largest ShaderRecordBufferKHR block the module declares, from its
	// Offset / ArrayStride / MatrixStride decorations; 0 if it declares none (or the walk cannot
	// size it). The shader compiler lets a ray tracing library read one local cbuffer, so this is
	// that cbuffer's record size: what a group's record must hold even when the engine leaves the
	// constant buffer unset.
	uint32_t SpirvShaderRecordSizeVulkan( const void* bytecode, size_t size );

	// -------------------------------------------------------------
// Description:
//   A low level wrapper around shaders / shader programs. DX11
//   specific version will also hold on to byte code for the 
//   shader as this is needed for construction of input layouts.
//   Avoid using this class directly; instead use effects and Tr2Effect.
//   32bit - no support for shader blobs > 4 gig
// -------------------------------------------------------------
	class Tr2ShaderAL :
		public Tr2DeviceResourceAL<Tr2ShaderAL>
	{
	public:
		Tr2ShaderAL();
		~Tr2ShaderAL();

		ALResult Create(
			Tr2RenderContextEnum::ShaderType type,
			const Tr2ShaderBytecodeAL& bytecode,
			const Tr2ShaderSignatureAL& signature,
			const char* shaderPath,
			Tr2PrimaryRenderContextAL &renderContext );
		void Destroy();
		bool IsValid() const;

		bool operator==( const Tr2ShaderAL& shader ) const;

		Tr2RenderContextEnum::ShaderType GetType() const;
		ALResult GetBytecode( Tr2ShaderBytecodeAL& bytecode ) const;
		const Tr2ShaderSignatureAL& GetSignature() const;

		Tr2ALMemoryType GetMemoryClass() const;

		void SetNullShaderType( Tr2RenderContextEnum::ShaderType type );
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const;
		ALResult SetName( const char* name );

	private:
		Tr2ShaderAL( const Tr2ShaderAL& shader );
		Tr2ShaderAL& operator=( const Tr2ShaderAL& shader );

		VkShaderModule m_shader;
		Tr2PrimaryRenderContextAL* m_owner;
		Tr2ShaderSignatureAL m_signature;
		CcpMallocBuffer m_bytecode;
		Tr2RenderContextEnum::ShaderType m_type;

		// The name of this module's OpEntryPoint, read out of the SPIR-V at Create.
		//
		// It is NOT "main". dxc is invoked with -E <the HLSL entry point>
		// (shadercompiler/EffectCompilerDX11.cpp:1518) and -spirv names the SPIR-V entry point
		// after it, so what arrives here is whatever the .fx called its function -- "SpritePS",
		// "_new_symbol_0005" out of the preprocessor, and so on. A pipeline built with pName
		// "main" against such a module fails vkCreateGraphicsPipelines with VK_ERROR_UNKNOWN
		// (VUID-VkPipelineShaderStageCreateInfo-pName-00707), and every draw that wanted it is
		// then a no-op -- a black frame with no other symptom.
		//
		// Read from the module rather than plumbed down from the compiler because the module is
		// what the name has to agree with, and Tr2ShaderAL already holds the bytecode.
		// Tr2ShaderProgramAL points VkPipelineShaderStageCreateInfo::pName at this string, so
		// its lifetime has to be the shader's -- the same assumption pStages::module makes.
		std::string m_entryPoint;

		friend class TrinityALImpl::Tr2ShaderProgramAL;
	};
}

#endif 
