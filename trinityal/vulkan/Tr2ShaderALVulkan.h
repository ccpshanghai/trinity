// Copyright © 2026 CCP ehf.

#pragma once


#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2ShaderAL.h"


namespace TrinityALImpl
{
	class Tr2ShaderProgramAL;

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
