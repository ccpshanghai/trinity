// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2RtTopLevelAccelerationStructureALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2BufferALVulkan.h"
#include "UtilitiesVulkan.h"

namespace
{
// VkAccelerationStructureInstanceKHR's layout is D3D12_RAYTRACING_INSTANCE_DESC's, and the two
// instance flags the engine uses carry the DXR values.
static_assert( sizeof( VkAccelerationStructureInstanceKHR ) == 64, "instance descriptor size" );
static_assert( uint32_t( Tr2RtInstanceAL::FORCE_NON_OPAQUE ) == uint32_t( VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR ), "instance flag values" );

ALResult FillInstances( uint8_t* destination, size_t count, const Tr2RtInstanceAL* instances )
{
	auto* data = reinterpret_cast<VkAccelerationStructureInstanceKHR*>( destination );
	for( size_t i = 0; i < count; ++i )
	{
		if( !instances[i].blas || !instances[i].blas->IsValid() )
		{
			return E_INVALIDARG;
		}
		VkAccelerationStructureInstanceKHR d = {};
		memcpy( &d.transform, instances[i].transform, sizeof( d.transform ) );
		d.instanceCustomIndex = uint32_t( i );
		d.mask = 0xff;
		d.instanceShaderBindingTableRecordOffset = instances[i].materialIndex;
		// DX12 sets TRIANGLE_FRONT_COUNTERCLOCKWISE on every instance; same bit here.
		d.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR | uint32_t( instances[i].flags );
		d.accelerationStructureReference = instances[i].blas->GetDeviceAddressVulkan();
		data[i] = d;
	}
	return S_OK;
}

VkAccelerationStructureGeometryKHR InstancesGeometry( VkDeviceAddress instances )
{
	VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geometry.geometry.instances.arrayOfPointers = VK_FALSE;
	geometry.geometry.instances.data.deviceAddress = instances;
	return geometry;
}

VkAccelerationStructureBuildGeometryInfoKHR BuildInfo( const VkAccelerationStructureGeometryKHR* geometry, Tr2RtBuildFlags::Type buildFlags )
{
	VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	info.flags = VkBuildAccelerationStructureFlagsKHR( buildFlags & ~Tr2RtBuildFlags::PERFORM_UPDATE );
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = geometry;
	return info;
}
}


