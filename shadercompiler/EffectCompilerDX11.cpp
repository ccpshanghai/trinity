// Copyright © 2023 CCP ehf.

#include "stdafx.h"
#include "EffectCompilerDX11.h"
#include "CompileMessageQueue.h"
#include "StringTable.h"
#include "EffectData.h"

#include "SymbolTable.h"
#include "ASTNode.h"

#include "ParserUtils.h"
#include "HLSLParser.h"
#include "ParserState.h"

#include "FXAnalyzer.h"

#include "TextureFunctionConversionDX11.h"

#include "YamlOutput.h"
#include "Macro.h"
#include "OutputHLSL.h"

#include "DxReflection.h"
#include "trinityal/vulkan/Tr2ShaderBindingABIVulkan.h"
#include <WorkQueue.h>

#define DXIL_FOURCC( ch0, ch1, ch2, ch3 ) (                        \
	(uint32_t)(uint8_t)( ch0 ) | (uint32_t)(uint8_t)( ch1 ) << 8 | \
	(uint32_t)(uint8_t)( ch2 ) << 16 | (uint32_t)(uint8_t)( ch3 ) << 24 )

enum DxilFourCC
{
	DFCC_Container = DXIL_FOURCC( 'D', 'X', 'B', 'C' ), // for back-compat with tools that look for DXBC containers
	DFCC_ResourceDef = DXIL_FOURCC( 'R', 'D', 'E', 'F' ),
	DFCC_InputSignature = DXIL_FOURCC( 'I', 'S', 'G', '1' ),
	DFCC_OutputSignature = DXIL_FOURCC( 'O', 'S', 'G', '1' ),
	DFCC_PatchConstantSignature = DXIL_FOURCC( 'P', 'S', 'G', '1' ),
	DFCC_ShaderStatistics = DXIL_FOURCC( 'S', 'T', 'A', 'T' ),
	DFCC_ShaderDebugInfoDXIL = DXIL_FOURCC( 'I', 'L', 'D', 'B' ),
	DFCC_ShaderDebugName = DXIL_FOURCC( 'I', 'L', 'D', 'N' ),
	DFCC_FeatureInfo = DXIL_FOURCC( 'S', 'F', 'I', '0' ),
	DFCC_PrivateData = DXIL_FOURCC( 'P', 'R', 'I', 'V' ),
	DFCC_RootSignature = DXIL_FOURCC( 'R', 'T', 'S', '0' ),
	DFCC_DXIL = DXIL_FOURCC( 'D', 'X', 'I', 'L' ),
	DFCC_PipelineStateValidation = DXIL_FOURCC( 'P', 'S', 'V', '0' ),
	DFCC_RuntimeData = DXIL_FOURCC( 'R', 'D', 'A', 'T' ),
	DFCC_ShaderHash = DXIL_FOURCC( 'H', 'A', 'S', 'H' ),
};

extern CompileMessageQueue g_messages;
extern StringTable g_stringTable;
extern bool g_printWarnings;
extern unsigned g_optimizationLevel;
extern bool g_avoidFlowControl;
extern bool g_generatePDB;
extern bool g_skipOptimization;

namespace
{
struct VulkanBindingShiftArguments
{
	std::wstring b;
	std::wstring t;
	std::wstring s;
	std::wstring u;
};

const char* VulkanBindingStageName( InputStageType stage )
{
	switch( stage )
	{
	case VERTEX_STAGE:
		return "vertex";
	case PIXEL_STAGE:
		return "pixel";
	case COMPUTE_STAGE:
		return "compute";
	case GEOMETRY_STAGE:
		return "geometry";
	case HULL_STAGE:
		return "hull";
	case DOMAIN_STAGE:
		return "domain";
	default:
		return "unknown";
	}
}

bool TryGetVulkanBindingShiftArguments( VulkanBindingShiftArguments& shifts, InputStageType stage )
{
	using namespace Tr2VulkanBindingABI;

	if( stage != VERTEX_STAGE && stage != PIXEL_STAGE && stage != COMPUTE_STAGE )
	{
		// AddMessage sizes its own buffer, so the 160-byte stack array can go -- and it
		// was the only sprintf_s on this path, which matters because WinAdapter.h maps
		// sprintf_s onto snprintf, whose second parameter is a size, not the format.
		g_messages.AddMessage( "\\memory(0): error X0000: SPIR-V binding ABI does not support the %s shader stage", VulkanBindingStageName( stage ) );
		return false;
	}

	uint32_t stageBase = uint32_t( stage ) * REGISTER_SIZE;
	shifts.b = std::to_wstring( REGISTER_CLASS_CONSTANT_BUFFER * CLASS_BLOCK + stageBase );
	shifts.t = std::to_wstring( REGISTER_CLASS_SRV * CLASS_BLOCK + stageBase );
	shifts.s = std::to_wstring( REGISTER_CLASS_SAMPLER * CLASS_BLOCK + stageBase );
	shifts.u = std::to_wstring( REGISTER_CLASS_UAV * CLASS_BLOCK + stageBase );
	return true;
}

// One DXC compile. Fails on a failed call and on a failed status alike -- IDxcCompiler3 reports
// a source error as S_OK with a failed status -- and forwards DXC's messages either way.
bool CompileWithDxc( IDxcCompiler3* compiler, const DxcBuffer& sourceBuffer, std::vector<LPCWSTR>& arguments, bool reportSpirvBackendMissing, IDxcBlob** objectOutput, IDxcBlob** reflectionOutput )
{
	CComPtr<IDxcResult> pCompileResult;
	HRESULT hrCompilation = compiler->Compile( &sourceBuffer, arguments.data(), UINT32( arguments.size() ), nullptr, IID_PPV_ARGS( &pCompileResult ) );
	HRESULT hrStatus = hrCompilation;
	if( SUCCEEDED( hrCompilation ) && pCompileResult )
	{
		pCompileResult->GetStatus( &hrStatus );
	}

	if( FAILED( hrCompilation ) || FAILED( hrStatus ) )
	{
		if( objectOutput )
		{
			*objectOutput = nullptr;
		}
		if( reflectionOutput )
		{
			*reflectionOutput = nullptr;
		}

		CComPtr<IDxcBlobUtf8> compileErrors;
		if( pCompileResult )
		{
			pCompileResult->GetOutput( DXC_OUT_ERRORS, IID_PPV_ARGS( &compileErrors ), nullptr );
		}
		if( compileErrors && compileErrors->GetStringLength() > 0 )
		{
			g_messages.AddMessages( compileErrors );
			if( reportSpirvBackendMissing && strstr( compileErrors->GetStringPointer(), "spirv" ) )
			{
				g_messages.AddMessage( "\\memory(0): error X0000: dxc rejected -spirv: this dxcompiler.dll was built without the SPIR-V backend (Windows SDK copy?) -- the vcpkg directx-dxc port's DLL is required" );
			}
		}
		return false;
	}

	if( objectOutput )
	{
		CComPtr<IDxcBlob> compiled;
		pCompileResult->GetOutput( DXC_OUT_OBJECT, IID_PPV_ARGS( &compiled ), nullptr );
		*objectOutput = compiled.Detach();
	}
	if( reflectionOutput )
	{
		CComPtr<IDxcBlob> reflectionData;
		pCompileResult->GetOutput( DXC_OUT_REFLECTION, IID_PPV_ARGS( &reflectionData ), nullptr );
		*reflectionOutput = reflectionData.Detach();
	}
	return true;
}

// The register specifiers ForceVulkanRegisterSpaces may rewrite, so a ray tracing library can
// emit its SPIR-V source with the Vulkan spaces and then hand the tree back exactly as the DXIL
// reflection compile and AddGlobalInputs saw it.
class RegisterSpecifierSnapshot
{
public:
	explicit RegisterSpecifierSnapshot( ASTNode* root )
	{
		Collect( root );
	}

	void Restore()
	{
		for( auto& saved : m_saved )
		{
			saved.first->registerSpecifier = saved.second;
		}
	}

private:
	void Collect( ASTNode* node )
	{
		if( !node )
		{
			return;
		}
		switch( node->GetNodeType() )
		{
		case NT_CBUFFER:
		case NT_NAME_DECLARATION:
			if( node->GetSymbol() )
			{
				m_saved.emplace_back( node->GetSymbol(), node->GetSymbol()->registerSpecifier );
			}
			break;
		case NT_PROGRAM:
		case NT_VAR_DECLARATION_LIST:
			for( size_t i = 0; i < node->GetChildrenCount(); ++i )
			{
				Collect( node->GetChild( i ) );
			}
			break;
		default:
			break;
		}
	}

	std::vector<std::pair<Symbol*, std::map<InlineString, RegisterSpecifier>>> m_saved;
};

// A library's local cbuffers: those the globalinput list does not name. The same rule, by the
// same name, as DxReflection's LOCAL_INPUT target, so the cbuffers that become shader records
// are the ones Library::localInputs describes.
void CollectLocalCBuffers( ASTNode* node, const std::vector<GlobalInputElement>& globalInputs, std::set<const Symbol*>& localCBuffers )
{
	if( !node )
	{
		return;
	}
	switch( node->GetNodeType() )
	{
	case NT_CBUFFER:
		// A tbuffer is an NT_CBUFFER too (token OP_TBUFFER), but it reflects as D3D_SIT_TBUFFER,
		// an SRV, which LOCAL_INPUT does not count: it is never a shader record.
		if( node->GetToken() && node->GetToken()->type == OP_TBUFFER )
		{
			break;
		}
		if( auto symbol = node->GetSymbol() )
		{
			auto named = std::find_if( begin( globalInputs ), end( globalInputs ), [symbol]( const GlobalInputElement& element ) { return element.name == symbol->name; } );
			if( named == end( globalInputs ) )
			{
				localCBuffers.insert( symbol );
			}
		}
		break;
	case NT_PROGRAM:
	case NT_VAR_DECLARATION_LIST:
		for( size_t i = 0; i < node->GetChildrenCount(); ++i )
		{
			CollectLocalCBuffers( node->GetChild( i ), globalInputs, localCBuffers );
		}
		break;
	default:
		break;
	}
}

// One scalar of a laid-out block: where it starts and how wide it is. The layout comparison below
// walks both sides into the same order and compares these index by index.
struct ScalarSlot
{
	uint32_t offset;
	uint32_t size;
	bool operator==( const ScalarSlot& other ) const
	{
		return offset == other.offset && size == other.size;
	}
	bool operator!=( const ScalarSlot& other ) const
	{
		return !( *this == other );
	}
};

// What a ray tracing library needs from its SPIR-V module: entry points with their interfaces,
// the ShaderRecordBufferKHR variables, and enough of the type section to lay a record out.
struct SpirvModule
{
	enum Op : uint32_t
	{
		OP_NAME = 5,
		OP_ENTRY_POINT = 15,
		OP_TYPE_BOOL = 20,
		OP_TYPE_INT = 21,
		OP_TYPE_FLOAT = 22,
		OP_TYPE_VECTOR = 23,
		OP_TYPE_MATRIX = 24,
		OP_TYPE_ARRAY = 28,
		OP_TYPE_STRUCT = 30,
		OP_TYPE_POINTER = 32,
		OP_CONSTANT = 43,
		OP_VARIABLE = 59,
		OP_DECORATE = 71,
		OP_MEMBER_DECORATE = 72,
	};
	enum Decoration : uint32_t
	{
		DECORATION_ROW_MAJOR = 4,
		DECORATION_ARRAY_STRIDE = 6,
		DECORATION_MATRIX_STRIDE = 7,
		DECORATION_OFFSET = 35,
	};
	static constexpr uint32_t MAGIC = 0x07230203;
	static constexpr uint32_t STORAGE_CLASS_SHADER_RECORD_BUFFER = 5343;

	struct Type
	{
		uint32_t opcode = 0;
		std::vector<uint32_t> operands; // the words after the result id; OP_CONSTANT: { value }
	};
	struct MemberLayout
	{
		uint32_t offset = 0;
		uint32_t matrixStride = 0;
		bool rowMajor = false;
	};
	struct EntryPoint
	{
		std::string name;
		std::vector<uint32_t> interfaceIds;
	};

	std::map<uint32_t, Type> types;
	std::map<uint32_t, std::string> names;
	std::map<uint32_t, uint32_t> arrayStrides;
	std::map<std::pair<uint32_t, uint32_t>, MemberLayout> members;
	std::map<uint32_t, uint32_t> shaderRecordVariables; // variable id -> pointee type id
	std::vector<EntryPoint> entryPoints;

	static std::string ReadString( const uint32_t* words, size_t wordCount, size_t& consumed )
	{
		std::string result;
		for( consumed = 0; consumed < wordCount; )
		{
			uint32_t word = words[consumed++];
			for( int i = 0; i < 4; ++i )
			{
				char c = char( ( word >> ( 8 * i ) ) & 0xFF );
				if( c == 0 )
				{
					return result;
				}
				result += c;
			}
		}
		return result;
	}

	bool Parse( const void* data, size_t size )
	{
		auto words = static_cast<const uint32_t*>( data );
		size_t wordCount = size / 4;
		if( wordCount < 5 || words[0] != MAGIC )
		{
			return false;
		}
		std::map<uint32_t, std::pair<uint32_t, uint32_t>> pointers; // id -> { storage class, pointee }
		std::vector<std::pair<uint32_t, uint32_t>> variables; // { id, pointer type }
		for( size_t i = 5; i < wordCount; )
		{
			uint32_t opcode = words[i] & 0xFFFF;
			uint32_t length = words[i] >> 16;
			if( length == 0 || i + length > wordCount )
			{
				return false;
			}
			const uint32_t* op = words + i;
			size_t consumed = 0;
			switch( opcode )
			{
			case OP_NAME:
				if( length >= 3 )
				{
					names[op[1]] = ReadString( op + 2, length - 2, consumed );
				}
				break;
			case OP_ENTRY_POINT:
				if( length >= 4 )
				{
					EntryPoint entryPoint;
					entryPoint.name = ReadString( op + 3, length - 3, consumed );
					entryPoint.interfaceIds.assign( op + 3 + consumed, op + length );
					entryPoints.push_back( entryPoint );
				}
				break;
			case OP_TYPE_BOOL:
			case OP_TYPE_INT:
			case OP_TYPE_FLOAT:
			case OP_TYPE_VECTOR:
			case OP_TYPE_MATRIX:
			case OP_TYPE_ARRAY:
			case OP_TYPE_STRUCT:
				if( length >= 2 )
				{
					types[op[1]] = { opcode, std::vector<uint32_t>( op + 2, op + length ) };
				}
				break;
			case OP_CONSTANT:
				if( length >= 4 )
				{
					types[op[2]] = { opcode, { op[3] } };
				}
				break;
			case OP_TYPE_POINTER:
				if( length == 4 )
				{
					pointers[op[1]] = { op[2], op[3] };
				}
				break;
			case OP_VARIABLE:
				if( length >= 4 && op[3] == STORAGE_CLASS_SHADER_RECORD_BUFFER )
				{
					variables.push_back( { op[2], op[1] } );
				}
				break;
			case OP_DECORATE:
				if( length >= 4 && op[2] == DECORATION_ARRAY_STRIDE )
				{
					arrayStrides[op[1]] = op[3];
				}
				break;
			case OP_MEMBER_DECORATE:
				if( length >= 4 )
				{
					auto& member = members[{ op[1], op[2] }];
					if( op[3] == DECORATION_OFFSET && length >= 5 )
					{
						member.offset = op[4];
					}
					else if( op[3] == DECORATION_MATRIX_STRIDE && length >= 5 )
					{
						member.matrixStride = op[4];
					}
					else if( op[3] == DECORATION_ROW_MAJOR )
					{
						member.rowMajor = true;
					}
				}
				break;
			default:
				break;
			}
			i += length;
		}
		for( auto& variable : variables )
		{
			auto pointer = pointers.find( variable.second );
			if( pointer == end( pointers ) )
			{
				return false;
			}
			shaderRecordVariables[variable.first] = pointer->second.second;
		}
		return true;
	}

