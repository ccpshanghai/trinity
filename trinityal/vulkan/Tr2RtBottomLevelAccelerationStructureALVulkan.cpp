// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2RtBottomLevelAccelerationStructureALVulkan.h"
#include "Tr2RtUtilitiesVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2BufferALVulkan.h"
#include "UtilitiesVulkan.h"
#include "../ALLog.h"

#include <algorithm>

namespace
{
// The triangles of `geometry` as Vulkan describes them, addressed through the buffers'
// device addresses. The vertex address adds the buffer's current offset: a renaming buffer
// keeps its contents in a slot (Tr2BufferALVulkan.h). E_INVALIDARG with `reason` saying why
// when the geometry cannot be built from.
ALResult DescribeTriangles( const Tr2RtGeometryAL& geometry, Tr2RtBlasGeometryFlags::Type geometryFlags, VkAccelerationStructureGeometryKHR& result, uint32_t& primitiveCount, const char*& reason )
{
	reason = nullptr;
	auto vb = geometry.positions.m_vertexBuffer.TrinityALImpl_GetObject();
	auto ib = geometry.indices.m_indexBuffer.TrinityALImpl_GetObject();
	if( !vb || !ib )
	{
		reason = "no vertex or no index buffer";
		return E_INVALIDARG;
	}
	const VkDeviceAddress vbAddress = vb->GetDeviceAddressVulkan();
	const VkDeviceAddress ibAddress = ib->GetDeviceAddressVulkan();
	if( !vbAddress || !ibAddress )
	{
		reason = "the vertex or index buffer has no device address (created without a build-input usage, or before the device could build acceleration structures)";
		return E_INVALIDARG;
	}
	const VkFormat vertexFormat = TrinityALImpl::GetVulkanFormat( geometry.positions.m_positionFormat );
	if( vertexFormat == VK_FORMAT_UNDEFINED )
	{
		reason = "the position format has no Vulkan format";
		return E_INVALIDARG;
	}
	if( geometry.positions.m_stride == 0 || geometry.positions.m_vertexCount == 0 )
	{
		reason = "a zero vertex stride or vertex count";
		return E_INVALIDARG;
	}
	if( geometry.indices.m_stride != 2 && geometry.indices.m_stride != 4 )
	{
		reason = "an index stride other than 2 or 4";
		return E_INVALIDARG;
	}

	result = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	result.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	// The same test DX12 makes: any geometry flag means opaque.
	result.flags = geometryFlags ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
	auto& triangles = result.geometry.triangles;
	triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	triangles.vertexFormat = vertexFormat;
	triangles.vertexData.deviceAddress = vbAddress + vb->GetOffsetVulkan() + VkDeviceSize( geometry.positions.m_vertexOffset ) * geometry.positions.m_stride + geometry.positions.m_positionOffset;
	triangles.vertexStride = geometry.positions.m_stride;
	triangles.maxVertex = geometry.positions.m_vertexCount - 1;
	triangles.indexType = geometry.indices.m_stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
	triangles.indexData.deviceAddress = ibAddress + ib->GetOffsetVulkan() + VkDeviceSize( geometry.indices.m_indexOffset ) * geometry.indices.m_stride;
	triangles.transformData.deviceAddress = 0;
	primitiveCount = geometry.indices.m_indexCount / 3;
	if( !primitiveCount )
	{
		reason = "fewer than three indices";
		return E_INVALIDARG;
	}
	return S_OK;
}

// Create's DescribeTriangles failures, once per reason: the engine logs a generic line for a
// skinned BLAS only and ignores a static one's result (Tr2RaytracingGeometry.cpp).
ALResult DescribeTrianglesForCreate( const Tr2RtGeometryAL& geometry, Tr2RtBlasGeometryFlags::Type geometryFlags, VkAccelerationStructureGeometryKHR& result, uint32_t& primitiveCount, const char* which )
{
	const char* reason = nullptr;
	const ALResult hr = DescribeTriangles( geometry, geometryFlags, result, primitiveCount, reason );
	static char s_site;
	if( FAILED( hr ) && reason && CCP::LogOnceAL( &s_site, uint64_t( uintptr_t( reason ) ) ) )
	{
		CCP_AL_LOGERR( "Tr2RtBottomLevelAccelerationStructureAL::Create: no BLAS from the %s geometry: %s", which, reason );
	}
	return hr;
}

VkAccelerationStructureBuildGeometryInfoKHR BuildInfo( const VkAccelerationStructureGeometryKHR* geometry, Tr2RtBuildFlags::Type buildFlags )
{
	VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	// 0x1-0x10 are D3D12's values and Vulkan's alike; PERFORM_UPDATE (0x20) is a build mode
	// on Vulkan, not a flag.
	info.flags = VkBuildAccelerationStructureFlagsKHR( buildFlags & ~Tr2RtBuildFlags::PERFORM_UPDATE );
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = geometry;
	return info;
}
}