namespace TrinityALImpl
{
Tr2RtTopLevelAccelerationStructureAL::Tr2RtTopLevelAccelerationStructureAL() :
	m_owner( nullptr ),
	m_capacity( 0 ),
	m_builtCount( 0 ),
	m_buildFlags( Tr2RtBuildFlags::NONE )
{
}

Tr2RtTopLevelAccelerationStructureAL::~Tr2RtTopLevelAccelerationStructureAL()
{
	Destroy();
}

ALResult Tr2RtTopLevelAccelerationStructureAL::Create( const size_t count, const Tr2RtInstanceAL* instances, Tr2RtBuildFlags::Type buildFlags, Tr2PrimaryRenderContextAL& renderContext )
{
	Destroy();
	if( !renderContext.IsValid() || !renderContext.GetCaps().SupportsRaytracing() )
	{
		return E_INVALIDCALL;
	}
	if( count == 0 || !instances )
	{
		return E_INVALIDARG;
	}

	const size_t capacity = ( count + 127 ) / 128 * 128;
	m_owner = &renderContext;
	m_capacity = capacity;
	m_buildFlags = buildFlags;

	// Sized for the capacity (instance data address only has to be valid for the query's
	// purposes; the count is what matters).
	VkAccelerationStructureGeometryKHR geometry = InstancesGeometry( 0 );
	VkAccelerationStructureBuildGeometryInfoKHR info = BuildInfo( &geometry, buildFlags );
	const uint32_t maxInstances = uint32_t( capacity );
	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	renderContext.m_raytracing.getAccelerationStructureBuildSizes( renderContext.m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &maxInstances, &sizes );
	if( !sizes.accelerationStructureSize )
	{
		Destroy();
		return E_INVALIDCALL;
	}

	ALResult hr = m_buffer.Create(
		Tr2BufferDescriptionAL( 1,
								uint32_t( sizes.accelerationStructureSize ),
								Tr2GpuUsage::ACCELERATION_STRUCTURE | Tr2GpuUsage::SHADER_RESOURCE | Tr2GpuUsage::UNORDERED_ACCESS,
								Tr2CpuUsage::NONE ),
		nullptr,
		renderContext );
	if( FAILED( hr ) || !m_buffer.IsValid() )
	{
		Destroy();
		return FAILED( hr ) ? hr : ALResult( E_INVALIDCALL );
	}

	auto* storage = m_buffer.TrinityALImpl_GetObject();
	VkAccelerationStructureCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
	createInfo.buffer = storage->GetBufferVulkan();
	createInfo.size = sizes.accelerationStructureSize;
	createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	VkAccelerationStructureKHR accelerationStructure = VK_NULL_HANDLE;
	if( FAILED( Vk2Al( renderContext.m_raytracing.createAccelerationStructure( renderContext.m_device, &createInfo, nullptr, &accelerationStructure ) ) ) )
	{
		Destroy();
		return E_FAIL;
	}
	storage->SetAccelerationStructureVulkan( accelerationStructure );

	const VkDeviceSize scratchSize = HasFlag( buildFlags, Tr2RtBuildFlags::ALLOW_UPDATE ) ? std::max( sizes.buildScratchSize, sizes.updateScratchSize ) : sizes.buildScratchSize;
	if( FAILED( m_scratch.Create( scratchSize, renderContext.m_accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, renderContext ) ) )
	{
		Destroy();
		return E_OUTOFMEMORY;
	}

	hr = Build( count, instances, renderContext );
	if( FAILED( hr ) )
	{
		Destroy();
	}
	return hr;
}

ALResult Tr2RtTopLevelAccelerationStructureAL::Update( const size_t count, const Tr2RtInstanceAL* instances, Tr2RenderContextAL& renderContext )
{
	if( !IsValid() )
	{
		return E_INVALIDCALL;
	}
	if( !renderContext.IsValid() || count == 0 || !instances || count > m_capacity )
	{
		return E_INVALIDARG;
	}
	return Build( count, instances, renderContext );
}

ALResult Tr2RtTopLevelAccelerationStructureAL::AcquireInstanceBuffer( RtBufferVulkan*& buffer )
{
	const uint64_t rendered = m_owner->GetRenderedFrameNumber();
	const uint64_t recording = m_owner->GetRecordingFrameNumber();
	for( auto it = begin( m_instanceBuffers ); it != end( m_instanceBuffers ); ++it )
	{
		if( it->frame <= rendered )
		{
			it->frame = recording;
			buffer = &it->buffer;
			return S_OK;
		}
	}
	InstanceBuffer instanceBuffer;
	instanceBuffer.frame = recording;
	// 16: the instance array's address alignment (VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03715).
	FORWARD_HR( instanceBuffer.buffer.Create( m_capacity * sizeof( VkAccelerationStructureInstanceKHR ), 16, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, *m_owner ) );
	m_instanceBuffers.push_back( instanceBuffer );
	buffer = &m_instanceBuffers.back().buffer;
	return S_OK;
}

ALResult Tr2RtTopLevelAccelerationStructureAL::Build( size_t count, const Tr2RtInstanceAL* instances, Tr2RenderContextAL& renderContext )
{
	RtBufferVulkan* instanceBuffer = nullptr;
	FORWARD_HR( AcquireInstanceBuffer( instanceBuffer ) );
	// Host-coherent and written before the frame is submitted, so no barrier is needed for
	// the build to see it.
	FORWARD_HR( FillInstances( instanceBuffer->mapped, count, instances ) );

	VkAccelerationStructureGeometryKHR geometry = InstancesGeometry( instanceBuffer->address );
	VkAccelerationStructureBuildGeometryInfoKHR info = BuildInfo( &geometry, m_buildFlags );
	const VkAccelerationStructureKHR accelerationStructure = m_buffer.TrinityALImpl_GetObject()->GetAccelerationStructureVulkan();
	// A refit needs the same instance count as the build it refits; anything else is a full
	// rebuild in place, which is what the engine's TLAS (PREFER_FAST_TRACE only) always gets.
	if( HasFlag( m_buildFlags, Tr2RtBuildFlags::ALLOW_UPDATE ) && m_builtCount == count )
	{
		info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
		info.srcAccelerationStructure = accelerationStructure;
	}
	info.dstAccelerationStructure = accelerationStructure;
	info.scratchData.deviceAddress = m_scratch.address;
	VkAccelerationStructureBuildRangeInfoKHR range = { uint32_t( count ), 0, 0, 0 };
	RecordAccelerationStructureBuildVulkan( renderContext, info, range );
	m_builtCount = count;
	return S_OK;
}

void Tr2RtTopLevelAccelerationStructureAL::Destroy()
{
	if( m_owner )
	{
		for( auto it = begin( m_instanceBuffers ); it != end( m_instanceBuffers ); ++it )
		{
			it->buffer.Destroy( *m_owner );
		}
		if( m_scratch.buffer )
		{
			m_scratch.Destroy( *m_owner );
		}
		m_owner = nullptr;
	}
	m_instanceBuffers.clear();
	m_scratch = RtBufferVulkan();
	// The buffer's Destroy queues the structure built over it, then the storage.
	m_buffer = ::Tr2BufferAL();
	m_capacity = 0;
	m_builtCount = 0;
	m_buildFlags = Tr2RtBuildFlags::NONE;
}

bool Tr2RtTopLevelAccelerationStructureAL::IsValid() const
{
	return m_owner != nullptr && m_buffer.IsValid() && m_builtCount != 0;
}

Tr2ALMemoryType Tr2RtTopLevelAccelerationStructureAL::GetMemoryClass() const
{
	return AL_MEMORY_MANAGED;
}

void Tr2RtTopLevelAccelerationStructureAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
{
	description["type"] = "Tr2RtTopLevelAccelerationStructureAL";
}

const ::Tr2BufferAL& Tr2RtTopLevelAccelerationStructureAL::GetBuffer() const
{
	return m_buffer;
}

size_t Tr2RtTopLevelAccelerationStructureAL::GetCapacity() const
{
	return m_capacity;
}
}

#endif
