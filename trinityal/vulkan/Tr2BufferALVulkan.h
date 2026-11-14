// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2BufferAL.h"
#include <vector>

class Tr2RenderContextAL;

namespace TrinityALImpl
{
	class Tr2BufferAL : public Tr2DeviceResourceAL<Tr2BufferAL>
	{
	public:
		Tr2BufferAL();
		~Tr2BufferAL();

		ALResult Create(
			const Tr2BufferDescriptionAL& desc,
			const void* initialData,
			Tr2PrimaryRenderContextAL& renderContext );
		void Destroy();
		bool IsValid() const;

		Tr2ALMemoryType GetMemoryClass() const;
		const Tr2BufferDescriptionAL& GetDesc() const;

		ALResult MapForReading( const void*& data, Tr2RenderContextAL& renderContext );
		ALResult MapForReading( const void*& data, uint32_t offset, uint32_t size, Tr2RenderContextAL& renderContext );
		void UnmapForReading( Tr2RenderContextAL& renderContext );
		ALResult MapForWriting( void*& data, Tr2RenderContextAL& renderContext );
		void UnmapForWriting( Tr2RenderContextAL& renderContext );
		ALResult UpdateBuffer( uint32_t offset, uint32_t size, const void* data, Tr2RenderContextAL & renderContext );
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const;
		ALResult SetName( const char* name );

		uint32_t GetSrvIndexInHeap() const;
		uint32_t GetUavIndexInHeap() const;

		VkBuffer GetBufferVulkan() const { return m_buffer; }
		VkBufferView GetBufferViewVulkan() const { return m_bufferView; }
		// Where the buffer's current contents start inside m_buffer: 0 unless the buffer
		// renames on MapForWriting (see the .cpp), in which case the slot the last map
		// took. Every bind of a vertex or index buffer adds it, and the render context
		// re-binds before a draw when it has moved.
		VkDeviceSize GetOffsetVulkan() const { return VkDeviceSize( m_slot ) * m_slotStride; }

		// The address of m_buffer's first byte, for acceleration structure builds; add
		// GetOffsetVulkan() for where the current contents start. 0 for a buffer created
		// without SHADER_DEVICE_ADDRESS usage, which is every buffer on a device whose
		// caps report no ray tracing. Not cached: a renaming buffer that grows its ring
		// moves to a new VkBuffer.
		VkDeviceAddress GetDeviceAddressVulkan() const;

		// The acceleration structure built over this buffer, for a buffer created with
		// Tr2GpuUsage::ACCELERATION_STRUCTURE: the TLAS (Tr2RtTopLevelAccelerationStructureAL)
		// creates it and hands it here, because the engine binds the TLAS as this buffer
		// (an SRV) and the resource set has to name the structure, not the buffer. Owned:
		// Destroy queues it ahead of the buffer.
		VkAccelerationStructureKHR GetAccelerationStructureVulkan() const
		{
			return m_accelerationStructure;
		}
		void SetAccelerationStructureVulkan( VkAccelerationStructureKHR accelerationStructure )
		{
			m_accelerationStructure = accelerationStructure;
		}

	private:
		VkBuffer m_buffer;
		VkDeviceMemory m_memory;
		VkBufferView m_bufferView;
		Tr2PrimaryRenderContextAL* m_owner;
		Tr2BufferDescriptionAL m_desc;
		// The host pointer while a Map* is outstanding, null otherwise. The unmaps key
		// off this rather than off IsValid(), because the AL's tests call Unmap after a
		// Map that failed, and vkUnmapMemory on unmapped memory is undefined behaviour.
		void* m_mapped;
		// The frame's upload-ring range behind a synchronized MapForWriting, between the map
		// and its unmap. VK_NULL_HANDLE while a non-synchronized map (or none) is outstanding.
		VkBuffer m_stagingBuffer;
		VkDeviceSize m_stagingOffset;
		VkAccelerationStructureKHR m_accelerationStructure;

		// The renaming ring for a synchronized vertex/index buffer: m_slotCount slots of
		// m_slotStride bytes in one allocation, the current contents in m_slot, and the
		// recording frame that last took each slot (0 = never). Non-renaming buffers have
		// one slot and never move. m_usage/m_memoryProperty are kept to grow the ring.
		bool m_renames;
		uint32_t m_slotCount;
		uint32_t m_slot;
		VkDeviceSize m_slotStride;
		std::vector<uint64_t> m_slotFrame;
		VkBufferUsageFlags m_usage;
		VkMemoryPropertyFlagBits m_memoryProperty;

		static bool NonSynchronizedVulkan( const Tr2BufferDescriptionAL& desc );
		bool IsNonSynchronizedVulkan() const;
		ALResult AllocateSlotsVulkan( uint32_t slotCount );
		void RecordStagingCopyVulkan( VkBuffer staging, VkDeviceSize stagingOffset, uint32_t offset, uint32_t size );

		// ::-qualified: inside namespace TrinityALImpl an unqualified friend declaration
		// names a new TrinityALImpl::Tr2RenderContextAL instead of the global class --
		// MSVC resolved it to the global anyway, clang does not.
		friend class ::Tr2RenderContextAL;
		friend class Tr2ResourceSetAL;
	};
}

#endif