namespace TrinityALImpl
{
Tr2RtBottomLevelAccelerationStructureAL::Tr2RtBottomLevelAccelerationStructureAL() :
	m_owner( nullptr ),
	m_accelerationStructure( VK_NULL_HANDLE ),
	m_address( 0 ),
	m_buildScratchSize( 0 ),
	m_updateScratchSize( 0 ),
	m_key{},
	m_geometryFlags( Tr2RtBlasGeometryFlags::NONE ),
	m_buildFlags( Tr2RtBuildFlags::NONE )
{
}

Tr2RtBottomLevelAccelerationStructureAL::~Tr2RtBottomLevelAccelerationStructureAL()
{
	Destroy();
}

ALResult Tr2RtBottomLevelAccelerationStructureAL::Create( const Tr2RtGeometryAL& geometry, const Tr2RtGeometryAL& capacity, Tr2RtBlasGeometryFlags::Type geometryFlags, Tr2RtBuildFlags::Type buildFlags, Tr2PrimaryRenderContextAL& renderContext )
{
	Destroy();
	if( !renderContext.IsValid() || !renderContext.GetCaps().SupportsAccelerationStructuresVulkan() )
	{
		return E_INVALIDCALL;
	}
	if( !geometry.positions.IsValid() || !geometry.indices.IsValid() || !capacity.positions.IsValid() || !capacity.indices.IsValid() )
	{
		return E_INVALIDARG;
	}

	VkAccelerationStructureGeometryKHR capacityGeometry;
	uint32_t capacityPrimitives = 0;
	FORWARD_HR( DescribeTrianglesForCreate( capacity, geometryFlags, capacityGeometry, capacityPrimitives, "capacity" ) );
	VkAccelerationStructureGeometryKHR buildGeometry;
	uint32_t buildPrimitives = 0;
	FORWARD_HR( DescribeTrianglesForCreate( geometry, geometryFlags, buildGeometry, buildPrimitives, "build" ) );
	if( buildPrimitives > capacityPrimitives || buildGeometry.geometry.triangles.maxVertex > capacityGeometry.geometry.triangles.maxVertex )
	{
		return E_INVALIDARG;
	}

	auto& rt = renderContext.m_raytracing;
	VkAccelerationStructureBuildGeometryInfoKHR info = BuildInfo( &capacityGeometry, buildFlags );
	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	rt.getAccelerationStructureBuildSizes( renderContext.m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &capacityPrimitives, &sizes );
	if( !sizes.accelerationStructureSize )
	{
		return E_INVALIDCALL;
	}

	FORWARD_HR( m_storage.Create( sizes.accelerationStructureSize, 1, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, renderContext ) );
	m_owner = &renderContext;

	VkAccelerationStructureCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
	createInfo.buffer = m_storage.buffer;
	createInfo.size = m_storage.size;
	createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if( FAILED( Vk2Al( rt.createAccelerationStructure( renderContext.m_device, &createInfo, nullptr, &m_accelerationStructure ) ) ) )
	{
		Destroy();
		return E_FAIL;
	}

	// Sized for the capacity, which covers the build below (fewer or as many primitives).
	m_buildScratchSize = sizes.buildScratchSize;
	m_updateScratchSize = sizes.updateScratchSize;
	VkDeviceAddress scratch = 0;
	if( FAILED( renderContext.AllocateRtScratchVulkan( m_buildScratchSize, scratch ) ) )
	{
		Destroy();
		return E_OUTOFMEMORY;
	}

	info = BuildInfo( &buildGeometry, buildFlags );
	info.dstAccelerationStructure = m_accelerationStructure;
	info.scratchData.deviceAddress = scratch;
	VkAccelerationStructureBuildRangeInfoKHR range = { buildPrimitives, 0, 0, 0 };
	RecordAccelerationStructureBuildVulkan( renderContext, info, range, m_buildScratchSize );

	VkAccelerationStructureDeviceAddressInfoKHR addressInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
	addressInfo.accelerationStructure = m_accelerationStructure;
	m_address = rt.getAccelerationStructureDeviceAddress( renderContext.m_device, &addressInfo );

	const auto& triangles = buildGeometry.geometry.triangles;
	m_key = { uint32_t( triangles.vertexStride ), triangles.maxVertex + 1, triangles.vertexFormat, triangles.indexType, geometry.indices.m_indexCount };
	m_geometryFlags = geometryFlags;
	m_buildFlags = buildFlags;
	return S_OK;
}

ALResult Tr2RtBottomLevelAccelerationStructureAL::Update( const Tr2RtGeometryAL& geometry, Tr2RenderContextAL& renderContext )
{
	// Only a structure built with ALLOW_UPDATE, as before the scratch was shared: anything else
	// answers E_INVALIDCALL, which the engine takes as "create a new one".
	if( !IsValid() || !HasFlag( m_buildFlags, Tr2RtBuildFlags::ALLOW_UPDATE ) )
	{
		return E_INVALIDCALL;
	}
	if( !renderContext.IsValid() )
	{
		return E_INVALIDARG;
	}

	// No log here: a failed Update is the engine's signal to create a new structure.
	VkAccelerationStructureGeometryKHR buildGeometry;
	uint32_t buildPrimitives = 0;
	const char* reason = nullptr;
	FORWARD_HR( DescribeTriangles( geometry, m_geometryFlags, buildGeometry, buildPrimitives, reason ) );
	const auto& triangles = buildGeometry.geometry.triangles;
	const GeometryKey key = { uint32_t( triangles.vertexStride ), triangles.maxVertex + 1, triangles.vertexFormat, triangles.indexType, geometry.indices.m_indexCount };

	VkAccelerationStructureBuildGeometryInfoKHR info = BuildInfo( &buildGeometry, m_buildFlags );
	const bool rebuild = !( key == m_key );
	VkDeviceSize scratchSize = m_updateScratchSize;
	if( rebuild )
	{
		// A different LOD: rebuild into the same structure if it fits, as DX12 does.
		VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
		m_owner->m_raytracing.getAccelerationStructureBuildSizes( m_owner->m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &buildPrimitives, &sizes );
		if( !sizes.accelerationStructureSize || sizes.accelerationStructureSize > m_storage.size )
		{
			return E_INVALIDARG;
		}
		scratchSize = sizes.buildScratchSize;
	}
	else
	{
		// Same topology: a refit. Vulkan allows it only from a structure built with
		// ALLOW_UPDATE and with the counts, formats and index type of that build -- the key.
		info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
		info.srcAccelerationStructure = m_accelerationStructure;
	}
	VkDeviceAddress scratch = 0;
	FORWARD_HR( m_owner->AllocateRtScratchVulkan( scratchSize, scratch ) );
	info.dstAccelerationStructure = m_accelerationStructure;
	info.scratchData.deviceAddress = scratch;
	VkAccelerationStructureBuildRangeInfoKHR range = { buildPrimitives, 0, 0, 0 };
	RecordAccelerationStructureBuildVulkan( renderContext, info, range, scratchSize );
	m_key = key;
	return S_OK;
}

ALResult Tr2RtBottomLevelAccelerationStructureAL::CompactBlas( Tr2PrimaryRenderContextAL& )
{
	return E_NOTIMPL;
}

void Tr2RtBottomLevelAccelerationStructureAL::Destroy()
{
	if( m_owner )
	{
		// The structure ahead of its storage: the pending list drains in order.
		m_owner->DestroyLaterVulkan( m_accelerationStructure, m_owner->m_raytracing.destroyAccelerationStructure );
		m_storage.Destroy( *m_owner );
		m_owner = nullptr;
	}
	m_accelerationStructure = VK_NULL_HANDLE;
	m_storage = RtBufferVulkan();
	m_address = 0;
	m_buildScratchSize = 0;
	m_updateScratchSize = 0;
	m_key = {};
}

bool Tr2RtBottomLevelAccelerationStructureAL::IsValid() const
{
	return m_accelerationStructure != VK_NULL_HANDLE && m_address != 0;
}

Tr2ALMemoryType Tr2RtBottomLevelAccelerationStructureAL::GetMemoryClass() const
{
	return AL_MEMORY_MANAGED;
}

void Tr2RtBottomLevelAccelerationStructureAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
{
	description["type"] = "Tr2RtBottomLevelAccelerationStructureAL";
}
}

#endif
