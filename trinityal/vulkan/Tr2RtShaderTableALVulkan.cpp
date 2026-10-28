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

#include <algorithm>
#include <atomic>

namespace
{
VkDeviceSize AlignUp( VkDeviceSize value, VkDeviceSize alignment )
{
	return alignment ? ( value + alignment - 1 ) / alignment * alignment : value;
}

typedef Tr2PrimaryRenderContextAL::RtTableBufferVulkan TableBuffer;

// The smallest allocation, and the growth step: tables differ in size from frame to frame (a
// hull's hit groups come and go), and a buffer sized to the byte would rarely fit the next one.
const VkDeviceSize TABLE_BUFFER_GRANULE = 64 * 1024;
// Free buffers kept beyond this are released: each table in flight needs one, and a frame
// builds two or three tables with VIRTUAL_FRAMES frames in flight.
const size_t TABLE_BUFFER_POOL_LIMIT = 16;

// A free buffer of at least `size` bytes whose last table has finished rendering -- the
// smallest such -- or a new one.
ALResult AcquireTableBuffer( VkDeviceSize size, Tr2PrimaryRenderContextAL& renderContext, TableBuffer& result )
{
	auto& pool = renderContext.m_rtTableBuffers;
	const uint64_t rendered = renderContext.GetRenderedFrameNumber();
	auto best = pool.end();
	for( auto it = pool.begin(); it != pool.end(); ++it )
	{
		if( it->size >= size && it->lastUsedFrame <= rendered && ( best == pool.end() || it->size < best->size ) )
		{
			best = it;
		}
	}
	if( best != pool.end() )
	{
		result = *best;
		pool.erase( best );
		return S_OK;
	}

	TableBuffer created;
	created.size = AlignUp( size, TABLE_BUFFER_GRANULE );
	FORWARD_HR( TrinityALImpl::CreateBuffer( created.buffer, created.memory, size_t( created.size ), VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VkMemoryPropertyFlagBits( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ), renderContext ) );
	VkBufferDeviceAddressInfo addressInfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	addressInfo.buffer = created.buffer;
	created.address = renderContext.m_raytracing.getBufferDeviceAddress( renderContext.m_device, &addressInfo );
	void* mapped = nullptr;
	if( FAILED( Vk2Al( vkMapMemory( renderContext.m_device, created.memory, 0, VK_WHOLE_SIZE, 0, &mapped ) ) ) )
	{
		renderContext.DestroyLaterVulkan( created.buffer, &vkDestroyBuffer );
		renderContext.DestroyLaterVulkan( created.memory, &vkFreeMemory );
		return E_FAIL;
	}
	created.mapped = static_cast<uint8_t*>( mapped );
	result = created;
	return S_OK;
}

// Back to the pool, not to be handed out before the frame recording now has rendered: the
// table may have been used by a dispatch recorded in it.
void ReleaseTableBuffer( const TableBuffer& buffer, Tr2PrimaryRenderContextAL& renderContext )
{
	auto& pool = renderContext.m_rtTableBuffers;
	TableBuffer released = buffer;
	released.lastUsedFrame = renderContext.GetRecordingFrameNumber();
	pool.push_back( released );
	if( pool.size() > TABLE_BUFFER_POOL_LIMIT )
	{
		// The least recently used goes, through the deferred path because it may be in flight.
		auto oldest = std::min_element( pool.begin(), pool.end(), []( const TableBuffer& a, const TableBuffer& b ) {
			return a.lastUsedFrame < b.lastUsedFrame;
		} );
		renderContext.DestroyLaterVulkan( oldest->buffer, &vkDestroyBuffer );
		renderContext.DestroyLaterVulkan( oldest->memory, &vkFreeMemory );
		pool.erase( oldest );
	}
}
}


namespace TrinityALImpl
{
Tr2RtShaderTableAL::Tr2RtShaderTableAL() :
	m_owner( nullptr ),
	m_buffer( VK_NULL_HANDLE ),
	m_memory( VK_NULL_HANDLE ),
	m_bufferSize( 0 ),
	m_bufferAddress( 0 ),
	m_bufferMapped( nullptr ),
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
		const ::Tr2ConstantBufferAL* cb = reg < 8 ? shaderRecord.material.m_constants[reg] : nullptr;
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
	const VkDeviceSize rayGenStride = AlignUp( handleSize + rayGenData, handleAlignment );
	const VkDeviceSize rayGenSpacing = AlignUp( rayGenStride, baseAlignment );
	const VkDeviceSize missStride = AlignUp( handleSize + missData, handleAlignment );
	const VkDeviceSize hitStride = AlignUp( handleSize + hitData, handleAlignment );
	if( rayGenStride > properties.maxShaderGroupStride || missStride > properties.maxShaderGroupStride || hitStride > properties.maxShaderGroupStride )
	{
		CCP_AL_LOGERR( "Tr2RtShaderTableAL: a shader record is larger than maxShaderGroupStride (%u)", properties.maxShaderGroupStride );
		return E_INVALIDARG;
	}
	const VkDeviceSize missOffset = AlignUp( rayGenSpacing * rayGen.size(), baseAlignment );
	const VkDeviceSize hitOffset = AlignUp( missOffset + missStride * miss.size(), baseAlignment );
	const VkDeviceSize total = hitOffset + hitStride * hit.size();

	// Over-asked by the alignment: nothing promises the buffer's address is base-aligned.
	TableBuffer buffer;
	FORWARD_HR( AcquireTableBuffer( total + ( baseAlignment > 1 ? baseAlignment - 1 : 0 ), renderContext, buffer ) );
	const VkDeviceAddress address = AlignUp( buffer.address, baseAlignment );
	uint8_t* const mapped = buffer.mapped + ( address - buffer.address );
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
		ReleaseTableBuffer( buffer, renderContext );
		return hr;
	}

	m_owner = &renderContext;
	m_buffer = buffer.buffer;
	m_memory = buffer.memory;
	m_bufferSize = buffer.size;
	m_bufferAddress = buffer.address;
	m_bufferMapped = buffer.mapped;
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
	if( m_owner && m_buffer != VK_NULL_HANDLE )
	{
		TableBuffer buffer;
		buffer.buffer = m_buffer;
		buffer.memory = m_memory;
		buffer.size = m_bufferSize;
		buffer.address = m_bufferAddress;
		buffer.mapped = m_bufferMapped;
		ReleaseTableBuffer( buffer, *m_owner );
	}
	m_owner = nullptr;
	m_buffer = VK_NULL_HANDLE;
	m_memory = VK_NULL_HANDLE;
	m_bufferSize = 0;
	m_bufferAddress = 0;
	m_bufferMapped = nullptr;
	m_address = 0;
	m_rayGenNames.clear();
	m_rayGenSpacing = 0;
	m_rayGenStride = 0;
	m_missRegion = {};
	m_hitRegion = {};
}

bool Tr2RtShaderTableAL::IsValid() const
{
	return m_owner != nullptr && m_buffer != VK_NULL_HANDLE;
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