	std::string NameOf( uint32_t id ) const
	{
		auto found = names.find( id );
		return found == end( names ) ? std::string( "%" ) + std::to_string( id ) : found->second;
	}

	uint32_t ScalarSize( uint32_t typeId ) const
	{
		auto found = types.find( typeId );
		if( found == end( types ) || found->second.opcode == OP_TYPE_BOOL || found->second.operands.empty() )
		{
			return 4;
		}
		return found->second.operands[0] / 8;
	}

	// Every scalar of the type, in declaration order: struct members in order, array elements
	// outer, and a matrix by HLSL row, then HLSL column. DXC turns an HLSL matrix's rows into
	// SPIR-V columns (column_major float4x3 is OpTypeMatrix of 4 vec3, decorated RowMajor), so the
	// SPIR-V column-outer walk below already is HLSL row-outer, whichever the decoration.
	bool Flatten( uint32_t typeId, uint32_t base, const MemberLayout& layout, std::vector<ScalarSlot>& offsets ) const
	{
		auto found = types.find( typeId );
		if( found == end( types ) )
		{
			return false;
		}
		auto& type = found->second;
		switch( type.opcode )
		{
		case OP_TYPE_BOOL:
		case OP_TYPE_INT:
		case OP_TYPE_FLOAT:
			offsets.push_back( { base, ScalarSize( typeId ) } );
			return true;
		case OP_TYPE_VECTOR:
			for( uint32_t i = 0; i < type.operands[1]; ++i )
			{
				offsets.push_back( { base + i * ScalarSize( type.operands[0] ), ScalarSize( type.operands[0] ) } );
			}
			return true;
		case OP_TYPE_MATRIX: {
			auto column = types.find( type.operands[0] );
			if( column == end( types ) || column->second.opcode != OP_TYPE_VECTOR )
			{
				return false;
			}
			uint32_t rows = column->second.operands[1];
			uint32_t scalarSize = ScalarSize( column->second.operands[0] );
			for( uint32_t c = 0; c < type.operands[1]; ++c )
			{
				for( uint32_t r = 0; r < rows; ++r )
				{
					offsets.push_back( { base + ( layout.rowMajor ? r * layout.matrixStride + c * scalarSize : c * layout.matrixStride + r * scalarSize ), scalarSize } );
				}
			}
			return true;
		}
		case OP_TYPE_ARRAY: {
			auto length = types.find( type.operands[1] );
			auto stride = arrayStrides.find( typeId );
			if( length == end( types ) || length->second.opcode != OP_CONSTANT || stride == end( arrayStrides ) )
			{
				return false;
			}
			for( uint32_t i = 0; i < length->second.operands[0]; ++i )
			{
				if( !Flatten( type.operands[0], base + i * stride->second, layout, offsets ) )
				{
					return false;
				}
			}
			return true;
		}
		case OP_TYPE_STRUCT:
			for( uint32_t i = 0; i < type.operands.size(); ++i )
			{
				auto member = members.find( { typeId, i } );
				MemberLayout memberLayout = member == end( members ) ? MemberLayout() : member->second;
				if( !Flatten( type.operands[i], base + memberLayout.offset, memberLayout, offsets ) )
				{
					return false;
				}
			}
			return true;
		default:
			return false;
		}
	}
};

uint32_t AlignTo16( uint32_t size )
{
	return ( size + 15 ) & ~15u;
}

// D3D constant buffer packing, from the DXIL reflection. 64-bit integers are not modelled: a
// cbuffer holding one fails the layout comparison below and is reported, never mis-copied.
uint32_t D3DComponentSize( const D3D12_SHADER_TYPE_DESC& desc )
{
	return desc.Type == D3D_SVT_DOUBLE ? 8 : 4;
}

uint32_t D3DTotalSize( ID3D12ShaderReflectionType* type );

uint32_t D3DElementSize( ID3D12ShaderReflectionType* type )
{
	D3D12_SHADER_TYPE_DESC desc;
	type->GetDesc( &desc );
	uint32_t componentSize = D3DComponentSize( desc );
	switch( desc.Class )
	{
	case D3D_SVC_SCALAR:
	case D3D_SVC_VECTOR:
		return desc.Columns * componentSize;
	case D3D_SVC_MATRIX_COLUMNS:
		return ( desc.Columns - 1 ) * 16 + desc.Rows * componentSize;
	case D3D_SVC_MATRIX_ROWS:
		return ( desc.Rows - 1 ) * 16 + desc.Columns * componentSize;
	case D3D_SVC_STRUCT: {
		uint32_t end = 0;
		for( UINT i = 0; i < desc.Members; ++i )
		{
			auto member = type->GetMemberTypeByIndex( i );
			D3D12_SHADER_TYPE_DESC memberDesc;
			member->GetDesc( &memberDesc );
			end = std::max( end, memberDesc.Offset + D3DTotalSize( member ) );
		}
		return end;
	}
	default:
		return 0;
	}
}

uint32_t D3DTotalSize( ID3D12ShaderReflectionType* type )
{
	D3D12_SHADER_TYPE_DESC desc;
	type->GetDesc( &desc );
	uint32_t elementSize = D3DElementSize( type );
	return desc.Elements ? ( desc.Elements - 1 ) * AlignTo16( elementSize ) + elementSize : elementSize;
}

// Every array element starts a new 16-byte row; so does every struct and every matrix column
// (row, for row_major). Scalars come out in SpirvModule::Flatten's order -- declaration order,
// array elements outer, a matrix by row then column whatever its packing -- so the two lists
// compare index by index.
void D3DFlatten( ID3D12ShaderReflectionType* type, uint32_t base, std::vector<ScalarSlot>& offsets )
{
	D3D12_SHADER_TYPE_DESC desc;
	type->GetDesc( &desc );
	uint32_t componentSize = D3DComponentSize( desc );
	uint32_t stride = AlignTo16( D3DElementSize( type ) );
	for( uint32_t element = 0; element < std::max( 1u, uint32_t( desc.Elements ) ); ++element )
	{
		uint32_t elementBase = base + element * stride;
		switch( desc.Class )
		{
		case D3D_SVC_SCALAR:
		case D3D_SVC_VECTOR:
			for( UINT c = 0; c < desc.Columns; ++c )
			{
				offsets.push_back( { elementBase + c * componentSize, componentSize } );
			}
			break;
		case D3D_SVC_MATRIX_COLUMNS:
			// Stored column by column, visited row by row (see above).
			for( UINT r = 0; r < desc.Rows; ++r )
			{
				for( UINT c = 0; c < desc.Columns; ++c )
				{
					offsets.push_back( { elementBase + c * 16 + r * componentSize, componentSize } );
				}
			}
			break;
		case D3D_SVC_MATRIX_ROWS:
			for( UINT r = 0; r < desc.Rows; ++r )
			{
				for( UINT c = 0; c < desc.Columns; ++c )
				{
					offsets.push_back( { elementBase + r * 16 + c * componentSize, componentSize } );
				}
			}
			break;
		case D3D_SVC_STRUCT:
			for( UINT i = 0; i < desc.Members; ++i )
			{
				auto member = type->GetMemberTypeByIndex( i );
				D3D12_SHADER_TYPE_DESC memberDesc;
				member->GetDesc( &memberDesc );
				D3DFlatten( member, elementBase + memberDesc.Offset, offsets );
			}
			break;
		default:
			break;
		}
	}
}

// The DXIL reflection of the cbuffer of that name in any of the library's functions, or null.
ID3D12ShaderReflectionConstantBuffer* FindLibraryCBuffer( ID3D12LibraryReflection* reflection, const std::string& name )
{
	D3D12_LIBRARY_DESC libraryDesc;
	if( FAILED( reflection->GetDesc( &libraryDesc ) ) )
	{
		return nullptr;
	}
	for( UINT f = 0; f < libraryDesc.FunctionCount; ++f )
	{
		auto function = reflection->GetFunctionByIndex( INT( f ) );
		D3D12_FUNCTION_DESC functionDesc;
		if( !function || FAILED( function->GetDesc( &functionDesc ) ) )
		{
			continue;
		}
		for( UINT c = 0; c < functionDesc.ConstantBuffers; ++c )
		{
			auto cbuffer = function->GetConstantBufferByIndex( c );
			D3D12_SHADER_BUFFER_DESC bufferDesc;
			if( cbuffer && SUCCEEDED( cbuffer->GetDesc( &bufferDesc ) ) && name == bufferDesc.Name )
			{
				return cbuffer;
			}
		}
	}
	return nullptr;
}

// Vulkan's guaranteed minimum for maxShaderGroupStride, and its required shaderGroupHandleSize:
// the largest shader binding table record any conformant device takes is a 32-byte group handle
// followed by 4064 bytes of record data.
constexpr uint32_t VULKAN_SHADER_GROUP_HANDLE_SIZE = 32;
constexpr uint32_t VULKAN_MIN_MAX_SHADER_GROUP_STRIDE = 4096;

// The contract a Vulkan ray tracing library's local cbuffers keep with the AL: the shader record
// after a group's handle is a byte copy of the library's one local constant buffer, as the
// engine filled it in D3D packing. Checked on the compiled module, because that is where
// "referenced by an entry point" and "laid out as" are decided:
//
// - One local cbuffer per library, not per entry point. The engine hands the AL one local
//   signature for the whole library (Library::localInputs, shared by every export the library
//   adds), and the AL lays out one inline record per group from it: the record is the bytes of
//   the one constant buffer that signature names, so Tr2RtPipelineStateAL refuses a signature
//   with two. A library whose entry points each read a different local cbuffer would compile
//   and then switch ray tracing off at run time. (Within one entry point DXC would accept two
//   as well, every ShaderRecordBufferKHR block starting at record offset 0, aliasing the same
//   bytes.)
// - Group handle + cbuffer fits VULKAN_MIN_MAX_SHADER_GROUP_STRIDE.
// - Same layout. DXC lays a shader record out as a storage buffer (std430: an array of floats has
//   a 4-byte stride), not as a constant buffer (16), and no -fvk-use-*-layout switch gives the
//   D3D cbuffer rule to a shader record. So the scalars' offsets are compared, and a cbuffer
//   whose two layouts differ is refused by name.
//
// Messages start with \memory(0), which CompileMessageQueue replaces with the effect's path.
bool CheckShaderRecords( const SpirvModule& module, ID3D12LibraryReflection* reflection, const Library& library, const std::string& libraryName )
{
	bool ok = true;

	// Each record variable any entry point reads, with the entry points that read it, in the
	// order the module lists them.
	std::vector<std::pair<uint32_t, std::vector<std::string>>> records;
	for( auto& entryPoint : module.entryPoints )
	{
		for( auto id : entryPoint.interfaceIds )
		{
			if( !module.shaderRecordVariables.count( id ) )
			{
				continue;
			}
			auto found = std::find_if( begin( records ), end( records ), [id]( const auto& r ) { return r.first == id; } );
			if( found == end( records ) )
			{
				records.push_back( { id, {} } );
				found = end( records ) - 1;
			}
			found->second.push_back( entryPoint.name );
		}
	}
	auto readers = []( const std::vector<std::string>& entryPoints ) {
		std::string list;
		for( auto& name : entryPoints )
		{
			list += ( list.empty() ? "'" : ", '" ) + name + "'";
		}
		return ( entryPoints.size() > 1 ? "entry points " : "entry point " ) + list;
	};

	if( records.size() > 1 )
	{
		std::string list;
		for( auto& record : records )
		{
			list += ( list.empty() ? "'" : ", '" ) + module.NameOf( record.first ) + "' (" + readers( record.second ) + ")";
		}
		g_messages.AddMessage(
			"\\memory(0): error X0000: ray tracing library '%s' reads the local cbuffers %s. On Vulkan a library has one local signature and every group's shader record is the bytes of its one constant buffer, so a library can read at most one cbuffer that is not in its globalinput list.",
			libraryName.c_str(),
			list.c_str() );
		ok = false;
	}
	else
	{
		// The same rule on what the AL is actually handed: the reflection's local input.
		std::set<uint32_t> localCBuffers;
		for( auto& input : library.localInputs.registerInputs )
		{
			if( input.registerType == RT_CONSTANT_BUFFER )
			{
				localCBuffers.insert( input.registerIndex );
			}
		}
		if( localCBuffers.size() > 1 )
		{
			std::string list;
			for( auto reg : localCBuffers )
			{
				list += ( list.empty() ? "b" : ", b" ) + std::to_string( reg );
			}
			g_messages.AddMessage(
				"\\memory(0): error X0000: ray tracing library '%s' has local constant buffers %s. On Vulkan a library has one local signature and every group's shader record is the bytes of its one constant buffer, so a library can read at most one cbuffer that is not in its globalinput list.",
				libraryName.c_str(),
				list.c_str() );
			ok = false;
		}
	}

	for( auto& record : records )
	{
		const uint32_t id = record.first;
		const std::string entryPoints = readers( record.second );
		std::string name = module.NameOf( id );
		auto cbuffer = FindLibraryCBuffer( reflection, name );
		D3D12_SHADER_BUFFER_DESC bufferDesc;
		if( !cbuffer || FAILED( cbuffer->GetDesc( &bufferDesc ) ) )
		{
			g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s', %s: the shader record '%s' has no cbuffer of that name in the DXIL reflection", libraryName.c_str(), entryPoints.c_str(), name.c_str() );
			ok = false;
			continue;
		}
		if( VULKAN_SHADER_GROUP_HANDLE_SIZE + bufferDesc.Size > VULKAN_MIN_MAX_SHADER_GROUP_STRIDE )
		{
			g_messages.AddMessage(
				"\\memory(0): error X0000: ray tracing library '%s', %s: local cbuffer '%s' is %u bytes; after the %u-byte group handle its shader record would be %u bytes, over the %u bytes Vulkan guarantees for maxShaderGroupStride.",
				libraryName.c_str(),
				entryPoints.c_str(),
				name.c_str(),
				bufferDesc.Size,
				VULKAN_SHADER_GROUP_HANDLE_SIZE,
				VULKAN_SHADER_GROUP_HANDLE_SIZE + bufferDesc.Size,
				VULKAN_MIN_MAX_SHADER_GROUP_STRIDE );
			ok = false;
		}
		// Scalar for scalar, in one order on both sides: equal offset sets are not enough, two
		// members swapping places would pass a sorted comparison and copy each into the other.
		std::vector<ScalarSlot> d3dSlots;
		std::vector<std::string> d3dVariables; // the top-level variable each slot belongs to
		for( UINT v = 0; v < bufferDesc.Variables; ++v )
		{
			auto variable = cbuffer->GetVariableByIndex( v );
			D3D12_SHADER_VARIABLE_DESC variableDesc;
			if( variable && SUCCEEDED( variable->GetDesc( &variableDesc ) ) )
			{
				D3DFlatten( variable->GetType(), variableDesc.StartOffset, d3dSlots );
				d3dVariables.resize( d3dSlots.size(), variableDesc.Name ? variableDesc.Name : "?" );
			}
		}
		std::vector<ScalarSlot> recordSlots;
		bool flattened = module.Flatten( module.shaderRecordVariables.at( id ), 0, SpirvModule::MemberLayout(), recordSlots );
		size_t first = 0;
		while( first < d3dSlots.size() && first < recordSlots.size() && d3dSlots[first] == recordSlots[first] )
		{
			++first;
		}
		if( !flattened || first < d3dSlots.size() || first < recordSlots.size() )
		{
			std::string detail;
			if( !flattened )
			{
				detail = "The record's SPIR-V type could not be laid out.";
			}
			else if( first < d3dSlots.size() && first < recordSlots.size() )
			{
				detail = "First difference: scalar " + std::to_string( first ) + " ('" + d3dVariables[first] + "') is at byte " + std::to_string( d3dSlots[first].offset ) + " (" + std::to_string( d3dSlots[first].size ) +
					" bytes) in the constant buffer and at byte " + std::to_string( recordSlots[first].offset ) + " (" + std::to_string( recordSlots[first].size ) + " bytes) in the shader record.";
			}
			else
			{
				detail = "The constant buffer has " + std::to_string( d3dSlots.size() ) + " scalars and the shader record " + std::to_string( recordSlots.size() ) + ".";
			}
			g_messages.AddMessage(
				"\\memory(0): error X0000: ray tracing library '%s', %s: local cbuffer '%s' is laid out differently as a Vulkan shader record (storage buffer rules: arrays of scalars and small vectors are packed) than as a D3D constant buffer, and the record is a byte copy of the constant buffer. Make every array element a 16-byte multiple (float4, uint4, a padded struct). %s",
				libraryName.c_str(),
				entryPoints.c_str(),
				name.c_str(),
				detail.c_str() );
			ok = false;
		}
	}
	return ok;
}
}


