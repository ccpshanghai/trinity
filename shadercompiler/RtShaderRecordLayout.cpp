// Copyright © 2026 CCP ehf.

#include "stdafx.h"
#include "RtShaderRecordLayout.h"
#include "CompileMessageQueue.h"
#include "EffectData.h"

#include <algorithm>

extern CompileMessageQueue g_messages;

namespace
{
uint32_t AlignTo16( uint32_t size )
{
	return ( size + 15 ) & ~15u;
}

uint32_t D3DComponentSize( const D3D12_SHADER_TYPE_DESC& desc )
{
	return desc.Type == D3D_SVT_DOUBLE ? 8 : 4;
}
}

namespace RtShaderRecordLayout
{
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

void D3DFlatten( ID3D12ShaderReflectionType* type, uint32_t base, std::vector<SpirvScalarSlot>& slots )
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
				slots.push_back( { elementBase + c * componentSize, componentSize } );
			}
			break;
		case D3D_SVC_MATRIX_COLUMNS:
			// Stored column by column, visited row by row (see the header).
			for( UINT r = 0; r < desc.Rows; ++r )
			{
				for( UINT c = 0; c < desc.Columns; ++c )
				{
					slots.push_back( { elementBase + c * 16 + r * componentSize, componentSize } );
				}
			}
			break;
		case D3D_SVC_MATRIX_ROWS:
			for( UINT r = 0; r < desc.Rows; ++r )
			{
				for( UINT c = 0; c < desc.Columns; ++c )
				{
					slots.push_back( { elementBase + r * 16 + c * componentSize, componentSize } );
				}
			}
			break;
		case D3D_SVC_STRUCT:
			for( UINT i = 0; i < desc.Members; ++i )
			{
				auto member = type->GetMemberTypeByIndex( i );
				D3D12_SHADER_TYPE_DESC memberDesc;
				member->GetDesc( &memberDesc );
				D3DFlatten( member, elementBase + memberDesc.Offset, slots );
			}
			break;
		default:
			break;
		}
	}
}

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

bool CompareShaderRecordLayout( const SpirvReflection::ShaderRecordBlock& record, ID3D12ShaderReflectionConstantBuffer* cbuffer, std::string& difference )
{
	difference.clear();
	D3D12_SHADER_BUFFER_DESC bufferDesc;
	if( !cbuffer || FAILED( cbuffer->GetDesc( &bufferDesc ) ) )
	{
		difference = "The constant buffer has no reflection.";
		return false;
	}
	std::vector<SpirvScalarSlot> d3dSlots;
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
	const auto& recordSlots = record.scalars;
	size_t first = 0;
	while( first < d3dSlots.size() && first < recordSlots.size() && d3dSlots[first] == recordSlots[first] )
	{
		++first;
	}
	if( record.laidOut && first == d3dSlots.size() && first == recordSlots.size() )
	{
		return true;
	}
	if( !record.laidOut )
	{
		difference = "The record's SPIR-V type could not be laid out.";
	}
	else if( first < d3dSlots.size() && first < recordSlots.size() )
	{
		difference = "First difference: scalar " + std::to_string( first ) + " ('" + d3dVariables[first] + "') is at byte " + std::to_string( d3dSlots[first].offset ) + " (" + std::to_string( d3dSlots[first].size ) +
			" bytes) in the constant buffer and at byte " + std::to_string( recordSlots[first].offset ) + " (" + std::to_string( recordSlots[first].size ) + " bytes) in the shader record.";
	}
	else
	{
		difference = "The constant buffer has " + std::to_string( d3dSlots.size() ) + " scalars and the shader record " + std::to_string( recordSlots.size() ) + ".";
	}
	return false;
}

bool CheckShaderRecords( const SpirvReflection& module, ID3D12LibraryReflection* reflection, const Library& library, const std::string& libraryName )
{
	bool ok = true;

	// Each record variable any entry point reads, with the entry points that read it, in the
	// order the module lists them.
	std::vector<std::pair<uint32_t, std::vector<std::string>>> records;
	for( auto& entryPoint : module.entryPoints )
	{
		for( auto id : entryPoint.interfaceIds )
		{
			if( !module.FindShaderRecord( id ) )
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
		std::string difference;
		if( !CompareShaderRecordLayout( *module.FindShaderRecord( id ), cbuffer, difference ) )
		{
			g_messages.AddMessage(
				"\\memory(0): error X0000: ray tracing library '%s', %s: local cbuffer '%s' is laid out differently as a Vulkan shader record (storage buffer rules: arrays of scalars and small vectors are packed) than as a D3D constant buffer, and the record is a byte copy of the constant buffer. Make every array element a 16-byte multiple (float4, uint4, a padded struct). %s",
				libraryName.c_str(),
				entryPoints.c_str(),
				name.c_str(),
				difference.c_str() );
			ok = false;
		}
	}
	return ok;
}
}
