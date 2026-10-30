// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2RtBottomLevelAccelerationStructureALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2BufferALVulkan.h"
#include "UtilitiesVulkan.h"
#include "../ALLog.h"

#include <algorithm>

namespace
{
VkDeviceSize AlignUp( VkDeviceSize value, VkDeviceSize alignment )
{
	return alignment ? ( value + alignment - 1 ) / alignment * alignment : value;
}

// The triangles of `geometry` as Vulkan describes them, addressed through the buffers'
// device addresses. The vertex address adds the buffer's current offset: a renaming buffer
// keeps its contents in a slot (Tr2BufferALVulkan.h). Fails when either buffer has no
// address, which is a buffer created before the device had ray tracing or without a usage
// that can be a build input.
ALResult DescribeTriangles( const Tr2RtGeometryAL& geometry, Tr2RtBlasGeometryFlags::Type geometryFlags, VkAccelerationStructureGeometryKHR& result, uint32_t& primitiveCount )
{
	auto vb = geometry.positions.m_vertexBuffer.TrinityALImpl_GetObject();
	auto ib = geometry.indices.m_indexBuffer.TrinityALImpl_GetObject();
	if( !vb || !ib )
	{
		return E_INVALIDARG;
	}
	const VkDeviceAddress vbAddress = vb->GetDeviceAddressVulkan();
	const VkDeviceAddress ibAddress = ib->GetDeviceAddressVulkan();
	if( !vbAddress || !ibAddress )
	{
		return E_INVALIDARG;
	}
	const VkFormat vertexFormat = TrinityALImpl::GetVulkanFormat( geometry.positions.m_positionFormat );
	if( vertexFormat == VK_FORMAT_UNDEFINED || geometry.positions.m_stride == 0 || geometry.positions.m_vertexCount == 0 )
	{
		return E_INVALIDARG;
	}
	if( geometry.indices.m_stride != 2 && geometry.indices.m_stride != 4 )
	{
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
	return primitiveCount ? S_OK : E_INVALIDARG;
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
ALResult RtBufferVulkan::Create( VkDeviceSize requestedSize, VkDeviceSize alignment, VkBufferUsageFlags usage, bool hostVisible, Tr2PrimaryRenderContextAL& renderContext )
{
	const VkDeviceSize slack = alignment > 1 ? alignment - 1 : 0;
	const VkMemoryPropertyFlagBits memoryProperty = hostVisible ? VkMemoryPropertyFlagBits( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	FORWARD_HR( CreateBuffer( buffer, memory, size_t( requestedSize + slack ), usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, memoryProperty, renderContext ) );

	VkBufferDeviceAddressInfo addressInfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	addressInfo.buffer = buffer;
	const VkDeviceAddress raw = renderContext.m_raytracing.getBufferDeviceAddress( renderContext.m_device, &addressInfo );
	address = AlignUp( raw, alignment );
	size = requestedSize;
	if( hostVisible )
	{
		void* base = nullptr;
		if( FAILED( Vk2Al( vkMapMemory( renderContext.m_device, memory, 0, VK_WHOLE_SIZE, 0, &base ) ) ) )
		{
			Destroy( renderContext );
			return E_FAIL;
		}
		mapped = static_cast<uint8_t*>( base ) + ( address - raw );
	}
	return S_OK;
}

void RtBufferVulkan::Destroy( Tr2PrimaryRenderContextAL& renderContext )
{
	renderContext.DestroyLaterVulkan( buffer, &vkDestroyBuffer );
	renderContext.DestroyLaterVulkan( memory, &vkFreeMemory );
	buffer = VK_NULL_HANDLE;
	memory = VK_NULL_HANDLE;
	address = 0;
	size = 0;
	mapped = nullptr;
}

void RecordAccelerationStructureBuildVulkan( Tr2RenderContextAL& renderContext, const VkAccelerationStructureBuildGeometryInfoKHR& info, const VkAccelerationStructureBuildRangeInfoKHR& range, VkDeviceSize scratchSize )
{
	auto& owner = renderContext.GetPrimaryRenderContext();
	auto& batch = renderContext.m_asBatch;
	VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependency.memoryBarrierCount = 1;

	const VkDeviceAddress scratchLow = info.scratchData.deviceAddress;
	const VkDeviceAddress scratchHigh = scratchLow + scratchSize;
	if( !batch.open )
	{
		// Closes the batch too, which is already closed; nothing between this and the build
		// below records a command, so it opens right after.
		renderContext.EndRenderPassVulkan();

		VkMemoryBarrier2 before = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
		before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		before.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
		before.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
		before.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT;
		dependency.pMemoryBarriers = &before;
		owner.m_vkCmdPipelineBarrier2( renderContext.m_commandBuffer, &dependency );

		batch.open = true;
		batch.scratchLow = batch.scratchHigh = 0;
		batch.written.clear();
		batch.topLevelBuilt = false;
	}
	else
	{
		auto builtInBatch = [&batch]( VkAccelerationStructureKHR structure ) {
			return structure != VK_NULL_HANDLE && std::find( batch.written.begin(), batch.written.end(), structure ) != batch.written.end();
		};
		// A TLAS reads every BLAS its instances name, and those were built in this batch or an
		// earlier one; telling which costs more than the one barrier. For the same reason any
		// build after a TLAS of the batch waits for it: it may rewrite a BLAS the TLAS read.
		const bool readsBatch = info.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR || batch.topLevelBuilt || builtInBatch( info.dstAccelerationStructure ) || builtInBatch( info.srcAccelerationStructure );
		const bool scratchOverlaps = batch.scratchLow < batch.scratchHigh && scratchLow < batch.scratchHigh && batch.scratchLow < scratchHigh;
		if( readsBatch || scratchOverlaps )
		{
			VkMemoryBarrier2 between = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			between.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
			between.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
			between.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
			between.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
			dependency.pMemoryBarriers = &between;
			owner.m_vkCmdPipelineBarrier2( renderContext.m_commandBuffer, &dependency );
			batch.scratchLow = batch.scratchHigh = 0;
			batch.written.clear();
			batch.topLevelBuilt = false;
		}
	}

	const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
	owner.m_raytracing.cmdBuildAccelerationStructures( renderContext.m_commandBuffer, 1, &info, &ranges );

	if( scratchSize )
	{
		batch.scratchLow = batch.scratchLow < batch.scratchHigh ? std::min( batch.scratchLow, scratchLow ) : scratchLow;
		batch.scratchHigh = std::max( batch.scratchHigh, scratchHigh );
	}
	batch.written.push_back( info.dstAccelerationStructure );
	batch.topLevelBuilt = batch.topLevelBuilt || info.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
}


Tr2RtBottomLevelAccelerationStructureAL::Tr2RtBottomLevelAccelerationStructureAL() :
	m_owner( nullptr ),
	m_buffer( VK_NULL_HANDLE ),
	m_memory( VK_NULL_HANDLE ),
	m_size( 0 ),
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
	if( !renderContext.IsValid() || !renderContext.GetCaps().SupportsRaytracing() )
	{
		return E_INVALIDCALL;
	}
	if( !geometry.positions.IsValid() || !geometry.indices.IsValid() || !capacity.positions.IsValid() || !capacity.indices.IsValid() )
	{
		return E_INVALIDARG;
	}

	VkAccelerationStructureGeometryKHR capacityGeometry;
	uint32_t capacityPrimitives = 0;
	FORWARD_HR( DescribeTriangles( capacity, geometryFlags, capacityGeometry, capacityPrimitives ) );
	VkAccelerationStructureGeometryKHR buildGeometry;
	uint32_t buildPrimitives = 0;
	FORWARD_HR( DescribeTriangles( geometry, geometryFlags, buildGeometry, buildPrimitives ) );
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

	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	FORWARD_HR( CreateBuffer( buffer, memory, size_t( sizes.accelerationStructureSize ), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderContext ) );
	m_owner = &renderContext;
	m_buffer = buffer;
	m_memory = memory;
	m_size = sizes.accelerationStructureSize;

	VkAccelerationStructureCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
	createInfo.buffer = m_buffer;
	createInfo.size = m_size;
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

	VkAccelerationStructureGeometryKHR buildGeometry;
	uint32_t buildPrimitives = 0;
	FORWARD_HR( DescribeTriangles( geometry, m_geometryFlags, buildGeometry, buildPrimitives ) );
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
		if( !sizes.accelerationStructureSize || sizes.accelerationStructureSize > m_size )
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
		m_owner->DestroyLaterVulkan( m_buffer, &vkDestroyBuffer );
		m_owner->DestroyLaterVulkan( m_memory, &vkFreeMemory );
		m_owner = nullptr;
	}
	m_accelerationStructure = VK_NULL_HANDLE;
	m_buffer = VK_NULL_HANDLE;
	m_memory = VK_NULL_HANDLE;
	m_size = 0;
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