static bool FindParameterBySemantics( ASTNode* node, const char** semantics, std::vector<Symbol*>* path, bool outParameter = false )
{
	for( unsigned j = 0; j < node->GetChildrenCount(); ++j )
	{
		Symbol* symbol = node->GetChild( j )->GetSymbol();
		if( symbol == nullptr )
		{
			continue;
		}
		if( outParameter )
		{
			if( node->GetChild( j )->GetToken() == 0 || node->GetChild( j )->GetToken()->type == OP_IN )
			{
				continue;
			}
		}
		for( unsigned k = 0; semantics[k]; ++k )
		{
			if( _stricmp( ToString( symbol->semantic ).c_str(), semantics[k] ) == 0 )
			{
				if( path )
				{
					path->clear();
					path->push_back( symbol );
				}
				return true;
			}
		}
		if( node->GetChild( j )->GetType().symbol && node->GetChild( j )->GetType().symbol->definition )
		{
			for( unsigned i = 0; i < node->GetChild( j )->GetType().symbol->definition->GetChildrenCount(); ++i )
			{
				if( FindParameterBySemantics( node->GetChild( j )->GetType().symbol->definition->GetChild( i ), semantics, path ) )
				{
					if( path )
					{
						path->insert( path->begin(), symbol );
					}
					return true;
				}
			}
		}
	}
	return false;
}

bool FindOutputBySemantics( ASTNode* node, const char** semantics, std::vector<Symbol*>* path )
{
	// check "out" function arguments
	if( FindParameterBySemantics( node, semantics, path, true ) )
	{
		return true;
	}
	// check return value if it has semantics
	if( node->GetSymbol() && node->GetSymbol()->semantic.start )
	{
		std::string sematic = ToString( node->GetSymbol()->semantic );
		for( unsigned i = 0; semantics[i]; ++i )
		{
			if( _stricmp( sematic.c_str(), semantics[i] ) == 0 )
			{
				if( path )
				{
					path->push_back( nullptr );
				}
				return true;
			}
		}
	}
	// finally check return value as a structure
	if( node->GetType().symbol &&
		node->GetType().symbol->definition )
	{
		for( unsigned i = 0; i < node->GetType().symbol->definition->GetChildrenCount(); ++i )
		{
			if( FindParameterBySemantics( node->GetType().symbol->definition->GetChild( i ), semantics, path ) )
			{
				if( path )
				{
					path->insert( path->begin(), nullptr );
				}
				return true;
			}
		}
	}
	return false;
}

void PrintValuePath( std::ostream& os, const std::vector<Symbol*>& path, const char* functionSymbol )
{
	bool first = true;
	for( auto it = path.begin(); it != path.end(); ++it )
	{
		if( first )
		{
			first = false;
		}
		else
		{
			os << ".";
		}
		if( *it )
		{
			os << ( *it )->name;
		}
		else
		{
			os << functionSymbol;
		}
	}
}

static void PatchSemantics( InputStageType shaderStage, ASTNode* callNode )
{
	Symbol* entryPointSymbol = callNode->GetSymbol();
	if( !entryPointSymbol )
	{
		return;
	}
	ASTNode* functionHeader = entryPointSymbol->definition->GetChildOrNull( 0 );
	if( functionHeader == nullptr )
	{
		return;
	}
	std::vector<Symbol*> targetPath;
	switch( shaderStage )
	{
	default:
		break; // only the vertex and pixel stages have semantics to patch
	case VERTEX_STAGE: {
		const char* semantics[] = { "position", nullptr };
		if( FindOutputBySemantics( functionHeader, semantics, &targetPath ) )
		{
			if( targetPath.back() )
			{
				targetPath.back()->semantic = MakeInlineString( "SV_Position" );
			}
			else
			{
				entryPointSymbol->semantic = MakeInlineString( "SV_Position" );
			}
		}
		break;
	}
	case PIXEL_STAGE: {
		{
			const char* semantics[] = { "color", "color0", nullptr };
			if( FindOutputBySemantics( functionHeader, semantics, &targetPath ) )
			{
				if( targetPath.back() )
				{
					targetPath.back()->semantic = MakeInlineString( "SV_Target0" );
				}
				else
				{
					entryPointSymbol->semantic = MakeInlineString( "SV_Target0" );
				}
			}
		}
		{
			const char* semantics[] = { "color1", nullptr };
			if( FindOutputBySemantics( functionHeader, semantics, &targetPath ) )
			{
				if( targetPath.back() )
				{
					targetPath.back()->semantic = MakeInlineString( "SV_Target1" );
				}
				else
				{
					entryPointSymbol->semantic = MakeInlineString( "SV_Target1" );
				}
			}
		}
		break;
	}
	}
}

static PatchAction PatchShader( InputStageType shaderStage, ASTNode* callNode, ParserState& state, CodeStream& os, std::string& entryPointName )
{
	ZoneScoped;

	// 1. wrap uniforms
	// 2. fix VPOS

	bool wrapUniforms = false;
	bool fixVPOS = false;
	bool fixVPOSType = false;

	Symbol* entryPointSymbol = callNode->GetSymbol();

	if( entryPointSymbol == nullptr || entryPointSymbol->definition == nullptr )
	{
		return PATCH_ERROR;
	}

	ASTNode* functionHeader = entryPointSymbol->definition->GetChildOrNull( 0 );
	if( functionHeader == nullptr )
	{
		return PATCH_ERROR;
	}

	std::vector<Symbol*> targetPath;
	std::vector<Symbol*> outPositionPath;
	std::vector<Symbol*> vfacePath;

	if( shaderStage == VERTEX_STAGE )
	{
		const char* position[] = { "position", "sv_position", nullptr };
		FindOutputBySemantics( functionHeader, position, &outPositionPath );
	}


	// check for uniform parameters
	if( callNode->GetChildrenCount() )
	{
		unsigned count = 0;
		for( unsigned i = 0; i < functionHeader->GetChildrenCount(); ++i )
		{
			if( IsUniformInputArgument( functionHeader->GetChild( i ) ) )
			{
				++count;
			}
		}
		if( count != callNode->GetChildrenCount() )
		{
			state.ShowMessage( callNode->GetLocation(), EC_NO_OVERRIDE, ToString( functionHeader->GetToken()->stringValue ).c_str(), "" );
			return PATCH_ERROR;
		}
		wrapUniforms = true;
	}

	// find VPOS+POSITION parameters
	std::vector<Symbol*> positionPath;
	std::vector<Symbol*> vposPath;
	const char* vpos[] = { "vpos", nullptr };
	fixVPOSType = fixVPOS = FindParameterBySemantics( functionHeader, vpos, &vposPath );
	if( fixVPOS )
	{
		const char* position[] = { "position", "sv_position", nullptr };
		bool foundPosition = FindParameterBySemantics( functionHeader, position, &positionPath );
		fixVPOS &= foundPosition;
	}

	if( fixVPOSType )
	{
		if( vposPath.back() )
		{
			if( vposPath.back()->type.symbol ||
				( vposPath.back()->type.builtInType == OP_FLOAT && vposPath.back()->type.width == 4 && vposPath.back()->type.height == 1 ) )
			{
				fixVPOSType = false;
			}
		}
	}

	if( !wrapUniforms && !fixVPOS && !fixVPOSType && ( shaderStage != VERTEX_STAGE || outPositionPath.empty() ) )
	{
		entryPointName = ToString( entryPointSymbol->name );
		return PATCH_USE;
	}

	if( entryPointSymbol->definition->GetChildOrNull( 2 ) )
	{
		os << HLSL{ entryPointSymbol->definition->GetChildOrNull( 2 ), nullptr };
	}
	// return type
	os << functionHeader->GetType();
	// name
	InlineString name = state.AllocateName();
	os << ' ' << name << "( ";
	bool first = true;

	for( size_t i = 0; i < functionHeader->GetChildrenCount(); ++i )
	{
		if( IsUniformInputArgument( functionHeader->GetChild( i ) ) )
		{
			continue;
		}

		Symbol* symbol = functionHeader->GetChild( i )->GetSymbol();
		if( symbol == nullptr )
		{
			return PATCH_ERROR;
		}
		if( fixVPOS && symbol->semantic.start && _stricmp( ToString( symbol->semantic ).c_str(), "vpos" ) == 0 )
		{
			continue;
		}
		if( !first )
		{
			os << ", ";
		}
		else
		{
			first = false;
		}
		if( fixVPOSType && symbol->semantic.start && _stricmp( ToString( symbol->semantic ).c_str(), "vpos" ) == 0 )
		{
			os << "float4 " << functionHeader->GetChild( i )->GetSymbol()->name << ": VPOS";
		}
		else
		{
			os << HLSL{ functionHeader->GetChild( i ), nullptr };
		}
	}
	os << " )";
	// semantics
	if( functionHeader->GetSymbol()->semantic.start )
	{
		os << " : " << functionHeader->GetSymbol()->semantic;
	}
	os << "\n{\n";
	unsigned uniformIndex = 0;
	std::map<size_t, InlineString> defaultUniformArguments;
	for( size_t i = 0; i < functionHeader->GetChildrenCount(); ++i )
	{
		if( IsUniformInputArgument( functionHeader->GetChild( i ) ) )
		{
			if( callNode->GetChildrenCount() <= uniformIndex && functionHeader->GetChild( i )->GetChildOrNull( 1 ) != nullptr )
			{
				InlineString uniformName = state.AllocateName();
				Type type = functionHeader->GetChild( i )->GetType();
				type.storageClass = 0;
				os << type << ' ' << uniformName << " = " << HLSL{ functionHeader->GetChild( i )->GetChildOrNull( 1 ), nullptr };
				os << ";\n";
				defaultUniformArguments[i] = uniformName;
			}
			uniformIndex++;
		}
	}

	if( functionHeader->GetSymbol()->type.symbol || functionHeader->GetSymbol()->type.builtInType != OP_VOID )
	{
		os << functionHeader->GetType() << " __returnValue = ";
	}
	// call original function
	uniformIndex = 0;
	os << entryPointSymbol->name << "(";
	for( size_t i = 0; i < functionHeader->GetChildrenCount(); ++i )
	{
		if( i > 0 )
		{
			os << ", ";
		}
		if( IsUniformInputArgument( functionHeader->GetChild( i ) ) )
		{
			if( callNode->GetChildrenCount() > uniformIndex )
			{
				os << HLSL{ callNode->GetChild( uniformIndex ), nullptr };
			}
			else if( functionHeader->GetChild( i )->GetChildOrNull( 1 ) != nullptr )
			{
				os << defaultUniformArguments[i];
			}
			uniformIndex++;
		}
		else
		{
			Symbol* symbol = functionHeader->GetChild( i )->GetSymbol();
			if( symbol == nullptr )
			{
				return PATCH_ERROR;
			}
			if( fixVPOS && symbol->semantic.start && _stricmp( ToString( symbol->semantic ).c_str(), "vpos" ) == 0 )
			{
				// pass VPOS value from POSITION
				PrintValuePath( os, positionPath, "" );
				if( symbol->type.symbol == nullptr && symbol->type.builtInType == OP_FLOAT && symbol->type.height == 1 &&
					positionPath.back()->type.symbol == nullptr && positionPath.back()->type.builtInType == OP_FLOAT &&
					positionPath.back()->type.height == 1 && positionPath.back()->type.width != symbol->type.width )
				{
					const char* swizzle = "xyzw";
					os << ".";
					for( int k = 0; k < symbol->type.width; ++k )
					{
						os << swizzle[std::min( k, positionPath.back()->type.width - 1 )];
					}
				}
			}
			else if( fixVPOSType && symbol->semantic.start && _stricmp( ToString( symbol->semantic ).c_str(), "vpos" ) == 0 )
			{
				Type type = symbol->type;
				os << type << "( " << symbol->name;
				if( type.width != 4 )
				{
					const char* swizzle = "xyzw";
					os << ".";
					for( int k = 0; k < symbol->type.width; ++k )
					{
						os << swizzle[std::min( k, symbol->type.width - 1 )];
					}
				}
				os << " )";
			}
			else
			{
				os << symbol->name;
			}
		}
	}
	os << ");\n";
	if( functionHeader->GetSymbol()->type.symbol || functionHeader->GetSymbol()->type.builtInType != OP_VOID )
	{
		os << "return __returnValue;\n";
	}
	os << "}\n";

	entryPointName = ToString( name );
	return PATCH_USE;
}

