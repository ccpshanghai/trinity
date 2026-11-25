// Copyright © 2026 CCP ehf.

#pragma once

// What a SPIR-V module says about itself, read by one walk over its words (no SPIRV-Tools, no
// SPIRV-Reflect): its entry points, its descriptor bindings, and the layout of its shader record
// blocks.
//
// One reader for three users that used to keep one each:
//
//   1. the Vulkan AL (Tr2ShaderALVulkan.cpp, Tr2RtPipelineStateALVulkan.cpp) -- a pass shader's
//      entry point name, a ray tracing library's export stages, which bindings a library reads
//      and which of them are acceleration structures, and how large a group's shader record is;
//   2. the shader compiler (RtShaderRecordLayout.cpp) -- which entry points read which shader
//      record, and every scalar of a record, to compare with the D3D constant buffer the engine
//      fills;
//   3. the compiler's tests (VulkanCompilerTest.cpp).
//
// The compiler's layout check and the AL's record size are the two halves of one contract (the
// record after a group handle is a byte copy of the library's local constant buffer; spec
// vulkan-ray-tracing.md section 3.3), so they read the record through this one type table.
//
// Like Tr2ShaderBindingABIVulkan.h beside it, this header includes the standard library only,
// mentions no AL or Vulkan type and has no TRINITY_PLATFORM guard, so the host-side compiler can
// include it. Numbers below are the SPIR-V specification's and frozen by it.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace SpirvReflectVulkan
{
// One scalar of a laid-out block: where it starts and how wide it is (4, or 8 for a 64-bit type).
struct SpirvScalarSlot
{
	uint32_t offset;
	uint32_t size;
	bool operator==( const SpirvScalarSlot& other ) const
	{
		return offset == other.offset && size == other.size;
	}
	bool operator!=( const SpirvScalarSlot& other ) const
	{
		return !( *this == other );
	}
};

struct SpirvReflection
{
	struct EntryPoint
	{
		uint32_t executionModel; // Vertex 0 ... GLCompute 5, RayGenerationKHR 5313 ... CallableKHR 5318
		std::string name;
		std::vector<uint32_t> interfaceIds; // the variables the entry point declares it uses
	};
	// Every OpVariable, function-local ones included (in module order).
	struct Variable
	{
		uint32_t id;
		uint32_t storageClass;
		uint32_t pointeeType; // 0 when the pointer type is not in the module
	};
	// A variable decorated with both DescriptorSet and Binding.
	struct Binding
	{
		uint32_t variable;
		uint32_t set;
		uint32_t binding;
		// Its type under any arrays is OpTypeAccelerationStructureKHR. The effect reflection
		// reports a RaytracingAccelerationStructure as a plain SRV_BUFFER register, which on Vulkan
		// would be a uniform texel buffer; the module is the one place that still knows it is not.
		bool accelerationStructure;
	};
	// A variable in the ShaderRecordBufferKHR storage class: what a ray tracing library's local
	// cbuffer becomes ([[vk::shader_record_ext]]).
	struct ShaderRecordBlock
	{
		uint32_t variable;
		// Where the block's last byte ends, from its Offset / ArrayStride / MatrixStride / RowMajor
		// decorations: the bytes a group's record must hold even when the engine leaves the
		// constant buffer unset. A type this reader does not know counts as 0 bytes.
		uint32_t size;
		// Every scalar, in declaration order: struct members in order, array elements outer, and
		// a matrix by HLSL row, then HLSL column. DXC turns an HLSL matrix's rows into SPIR-V
		// columns (column_major float4x3 is OpTypeMatrix of 4 vec3, decorated RowMajor), so the
		// SPIR-V column-outer walk is HLSL row-outer, whichever the decoration.
		std::vector<SpirvScalarSlot> scalars;
		// False when `scalars` is incomplete: a type this reader cannot lay out (a runtime array,
		// an array without ArrayStride, a pointer), more than MAX_SCALARS scalars, or a type graph
		// over the module's visit limit (Detail::MAX_TYPE_VISITS; `size` is then short too).
		bool laidOut;
	};

	std::vector<EntryPoint> entryPoints;
	std::vector<Variable> variables;
	std::vector<Binding> bindings;
	std::vector<ShaderRecordBlock> shaderRecords;
	std::map<uint32_t, std::string> names; // OpName
	std::set<uint32_t> capabilities;

	// The OpName of `id`, or "%<id>".
	std::string NameOf( uint32_t id ) const
	{
		auto found = names.find( id );
		return found == names.end() ? "%" + std::to_string( id ) : found->second;
	}
	const ShaderRecordBlock* FindShaderRecord( uint32_t variable ) const
	{
		for( auto& record : shaderRecords )
		{
			if( record.variable == variable )
			{
				return &record;
			}
		}
		return nullptr;
	}
	// The largest shader record block's size; 0 if the module declares none.
	uint32_t ShaderRecordSize() const
	{
		uint32_t size = 0;
		for( auto& record : shaderRecords )
		{
			size = std::max( size, record.size );
		}
		return size;
	}
};

namespace Detail
{
const uint32_t MAGIC = 0x07230203u;
const size_t HEADER_WORDS = 5;
// Limits that keep a corrupt module from costing more than a valid one could: a nesting depth
// no compiler emits, more scalars than a 64 KiB constant buffer holds, 256 MiB of words, and a
// number of type visits per module (Types::Extent / Flatten calls) far above what laying out
// that many scalars takes. The last one is what bounds the time: depth alone does not, because a
// struct of N members of a struct of N members ... is N^depth visits, and an array of an empty
// struct adds no scalar however long it says it is.
const size_t MAX_DEPTH = 64;
const size_t MAX_SCALARS = 1u << 16;
const size_t MAX_WORDS = size_t( 1 ) << 26;
const size_t MAX_TYPE_VISITS = size_t( 1 ) << 20;

enum Op : uint32_t
{
	OP_NAME = 5,
	OP_ENTRY_POINT = 15,
	OP_CAPABILITY = 17,
	OP_TYPE_BOOL = 20,
	OP_TYPE_INT = 21,
	OP_TYPE_FLOAT = 22,
	OP_TYPE_VECTOR = 23,
	OP_TYPE_MATRIX = 24,
	OP_TYPE_ARRAY = 28,
	OP_TYPE_RUNTIME_ARRAY = 29,
	OP_TYPE_STRUCT = 30,
	OP_TYPE_POINTER = 32,
	OP_CONSTANT = 43,
	OP_VARIABLE = 59,
	OP_DECORATE = 71,
	OP_MEMBER_DECORATE = 72,
	OP_TYPE_ACCELERATION_STRUCTURE_KHR = 5341,
};
enum Decoration : uint32_t
{
	DECORATION_ROW_MAJOR = 4,
	DECORATION_ARRAY_STRIDE = 6,
	DECORATION_MATRIX_STRIDE = 7,
	DECORATION_BINDING = 33,
	DECORATION_DESCRIPTOR_SET = 34,
	DECORATION_OFFSET = 35,
};
const uint32_t STORAGE_CLASS_SHADER_RECORD_BUFFER = 5343;

struct MemberLayout
{
	uint32_t offset = 0;
	uint32_t matrixStride = 0;
	bool rowMajor = false;
};

// The type section, as far as laying out a block needs it.
struct Types
{
	struct Type
	{
		uint32_t opcode;
		std::vector<uint32_t> operands; // the words after the result id
	};
	std::map<uint32_t, Type> types;
	std::map<uint32_t, uint32_t> constants; // OpConstant id -> its (low) value word
	std::map<uint32_t, uint32_t> arrayStrides;
	std::map<std::pair<uint32_t, uint32_t>, MemberLayout> members; // (struct, member index)
	// Extent and Flatten calls so far, against MAX_TYPE_VISITS; the walk itself is const.
	mutable size_t visits = 0;

	const Type* Find( uint32_t id ) const
	{
		auto found = types.find( id );
		return found == types.end() ? nullptr : &found->second;
	}

	// A scalar's width in bytes. A bool in a block is stored as a 32-bit integer.
	uint32_t ScalarSize( uint32_t id ) const
	{
		const Type* type = Find( id );
		if( !type || type->opcode == OP_TYPE_BOOL || type->operands.empty() )
		{
			return 4;
		}
		return type->operands[0] / 8;
	}

	// Where the type ends as laid out under `layout` (its member decorations): its last byte.
	uint32_t Extent( uint32_t id, const MemberLayout& layout, size_t depth ) const
	{
		const Type* type = Find( id );
		if( !type || depth > MAX_DEPTH || ++visits > MAX_TYPE_VISITS )
		{
			return 0;
		}
		switch( type->opcode )
		{
		case OP_TYPE_BOOL:
		case OP_TYPE_INT:
		case OP_TYPE_FLOAT:
			return ScalarSize( id );
		case OP_TYPE_VECTOR:
			return type->operands.size() < 2 ? 0 : type->operands[1] * Extent( type->operands[0], MemberLayout(), depth + 1 );
		case OP_TYPE_MATRIX: {
			if( type->operands.size() < 2 )
			{
				return 0;
			}
			const uint32_t columns = type->operands[1];
			const uint32_t column = Extent( type->operands[0], MemberLayout(), depth + 1 );
			if( !layout.matrixStride || !columns )
			{
				return columns * column;
			}
			if( !layout.rowMajor )
			{
				return layout.matrixStride * ( columns - 1 ) + column;
			}
			// Row major: one stride per row, each row a vector of `columns` components.
			const Type* columnType = Find( type->operands[0] );
			const uint32_t rows = columnType && columnType->operands.size() >= 2 ? columnType->operands[1] : 0;
			const uint32_t component = rows ? column / rows : 0;
			return rows ? layout.matrixStride * ( rows - 1 ) + columns * component : 0;
		}
		case OP_TYPE_ARRAY: {
			if( type->operands.size() < 2 )
			{
				return 0;
			}
			auto length = constants.find( type->operands[1] );
			const uint32_t count = length == constants.end() ? 0 : length->second;
			const uint32_t element = Extent( type->operands[0], layout, depth + 1 );
			auto stride = arrayStrides.find( id );
			return count ? ( stride == arrayStrides.end() ? element : stride->second ) * ( count - 1 ) + element : 0;
		}
		case OP_TYPE_STRUCT: {
			uint32_t end = 0;
			for( uint32_t m = 0; m < uint32_t( type->operands.size() ); ++m )
			{
				auto member = members.find( std::make_pair( id, m ) );
				const MemberLayout memberLayout = member == members.end() ? MemberLayout() : member->second;
				end = std::max( end, memberLayout.offset + Extent( type->operands[m], memberLayout, depth + 1 ) );
			}
			return end;
		}
		default:
			return 0;
		}
	}

	// Appends every scalar of the type at `base` in SpirvReflection::ShaderRecordBlock::scalars'
	// order; false when the type cannot be laid out (or there are too many scalars).
	bool Flatten( uint32_t id, uint32_t base, const MemberLayout& layout, size_t depth, std::vector<SpirvScalarSlot>& slots ) const
	{
		const Type* type = Find( id );
		if( !type || depth > MAX_DEPTH || slots.size() > MAX_SCALARS || ++visits > MAX_TYPE_VISITS )
		{
			return false;
		}
		switch( type->opcode )
		{
		case OP_TYPE_BOOL:
		case OP_TYPE_INT:
		case OP_TYPE_FLOAT:
			slots.push_back( { base, ScalarSize( id ) } );
			return true;
		case OP_TYPE_VECTOR: {
			if( type->operands.size() < 2 )
			{
				return false;
			}
			const uint32_t size = ScalarSize( type->operands[0] );
			if( slots.size() + type->operands[1] > MAX_SCALARS )
			{
				return false;
			}
			for( uint32_t i = 0; i < type->operands[1]; ++i )
			{
				slots.push_back( { base + i * size, size } );
			}
			return true;
		}
		case OP_TYPE_MATRIX: {
			const Type* column = type->operands.size() < 2 ? nullptr : Find( type->operands[0] );
			if( !column || column->opcode != OP_TYPE_VECTOR || column->operands.size() < 2 )
			{
				return false;
			}
			const uint32_t rows = column->operands[1];
			const uint32_t size = ScalarSize( column->operands[0] );
			if( slots.size() + uint64_t( type->operands[1] ) * rows > MAX_SCALARS )
			{
				return false;
			}
			for( uint32_t c = 0; c < type->operands[1]; ++c )
			{
				for( uint32_t r = 0; r < rows; ++r )
				{
					slots.push_back( { base + ( layout.rowMajor ? r * layout.matrixStride + c * size : c * layout.matrixStride + r * size ), size } );
				}
			}
			return true;
		}
		case OP_TYPE_ARRAY: {
			auto length = type->operands.size() < 2 ? constants.end() : constants.find( type->operands[1] );
			auto stride = arrayStrides.find( id );
			if( length == constants.end() || stride == arrayStrides.end() )
			{
				return false;
			}
			for( uint32_t i = 0; i < length->second; ++i )
			{
				if( !Flatten( type->operands[0], base + i * stride->second, layout, depth + 1, slots ) )
				{
					return false;
				}
			}
			return true;
		}
		case OP_TYPE_STRUCT:
			for( uint32_t m = 0; m < uint32_t( type->operands.size() ); ++m )
			{
				auto member = members.find( std::make_pair( id, m ) );
				const MemberLayout memberLayout = member == members.end() ? MemberLayout() : member->second;
				if( !Flatten( type->operands[m], base + memberLayout.offset, memberLayout, depth + 1, slots ) )
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

// A null-terminated string packed four bytes to a word; `consumed` is the words it took.
inline std::string ReadString( const uint32_t* words, size_t wordCount, size_t& consumed )
{
	const char* bytes = reinterpret_cast<const char*>( words );
	const size_t length = strnlen( bytes, wordCount * sizeof( uint32_t ) );
	consumed = std::min( wordCount, length / sizeof( uint32_t ) + 1 );
	return std::string( bytes, length );
}
}

// Reads `code`, `bytes` long, into `out` (cleared first). False when it is not native-endian
// SPIR-V with a whole header, when an instruction runs past the end or has no length, or when a
// variable's pointer type is missing; `out` then holds what was read before the fault. Native-
// endian only: a byte-reversed module would need every word swapped, and nothing in this
// pipeline produces one -- DXC writes the host's order. The header is five words, and every
// instruction after it starts with ( wordCount << 16 ) | opcode.
inline bool ReflectSpirv( const void* code, size_t bytes, SpirvReflection& out )
{
	using namespace Detail;
	out = SpirvReflection();
	const size_t wordCount = bytes / sizeof( uint32_t );
	if( !code || bytes % sizeof( uint32_t ) != 0 || wordCount <= HEADER_WORDS || wordCount > MAX_WORDS )
	{
		return false;
	}
	const uint32_t* words = static_cast<const uint32_t*>( code );
	if( words[0] != MAGIC )
	{
		return false;
	}

	Types types;
	std::map<uint32_t, std::pair<uint32_t, uint32_t>> pointers; // id -> { storage class, pointee }
	std::map<uint32_t, uint32_t> bindingNumbers, sets;
	std::set<uint32_t> accelerationStructureTypes;
	std::vector<std::pair<uint32_t, uint32_t>> variables; // { id, pointer type }
	bool wellFormed = true;
	for( size_t i = HEADER_WORDS; i < wordCount; )
	{
		const uint32_t opcode = words[i] & 0xffffu;
		const uint32_t length = words[i] >> 16;
		if( length == 0 || i + length > wordCount )
		{
			wellFormed = false; // reporting less beats reading past the end
			break;
		}
		const uint32_t* op = words + i;
		switch( opcode )
		{
		case OP_NAME:
			if( length >= 3 )
			{
				size_t consumed = 0;
				out.names[op[1]] = ReadString( op + 2, length - 2, consumed );
			}
			break;
		case OP_ENTRY_POINT:
			if( length >= 4 )
			{
				size_t consumed = 0;
				SpirvReflection::EntryPoint entryPoint;
				entryPoint.executionModel = op[1];
				entryPoint.name = ReadString( op + 3, length - 3, consumed );
				entryPoint.interfaceIds.assign( op + 3 + consumed, op + length );
				out.entryPoints.push_back( entryPoint );
			}
			break;
		case OP_CAPABILITY:
			if( length >= 2 )
			{
				out.capabilities.insert( op[1] );
			}
			break;
		case OP_TYPE_BOOL:
		case OP_TYPE_INT:
		case OP_TYPE_FLOAT:
		case OP_TYPE_VECTOR:
		case OP_TYPE_MATRIX:
		case OP_TYPE_ARRAY:
		case OP_TYPE_RUNTIME_ARRAY:
		case OP_TYPE_STRUCT:
			if( length >= 2 )
			{
				types.types[op[1]] = { opcode, std::vector<uint32_t>( op + 2, op + length ) };
			}
			break;
		case OP_TYPE_ACCELERATION_STRUCTURE_KHR:
			if( length >= 2 )
			{
				accelerationStructureTypes.insert( op[1] );
			}
			break;
		case OP_TYPE_POINTER:
			if( length >= 4 )
			{
				pointers[op[1]] = { op[2], op[3] };
			}
			break;
		case OP_CONSTANT:
			if( length >= 4 )
			{
				types.constants[op[2]] = op[3];
			}
			break;
		case OP_VARIABLE:
			if( length >= 4 )
			{
				variables.push_back( { op[2], op[1] } );
				out.variables.push_back( { op[2], op[3], 0 } );
			}
			break;
		case OP_DECORATE:
			if( length >= 4 )
			{
				switch( op[2] )
				{
				case DECORATION_ARRAY_STRIDE:
					types.arrayStrides[op[1]] = op[3];
					break;
				case DECORATION_BINDING:
					bindingNumbers[op[1]] = op[3];
					break;
				case DECORATION_DESCRIPTOR_SET:
					sets[op[1]] = op[3];
					break;
				default:
					break;
				}
			}
			break;
		case OP_MEMBER_DECORATE:
			if( length >= 4 )
			{
				auto& member = types.members[{ op[1], op[2] }];
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

	for( size_t v = 0; v < variables.size(); ++v )
	{
		const uint32_t id = variables[v].first;
		auto pointer = pointers.find( variables[v].second );
		if( pointer == pointers.end() )
		{
			wellFormed = false;
			continue;
		}
		const uint32_t pointee = pointer->second.second;
		out.variables[v].pointeeType = pointee;

		auto binding = bindingNumbers.find( id );
		auto set = sets.find( id );
		if( binding != bindingNumbers.end() && set != sets.end() )
		{
			uint32_t type = pointee;
			// Bounded: a valid module's types cannot form a cycle, a corrupt one must not hang.
			for( size_t depth = 0; depth < MAX_DEPTH; ++depth )
			{
				const Types::Type* element = types.Find( type );
				if( !element || ( element->opcode != OP_TYPE_ARRAY && element->opcode != OP_TYPE_RUNTIME_ARRAY ) || element->operands.empty() )
				{
					break;
				}
				type = element->operands[0];
			}
			out.bindings.push_back( { id, set->second, binding->second, accelerationStructureTypes.count( type ) != 0 } );
		}

		if( out.variables[v].storageClass == STORAGE_CLASS_SHADER_RECORD_BUFFER )
		{
			SpirvReflection::ShaderRecordBlock record;
			record.variable = id;
			record.size = types.Extent( pointee, MemberLayout(), 0 );
			record.laidOut = types.Flatten( pointee, 0, MemberLayout(), 0, record.scalars );
			out.shaderRecords.push_back( std::move( record ) );
		}
	}
	return wellFormed;
}
}
