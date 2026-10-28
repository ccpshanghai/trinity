// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2ShaderALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "UtilitiesVulkan.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>


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
	// read.
	//
	// dxc emits exactly one entry point per module for a pass shader, so the execution-model
	// match is a check rather than a search -- but it is worth making, because what it guards
	// against is binding a fragment entry point to a vertex stage, which validation reports under
	// the same VUID as the bug this function exists to fix.
	std::string FindEntryPointName( const void* bytecode, size_t size, uint32_t executionModel )
	{
		const auto entryPoints = TrinityALImpl::EnumerateSpirvEntryPointsVulkan( bytecode, size );
		for( auto it = begin( entryPoints ); it != end( entryPoints ); ++it )
		{
			if( it->executionModel == executionModel )
			{
				return it->name;
			}
		}
		return entryPoints.empty() ? std::string() : entryPoints.front().name;
	}

	// The module as words, or null if it is not native-endian SPIR-V with a whole header.
	// Native-endian only: a byte-reversed module would need every word swapped, and nothing in
	// this pipeline produces one -- dxc writes the host's order.
	const uint32_t* SpirvWords( const void* bytecode, size_t size, size_t& wordCount )
	{
		const uint32_t k_magic = 0x07230203u;
		const size_t k_headerWords = 5;
		wordCount = 0;
		if( !bytecode || size < ( k_headerWords + 1 ) * sizeof( uint32_t ) || ( size % sizeof( uint32_t ) ) != 0 )
		{
			return nullptr;
		}
		const uint32_t* words = reinterpret_cast<const uint32_t*>( bytecode );
		if( words[0] != k_magic )
		{
			return nullptr;
		}
		wordCount = size / sizeof( uint32_t );
		return words;
	}

	// Calls visit( opcode, operands, operandCount ) for every instruction after the header.
	// Walking the words is enough and pulls in no SPIRV-Tools dependency: the header is five
	// words and every instruction after it starts with ( wordCount << 16 ) | opcode.
	template <typename Visit>
	void ForEachSpirvInstruction( const void* bytecode, size_t size, Visit visit )
	{
		size_t wordCount = 0;
		const uint32_t* words = SpirvWords( bytecode, size, wordCount );
		if( !words )
		{
			return;
		}
		for( size_t i = 5; i < wordCount; )
		{
			const uint32_t instructionWords = words[i] >> 16;
			if( instructionWords == 0 || i + instructionWords > wordCount )
			{
				break; // Malformed. Reporting nothing beats reading past the end.
			}
			visit( words[i] & 0xffffu, &words[i + 1], instructionWords - 1 );
			i += instructionWords;
		}
	}
	}


	namespace TrinityALImpl
	{
	std::vector<SpirvEntryPointVulkan> EnumerateSpirvEntryPointsVulkan( const void* bytecode, size_t size )
	{
		// OpEntryPoint (15): execution model, the entry point's id, then the name as a
		// null-terminated string packed four bytes to a word, then the interface ids.
		const uint32_t k_opEntryPoint = 15u;
		std::vector<SpirvEntryPointVulkan> result;
		ForEachSpirvInstruction( bytecode, size, [&result]( uint32_t opcode, const uint32_t* operands, uint32_t operandCount ) {
			if( opcode == k_opEntryPoint && operandCount >= 3 )
			{
				const char* name = reinterpret_cast<const char*>( &operands[2] );
				const size_t available = ( operandCount - 2 ) * sizeof( uint32_t );
				SpirvEntryPointVulkan entryPoint = { operands[0], std::string( name, strnlen( name, available ) ) };
				result.push_back( entryPoint );
			}
		} );
		return result;
	}

	std::vector<SpirvBindingVulkan> EnumerateSpirvBindingsVulkan( const void* bytecode, size_t size )
	{
		const uint32_t k_opTypeArray = 28u;
		const uint32_t k_opTypeRuntimeArray = 29u;
		const uint32_t k_opTypePointer = 32u;
		const uint32_t k_opVariable = 59u;
		const uint32_t k_opDecorate = 71u;
		const uint32_t k_opTypeAccelerationStructureKHR = 5341u;
		const uint32_t k_decorationBinding = 33u;
		const uint32_t k_decorationDescriptorSet = 34u;

		std::map<uint32_t, uint32_t> bindings, sets; // id -> decoration value
		std::map<uint32_t, uint32_t> pointees, elements; // pointer -> pointee, array -> element
		std::set<uint32_t> accelerationStructureTypes;
		std::vector<std::pair<uint32_t, uint32_t>> variables; // ( id, pointer type )

		ForEachSpirvInstruction( bytecode, size, [&]( uint32_t opcode, const uint32_t* operands, uint32_t operandCount ) {
			switch( opcode )
			{
			case k_opDecorate:
				if( operandCount >= 3 && operands[1] == k_decorationBinding )
				{
					bindings[operands[0]] = operands[2];
				}
				else if( operandCount >= 3 && operands[1] == k_decorationDescriptorSet )
				{
					sets[operands[0]] = operands[2];
				}
				break;
			case k_opTypeAccelerationStructureKHR:
				if( operandCount >= 1 )
				{
					accelerationStructureTypes.insert( operands[0] );
				}
				break;
			case k_opTypeArray:
			case k_opTypeRuntimeArray:
				if( operandCount >= 2 )
				{
					elements[operands[0]] = operands[1];
				}
				break;
			case k_opTypePointer:
				if( operandCount >= 3 )
				{
					pointees[operands[0]] = operands[2];
				}
				break;
			case k_opVariable:
				if( operandCount >= 3 )
				{
					variables.push_back( std::make_pair( operands[1], operands[0] ) );
				}
				break;
			}
		} );

		std::vector<SpirvBindingVulkan> result;
		for( auto it = begin( variables ); it != end( variables ); ++it )
		{
			auto binding = bindings.find( it->first );
			auto set = sets.find( it->first );
			if( binding == bindings.end() || set == sets.end() )
			{
				continue;
			}
			uint32_t type = 0;
			auto pointee = pointees.find( it->second );
			if( pointee != pointees.end() )
			{
				type = pointee->second;
				// Bounded: a valid module's types cannot form a cycle, a corrupt one must not
				// hang the walk.
				size_t depth = 0;
				for( auto element = elements.find( type ); element != elements.end() && depth < 64; element = elements.find( type ), ++depth )
				{
					type = element->second;
				}
			}
			SpirvBindingVulkan entry = { set->second, binding->second, accelerationStructureTypes.count( type ) != 0 };
			result.push_back( entry );
		}
		return result;
	}

	uint32_t SpirvShaderRecordSizeVulkan( const void* bytecode, size_t size )
	{
		const uint32_t k_opTypeInt = 21u;
		const uint32_t k_opTypeFloat = 22u;
		const uint32_t k_opTypeVector = 23u;
		const uint32_t k_opTypeMatrix = 24u;
		const uint32_t k_opTypeArray = 28u;
		const uint32_t k_opTypeStruct = 30u;
		const uint32_t k_opTypePointer = 32u;
		const uint32_t k_opConstant = 43u;
		const uint32_t k_opVariable = 59u;
		const uint32_t k_opDecorate = 71u;
		const uint32_t k_opMemberDecorate = 72u;
		const uint32_t k_decorationRowMajor = 4u;
		const uint32_t k_decorationArrayStride = 6u;
		const uint32_t k_decorationMatrixStride = 7u;
		const uint32_t k_decorationOffset = 35u;
		const uint32_t k_storageClassShaderRecordBuffer = 5343u;

		struct Type
		{
			uint32_t opcode;
			std::vector<uint32_t> operands; // after the result id
		};
		struct MemberLayout
		{
			uint32_t offset = 0;
			uint32_t matrixStride = 0;
			bool rowMajor = false;
		};
		std::map<uint32_t, Type> types;
		std::map<uint32_t, uint32_t> constants, arrayStrides, pointees;
		std::map<std::pair<uint32_t, uint32_t>, MemberLayout> members;
		std::vector<uint32_t> recordPointers; // pointer types of ShaderRecordBufferKHR variables

		ForEachSpirvInstruction( bytecode, size, [&]( uint32_t opcode, const uint32_t* operands, uint32_t operandCount ) {
			switch( opcode )
			{
			case k_opTypeInt:
			case k_opTypeFloat:
			case k_opTypeVector:
			case k_opTypeMatrix:
			case k_opTypeArray:
			case k_opTypeStruct:
				if( operandCount >= 1 )
				{
					types[operands[0]] = Type{ opcode, std::vector<uint32_t>( operands + 1, operands + operandCount ) };
				}
				break;
			case k_opTypePointer:
				if( operandCount >= 3 )
				{
					pointees[operands[0]] = operands[2];
				}
				break;
			case k_opConstant:
				if( operandCount >= 3 )
				{
					constants[operands[1]] = operands[2];
				}
				break;
			case k_opVariable:
				if( operandCount >= 3 && operands[2] == k_storageClassShaderRecordBuffer )
				{
					recordPointers.push_back( operands[0] );
				}
				break;
			case k_opDecorate:
				if( operandCount >= 3 && operands[1] == k_decorationArrayStride )
				{
					arrayStrides[operands[0]] = operands[2];
				}
				break;
			case k_opMemberDecorate:
				if( operandCount >= 3 )
				{
					auto& layout = members[std::make_pair( operands[0], operands[1] )];
					if( operands[2] == k_decorationOffset && operandCount >= 4 )
					{
						layout.offset = operands[3];
					}
					else if( operands[2] == k_decorationMatrixStride && operandCount >= 4 )
					{
						layout.matrixStride = operands[3];
					}
					else if( operands[2] == k_decorationRowMajor )
					{
						layout.rowMajor = true;
					}
				}
				break;
			}
		} );

		// The extent of a type as laid out: where its last byte ends. Bounded depth, as above.
		std::function<uint32_t( uint32_t, const MemberLayout&, size_t )> extent = [&]( uint32_t id, const MemberLayout& layout, size_t depth ) -> uint32_t {
			auto found = types.find( id );
			if( found == types.end() || depth > 64 )
			{
				return 0;
			}
			const Type& type = found->second;
			switch( type.opcode )
			{
			case k_opTypeInt:
			case k_opTypeFloat:
				return type.operands.empty() ? 0 : type.operands[0] / 8;
			case k_opTypeVector:
				return type.operands.size() < 2 ? 0 : type.operands[1] * extent( type.operands[0], MemberLayout(), depth + 1 );
			case k_opTypeMatrix:
			{
				if( type.operands.size() < 2 )
				{
					return 0;
				}
				const uint32_t columns = type.operands[1];
				const uint32_t column = extent( type.operands[0], MemberLayout(), depth + 1 );
				if( !layout.matrixStride || !columns )
				{
					return columns * column;
				}
				if( !layout.rowMajor )
				{
					return layout.matrixStride * ( columns - 1 ) + column;
				}
				// Row major: one stride per row, each row a vector of `columns` components.
				auto columnType = types.find( type.operands[0] );
				const uint32_t rows = columnType != types.end() && columnType->second.operands.size() >= 2 ? columnType->second.operands[1] : 0;
				const uint32_t component = rows ? column / rows : 0;
				return rows ? layout.matrixStride * ( rows - 1 ) + columns * component : 0;
			}
			case k_opTypeArray:
			{
				if( type.operands.size() < 2 )
				{
					return 0;
				}
				auto length = constants.find( type.operands[1] );
				const uint32_t count = length == constants.end() ? 0 : length->second;
				const uint32_t element = extent( type.operands[0], layout, depth + 1 );
				auto stride = arrayStrides.find( id );
				return count ? ( stride == arrayStrides.end() ? element : stride->second ) * ( count - 1 ) + element : 0;
			}
			case k_opTypeStruct:
			{
				uint32_t end = 0;
				for( uint32_t m = 0; m < uint32_t( type.operands.size() ); ++m )
				{
					auto member = members.find( std::make_pair( id, m ) );
					const MemberLayout memberLayout = member == members.end() ? MemberLayout() : member->second;
					end = std::max( end, memberLayout.offset + extent( type.operands[m], memberLayout, depth + 1 ) );
				}
				return end;
			}
			}
			return 0;
		};

		uint32_t result = 0;
		for( auto pointer : recordPointers )
		{
			auto pointee = pointees.find( pointer );
			if( pointee != pointees.end() )
			{
				result = std::max( result, extent( pointee->second, MemberLayout(), 0 ) );
			}
		}
		return result;
	}


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