bool EffectCompilerDX11::Create()
{
	ZoneScoped;
	return SUCCEEDED( ::DxcCreateInstance( CLSID_DxcUtils, IID_PPV_ARGS( &m_dxilUtils ) ) );
}

template <typename Reflection, typename ShaderDesc, typename SignatureParamDesc>
bool MatchShaderInputOutputImpl( Reflection* output, Reflection* input )
{
	ShaderDesc vsReflDesc;
	if( FAILED( output->GetDesc( &vsReflDesc ) ) )
	{
		g_messages.AddMessage( "\\memory(0): error X0000: Could not get shader reflection description" );
		return false;
	}
	ShaderDesc psReflDesc;
	if( FAILED( input->GetDesc( &psReflDesc ) ) )
	{
		g_messages.AddMessage( "\\memory(0): error X0000: Could not get shader reflection description" );
		return false;
	}

	for( unsigned k = 0; k < psReflDesc.InputParameters; ++k )
	{
		SignatureParamDesc psDesc;
		if( FAILED( input->GetInputParameterDesc( k, &psDesc ) ) )
		{
			g_messages.AddMessage( "\\memory(0): error X0000: Could not get shader input parameter description" );
			return false;
		}
		if( psDesc.SystemValueType != D3D10_NAME_UNDEFINED )
		{
			continue;
		}
		bool found = false;
		for( unsigned n = 0; n < vsReflDesc.OutputParameters; ++n )
		{
			SignatureParamDesc vsDesc;
			if( FAILED( output->GetOutputParameterDesc( n, &vsDesc ) ) )
			{
				g_messages.AddMessage( "\\memory(0): error X0000: Could not get shader output parameter description" );
				return false;
			}
			if( vsDesc.Register == psDesc.Register && ( ( ~vsDesc.Mask & psDesc.Mask ) == 0 ) )
			{
				if( _stricmp( vsDesc.SemanticName, psDesc.SemanticName ) || vsDesc.SemanticIndex != psDesc.SemanticIndex )
				{
					g_messages.AddMessage( "\\memory(0): error X0000: Vertex/pixel shader signature mismatch" );
					return false;
				}
				found = true;
				break;
			}
		}
		if( !found )
		{
			g_messages.AddMessage( "\\memory(0): error X0000: Vertex/pixel shader signature mismatch" );
			return false;
		}
	}
	return true;
}

#if _WIN32
// d3d11shader.h is the Windows SDK's; dxc's DirectX-Headers carry d3d12shader.h and no
// sibling for DX11. So everything ID3D11* in this file is Windows-only, which costs the
// macOS build nothing: the DX11 and DX12 back ends are Windows-only anyway, and the
// SPIR-V path reflects through ID3D12ShaderReflection.
bool MatchShaderInputOutput( ID3D11ShaderReflection* output, ID3D11ShaderReflection* input )
{
	return MatchShaderInputOutputImpl<ID3D11ShaderReflection, D3D11_SHADER_DESC, D3D11_SIGNATURE_PARAMETER_DESC>( output, input );
}
#endif

bool MatchShaderInputOutput( ID3D12ShaderReflection* output, ID3D12ShaderReflection* input )
{
	return MatchShaderInputOutputImpl<ID3D12ShaderReflection, D3D12_SHADER_DESC, D3D12_SIGNATURE_PARAMETER_DESC>( output, input );
}

std::string PrintPrettyCode( const char* code, const char* indent )
{
	std::stringstream os;
	while( *code )
	{
		const char* end = code;
		while( *end && *end != '\n' )
		{
			++end;
		}
		if( strncmp( code, "#line", 5 ) && code != end )
		{
			os << indent;
			os.write( code, end - code );
			os << std::endl;
		}
		code = end + 1;
		if( *end == 0 )
		{
			break;
		}
	}
	return os.str();
}

template <typename Reflection, typename ShaderDesc>
void PrintShaderOutListingImpl( YamlOutput& listing, ID3DBlob* effectData, Reflection* reflection )
{
	ZoneScoped;

	if( !listing.enabled() )
	{
		return;
	}

#if _WIN32
	if( effectData )
	{
		CComPtr<ID3DBlob> disassembly;
		if( SUCCEEDED( D3DDisassemble( effectData->GetBufferPointer(), effectData->GetBufferSize(), D3D_DISASM_ENABLE_DEFAULT_VALUE_PRINTS, nullptr, &disassembly ) ) )
		{
			listing.literal( "asm" ).literal( reinterpret_cast<const char*>( disassembly->GetBufferPointer() ) );
		}
	}
#else
	// D3DDisassemble is d3dcompiler's. The SPIR-V path passes effectData == nullptr
	// anyway -- there is no DXBC to disassemble -- so the listing loses nothing.
	(void)effectData;
#endif

	ShaderDesc desc;
	if( reflection && SUCCEEDED( reflection->GetDesc( &desc ) ) )
	{
		listing.literal( "stats" ).dict().literal( "Resources" ).dict().literal( "constantBuffers" ).literal( desc.ConstantBuffers ).literal( "boundResources" ).literal( desc.BoundResources ).literal( "inputParameters" ).literal( desc.InputParameters ).literal( "outputParameters" ).literal( desc.OutputParameters ).literal( "tempRegisterCount" ).literal( desc.TempRegisterCount ).literal( "tempArrayCount" ).literal( desc.TempArrayCount ).end().literal( "Instructions" ).dict().literal( "instructionCount" ).literal( desc.InstructionCount ).literal( "defCount" ).literal( desc.DefCount ).literal( "textureNormalInstructions" ).literal( desc.TextureNormalInstructions ).literal( "textureLoadInstructions" ).literal( desc.TextureLoadInstructions ).literal( "textureCompInstructions" ).literal( desc.TextureCompInstructions ).literal( "textureBiasInstructions" ).literal( desc.TextureBiasInstructions ).literal( "textureGradientInstructions" ).literal( desc.TextureGradientInstructions ).literal( "floatInstructionCount" ).literal( desc.FloatInstructionCount ).literal( "intInstructionCount" ).literal( desc.IntInstructionCount ).literal( "uintInstructionCount" ).literal( desc.UintInstructionCount ).literal( "staticFlowControlCount" ).literal( desc.StaticFlowControlCount ).literal( "dynamicFlowControlCount" ).literal( desc.DynamicFlowControlCount ).literal( "macroInstructionCount" ).literal( desc.MacroInstructionCount ).literal( "arrayInstructionCount" ).literal( desc.ArrayInstructionCount ).literal( "cutInstructionCount" ).literal( desc.CutInstructionCount ).literal( "emitInstructionCount" ).literal( desc.EmitInstructionCount ).literal( "cBarrierInstructions" ).literal( desc.cBarrierInstructions ).literal( "cInterlockedInstructions" ).literal( desc.cInterlockedInstructions ).literal( "cTextureStoreInstructions" ).literal( desc.cTextureStoreInstructions ).end().literal( "Misc" ).dict().literal( "GSOutputTopology" ).literal( desc.GSOutputTopology ).literal( "inputPrimitive" ).literal( desc.InputPrimitive ).literal( "GSMaxOutputVertexCount" ).literal( desc.GSMaxOutputVertexCount ).literal( "patchConstantParameters" ).literal( desc.PatchConstantParameters ).literal( "cGSInstanceCount" ).literal( desc.cGSInstanceCount ).literal( "HSOutputPrimitive" ).literal( desc.HSOutputPrimitive ).literal( "HSPartitioning" ).literal( desc.HSPartitioning ).literal( "tessellatorDomain" ).literal( desc.TessellatorDomain ).end().end();
	}
}

#if _WIN32
void PrintShaderOutListing( YamlOutput& listing, ID3DBlob* effectData, ID3D11ShaderReflection* reflection )
{
	PrintShaderOutListingImpl<ID3D11ShaderReflection, D3D11_SHADER_DESC>( listing, effectData, reflection );
}
#endif

void PrintShaderOutListing( YamlOutput& listing, ID3DBlob* effectData, ID3D12ShaderReflection* reflection )
{
	PrintShaderOutListingImpl<ID3D12ShaderReflection, D3D12_SHADER_DESC>( listing, effectData, reflection );
}

void PrintAnnotations( YamlOutput& listing, const std::map<StringReference, Annotation>& annotations )
{
	if( !listing.enabled() )
	{
		return;
	}
	listing.literal( "annotations" ).list();
	for( auto a = annotations.begin(); a != annotations.end(); ++a )
	{
		listing.dict()
			.literal( "name" )
			.literal( g_stringTable.GetString( a->first ) )
			.literal( "annotationType" );
		switch( a->second.type )
		{
		case ANNOTATION_TYPE_BOOL:
			listing
				.literal( "bool" )
				.literal( "value" )
				.literal( a->second.intValue != 0 );
			break;
		case ANNOTATION_TYPE_INT:
			listing
				.literal( "int" )
				.literal( "value" )
				.literal( a->second.intValue );
			break;
		case ANNOTATION_TYPE_FLOAT:
			listing
				.literal( "float" )
				.literal( "value" )
				.literal( a->second.floatValue );
			break;
		default:
			listing
				.literal( "string" )
				.literal( "value" )
				.literal( g_stringTable.GetString( a->second.stringValue ) );
			break;
		}
		listing.end();
	}
	listing.end();
}


YamlOutput& YamlTextureType( YamlOutput& listing, TextureType type )
{
	switch( type )
	{
	case TEX_TYPE_1D:
		listing.literal( "1D texture" );
		return listing;
	case TEX_TYPE_2D:
		listing.literal( "2D texture" );
		return listing;
	case TEX_TYPE_3D:
		listing.literal( "3D texture" );
		return listing;
	case TEX_TYPE_CUBE:
		listing.literal( "CUBE texture" );
		return listing;
	case TEX_TYPE_TYPELESS:
		listing.literal( "typeless texture" );
		return listing;
	case TEX_TYPE_BUFFER:
		listing.literal( "buffer" );
		return listing;
	case TEX_TYPE_STRUCTURED_BUFFER:
		listing.literal( "structured buffer" );
		return listing;
	case TEX_TYPE_TBUFFER:
		listing.literal( "TBuffer" );
		return listing;
	case TEX_TYPE_BYTEADDRESS_BUFFER:
		listing.literal( "byte address buffer" );
		return listing;

	case TEX_TYPE_UAV_RWTYPED:
		listing.literal( "UAV typed" );
		return listing;
	case TEX_TYPE_UAV_RWSTRUCTURED:
		listing.literal( "UAV structured" );
		return listing;
	case TEX_TYPE_UAV_RWBYTEADDRESS:
		listing.literal( "UAV RW byte address" );
		return listing;
	case TEX_TYPE_UAV_APPEND_STRUCTURED:
		listing.literal( "UAV append structured" );
		return listing;
	case TEX_TYPE_UAV_CONSUME_STRUCTURED:
		listing.literal( "UAV consume structured" );
		return listing;
	case TEX_TYPE_UAV_RWSTRUCTURED_WITH_COUNTER:
		listing.literal( "UAV structured with counter" );
		return listing;
	default:
		listing.literal( "other" );
		return listing;
	}
}


void PrintStageInfo( YamlOutput& listing, const StageData& stage, const EffectData& result )
{
	if( !listing.enabled() )
	{
		return;
	}
	if( !stage.constants.empty() )
	{
		listing.literal( "constants" ).list();
		for( auto it = stage.constants.begin(); it != stage.constants.end(); ++it )
		{
			listing.dict()
				.literal( "name" )
				.literal( g_stringTable.GetString( it->name ) )
				.literal( "constantType" )
				.literal( ToString( it->type ) )
				.literal( "dimension" )
				.literal( it->dimension )
				.literal( "arrayElements" )
				.literal( it->elements )
				.literal( "autoregister" )
				.literal( it->isAutoregister )
				.literal( "sRGB" )
				.literal( it->isSRGB );
			auto annotations = result.annotations.find( it->name );
			if( annotations != result.annotations.end() )
			{
				PrintAnnotations( listing, annotations->second.annotations );
			}
			listing.end();
		}
		listing.end();
	}
	if( !stage.samplers.empty() )
	{
		listing.literal( "samplers" ).list();
		for( auto it = stage.samplers.begin(); it != stage.samplers.end(); ++it )
		{
			listing.dict()
				.literal( "register" )
				.literal( it->first )
				.literal( "name" )
				.literal( g_stringTable.GetString( it->second.name ) )
				.literal( "filter" )
				.literal( int( it->second.filter ) )
				.literal( "comparison" )
				.literal( int( it->second.comparison ) )
				.literal( "minFilter" )
				.literal( int( it->second.minFilter ) )
				.literal( "magFilter" )
				.literal( int( it->second.magFilter ) )
				.literal( "mipFilter" )
				.literal( int( it->second.mipFilter ) )
				.literal( "addressU" )
				.literal( int( it->second.addressU ) )
				.literal( "addressV" )
				.literal( int( it->second.addressV ) )
				.literal( "addressW" )
				.literal( int( it->second.addressW ) )
				.literal( "mipLODBias" )
				.literal( it->second.mipLODBias )
				.literal( "maxAnisotropy" )
				.literal( int( it->second.maxAnisotropy ) )
				.literal( "comparisonFunc" )
				.literal( int( it->second.comparisonFunc ) )
				.literal( "borderColor" )
				.list()
				.literal( it->second.borderColor.x )
				.literal( it->second.borderColor.y )
				.literal( it->second.borderColor.z )
				.literal( it->second.borderColor.w )
				.end()
				.literal( "minLOD" )
				.literal( it->second.minLOD )
				.literal( "maxLOD" )
				.literal( it->second.maxLOD )
				.literal( "srgbTexture" )
				.literal( it->second.srgbTexture != 0 )
				.literal( "isDynamic" )
				.literal( it->second.isDynamic != 0 );
			auto annotations = result.annotations.find( it->second.name );
			if( annotations != result.annotations.end() )
			{
				PrintAnnotations( listing, annotations->second.annotations );
			}
			listing.end();
		}
		listing.end();
	}
	if( !stage.textures.empty() )
	{
		listing.literal( "textures" ).list();
		for( auto it = stage.textures.begin(); it != stage.textures.end(); ++it )
		{
			listing.dict()
				.literal( "register" )
				.literal( it->first )
				.literal( "name" )
				.literal( g_stringTable.GetString( it->second.name ) )
				.literal( "textureType" )
				.literal( it->second.type )
				.literal( "autoregister" )
				.literal( it->second.isAutoregister )
				.literal( "sRGB" )
				.literal( it->second.isSRGB );
			auto annotations = result.annotations.find( it->second.name );
			if( annotations != result.annotations.end() )
			{
				PrintAnnotations( listing, annotations->second.annotations );
			}
			listing.end();
		}
		listing.end();
	}
	if( !stage.uavs.empty() )
	{
		listing.literal( "uavs" ).list();
		for( auto it = stage.uavs.begin(); it != stage.uavs.end(); ++it )
		{
			listing.dict()
				.literal( "register" )
				.literal( it->first )
				.literal( "name" )
				.literal( g_stringTable.GetString( it->second.name ) )
				.literal( "resourceType" )
				.literal( it->second.type )
				.literal( "autoregister" )
				.literal( it->second.isAutoregister );
			auto annotations = result.annotations.find( it->second.name );
			if( annotations != result.annotations.end() )
			{
				PrintAnnotations( listing, annotations->second.annotations );
			}
			listing.end();
		}
		listing.end();
	}
	if( !stage.registerInputs.empty() )
	{
		listing.literal( "registers" ).list();
		for( auto it = stage.registerInputs.begin(); it != stage.registerInputs.end(); ++it )
		{
			listing.dict()
				.literal( "registerType" )
				.literal( it->registerType )
				.literal( "register" )
				.literal( it->registerIndex )
				.end();
		}
		listing.end();
	}
}

