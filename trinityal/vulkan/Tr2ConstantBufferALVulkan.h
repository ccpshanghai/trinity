// Copyright © 2026 CCP ehf.

#pragma once
#include <vector>


#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2ConstantBufferAL.h"

namespace TrinityALImpl
{
	// -------------------------------------------------------------
	// Description:
	//   A low level wrapper around the calls needed to set up a constant
	//   buffer for a given platform. Any higher level logic should be
	//   handled one level up, this is only to avoid ifdef soup when
	//   creating and locking buffers.
	//	 32bit: no support for 64bit-sized buffers.
	// -------------------------------------------------------------
	class Tr2ConstantBufferAL: public Tr2DeviceResourceAL<Tr2ConstantBufferAL>
	{
	public:
		Tr2ConstantBufferAL();
		~Tr2ConstantBufferAL();

		ALResult Create( uint32_t size, Tr2ConstantUsageAL::Type usage, const void* initialData, Tr2PrimaryRenderContextAL& renderContext );

		ALResult Lock( void** data, Tr2RenderContextAL& renderContext );
		ALResult Unlock( Tr2RenderContextAL& renderContext );

		bool IsValid() const;

		void Destroy();

		Tr2ConstantUsageAL::Type GetUsage() const;
		uint32_t GetSize() const;
		VkBuffer GetBufferVulkan() const { return m_buffer; }

		// Where the CURRENT contents live inside m_buffer. Lock hands out a fresh slot each
		// time (see the .cpp), so the offset moves; the render context reads it at bind
		// time and puts it in the descriptor, and a descriptor that still names the old
		// slot is how it knows the buffer was locked again.
		VkDeviceSize GetOffsetVulkan() const { return VkDeviceSize( m_slot ) * m_stride; }

		Tr2ALMemoryType GetMemoryClass() const;
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const;
		ALResult SetName( const char* name );

	private:
		Tr2ConstantBufferAL( const Tr2ConstantBufferAL& ) /* = delete */;
		Tr2ConstantBufferAL& operator=( const Tr2ConstantBufferAL& ) /* = delete */;

		// (Re)allocates m_buffer/m_memory with `slotCount` slots of m_stride bytes; the old
		// allocation, if any, is destroyed once the frames that could read it are done.
		ALResult AllocateSlotsVulkan( uint32_t slotCount );

		VkBuffer m_buffer;
		VkDeviceMemory m_memory;
		Tr2PrimaryRenderContextAL* m_owner;
		uint32_t m_size;                   // what the caller asked for, and what a descriptor's range is
		uint32_t m_stride;                 // m_size rounded up to the device's uniform offset alignment
		uint32_t m_slotCount;
		uint32_t m_slot;                   // the slot the current contents are in
		std::vector<uint64_t> m_slotFrame; // the recording frame that last took each slot, 0 = never
		Tr2ConstantUsageAL::Type m_usage;
	};
}
#endif
