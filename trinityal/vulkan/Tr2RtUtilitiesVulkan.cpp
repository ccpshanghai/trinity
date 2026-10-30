// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2RtUtilitiesVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "UtilitiesVulkan.h"

#include <algorithm>

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
	address = AlignUpVulkan( raw, alignment );
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
}

#endif