void PrintStageInfo( YamlOutput& listing, const StageInput& stage, const EffectData& result )
{
	PrintStageInfo( listing, static_cast<const StageData&>( stage ), result );
	if( !stage.pipelineInputs.empty() )
	{
		listing.literal( "inputs" ).list();
		for( auto it = stage.pipelineInputs.begin(); it != stage.pipelineInputs.end(); ++it )
		{
			listing.dict()
				.literal( "register" )
				.literal( it->registerIndex )
				.literal( "name" )
				.literal( it->name )
				.literal( "index" )
				.literal( it->index )
				.literal( "usedMask" )
				.literal( it->usedMask )
				.end();
		}
		listing.end(); // inputs
	}
	if( !stage.staticSamplers.empty() )
	{
		listing.literal( "staticSamplers" ).list();
		for( auto& it : stage.staticSamplers )
		{
			listing.dict()
				.literal( "register" )
				.literal( it.registerIndex )
				.literal( "space" )
				.literal( it.registerSpace )
				.literal( "comparison" )
				.literal( int( it.comparison ) )
				.literal( "minFilter" )
				.literal( int( it.minFilter ) )
				.literal( "magFilter" )
				.literal( int( it.magFilter ) )
				.literal( "mipFilter" )
				.literal( int( it.mipFilter ) )
				.literal( "addressU" )
				.literal( int( it.addressU ) )
				.literal( "addressV" )
				.literal( int( it.addressV ) )
				.literal( "addressW" )
				.literal( int( it.addressW ) )
				.literal( "mipLODBias" )
				.literal( it.mipLODBias )
				.literal( "maxAnisotropy" )
				.literal( int( it.maxAnisotropy ) )
				.literal( "comparisonFunc" )
				.literal( int( it.comparisonFunc ) )
				.literal( "borderColor" )
				.literal( it.borderColor )
				.literal( "minLOD" )
				.literal( it.minLOD )
				.literal( "maxLOD" )
				.literal( it.maxLOD )
				.end();
		}
		listing.end(); //staticSamplers
	}
}

std::string SanitizeCode( const std::string& src )
{
	ZoneScoped;
	std::regex line( "#line[^\\n]*\\n?\\n" );
	return std::regex_replace( src, line, std::string( "" ) );
}


bool ParseShaderName( const InlineString& name, InputStageType& type )
{
	if( _stricmp( ToString( name ).c_str(), "vertexshader" ) == 0 )
	{
		type = VERTEX_STAGE;
	}
	else if( _stricmp( ToString( name ).c_str(), "pixelshader" ) == 0 )
	{
		type = PIXEL_STAGE;
	}
	else if( _stricmp( ToString( name ).c_str(), "computeshader" ) == 0 )
	{
		type = COMPUTE_STAGE;
	}
	else if( _stricmp( ToString( name ).c_str(), "geometryshader" ) == 0 )
	{
		type = GEOMETRY_STAGE;
	}
	else if( _stricmp( ToString( name ).c_str(), "hullshader" ) == 0 )
	{
		type = HULL_STAGE;
	}
	else if( _stricmp( ToString( name ).c_str(), "domainshader" ) == 0 )
	{
		type = DOMAIN_STAGE;
	}
	else
	{
		return false;
	}
	return true;
}

bool EffectCompilerDX11::CompileEffect( const char* source, size_t sourceLength, const std::vector<Macro>& defines, EffectData& result, IWorkQueue* workQueue )
{
	return CompileEffect( source, sourceLength, defines, result, { nullptr, false, false }, workQueue );
}

#if _WIN32
// The D3DCOMPILE_* flags are d3dcompiler's, and only D3DCompile consumes them.
DWORD GetOptimizationLevel()
{
	switch( g_optimizationLevel )
	{
	case 0:
		return D3DCOMPILE_OPTIMIZATION_LEVEL0;
	case 1:
		return D3DCOMPILE_OPTIMIZATION_LEVEL1;
	case 2:
		return D3DCOMPILE_OPTIMIZATION_LEVEL2;
	default:
		return D3DCOMPILE_OPTIMIZATION_LEVEL3;
	}
}
#endif

RegisterInputDescription GetRegisterInputDescription( const Type& type, const RegisterSpecifier& reg )
{
	RegisterInputDescription desc;

	switch( type.builtInType )
	{
	case OP_BUFFER:
	case OP_RAYTRACING_ACCELERATION_STRUCTURE:
		desc.registerType = RT_SRV_BUFFER;
		break;
	case OP_STRUCTUREDBUFFER:
		desc.registerType = RT_SRV_STRUCTURED_BUFFER;
		break;
	case OP_TEXTURE1D:
		desc.registerType = RT_SRV_TEXTURE1D;
		break;
	case OP_TEXTURE1DARRAY:
		desc.registerType = RT_SRV_TEXTURE1DARRAY;
		break;
	case OP_TEXTURE:
	case OP_TEXTURE2D:
		desc.registerType = RT_SRV_TEXTURE2D;
		break;
	case OP_TEXTURE2DARRAY:
		desc.registerType = RT_SRV_TEXTURE2DARRAY;
		break;
	case OP_TEXTURE2DMS:
		desc.registerType = RT_SRV_TEXTURE2DMS;
		break;
	case OP_TEXTURE2DMSARRAY:
		desc.registerType = RT_SRV_TEXTURE2DMSARRAY;
		break;
	case OP_TEXTURE3D:
		desc.registerType = RT_SRV_TEXTURE3D;
		break;
	case OP_TEXTURECUBE:
		desc.registerType = RT_SRV_TEXTURECUBE;
		break;
	case OP_TEXTURECUBEARRAY:
		desc.registerType = RT_SRV_TEXTURECUBEARRAY;
		break;
	case OP_RWBUFFER:
		desc.registerType = RT_UAV_BUFFER;
		break;
	case OP_RWSTRUCTUREDBUFFER:
		desc.registerType = RT_UAV_STRUCTURED_BUFFER;
		break;
	case OP_RWTEXTURE1D:
		desc.registerType = RT_UAV_TEXTURE1D;
		break;
	case OP_RWTEXTURE1DARRAY:
		desc.registerType = RT_UAV_TEXTURE1DARRAY;
		break;
	case OP_RWTEXTURE2D:
		desc.registerType = RT_UAV_TEXTURE2D;
		break;
	case OP_RWTEXTURE2DARRAY:
		desc.registerType = RT_UAV_TEXTURE2DARRAY;
		break;
	case OP_RWTEXTURE3D:
		desc.registerType = RT_UAV_TEXTURE3D;
		break;
	case OP_SAMPLER:
	case OP_SAMPLER1D:
	case OP_SAMPLER2D:
	case OP_SAMPLER3D:
	case OP_SAMPLERCUBE:
	case OP_SAMPLERCOMPARISON:
		desc.registerType = RT_SAMPLER;
		break;
	default:
		desc.registerType = RT_CONSTANT_BUFFER;
		break;
	}
	desc.registerIndex = uint32_t( reg.registerNumber );
	desc.registerSpace = uint8_t( reg.space );
	desc.registerCount = 1;
	for( int i = 0; i < type.arrayDimensions; ++i )
	{
		desc.registerCount *= type.arraySizes[i];
	}
	desc.registerCount = std::max( desc.registerCount, 1u );
	return desc;
}


TextureType TypeToTextureType( const Type& type )
{
	switch( type.builtInType )
	{
	case OP_TEXTURE1D:
	case OP_RWTEXTURE1D:
		return TEX_TYPE_1D;
	case OP_TEXTURE:
	case OP_TEXTURE2D:
	case OP_RWTEXTURE2D:
		return TEX_TYPE_2D;
	case OP_TEXTURE3D:
	case OP_RWTEXTURE3D:
		return TEX_TYPE_3D;
	case OP_TEXTURECUBE:
		return TEX_TYPE_CUBE;
	case OP_TEXTURE2DARRAY:
	case OP_RWTEXTURE2DARRAY:
	case OP_TEXTURE2DMS:
	case OP_TEXTURE2DMSARRAY:
	case OP_TEXTURE3DARRAY:
	case OP_RWTEXTURE3DARRAY:
	case OP_TEXTURECUBEARRAY:
		return TEX_TYPE_TYPELESS;
	case OP_BUFFER:
		return TEX_TYPE_BUFFER;
	case OP_STRUCTUREDBUFFER:
		return TEX_TYPE_STRUCTURED_BUFFER;
	case OP_TBUFFER:
		return TEX_TYPE_TBUFFER;
	case OP_BYTEADDRESSBUFFER:
		return TEX_TYPE_BYTEADDRESS_BUFFER;
	case OP_RWBUFFER:
		return TEX_TYPE_UAV_RWTYPED;
	case OP_RWSTRUCTUREDBUFFER:
		return TEX_TYPE_UAV_RWSTRUCTURED;
	case OP_RWBYTEADDRESSBUFFER:
		return TEX_TYPE_UAV_RWBYTEADDRESS;
	case OP_RAYTRACING_ACCELERATION_STRUCTURE:
		return TEX_TYPE_RAYTRACING_ACCELERATION_STRUCTURE;
	default:
		return TEX_TYPE_TYPELESS;
	}
}


bool AddGlobalInputs( StageData& globalsData, std::map<StringReference, ParameterAnnotation>& annotations, const std::vector<GlobalInputElement>& globalInputs, ParserState& state, bool useStaticSamplers )
{
	auto AddAnnotations = [&annotations]( const Symbol* symbol, bool& srgb, bool& autoregister ) {
		if( symbol && symbol->annotations )
		{
			ParameterAnnotation paramAnnotations;
			for( auto ai = symbol->annotations->begin(); ai != symbol->annotations->end(); ++ai )
			{
				Annotation result;
				if( DxReflection::MakeEffectAnnotationFromSymbolAnnotation( *ai, result, srgb, autoregister ) )
				{
					paramAnnotations.annotations[g_stringTable.AddString( ToString( ai->name ).c_str() )] = result;
				}
			}
			if( !paramAnnotations.annotations.empty() )
			{
				annotations[g_stringTable.AddString( symbol->name )] = paramAnnotations;
			}
		}
	};

	for( auto& gi : globalInputs )
	{
		if( !gi.symbol )
		{
			continue;
		}
		auto symbol = gi.symbol;
		if( !symbol->registerSpecifier.empty() )
		{
			auto& reg = symbol->registerSpecifier.begin()->second;
			auto outReg = GetRegisterInputDescription( symbol->type, reg );
			auto existingReg = find_if( begin( globalsData.registerInputs ), end( globalsData.registerInputs ), [&outReg]( auto& x ) {
				return outReg.registerIndex == x.registerIndex && outReg.registerSpace == x.registerSpace && outReg.registerType == x.registerType;
			} );
			if( existingReg != end( globalsData.registerInputs ) )
			{
				continue;
			}

			switch( reg.registerType )
			{
			case 's': {
				Sampler sampler;
				if( !GetSamplerState( state, symbol->definition, sampler ) )
				{
					return false;
				}
				StaticSampler staticSampler;
				if( symbol->type.arrayDimensions == 0 && useStaticSamplers && ConvertToStaticSampler( staticSampler, sampler ) )
				{
					staticSampler.registerIndex = reg.registerNumber;
					staticSampler.registerSpace = uint8_t( reg.space );
					auto found = find_if( begin( globalsData.staticSamplers ), end( globalsData.staticSamplers ), [&staticSampler]( auto& x ) {
						return staticSampler.registerIndex == x.registerIndex && staticSampler.registerSpace == x.registerSpace;
					} );
					if( found == end( globalsData.staticSamplers ) )
					{
						globalsData.staticSamplers.push_back( staticSampler );
					}
				}
				else
				{
					sampler.name = g_stringTable.AddString( symbol->name );
					globalsData.samplers[uint8_t( reg.registerNumber )] = sampler;
					globalsData.registerInputs.push_back( outReg );
				}
				bool srgb, autoregister;
				AddAnnotations( symbol, srgb, autoregister );
				break;
			}
			case 'u': {
				globalsData.registerInputs.push_back( outReg );

				Uav uav;
				uav.name = g_stringTable.AddString( symbol->name );
				uav.type = TypeToTextureType( symbol->type );
				uav.count = outReg.registerCount;
				uav.isAutoregister = false;

				bool srgb;
				AddAnnotations( symbol, srgb, uav.isAutoregister );
				globalsData.uavs[uint8_t( reg.registerNumber )] = uav;
				break;
			}
			case 't': {
				globalsData.registerInputs.push_back( outReg );

				Texture texture;
				texture.name = g_stringTable.AddString( symbol->name );
				texture.type = TypeToTextureType( symbol->type );
				texture.count = outReg.registerCount;
				texture.isSRGB = false;
				texture.isAutoregister = false;

				AddAnnotations( symbol, texture.isSRGB, texture.isAutoregister );
				globalsData.textures[uint8_t( reg.registerNumber )] = texture;
				break;
			}
			case 'b': {
				globalsData.registerInputs.push_back( outReg );
				break;
			}
			default:
				break;
			}
		}
	}
	return true;
}

