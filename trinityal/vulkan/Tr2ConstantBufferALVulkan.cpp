// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2ConstantBufferALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "UtilitiesVulkan.h"
#include <algorithm>
#include "../ALLog.h"

namespace TrinityALImpl
{

	Tr2ConstantBufferAL::Tr2ConstantBufferAL()
		:m_buffer( VK_NULL_HANDLE ),
		m_memory( VK_NULL_HANDLE ),
		m_owner( nullptr ),
		m_size( 0 ),
		m_stride( 0 ),
		m_slotCount( 0 ),
		m_slot( 0 ),
		m_usage( Tr2ConstantUsageAL::REUSABLE )
	{
	}

	namespace
	{
		// Slots to start with for a buffer that can be locked. Grown by doubling on demand, so
		// the number only decides how many Locks the first frames take before the buffer has
		// found its size; the ceiling is what stops a runaway caller from taking the device.
		const uint32_t INITIAL_SLOTS = 4;
		const uint32_t MAX_SLOTS = 4096;
	}

	Tr2ConstantBufferAL::~Tr2ConstantBufferAL()
	{
		Destroy();
	}

	ALResult Tr2ConstantBufferAL::Create( uint32_t size, Tr2ConstantUsageAL::Type usage, const void* initialData, Tr2PrimaryRenderContextAL& renderContext )
	{
		Destroy();

		if( size == 0 )
		{
			return E_INVALIDARG;
		}

		if( !renderContext.IsValid() )
		{
			return E_INVALIDCALL;
		}

		m_owner = &renderContext;
		m_size = size;
		m_usage = usage;
		const VkDeviceSize alignment = renderContext.GetUniformBufferOffsetAlignmentVulkan();
		m_stride = uint32_t( ( VkDeviceSize( size ) + alignment - 1 ) / alignment * alignment );

		// One slot for a buffer that is never locked; a few to start with for one that is.
		// The ring is what makes Lock behave like D3D11's MAP_WRITE_DISCARD -- see Lock.
		const ALResult allocated = AllocateSlotsVulkan( usage == Tr2ConstantUsageAL::IMMUTABLE ? 1 : INITIAL_SLOTS );
		if( FAILED( allocated ) )
		{
			m_owner = nullptr;
			m_size = 0;
			return allocated;
		}

		if( initialData )
		{
			// No flush: the memory is HOST_VISIBLE | HOST_COHERENT, so the write is already
			// visible to the device. The flush that used to be here was illegal as well --
			// see the note in Unlock. Slot 0, and it is marked as this frame's so that a Lock
			// in the same frame does not hand the slot straight back out.
			void* mapped;
			CR_RETURN_HR( Vk2Al( vkMapMemory( renderContext.m_device, m_memory, 0, size, 0, &mapped ) ) );
			memcpy( mapped, initialData, size );
			vkUnmapMemory( renderContext.m_device, m_memory );
			m_slotFrame[0] = renderContext.GetRecordingFrameNumber();
		}

		return S_OK;
	}

	ALResult Tr2ConstantBufferAL::AllocateSlotsVulkan( uint32_t slotCount )
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		FORWARD_HR( TrinityALImpl::CreateBuffer( buffer, memory, VkDeviceSize( m_stride ) * slotCount, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VkMemoryPropertyFlagBits( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ), *m_owner ) );

