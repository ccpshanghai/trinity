// Copyright © 2023 CCP ehf.

#if TRINITY_PLATFORM == TRINITY_METAL

#include "MetalResourceArray.h"
#include "MetalUtils.h"
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>


namespace TrinityALImpl
{

void ResourceArrayArgumentBuffer::Initialize( uint32_t initialSize, id<MTLDevice> device )
{
	m_device = device;
	Grow( initialSize );
}

ResourceArrayArgumentBuffer::operator bool() const
{
	return m_buffer != nil;
}

uint32_t ResourceArrayArgumentBuffer::Allocate( GpuResourceID gpuResourceID )
{
	if( !m_buffer )
	{
		return 0xffffffff;
	}
	if( m_free.empty() )
	{
		Grow( m_size * 2 );
	}
	if( m_free.empty() )
	{
		CCP_ASSERT( false );
		return 0xffffffff;
	}
	auto index = m_free.back();
	m_free.pop_back();
	m_data[index] = gpuResourceID;
	reinterpret_cast<GpuResourceID*>( m_buffer.contents )[index] = gpuResourceID;
	// CPU writes m_buffer.contents directly above; sync it here. Managed needs the
	// explicit call, Shared (spec D7) is a no-op, and didModifyRange is macOS-only API.
#if TARGET_OS_OSX
	if( m_buffer.storageMode == MTLStorageModeManaged )
	{
		[m_buffer didModifyRange:NSMakeRange( index * sizeof( GpuResourceID ), sizeof( GpuResourceID ) )];
	}
#endif
	return index;
}

void ResourceArrayArgumentBuffer::Deallocate( uint32_t index )
{
	if( index == 0xffffffff )
	{
		return;
	}
	CCP_ASSERT( index < m_size );
	m_pendingFree.push_back( { index, m_recordingFrame } );
}

void ResourceArrayArgumentBuffer::Grow( uint32_t size )
{
	if( size <= m_size )
	{
		return;
	}
	for( uint32_t i = size; i > m_size; --i )
	{
		m_free.push_back( i - 1 );
	}
	m_data.resize( size );
	// This buffer is subsequently CPU-written directly via m_buffer.contents in
	// Allocate() above, which pairs with the guarded didModifyRange there.
	id<MTLBuffer> buffer;
	buffer = [m_device newBufferWithBytes:m_data.data()
								   length:size * sizeof( GpuResourceID )
								  options:MetalDefaultUploadStorageMode( m_device )];
	m_buffer = buffer;
	m_size = size;
}

void ResourceArrayArgumentBuffer::SetFrameIndices( uint64_t recordingFrame, uint64_t renderedFrame )
{
	m_recordingFrame = recordingFrame;
	size_t i = 0;
	while( i < m_pendingFree.size() )
	{
		auto& it = m_pendingFree[i];
		if( it.second < renderedFrame )
		{
			m_free.push_back( it.first );
			it = m_pendingFree.back();
			m_pendingFree.pop_back();
		}
		else
		{
			++i;
		}
	}
}

id<MTLBuffer> ResourceArrayArgumentBuffer::GetBuffer() const
{
	return m_buffer;
}

}

#endif