// Vulkan: a ray tracing library is one SPIR-V module with an entry point per export. The DXIL
// compile in CompileEffect stays the source of the library's reflection (globalInputs,
// localInputs) exactly as on DX12; this is the module that ships instead of its DXIL.
bool EffectCompilerDX11::CompileLibrarySpirv( ParserState& state, const std::vector<GlobalInputElement>& globalInputs, ID3D12LibraryReflection* reflection, const std::string& libraryName, Library& library, IWorkQueue* workQueue )
{
	ZoneScoped;

	// The same tree, emitted again with two Vulkan differences: t/s/u on the resource
	// descriptor set's space (the pass path's ForceVulkanRegisterSpaces), and every local cbuffer
	// as [[vk::shader_record_ext]]. Only into this source: the DXIL compile is -WX and refuses
	// the attribute as ignored. The register spaces are put back afterwards so that the next
	// library's DXIL compile sees the tree the DX12 path would.
	std::string code;
	{
		RegisterSpecifierSnapshot snapshot( state.GetTree() );
		VulkanRegisterSpaceReject reject;
		if( !ForceVulkanRegisterSpaces( state.GetTree(), reject ) )
		{
			snapshot.Restore();
			g_messages.AddMessage(
				"\\memory(0): error X0000: '%.*s' was assigned register space %d, which the SPIR-V binding ABI cannot place: it has one descriptor set for constant buffers and one for every '%c' register. A ray tracing library may declare at most one resource array per register class in its globalinput, because each array declaration takes a register space of its own.",
				int( reject.name.end - reject.name.start ),
				reject.name.start,
				reject.space,
				reject.registerType );
			return false;
		}
		std::set<const Symbol*> localCBuffers;
		CollectLocalCBuffers( state.GetTree(), globalInputs, localCBuffers );

		// The DXIL emission just consumed the pragmas; this source should carry them as well.
		state.ResetPragmaUsage();
		CompilerInputStream os( state, ShadingLanguage::HLSL );
		os << HLSL{ state.GetTree(), &state.GetSymbolTable(), &localCBuffers };
		code = os.str();
		snapshot.Restore();
	}
	library.source = code;

	// The cache key is the source this compile reads, marked so it can never meet a pass's or a
	// DXIL library's entry for the same text.
	std::string cacheKey = "spirv library\n" + code;
	bool needsToCompile = false;
	std::shared_ptr<SyncData> syncData;
	{
		std::lock_guard scope( m_compiledCS );
		auto found = m_compiled.find( cacheKey );
		if( found == end( m_compiled ) )
		{
			syncData = std::make_shared<SyncData>();
			m_compiled[cacheKey] = syncData;
			needsToCompile = true;
		}
		else
		{
			syncData = found->second;
		}
	}

	if( !needsToCompile )
	{
		if( workQueue )
		{
			workQueue->OnBlocked();
		}
		{
			std::unique_lock<std::mutex> lock( syncData->mutex );
			syncData->conditionVariable.wait( lock, [&syncData] { return syncData->compiled; } );
		}
		if( workQueue )
		{
			workQueue->OnUnblocked();
		}
	}
	else
	{
		ZoneScopedN( "DxcCompileSpirvLibrary" );

		CComPtr<IDxcBlobEncoding> src;
		CComPtr<IDxcCompiler3> compiler;
		VulkanBindingShiftArguments shiftValues;
		// The engine binds a ray tracing library's global inputs through the COMPUTE_SHADER
		// slots (Tr2Material::ApplyMaterialDataForRtState, DX12's compute root signature), so its
		// bindings are numbered as a compute shader's. AssignRegisters' pseudo-stage 8 is not a
		// stage the ABI knows.
		HRESULT hrBlob = m_dxilUtils->CreateBlobFromPinned( code.c_str(), UINT32( code.size() ), CP_UTF8, &src );
		HRESULT hrCompiler = SUCCEEDED( hrBlob ) ? DxcCreateInstance( CLSID_DxcCompiler, IID_PPV_ARGS( &compiler ) ) : E_FAIL;
		if( FAILED( hrBlob ) )
		{
			g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s': IDxcUtils::CreateBlobFromPinned failed (0x%08x)", libraryName.c_str(), unsigned( hrBlob ) );
		}
		else if( FAILED( hrCompiler ) )
		{
			g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s': DxcCreateInstance(CLSID_DxcCompiler) failed (0x%08x)", libraryName.c_str(), unsigned( hrCompiler ) );
		}
		else if( !TryGetVulkanBindingShiftArguments( shiftValues, COMPUTE_STAGE ) )
		{
			g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s': no SPIR-V binding shifts for its global inputs", libraryName.c_str() );
		}
		else
		{
			DxcBuffer sourceBuffer;
			sourceBuffer.Ptr = src->GetBufferPointer();
			sourceBuffer.Size = src->GetBufferSize();
			sourceBuffer.Encoding = 0;

			std::vector<LPCWSTR> arguments = {
				L"-T",
				L"lib_6_3",
				L"-spirv",
				L"-fspv-target-env=vulkan1.3",
				L"-Zpc",
				// No -HV: DXC's default grammar (HLSL 2021), the one the DXIL lib_6_3 compile of
				// this library uses, here for its reflection and on DX12 for the bytes that ship.
				// A library has only ever been compiled by DXC, so it needs none of the FXC-era
				// 2018 grammar the pass path keeps, and both compiles of one source then read it
				// alike (2021 short-circuits scalar && || ?:, as DX12 always has).
				// An allow-list, and the whole of it. Without one DXC declares the RayQueryKHR
				// capability in every module that names a RaytracingAccelerationStructure, used
				// for TraceRay or not -- and a device with ray_query but no ray tracing pipeline
				// (the S23) would then be asked for a capability the module does not use. With
				// one, an extension the source really needs and this list lacks is a DXC error
				// naming it, not a silent difference.
				L"-fspv-extension=SPV_KHR_ray_tracing",
				L"-fvk-b-shift",
				shiftValues.b.c_str(),
				L"0",
				L"-fvk-t-shift",
				shiftValues.t.c_str(),
				L"1",
				L"-fvk-s-shift",
				shiftValues.s.c_str(),
				L"1",
				L"-fvk-u-shift",
				shiftValues.u.c_str(),
				L"1",
			};
			if( g_skipOptimization )
			{
				arguments.push_back( DXC_ARG_SKIP_OPTIMIZATIONS );
			}
			CompileWithDxc( compiler, sourceBuffer, arguments, true, &syncData->librarySpirv, nullptr );
		}

		{
			std::lock_guard scope( syncData->mutex );
			syncData->compiled = true;
		}
		syncData->conditionVariable.notify_all();
	}

	// A raw pointer, as CompileEffect takes its cache entries': the entry keeps the blob alive.
	IDxcBlob* spirv = syncData->librarySpirv;
	{
		std::lock_guard scope( m_compiledCS );
		syncData.reset();
	}
	if( !spirv )
	{
		return false;
	}

	SpirvModule module;
	if( !module.Parse( spirv->GetBufferPointer(), spirv->GetBufferSize() ) )
	{
		g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s': DXC's output is not a SPIR-V module this compiler can read", libraryName.c_str() );
		return false;
	}
	if( !CheckShaderRecords( module, reflection, library, libraryName ) )
	{
		return false;
	}

	library.shaderDataStr = g_stringTable.AddString( spirv->GetBufferPointer(), spirv->GetBufferSize() );
	library.shaderSize = uint32_t( spirv->GetBufferSize() );
	return true;
}

