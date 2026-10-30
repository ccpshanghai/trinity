// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2RtShaderTableALVulkan.h"
#include "Tr2RtPipelineStateALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2ConstantBufferALVulkan.h"
#include "UtilitiesVulkan.h"
#include "../ALLog.h"
#include "VkResult.h"
#include "Tr2RtUtilitiesVulkan.h"

#include <algorithm>
#include <atomic>
#include <iterator>

namespace TrinityALImpl
{
Tr2RtShaderTableAL::Tr2RtShaderTableAL() :
	m_owner( nullptr ),
	m_frame( 0 ),
	m_address( 0 ),
	m_rayGenSpacing( 0 ),
	m_rayGenStride( 0 ),
	m_missRegion{},
	m_hitRegion{}
{
}

Tr2RtShaderTableAL::~Tr2RtShaderTableAL()
{
	Destroy();
}

ALResult Tr2RtShaderTableAL::Create( const Tr2RtShaderTableDescriptionAL& desc, const ::Tr2RtPipelineStateAL& pipeline, Tr2PrimaryRenderContextAL& renderContext )
{
	Destroy();
	if( !renderContext.IsValid() || !renderContext.GetCaps().SupportsRaytracing() )
	{
		return E_INVALIDCALL;
	}
	if( !pipeline.IsValid() )
	{
		return E_INVALIDARG;
	}
	const auto& rt = *pipeline.TrinityALImpl_GetObject();
	const auto& properties = renderContext.m_rayTracingPipelineProperties;
	const VkDeviceSize handleSize = properties.shaderGroupHandleSize;

	// Each record: its group, and the constant buffer whose bytes follow the handle.
	struct Record
	{
		const Tr2RtPipelineStateAL::GroupInfoVulkan* group;
		const Tr2ConstantBufferAL* constants;
		uint32_t dataSize;
	};
	auto resolve = [&rt]( const Tr2RtShaderTableDescriptionAL::ShaderRecord& shaderRecord, Record& record ) -> ALResult {
		record.group = rt.FindGroupVulkan( shaderRecord.name );
		record.constants = nullptr;
		record.dataSize = 0;
		if( !record.group )
		{
			return E_INVALIDARG;
		}
		if( record.group->recordRegister == Tr2RtPipelineStateAL::NO_RECORD )
		{
			return S_OK;
		}
		const uint32_t reg = record.group->recordRegister;
		const ::Tr2ConstantBufferAL* cb = reg < uint32_t( std::size( shaderRecord.material.m_constants ) ) ? shaderRecord.material.m_constants[reg] : nullptr;
		if( !cb || !cb->IsValid() )
		{
			// DX12 points the record at its null constant buffer, which reads as zeros. The
			// Vulkan record is the data, so it gets the zeros itself: as many bytes as the
			// module's shader record block, left as the table's memset leaves them.
			static std::atomic<bool> logged( false );
			if( !logged.exchange( true ) )
			{
				CCP_AL_LOG( "Tr2RtShaderTableAL: the record for '%ls' needs local constant buffer b%u and none is set; its %u record bytes are zero (DX12 binds its null constant buffer). Logged once.", shaderRecord.name, reg, record.group->recordSize );
			}
			record.dataSize = record.group->recordSize;
			return S_OK;
		}
		record.constants = cb->TrinityALImpl_GetObject();
		record.dataSize = cb->GetSize();
		return S_OK;
	};

	std::vector<Record> rayGen( desc.m_rayGenNames.size() ), miss( desc.m_missNames.size() ), hit( desc.m_hitGroupNames.size() );
	uint32_t rayGenData = 0, missData = 0, hitData = 0;
	for( size_t i = 0; i < rayGen.size(); ++i )
	{
		FORWARD_HR( resolve( desc.m_rayGenNames[i], rayGen[i] ) );
		rayGenData = std::max( rayGenData, rayGen[i].dataSize );
	}
	for( size_t i = 0; i < miss.size(); ++i )
	{
		FORWARD_HR( resolve( desc.m_missNames[i], miss[i] ) );
		missData = std::max( missData, miss[i].dataSize );
	}
	for( size_t i = 0; i < hit.size(); ++i )
	{
		FORWARD_HR( resolve( desc.m_hitGroupNames[i], hit[i] ) );
		hitData = std::max( hitData, hit[i].dataSize );
	}
	if( rayGen.empty() )
	{
		return E_INVALIDARG;
	}

	const VkDeviceSize baseAlignment = properties.shaderGroupBaseAlignment;
	const VkDeviceSize handleAlignment = properties.shaderGroupHandleAlignment;
	const VkDeviceSize rayGenStride = AlignUpVulkan( handleSize + rayGenData, handleAlignment );
	const VkDeviceSize rayGenSpacing = AlignUpVulkan( rayGenStride, baseAlignment );
	const VkDeviceSize missStride = AlignUpVulkan( handleSize + missData, handleAlignment );
	const VkDeviceSize hitStride = AlignUpVulkan( handleSize + hitData, handleAlignment );
	if( rayGenStride > properties.maxShaderGroupStride || missStride > properties.maxShaderGroupStride || hitStride > properties.maxShaderGroupStride )
	{
		CCP_AL_LOGERR( "Tr2RtShaderTableAL: a shader record is larger than maxShaderGroupStride (%u)", properties.maxShaderGroupStride );
		return E_INVALIDARG;
	}
	const VkDeviceSize missOffset = AlignUpVulkan( rayGenSpacing * rayGen.size(), baseAlignment );
	const VkDeviceSize hitOffset = AlignUpVulkan( missOffset + missStride * miss.size(), baseAlignment );
	const VkDeviceSize total = hitOffset + hitStride * hit.size();

	// This frame's upload memory: the engine builds every table in the frame it traces with it
	// (Tr2RaytracingManager, Tr2LightManager, a stack-local one per froxel-fog dispatch), and the
	// memory stays valid until this frame slot's fence has passed.
	VkDeviceAddress address = 0;
	void* upload = nullptr;
	FORWARD_HR( renderContext.AllocateUploadVulkan( total, baseAlignment, address, upload ) );
	uint8_t* const mapped = static_cast<uint8_t*>( upload );
	memset( mapped, 0, size_t( total ) );

	auto write = [&]( uint8_t* destination, const Record& record ) -> ALResult {
		memcpy( destination, rt.GetGroupHandleVulkan( record.group->group ), size_t( handleSize ) );
		if( record.constants )
		{
			return record.constants->ReadCurrentVulkan( destination + handleSize, record.dataSize );
		}
		return S_OK;
	};
	ALResult hr = S_OK;
	for( size_t i = 0; i < rayGen.size() && SUCCEEDED( hr ); ++i )
	{
		hr = write( mapped + i * rayGenSpacing, rayGen[i] );
	}
	for( size_t i = 0; i < miss.size() && SUCCEEDED( hr ); ++i )
	{
		hr = write( mapped + missOffset + i * missStride, miss[i] );
	}
	for( size_t i = 0; i < hit.size() && SUCCEEDED( hr ); ++i )
	{
		hr = write( mapped + hitOffset + i * hitStride, hit[i] );
	}
	if( FAILED( hr ) )
	{
		return hr; // the upload memory goes back with the frame
	}

	m_owner = &renderContext;
	m_frame = renderContext.GetRecordingFrameNumber();
	m_address = address;
	m_rayGenNames.clear();
	for( auto& record : desc.m_rayGenNames )
	{
		m_rayGenNames.push_back( record.name ? record.name : L"" );
	}
	m_rayGenSpacing = rayGenSpacing;
	m_rayGenStride = rayGenStride;
	m_missRegion = {};
	if( !miss.empty() )
	{
		m_missRegion = { address + missOffset, missStride, missStride * miss.size() };
	}
	m_hitRegion = {};
	if( !hit.empty() )
	{
		m_hitRegion = { address + hitOffset, hitStride, hitStride * hit.size() };
	}
	return S_OK;
}

bool Tr2RtShaderTableAL::GetRayGenRegionVulkan( const wchar_t* name, VkStridedDeviceAddressRegionKHR& region ) const
{
	if( !IsValid() || !name )
	{
		return false;
	}
	for( size_t i = 0; i < m_rayGenNames.size(); ++i )
	{
		if( m_rayGenNames[i] == name )
		{
			region = { m_address + i * m_rayGenSpacing, m_rayGenStride, m_rayGenStride };
			return true;
		}
	}
	return false;
}

void Tr2RtShaderTableAL::Destroy()
{
	// Nothing to free: the records live in the frame's upload memory.
	m_owner = nullptr;
	m_frame = 0;
	m_address = 0;
	m_rayGenNames.clear();
	m_rayGenSpacing = 0;
	m_rayGenStride = 0;
	m_missRegion = {};
	m_hitRegion = {};
}

bool Tr2RtShaderTableAL::IsValid() const
{
	return m_owner != nullptr && m_address != 0;
}

Tr2ALMemoryType Tr2RtShaderTableAL::GetMemoryClass() const
{
	return AL_MEMORY_MANAGED;
}

void Tr2RtShaderTableAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
{
	description["type"] = "Tr2RtShaderTableAL";
}
}

#endif