		// The old allocation may still be read by a frame in flight; DestroyLater holds it
		// until that frame's fence says otherwise. Any descriptor that named it keeps
		// working for as long as the command buffer that bound it can run.
		if( m_buffer )
		{
			m_owner->DestroyLaterVulkan( m_buffer, &vkDestroyBuffer );
		}
		if( m_memory )
		{
			m_owner->DestroyLaterVulkan( m_memory, &vkFreeMemory );
		}
		m_buffer = buffer;
		m_memory = memory;
		m_slotCount = slotCount;
		m_slot = 0;
		m_slotFrame.assign( slotCount, 0 );
		return S_OK;
	}

	ALResult Tr2ConstantBufferAL::Lock( void** data, Tr2RenderContextAL& )
	{
		if( !IsValid() )
		{
			return E_INVALIDCALL;
		}

		// A fresh slot every time, which is what D3D11's MAP_WRITE_DISCARD gives the engine
		// and what the engine is written against: Tr2PerObjectData writes each object's
		// constants into the SAME Tr2ConstantBufferAL and draws, object after object, in one
		// frame (FillAndSetConstants). dx12 and metal copy the bytes into a per-frame ring
		// at SetConstants. Mapping one allocation in place -- which is what this did -- lets
		// every draw of the frame read whatever was written LAST by the time the GPU runs
		// them: on the S23 that was 43 draws a frame counted, a quarter of a million
		// vertices, and not one pixel from any of them, because the hull was drawn with
		// somebody else's matrix (2026-09-11, ConstantBufferRename.cpp is the proof).
		//
		// A slot is free again once the frame that took it has rendered; if the next one
		// has not, the ring doubles. An IMMUTABLE buffer has one slot and is written in
		// place, which is what its usage promises.
		if( m_slotCount > 1 )
		{
			uint32_t next = ( m_slot + 1 ) % m_slotCount;
			const uint64_t rendered = m_owner->GetRenderedFrameNumber();
			if( m_slotFrame[next] != 0 && m_slotFrame[next] > rendered )
			{
				if( m_slotCount < MAX_SLOTS )
				{
					FORWARD_HR( AllocateSlotsVulkan( std::min( m_slotCount * 2, MAX_SLOTS ) ) );
					next = 0;
				}
				else
				{
					static bool logged = false;
					if( !logged )
					{
						logged = true;
						CCP_AL_LOGERR( "Tr2ConstantBufferAL: %u slots of %u bytes are all in flight; reusing one, and a draw may read the wrong constants", m_slotCount, m_size );
					}
				}
			}
			m_slot = next;
		}
		m_slotFrame[m_slot] = m_owner->GetRecordingFrameNumber();
		return Vk2Al( vkMapMemory( m_owner->m_device, m_memory, GetOffsetVulkan(), m_size, 0, data ) );
	}

	ALResult Tr2ConstantBufferAL::Unlock( Tr2RenderContextAL& renderContext )
	{
		if( !IsValid() )
		{
			return E_INVALIDCALL;
		}
		// No flush: Create allocates this memory HOST_VISIBLE | HOST_COHERENT, which is
		// the same reason none of Tr2BufferALVulkan's host-visible paths flush either.
		//
		// VK_WHOLE_SIZE in a VkMappedMemoryRange is not an exemption from the
		// nonCoherentAtomSize rule. It runs to the end of the current *mapping*, and
		// VUID-VkMappedMemoryRange-size-01389 requires that end to be either a multiple of
		// nonCoherentAtomSize or the end of the memory object. vkGetBufferMemoryRequirements
		// rounds the allocation up -- a 16-byte uniform buffer gets 64 bytes here -- so a
		// mapping of [0, size) ends inside the allocation and satisfies neither.
		vkUnmapMemory( m_owner->m_device, m_memory );
		return S_OK;
	}

	bool Tr2ConstantBufferAL::IsValid() const
	{
		return m_owner != nullptr;
	}

	void Tr2ConstantBufferAL::Destroy()
	{
		if( m_owner )
		{
			m_owner->DestroyLaterVulkan( m_buffer, &vkDestroyBuffer );
			m_buffer = VK_NULL_HANDLE;

			m_owner->DestroyLaterVulkan( m_memory, &vkFreeMemory );
			m_memory = VK_NULL_HANDLE;

			m_owner = nullptr;
			m_size = 0;
			m_stride = 0;
			m_slotCount = 0;
			m_slot = 0;
			m_slotFrame.clear();
		}
	}

	Tr2ConstantUsageAL::Type Tr2ConstantBufferAL::GetUsage() const
	{
		return Tr2ConstantUsageAL::REUSABLE;
	}

	uint32_t Tr2ConstantBufferAL::GetSize() const
	{
		return m_size;
	}

	Tr2ALMemoryType Tr2ConstantBufferAL::GetMemoryClass() const
	{
		return AL_MEMORY_MANAGED;
	}

	void Tr2ConstantBufferAL::Describe( Tr2DeviceResourceDescriptionAL& description ) const
	{
		description["type"] = "Tr2ConstantBufferAL";
	}

	ALResult Tr2ConstantBufferAL::SetName( const char* )
	{
		return E_NOTIMPL;
	}
}

#endif