bool EffectCompilerDX11::CompileEffect( const char* source, size_t sourceLength, const std::vector<Macro>& defines, EffectData& result, const CompileOptions& compileOptions, IWorkQueue* workQueue )
{
	ZoneScoped;

	ParserState state( MakeInlineString( source, source + sourceLength ) );
	for( auto it = begin( defines ); it != end( defines ); ++it )
	{
		PreprocessorDefine d;
		d.location.fileName = MakeInlineString( "" );
		d.location.lineNumber = 0;
		d.value = MakeInlineString( it->value.c_str() );
		state.m_defines[MakeInlineString( it->name.c_str() )] = d;
	}

	if( !state.Parse() )
	{
		return false;
	}

	PatchCBuffers( state );
	TransferSRGBToTexturesDX11( state );
	ConvertTextureFunctionsDX11( state );
	MergeSamplers( state );

	std::vector<ASTNode*> techniqueNodes;
	state.GetTree()->FindNodes( NT_TECHNIQUE, techniqueNodes );
	if( techniqueNodes.empty() )
	{
		g_messages.AddMessage( "\\memory(0): error X0000: No technique or libraries found" );
		return false;
	}

#if !_WIN32
	// This file is compiled on macOS for its SPIR-V branch alone (EffectCompilerVulkan).
	// D3DCompile, D3DStripShader, D3DReflect and D3DDisassemble are d3dcompiler.dll's and
	// have no macOS equivalent, so a DX11 or DX12 request here has to say so rather than
	// walk into a half-guarded body and produce an empty effect.
	if( !compileOptions.spirv )
	{
		g_messages.AddMessage( "\\memory(0): error X0000: DX11/DX12 compilation needs Windows" );
		return false;
	}
#endif

	YamlListing listing;
	listing.dict();
	listing.literal( "permutation" ).dict().literal( "platform" ).literal( "DX11" ).literal( "id" ).literal( "000" ).literal( "defines" ).dict();
	for( auto it = begin( defines ); it != end( defines ); ++it )
	{
		listing.literal( it->name ).literal( it->value );
	}
	listing.end();
	listing.literal( "techniques" ).list();

	for( auto techniqueIt = techniqueNodes.begin(); techniqueIt != techniqueNodes.end(); ++techniqueIt )
	{
		auto techniqueNode = *techniqueIt;
		Technique technique;
		technique.name = g_stringTable.AddString( ToString( techniqueNode->GetToken()->stringValue ).c_str() );

		listing.dict()
			.literal( "name" )
			.literal( techniqueNode->GetToken()->stringValue )
			.literal( "passes" )
			.list();

		for( size_t passIx = 0; passIx < techniqueNode->GetChildrenCount(); ++passIx )
		{
			ASTNode* passNode = techniqueNode->GetChild( passIx );
			if( passNode->GetNodeType() != NT_PASS )
			{
				continue;
			}
			listing.list();
			Pass outPass;
#if _WIN32
			CComPtr<ID3D11ShaderReflection> reflections[6];
#endif
			CComPtr<ID3D12ShaderReflection> reflectionsDx12[6];
			for( size_t stateIx = 0; stateIx < passNode->GetChildrenCount(); ++stateIx )
			{
				if( passNode->GetChild( stateIx )->GetNodeType() == NT_STATE_ASSIGNMENT )
				{
					DWORD stateCode = 0;
					DWORD value = 0;
					if( ParseStateAssignment( state, passNode->GetChild( stateIx ), g_renderStates, &value ) )
					{
						std::string name = ToString( passNode->GetChild( stateIx )->GetToken()->stringValue );
						for( int i = 0; g_renderStateNames[i].name; ++i )
						{
							if( _stricmp( name.c_str(), g_renderStateNames[i].name ) == 0 )
							{
								stateCode = g_renderStateNames[i].value;
							}
						}
						switch( stateCode )
						{
						// g_renderStateNames stores these as `unsigned`, so the labels are
						// 0xFFFFFFFF down to 0xFFFFFFFA. Spelled as a conversion because a
						// negative label on an unsigned switch is a narrowing error for
						// clang, where MSVC converts it silently -- same values either way.
						case DWORD( -1 ):
						case DWORD( -2 ):
						case DWORD( -3 ):
						case DWORD( -4 ):
						case DWORD( -5 ):
						case DWORD( -6 ):
							if( value != 0 )
							{
								state.ShowMessage( passNode->GetChild( stateIx )->GetLocation(), EC_INVALID_STATE_VALUE, name.c_str() );
							}
							break;
						case 0:
							state.ShowMessage( passNode->GetChild( stateIx )->GetLocation(), EC_STATE_DEPRECATED, name.c_str() );
							break;
						default:
							outPass.states[stateCode] = value;
						}
					}
					continue;
				}

				if( passNode->GetChild( stateIx )->GetNodeType() != NT_SHADER_ASSIGNMENT )
				{
					continue;
				}

				ASTNode* shaderNode = passNode->GetChild( stateIx );

				StageInput stage;
				if( !ParseShaderName( shaderNode->GetToken()->stringValue, stage.type ) )
				{
					state.ShowMessage( shaderNode->GetToken()->fileLocation, EC_INVALID_STATE, ToString( shaderNode->GetToken()->stringValue ).c_str() );
					return false;
				}

				std::string profile = ToString( shaderNode->GetChild( 0 )->GetToken()->stringValue );
				if( compileOptions.minShaderVersion )
				{
					profile = profile.substr( 0, 3 ) + compileOptions.minShaderVersion;
				}
				else
				{
					if( profile[0] == 'v' )
					{
						profile = "vs_5_0";
					}
					else if( profile[0] == 'p' )
					{
						profile = "ps_5_0";
					}
				}
				ID3D10Blob* effectData = nullptr;
				IDxcBlob* spirvEffectData = nullptr;
				IDxcBlob* spirvReflectionData = nullptr;
#if _WIN32
				CComPtr<ID3D10Blob> errors = nullptr;
#endif


				if( shaderNode->GetChild( 1 )->GetSymbol() == nullptr )
				{
					return false;
				}

				if( compileOptions.minShaderVersion )
				{
					PatchSemantics( stage.type, shaderNode->GetChild( 1 ) );
				}

				state.GetSymbolTable().ResetUsedFlag();
				MarkUsedSymbols( shaderNode->GetChild( 1 ), state );

				if( compileOptions.addSpaces )
				{
					CreateGlobalsCB( state );
					AssignRegisters( state.GetTree(), stage.type );
					if( compileOptions.spirv )
					{
						VulkanRegisterSpaceReject reject;
						if( !ForceVulkanRegisterSpaces( state.GetTree(), reject ) )
						{
							// AddMessage sizes its own buffer, so the symbol name needs no cap here.
							g_messages.AddMessage(
								"\\memory(0): error X0000: '%.*s' was assigned register space %d, which the SPIR-V binding ABI cannot place: it has one descriptor set for constant buffers and one for every '%c' register. A stage may declare at most one resource array per register class, because each array declaration takes a register space of its own.",
								int( reject.name.end - reject.name.start ), reject.name.start, reject.space, reject.registerType );
							return false;
						}
					}
				}

				CompilerInputStream os( state, ShadingLanguage::HLSL );
				os << HLSL{ state.GetTree(), &state.GetSymbolTable() };

				std::string entryPoint = ToString( shaderNode->GetChild( 1 )->GetSymbol()->name );

				std::string patchEntryPoint = entryPoint;
				switch( PatchShader( stage.type, shaderNode->GetChild( 1 ), state, os, patchEntryPoint ) )
				{
				case PATCH_ERROR:
					return false;
				default:
					break; // PATCH_SKIP and PATCH_USE both continue
				}
				state.ResetPragmaUsage();
				std::string code = os.str();


				// Let only one thread compile this permutation.
				bool needsToCompile = false;
				std::shared_ptr<SyncData> syncData;
				{
					std::lock_guard scope( m_compiledCS );
					auto found = m_compiled.find( code );
					if( found == end( m_compiled ) )
					{
						// Yep, this thread will have to compile. Make an entry into the cache.
						syncData = std::make_shared<SyncData>();
						m_compiled[code] = syncData;
						needsToCompile = true;
					}
					else
					{
						// Some other thread is going to compile for us. Grab his stuff from the cache.
						syncData = found->second;
					}
				}

				if( !needsToCompile )
				{
					// Let's wait for the other thread to compile for us
					if( workQueue )
					{
						workQueue->OnBlocked();
					}
					{
						std::unique_lock<std::mutex> lock( syncData->mutex );
						syncData->conditionVariable.wait( lock, [&syncData] { return syncData->compiled; } );
					}
					if( workQueue )
					{
						workQueue->OnUnblocked();
					}
				}
				else
				{
					// We need to compile, nobody else is doing it for this permutation.
					if( compileOptions.spirv )
					{
						ZoneScopedN( "DxcCompileSpirv" );

						CComPtr<IDxcBlobEncoding> src;
						HRESULT hrBlob = m_dxilUtils->CreateBlobFromPinned( code.c_str(), UINT32( code.size() ), CP_UTF8, &src );
						if( FAILED( hrBlob ) )
						{
							g_messages.AddMessage( "\\memory(0): error X0000: entry point '%s': IDxcUtils::CreateBlobFromPinned failed (0x%08x)", patchEntryPoint.c_str(), unsigned( hrBlob ) );
							syncData->passSpirv = nullptr;
							syncData->passSpirvReflection = nullptr;
						}
						else
						{
							DxcBuffer sourceBuffer;
							sourceBuffer.Ptr = src->GetBufferPointer();
							sourceBuffer.Size = src->GetBufferSize();
							sourceBuffer.Encoding = 0;

							std::wstring wideProfile( profile.begin(), profile.end() );
							std::wstring wideEntry( patchEntryPoint.begin(), patchEntryPoint.end() );
							std::vector<LPCWSTR> spirvArguments = {
								L"-T", wideProfile.c_str(),
								L"-E", wideEntry.c_str(),
								L"-spirv",
								L"-fspv-target-env=vulkan1.3",
								L"-Zpc",
								// The language the effects were written in. DXC defaults to HLSL 2021,
								// where `||` and `&&` on vectors are errors ("operands for short-circuiting
								// logical binary operator must be scalar"); FXC-era EVE effects use them --
								// VolumetricBooster.fxh's `any( ( coord < 0 ) || ( coord > 1 ) )` kept
								// boostervolumetric out of the S23's Vulkan closure on 2026-09-10. 2018 is
								// the last version that reads them as the element-wise operators they were.
								// The DXIL arguments below are left at the default until Windows can measure
								// the change; the two back ends read the same source under two grammars
								// for now, and the ledger says so.
								L"-HV", L"2018",
							};
							VulkanBindingShiftArguments shiftValues;
							if( !TryGetVulkanBindingShiftArguments( shiftValues, stage.type ) )
							{
								syncData->passSpirv = nullptr;
								syncData->passSpirvReflection = nullptr;
								std::lock_guard scope( syncData->mutex );
								syncData->compiled = true;
								syncData->conditionVariable.notify_all();
								return false;
							}
							const LPCWSTR shiftArguments[] = {
								L"-fvk-b-shift", shiftValues.b.c_str(), L"0",
								L"-fvk-t-shift", shiftValues.t.c_str(), L"1",
								L"-fvk-s-shift", shiftValues.s.c_str(), L"1",
								L"-fvk-u-shift", shiftValues.u.c_str(), L"1",
							};
							spirvArguments.insert( end( spirvArguments ), std::begin( shiftArguments ), std::end( shiftArguments ) );
							if( g_skipOptimization )
							{
								spirvArguments.push_back( DXC_ARG_SKIP_OPTIMIZATIONS );
							}

							// The reflection compile of the same source, on the same path: it has to
							// parse under the same grammar as the module above, or an effect the
							// SPIR-V compile accepted fails its reflection pass one line later.
							std::vector<LPCWSTR> dxilArguments = {
								L"-T", wideProfile.c_str(),
								L"-E", wideEntry.c_str(),
								L"-Zpc",
								L"-HV", L"2018",
							};
							if( g_skipOptimization )
							{
								dxilArguments.push_back( DXC_ARG_SKIP_OPTIMIZATIONS );
							}

							CComPtr<IDxcCompiler3> compiler;
							HRESULT hrCompiler = DxcCreateInstance( CLSID_DxcCompiler, IID_PPV_ARGS( &compiler ) );
							if( FAILED( hrCompiler ) )
							{
								g_messages.AddMessage( "\\memory(0): error X0000: entry point '%s': DxcCreateInstance(CLSID_DxcCompiler) failed (0x%08x)", patchEntryPoint.c_str(), unsigned( hrCompiler ) );
								syncData->passSpirv = nullptr;
								syncData->passSpirvReflection = nullptr;
							}
							else
							{
								CompileWithDxc( compiler, sourceBuffer, spirvArguments, true, &syncData->passSpirv, nullptr );
								if( syncData->passSpirv )
								{
									CompileWithDxc( compiler, sourceBuffer, dxilArguments, false, nullptr, &syncData->passSpirvReflection );
								}
							}
						}
					}
#if _WIN32
					else
					{
						CComPtr<ID3D10Blob> compiledEffectData;
						HRESULT hr;
						{
							ZoneScopedN( "D3DCompile" );
							hr = D3DCompile(
								code.c_str(),
								code.length(),
								"\\memory",
								nullptr,
								nullptr,
								patchEntryPoint.c_str(),
								profile.c_str(),
								( compileOptions.minShaderVersion ? D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES : D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY ) | ( g_generatePDB ? ( D3DCOMPILE_DEBUG | D3DCOMPILE_DEBUG_NAME_FOR_SOURCE ) : 0 ) | ( g_skipOptimization ? D3DCOMPILE_SKIP_OPTIMIZATION : GetOptimizationLevel() ) | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | ( g_avoidFlowControl ? D3DCOMPILE_AVOID_FLOW_CONTROL : 0 ),
								0,
								&compiledEffectData,
								&errors );
						}

						if( FAILED( hr ) )
						{
							// We failed compilation, let's print errors.
							syncData->passResource = nullptr;
							if( errors )
							{
								g_messages.AddMessages( errors );
							}
						}
						else
						{
							if( g_generatePDB )
							{
								// Get debug info and it's name.
								CComPtr<ID3DBlob> pdbBlob;
								D3DGetBlobPart( compiledEffectData->GetBufferPointer(), compiledEffectData->GetBufferSize(), D3D_BLOB_PDB, 0, &pdbBlob );

								CComPtr<ID3DBlob> pdbName;
								D3DGetBlobPart( compiledEffectData->GetBufferPointer(), compiledEffectData->GetBufferSize(), D3D_BLOB_DEBUG_NAME, 0, &pdbName );

								struct ShaderDebugName
								{
									uint16_t flags;
									uint16_t nameLength;
								};

								auto debugNameData = reinterpret_cast<const ShaderDebugName*>( pdbName->GetBufferPointer() );
								auto name = reinterpret_cast<const char*>( debugNameData + 1 );
								{
									// TODO: intern, I'm actually not sure if we need a mutex here at all...
									std::lock_guard scope( m_pdbCS );
									PDB pdb;
									pdb.name = name;
									pdb.pdbBlob = pdbBlob;
									result.pdbs.push_back( pdb );
								}
							}

							// Compilation succeeded! Hand over the resource to the cache entry.
							syncData->passResource.Attach( compiledEffectData.Detach() );
						}
					}
#endif

					// Let's wake up everyone waiting for this permutation's compilation.
					{
						std::lock_guard scope( syncData->mutex );
						syncData->compiled = true;
					}
					syncData->conditionVariable.notify_all();
				}

				// Grab a raw pointer from the cache. This is to avoid whatever funny business is going on with CComPtr reference counters.
				effectData = syncData->passResource;
				spirvEffectData = syncData->passSpirv;
				spirvReflectionData = syncData->passSpirvReflection;

				{
					// Not sure if this is necessary to avoid reference counter bugs. But anyway...
					std::lock_guard scope( m_compiledCS );
					syncData.reset();
				}

				if( !compileOptions.spirv && !effectData )
				{
					// No resource on the cache entry! This means compilation must have failed.
					return false;
				}

#if _WIN32
				CComPtr<ID3D11ShaderReflection> reflection;
#endif
				CComPtr<ID3D12ShaderReflection> reflectionDx12;
				if( compileOptions.spirv )
				{
					if( !spirvEffectData || !spirvReflectionData )
					{
						return false;
					}
					stage.shaderSize = uint32_t( spirvEffectData->GetBufferSize() );
					stage.shaderDataStr = g_stringTable.AddString( spirvEffectData->GetBufferPointer(), spirvEffectData->GetBufferSize() );
					stage.source = code;

					DxcBuffer reflectionBuffer = { spirvReflectionData->GetBufferPointer(), spirvReflectionData->GetBufferSize(), 0 };
					if( FAILED( m_dxilUtils->CreateReflection( &reflectionBuffer, IID_PPV_ARGS( &reflectionDx12 ) ) ) )
					{
						g_messages.AddMessage( "\\memory(0): error X0000: Could not get shader reflection" );
						return false;
					}
					if( !DxReflection::ProcessReflection<DxReflection::ReflectionDx12>( state, reflectionDx12.p, compileOptions.useStaticSamplers, stage, result.annotations ) )
					{
						return false;
					}
				}
#if _WIN32
				else
				{
					auto handleStrippedData = [&]( ID3DBlob* blob ) {
						// No idea what happens when assigning strippedEffectData = effectData, with effectData now being a raw pointer, and not taking any chances...
						stage.shaderSize = uint32_t( blob->GetBufferSize() );
						stage.shaderDataStr = g_stringTable.AddString( blob->GetBufferPointer(), blob->GetBufferSize() );
						stage.source = code;
					};
					CComPtr<ID3DBlob> strippedEffectData;
					{
						ZoneScopedN( "D3DStripShader" );
						if( FAILED( D3DStripShader(
								effectData->GetBufferPointer(),
								effectData->GetBufferSize(),
								D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO | D3DCOMPILER_STRIP_TEST_BLOBS,
								&strippedEffectData ) ) )
						{
							handleStrippedData( effectData );
						}
						else
						{
							handleStrippedData( strippedEffectData );
						}
					}

					{
						ZoneScopedN( "D3DReflect" );

						if( FAILED( D3DReflect( effectData->GetBufferPointer(), effectData->GetBufferSize(), IID_ID3D11ShaderReflection, (void**)&reflection.p ) ) )
						{
							g_messages.AddMessage( "\\memory(0): error X0000: Could not get shader reflection" );
							return false;
						}
					}

					if( !DxReflection::ProcessReflection<DxReflection::ReflectionDx11>( state, reflection.p, compileOptions.useStaticSamplers, stage, result.annotations ) )
					{
						return false;
					}
				}
#endif
				{
					stage.annotations.annotations.clear();
					Symbol* symbol = shaderNode->GetChild( 1 )->GetSymbol();
					if( symbol && symbol->annotations )
					{
						for( auto a = symbol->annotations->begin(); a != symbol->annotations->end(); ++a )
						{
							Annotation symbolAnnotation;
							bool isSrgb, isAutoregister;
							if( DxReflection::MakeEffectAnnotationFromSymbolAnnotation( *a, symbolAnnotation, isSrgb, isAutoregister ) )
							{
								stage.annotations.annotations[g_stringTable.AddString( ToString( a->name ).c_str() )] = symbolAnnotation;
							}
						}
					}
				}

				if( !stage.defaultValues.empty() )
				{
					stage.defaultValuesStr = g_stringTable.AddString( &stage.defaultValues[0], stage.defaultValues.size() );
				}
				else
				{
					stage.defaultValuesStr = INVALID_REFERENCE;
				}

				if( listing.enabled() )
				{
					listing.dict()
						.literal( "profile" )
						.literal( profile )
						.literal( "original" )
						.dict()
						.literal( "entryPoint" )
						.literal( patchEntryPoint )
						.literal( "source" )
						.literal( SanitizeCode( code ) );
					if( compileOptions.spirv )
					{
						PrintShaderOutListing( listing, nullptr, reflectionDx12.p );
					}
#if _WIN32
					else
					{
						PrintShaderOutListing( listing, effectData, reflection.p );
					}
#endif
					listing.end();
				}

				if( compileOptions.spirv )
				{
					reflectionsDx12[stage.type] = reflectionDx12;
				}
#if _WIN32
				else
				{
					reflections[stage.type] = reflection;
				}
#endif

				PrintStageInfo( listing, stage, result );
				listing.end();

				outPass.stages.push_back( stage );
			}


			// Check if PS input matches VS output
			InputStageType pipelineStages[] = {
				VERTEX_STAGE,
				HULL_STAGE,
				DOMAIN_STAGE,
				GEOMETRY_STAGE,
				PIXEL_STAGE,
			};
			// These two were written inline as `compileOptions.spirv ? <dx12> : <dx11>`
			// ternaries. A ternary cannot be half-compiled, and its DX11 arm names a type
			// that does not exist on macOS -- hence the lambdas. Same predicates, same
			// order, one call deeper: Windows behaviour is unchanged.
			auto hasReflection = [&]( int stageIndex ) -> bool {
				if( compileOptions.spirv )
				{
					return !!reflectionsDx12[stageIndex];
				}
#if _WIN32
				return !!reflections[stageIndex];
#else
				return false; // unreachable: !spirv returned above on this platform
#endif
			};
			auto signaturesMatch = [&]( int output, int input ) -> bool {
				if( compileOptions.spirv )
				{
					return MatchShaderInputOutput( reflectionsDx12[output], reflectionsDx12[input] );
				}
#if _WIN32
				return MatchShaderInputOutput( reflections[output], reflections[input] );
#else
				return true; // unreachable, as above
#endif
			};
			for( int i = 0; i < 6; ++i )
			{
				if( hasReflection( i ) )
				{
					for( int j = 0; j < int( sizeof( pipelineStages ) / sizeof( InputStageType ) ); ++j )
					{
						if( i == pipelineStages[j] )
						{
							for( int k = j - 1; k >= 0; --k )
							{
								if( hasReflection( pipelineStages[k] ) )
								{
									if( !signaturesMatch( pipelineStages[k], i ) )
									{
										return false;
									}
									break;
								}
							}
							break;
						}
					}
				}
			}
			technique.passes.push_back( outPass );

			listing.end(); // stages list
		}

		listing.end(); // passes list
		listing.literal( "libraries" ).list();

		for( size_t passIx = 0; passIx < techniqueNode->GetChildrenCount(); ++passIx )
		{
			ASTNode* libNode = techniqueNode->GetChild( passIx );
			if( libNode->GetNodeType() != NT_LIBRARY )
			{
				continue;
			}
			listing.dict()
				.literal( "name" )
				.literal( libNode->GetToken()->stringValue )
				.literal( "exports" )
				.list();

			Library library;
			library.payloadSize = 0;
			library.hitGroupName = g_stringTable.AddString( "" );
			const std::string libraryName = ToString( techniqueNode->GetToken()->stringValue ) + "/" + ToString( libNode->GetToken()->stringValue );

			std::map<std::string, std::string> shaders;
			std::vector<GlobalInputElement> globalInputs;

			state.GetSymbolTable().ResetUsedFlag();

			for( size_t i = 0; i < libNode->GetChildrenCount(); ++i )
			{
				auto childNode = libNode->GetChild( i );
				if( childNode->GetNodeType() == NT_SHADER_ASSIGNMENT )
				{
					ShaderExport shaderExport;
					if( auto parsed = ParseRtShaderName( childNode->GetToken()->stringValue ) )
					{
						shaderExport.type = parsed.value();
					}
					else
					{
						state.ShowMessage( childNode->GetToken()->fileLocation, EC_INVALID_STATE, ToString( childNode->GetToken()->stringValue ).c_str() );
						return false;
					}
					listing.literal( childNode->GetChild( 1 )->GetSymbol()->name );
					shaderExport.name = g_stringTable.AddString( ToString( childNode->GetChild( 1 )->GetSymbol()->name ).c_str() );
					library.exports.push_back( shaderExport );

					if( !ProcessGlobalInputAttribute( state, childNode->GetChild( 1 )->GetSymbol()->definition, globalInputs ) )
					{
						return false;
					}
					for( auto& gi : globalInputs )
					{
						if( gi.symbol )
						{
							gi.symbol->used = true;
							if( auto type = gi.symbol->type.symbol )
							{
								const_cast<Symbol*>( type )->used = true;
							}
						}
					}
					MarkUsedSymbols( childNode->GetChild( 1 ), state );
				}
				else if( childNode->GetNodeType() == NT_STATE_ASSIGNMENT )
				{
					auto name = childNode->GetToken()->stringValue;
					if( EqualsCaseInsensitive( name, "payloadsize" ) )
					{
						Type type;
						type.FromTokenType( OP_UINT );
						ExpressionValue value;
						if( !EvaluateExpression( state, childNode->GetChildOrNull( 0 ), type, value, nullptr ) )
						{
							state.ShowMessage( childNode->GetToken()->fileLocation, EC_INVALID_STATE, ToString( name ).c_str() );
							return false;
						}
						library.payloadSize = uint32_t( value[0].intValue );
					}
					else if( EqualsCaseInsensitive( name, "hitgroupname" ) )
					{
						Type type;
						type.FromTokenType( OP_STRING );
						ExpressionValue value;
						if( !EvaluateExpression( state, childNode->GetChildOrNull( 0 ), type, value, nullptr ) )
						{
							state.ShowMessage( childNode->GetToken()->fileLocation, EC_INVALID_STATE, ToString( name ).c_str() );
							return false;
						}
						library.hitGroupName = g_stringTable.AddString( value[0].stringValue.c_str() );
					}
				}
			}
			listing.end(); // exports list

			if( compileOptions.addSpaces )
			{
				CreateGlobalsCB( state );
				AssignRegisters( state.GetTree(), 8, globalInputs );
			}

			CompilerInputStream os( state, ShadingLanguage::HLSL );
			os << HLSL{ state.GetTree(), &state.GetSymbolTable() };
			std::string code = os.str();
			library.source = code;

			// Let only one thread compile this permutation.
			bool needsToCompile = false;
			std::shared_ptr<SyncData> syncData;
			{
				std::lock_guard scope( m_compiledCS );
				auto found = m_compiled.find( code );
				if( found == end( m_compiled ) )
				{
					// Yep, this thread will have to compile. Make an entry into the cache.
					syncData = std::make_shared<SyncData>();
					m_compiled[code] = syncData;
					needsToCompile = true;
				}
				else
				{
					// Some other thread is going to compile for us. Grab his stuff from the cache.
					syncData = found->second;
				}
			}

			if( !needsToCompile )
			{
				// Let's wait for the other thread to compile for us
				if( workQueue )
				{
					workQueue->OnBlocked();
				}
				{
					std::unique_lock<std::mutex> lock( syncData->mutex );
					syncData->conditionVariable.wait( lock, [&syncData] { return syncData->compiled; } );
				}
				if( workQueue )
				{
					workQueue->OnUnblocked();
				}
			}
			else
			{
				// We need to compile, nobody else is doing it for this permutation. Every step is
				// checked and says why it failed: a failed status used to be read only on Vulkan,
				// and on DX12 a source error then returned without printing DXC's messages.
				CComPtr<IDxcBlobEncoding> src;
				CComPtr<IDxcCompiler3> compiler;
				CComPtr<IDxcResult> pCompileResult;
				HRESULT hrCompilation = m_dxilUtils->CreateBlobFromPinned( code.c_str(), UINT32( code.size() ), CP_UTF8, &src );
				if( FAILED( hrCompilation ) )
				{
					g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s': IDxcUtils::CreateBlobFromPinned failed (0x%08x)", libraryName.c_str(), unsigned( hrCompilation ) );
				}
				else if( FAILED( hrCompilation = DxcCreateInstance( CLSID_DxcCompiler, IID_PPV_ARGS( &compiler ) ) ) )
				{
					g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s': DxcCreateInstance(CLSID_DxcCompiler) failed (0x%08x)", libraryName.c_str(), unsigned( hrCompilation ) );
				}
				else
				{
					LPCWSTR arguments[7];
					uint32_t argumentsSize = 5;
					arguments[0] = L"-T";
					arguments[1] = L"lib_6_3";
					arguments[2] = L"-Qstrip_debug";
					arguments[3] = L"-Qstrip_reflect";
					arguments[4] = DXC_ARG_WARNINGS_ARE_ERRORS; //-WX
					// Under spirv this DXIL is reflection only and never ships, so neither -Zi nor
					// its PDB: a PDB would describe code that is not in the .sm_*. -Zi changes the
					// object, so the argument list is the old one whenever !spirv.
					const bool dxilPdb = g_generatePDB && !compileOptions.spirv;
					if( dxilPdb )
					{
						arguments[argumentsSize++] = DXC_ARG_DEBUG; //-Zi
					}
					if( g_skipOptimization )
					{
						arguments[argumentsSize++] = DXC_ARG_SKIP_OPTIMIZATIONS; //-Od
					}

					DxcBuffer sourceBuffer;
					sourceBuffer.Ptr = src->GetBufferPointer();
					sourceBuffer.Size = src->GetBufferSize();
					sourceBuffer.Encoding = 0;

					hrCompilation = compiler->Compile( &sourceBuffer, arguments, argumentsSize, nullptr, IID_PPV_ARGS( &pCompileResult ) );
					if( SUCCEEDED( hrCompilation ) && !pCompileResult )
					{
						hrCompilation = E_FAIL;
					}
					if( SUCCEEDED( hrCompilation ) )
					{
						// IDxcCompiler3 reports a source error as S_OK with a failed status.
						pCompileResult->GetStatus( &hrCompilation );
					}
					if( FAILED( hrCompilation ) )
					{
						CComPtr<IDxcBlobUtf8> errors;
						if( pCompileResult )
						{
							pCompileResult->GetOutput( DXC_OUT_ERRORS, IID_PPV_ARGS( &errors ), nullptr );
						}
						if( errors && errors->GetStringLength() > 0 )
						{
							g_messages.AddMessages( errors );
						}
						else
						{
							g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s': DXC failed (0x%08x) and gave no message", libraryName.c_str(), unsigned( hrCompilation ) );
						}
					}
				}

				if( FAILED( hrCompilation ) )
				{
					syncData->libraryResource = nullptr;
				}
				else
				{
					CComPtr<IDxcBlob> reflectionData;
					pCompileResult->GetOutput( DXC_OUT_REFLECTION, IID_PPV_ARGS( &reflectionData ), nullptr );
					syncData->libraryReflection.Attach( reflectionData.Detach() );

#if _WIN32
					// EffectData.h declares PDB::pdbBlob under _WIN32, and wcstombs_s is
					// the CRT's. Ray tracing is a DX12 feature; nothing reaches here on
					// macOS, where /pdb would need a different container anyway.
					if( g_generatePDB && !compileOptions.spirv )
					{
						// Get debug info and it's name.
						CComPtr<IDxcBlob> pdbBlob;
						CComPtr<IDxcBlobUtf16> pdbName;
						pCompileResult->GetOutput( DXC_OUT_PDB, IID_PPV_ARGS( &pdbBlob ), &pdbName );

						char outputString[128];
						size_t outputSize = pdbName->GetStringLength() + 1;
						size_t charsConverted = 0;
						const wchar_t* inputW = pdbName->GetStringPointer();
						wcstombs_s( &charsConverted, outputString, outputSize, inputW, pdbName->GetStringLength() );

						{
							// TODO: intern, I'm actually not sure if we need a mutex here at all...
							std::lock_guard scope( m_pdbCS );
							PDB pdb;
							pdb.name = outputString;
							pdb.pdbBlob = pdbBlob;
							result.pdbs.push_back( pdb );
						}
					}
#endif

					// Compilation succeeded! Hand over the resource to the cache entry.
					CComPtr<IDxcBlob> compiled;
					pCompileResult->GetOutput( DXC_OUT_OBJECT, IID_PPV_ARGS( &compiled ), nullptr );
					syncData->libraryResource.Attach( compiled.Detach() );
				}

				// Let's wake up everyone waiting for this permutation's compilation.
				{
					std::lock_guard scope( syncData->mutex );
					syncData->compiled = true;
				}
				syncData->conditionVariable.notify_all();
			}

			// Grab a raw pointer from the cache. This is to avoid whatever funny business is going on with CComPtr reference counters.
			IDxcBlob* compiled = syncData->libraryResource;
			IDxcBlob* reflectionData = syncData->libraryReflection;

			{
				// Not sure if this is necessary to avoid reference counter bugs. But anyway...
				std::lock_guard scope( m_compiledCS );
				syncData.reset();
			}

			// Vulkan: a library that exports hit groups and nothing else is dropped, with a
			// warning, when it fails to compile, and the effect builds without it -- the V5 hull
			// and turret effects all carry such a library (technique RtShadow), and one that does
			// not compile for SPIR-V used to fail the whole hull effect, on devices that never ray
			// trace too. The engine skips a material whose RtShadow technique has no library
			// (Tr2RaytracingGeometry), so the hull just casts no ray-traced shadow. A library
			// exporting a ray generation or miss shader stays fatal: the effects that trace
			// (rtshadows, rtcalculatefroxels) index their library without a check.
			const bool dropOnFailure = compileOptions.spirv && std::none_of( begin( library.exports ), end( library.exports ), []( const ShaderExport& e ) { return e.type == RtShaderType::RAY_GEN || e.type == RtShaderType::MISS; } );
			bool dropped = false;

			if( !compiled || !reflectionData )
			{
				if( compiled )
				{
					g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s': DXC returned no reflection", libraryName.c_str() );
				}
				// No resource on the cache entry means compilation failed, and said why.
				if( !dropOnFailure )
				{
					return false;
				}
				dropped = true;
			}

			if( !dropped )
			{
				if( !compileOptions.spirv )
				{
					// Vulkan ships CompileLibrarySpirv's module instead: there this DXIL is the
					// reflection source only, and stays out of the string table.
					library.shaderDataStr = g_stringTable.AddString( compiled->GetBufferPointer(), compiled->GetBufferSize() );
					library.shaderSize = uint32_t( compiled->GetBufferSize() );
				}

				DxcBuffer reflectionBuffer;
				reflectionBuffer.Ptr = reflectionData->GetBufferPointer();
				reflectionBuffer.Size = reflectionData->GetBufferSize();
				reflectionBuffer.Encoding = 0;
				CComPtr<ID3D12LibraryReflection> shaderReflection;
				HRESULT hrReflection = m_dxilUtils->CreateReflection( &reflectionBuffer, IID_PPV_ARGS( &shaderReflection ) );

				D3D12_LIBRARY_DESC desc;
				if( FAILED( hrReflection ) || !shaderReflection || FAILED( shaderReflection->GetDesc( &desc ) ) )
				{
					g_messages.AddMessage( "\\memory(0): error X0000: ray tracing library '%s': could not read the DXIL library reflection (0x%08x)", libraryName.c_str(), unsigned( hrReflection ) );
					return false;
				}

				for( UINT i = 0; i < desc.FunctionCount; ++i )
				{
					if( !DxReflection::ProcessReflection<DxReflection::FunctionDx12>( state, shaderReflection->GetFunctionByIndex( i ), compileOptions.useStaticSamplers, library.globalInputs, result.annotations, DxReflection::ReflectionTarget::GLOBAL_INPUT, globalInputs ) )
					{
						return false;
					}
					if( !DxReflection::ProcessReflection<DxReflection::FunctionDx12>( state, shaderReflection->GetFunctionByIndex( i ), compileOptions.useStaticSamplers, library.localInputs, result.annotations, DxReflection::ReflectionTarget::LOCAL_INPUT, globalInputs ) )
					{
						return false;
					}
				}

				AddGlobalInputs( library.globalInputs, result.annotations, globalInputs, state, compileOptions.useStaticSamplers );

				if( compileOptions.spirv && !CompileLibrarySpirv( state, globalInputs, shaderReflection, libraryName, library, workQueue ) )
				{
					if( !dropOnFailure )
					{
						return false;
					}
					dropped = true;
				}
			}

			if( dropped )
			{
				// Unconditional, not a g_printWarnings warning: the staging run compiles with
				// /no_warnings, and a hull silently without ray-traced shadows is what this says.
				g_messages.AddMessage( "\\memory(0): warning X0000: ray tracing library '%s' dropped: it exports only hit group shaders and did not compile for Vulkan (errors above); the effect is built without it, and its geometry casts no ray-traced shadow", libraryName.c_str() );
			}
			else
			{
				technique.libraries.push_back( library );
			}

			if( listing.enabled() && !dropped )
			{
				CComPtr<IDxcCompiler> compiler;
				DxcCreateInstance( CLSID_DxcCompiler, IID_PPV_ARGS( &compiler ) );

				listing.literal( "profile" ).literal( "lib_6_3" );
				listing.literal( "original" ).dict();
				std::string osSrc;
				osSrc.reserve( os.str().size() );
				listing.literal( "source" ).literal( SanitizeCode( compileOptions.spirv ? library.source : std::string( os.str(), osSrc.size() ) ) );
				CComPtr<IDxcBlobEncoding> disassembly;
				// DXC's Disassemble reads DXIL only; a SPIR-V listing has no asm, as for passes.
				if( !compileOptions.spirv && SUCCEEDED( compiler->Disassemble( compiled, &disassembly ) ) )
				{
					listing.literal( "asm" ).literal( reinterpret_cast<const char*>( disassembly->GetBufferPointer() ) );
				}
				listing.end();
				PrintStageInfo( listing, library.globalInputs, result );
			}
			listing.end();
		}

		result.techniques.push_back( technique );
		listing.end(); // libraries list
		listing.end(); // technique dict
	}
	listing.end(); // technique list

	return true;
}
