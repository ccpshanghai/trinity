// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2ShaderALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "UtilitiesVulkan.h"


namespace
{
	// SPIR-V's Execution Model operand, for the stages this AL can build. The numbers are the
	// spec's and frozen by it, so a switch is honest here.
	const uint32_t k_noExecutionModel = 0xffffffffu;

	uint32_t ExecutionModelFor( Tr2RenderContextEnum::ShaderType type )
	{
		switch( type )
		{
		case Tr2RenderContextEnum::VERTEX_SHADER:
			return 0;  // Vertex
		case Tr2RenderContextEnum::HULL_SHADER:
			return 1;  // TessellationControl
		case Tr2RenderContextEnum::DOMAIN_SHADER:
			return 2;  // TessellationEvaluation
		case Tr2RenderContextEnum::GEOMETRY_SHADER:
			return 3;  // Geometry
		case Tr2RenderContextEnum::PIXEL_SHADER:
			return 4;  // Fragment
		case Tr2RenderContextEnum::COMPUTE_SHADER:
			return 5;  // GLCompute
		default:
			return k_noExecutionModel;
		}
	}

	// The name of the module's OpEntryPoint for `executionModel`, or "" if the module cannot be
	// read. Walking the words is enough and pulls in no SPIRV-Tools dependency: the header is
	// five words, every instruction after it starts with ( wordCount << 16 ) | opcode, and
	// OpEntryPoint (opcode 15) carries the execution model, the entry point's id, and then the
	// name as a null-terminated string packed four bytes to a word.
	//
	// dxc emits exactly one entry point per module, so the execution-model match is a check
	// rather than a search -- but it is worth making, because what it guards against is binding
	// a fragment entry point to a vertex stage, which validation reports under the same VUID as
	// the bug this function exists to fix.
	std::string FindEntryPointName( const void* bytecode, size_t size, uint32_t executionModel )
	{
		const uint32_t k_magic = 0x07230203u;
		const uint32_t k_opEntryPoint = 15u;
		const size_t k_headerWords = 5;

		if( !bytecode || size < ( k_headerWords + 1 ) * sizeof( uint32_t )
			|| ( size % sizeof( uint32_t ) ) != 0 )
		{
			return std::string();
		}

		const uint32_t* words = reinterpret_cast<const uint32_t*>( bytecode );
		const size_t wordCount = size / sizeof( uint32_t );

		// Native-endian SPIR-V only. A byte-reversed module would need every word swapped, and
		// nothing in this pipeline produces one -- dxc writes the host's order.
		if( words[0] != k_magic )
		{
			return std::string();
		}

		std::string firstFound;
		for( size_t i = k_headerWords; i < wordCount; )
		{
			const uint32_t instructionWords = words[i] >> 16;
			const uint32_t opcode = words[i] & 0xffffu;
			if( instructionWords == 0 || i + instructionWords > wordCount )
			{
				break;  // Malformed. Reporting nothing beats reading past the end.
			}

			if( opcode == k_opEntryPoint && instructionWords >= 4 )
			{
				const char* name = reinterpret_cast<const char*>( &words[i + 3] );
				const size_t available = ( instructionWords - 3 ) * sizeof( uint32_t );
				const size_t length = strnlen( name, available );
				if( words[i + 1] == executionModel )
				{
					return std::string( name, length );
				}
				if( firstFound.empty() )
				{
					firstFound.assign( name, length );
				}
			}
			i += instructionWords;
		}
		return firstFound;
	}
}


namespace TrinityALImpl
{

	Tr2ShaderAL::Tr2ShaderAL()
		:m_shader( 0 ),
		m_owner( nullptr )
	{
	}

	Tr2ShaderAL::~Tr2ShaderAL()
	{
		Destroy();
	}

	ALResult Tr2ShaderAL::Create(
		Tr2RenderContextEnum::ShaderType type,
		const Tr2ShaderBytecodeAL& bytecode,
		const Tr2ShaderSignatureAL& signature,
		const char* shaderPath,
		Tr2PrimaryRenderContextAL &renderContext )
	{
		Destroy();

		if( !bytecode.size )
		{
			return E_INVALIDARG;
		}
		if( !renderContext.IsValid() )
		{
			return E_INVALIDARG;
		}

		VkShaderModuleCreateInfo shader_module_create_info = {
			VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			nullptr,
			0,
			bytecode.size,
			reinterpret_cast<const uint32_t*>( bytecode.bytecode )
		};

		VkShaderModule shader;
		CR_RETURN_HR( Vk2Al( vkCreateShaderModule( renderContext.m_device, &shader_module_create_info, nullptr, &shader ) ) );

		m_bytecode.resize( "Tr2ShaderAL::m_bytecode", bytecode.size );
		if( m_bytecode.empty() )
		{
			vkDestroyShaderModule( renderContext.m_device, shader, nullptr );
			return E_OUTOFMEMORY;
		}
		m_shader = shader;
		m_owner = &renderContext;
		m_signature = signature;
		m_type = type;
		memcpy( m_bytecode.get(), bytecode.bytecode, bytecode.size );

		// Read from m_bytecode, not from the argument: pName is handed to Vulkan as a bare
		// pointer into this string, so the string has to be as long-lived as the module, and
		// deriving it from the copy we keep is what says so.
		m_entryPoint = FindEntryPointName( m_bytecode.get(), m_bytecode.size(), ExecutionModelFor( type ) );
		if( m_entryPoint.empty() )
		{
			// A module whose entry point could not be read is still worth trying: "main" is
			// what dxc produces under -fspv-entrypoint-name=main and what hand-written SPIR-V
			// almost always uses, so this is the better guess and not a silent one -- the
			// pipeline create will name the mismatch if it is wrong.
			m_entryPoint = "main";
		}

		return S_OK;
	}

	void Tr2ShaderAL::Destroy()
	{
		if( m_shader )
		{
			vkDestroyShaderModule( m_owner->m_device, m_shader, nullptr );
			m_shader = 0;
			m_owner = nullptr;
			m_bytecode.clear();
			m_type = Tr2RenderContextEnum::INVALID_SHADER;
			m_signature = Tr2ShaderSignatureAL();
			m_type = Tr2RenderContextEnum::INVALID_SHADER;
			m_entryPoint.clear();
		}
	}

	bool Tr2ShaderAL::IsValid() const
	{
		return m_shader != 0;
	}

	bool Tr2ShaderAL::operator==( const Tr2ShaderAL& shader ) const
	{
		return this == &shader;
	}

	Tr2RenderContextEnum::ShaderType Tr2ShaderAL::GetType() const 
	{ 
		return m_type;
	}
	
	ALResult Tr2ShaderAL::GetBytecode( Tr2ShaderBytecodeAL& bytecode ) const
	{
		if( !IsValid() )
		{
			bytecode = Tr2ShaderBytecodeAL();
			return E_INVALIDCALL;
		}
		bytecode.bytecode = m_bytecode.get();
		bytecode.size = m_bytecode.size();
		return S_OK;
	}

	const Tr2ShaderSignatureAL& Tr2ShaderAL::GetSignature() const
	{
		return m_signature;
	}

	Tr2ALMemoryType Tr2ShaderAL::GetMemoryClass() const 
	{ 
		return AL_MEMORY_MANAGED; 
	}

	void Tr2ShaderAL::SetNullShaderType( Tr2RenderContextEnum::ShaderType type )
	{
		Destroy();
		m_type = type;
	}

	void Tr2ShaderAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
	{
		description["type"] = "Tr2ShaderAL";
	}

	ALResult Tr2ShaderAL::SetName( const char* )
	{
		return E_NOTIMPL;
	}

}
#endif