// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2FenceAL.h"
#include "Tr2PrimaryRenderContextVulkan.h"

namespace TrinityALImpl
{
	// Deliberately not a VkFence.
	//
	// The AL contract here is "mark a point in the submission stream, then ask whether the
	// GPU has passed it", and that is frame granularity, not submit granularity. The
	// primary render context already owns one VkFence per virtual frame and already knows
	// which frames have retired, so a fence of our own would be a second, coarser copy of
	// what the context can already answer. dx12 reaches the same conclusion --
	// Tr2FenceALDx12 holds no ID3D12Fence either, only a frame number.
	//
	// The consequence worth knowing: a fence put and read inside one frame reads as not
	// reached until that frame retires, unless something flushed in between. That matches
	// dx12, and it is what callers want, because the frame is what their work belongs to.
	class Tr2FenceAL :
		public Tr2DeviceResourceAL<Tr2FenceAL>
	{
	public:
		Tr2FenceAL() :
			m_frameNumber( NOT_PUT ),
			m_owner( nullptr )
		{
		}
		~Tr2FenceAL()
		{
			Destroy();
		}

		ALResult Create( Tr2PrimaryRenderContextAL& renderContext )
		{
			Destroy();
			if( !renderContext.IsValid() )
			{
				return E_INVALIDARG;
			}
			m_owner = &renderContext;
			return S_OK;
		}
		void Destroy()
		{
			m_frameNumber = NOT_PUT;
			m_owner = nullptr;
		}

		bool IsValid() const
		{
			return m_owner != nullptr;
		}

		ALResult PutFence( Tr2RenderContextAL& )
		{
			if( !m_owner )
			{
				return E_INVALIDCALL;
			}
			m_frameNumber = m_owner->GetRecordingFrameNumber();
			return S_OK;
		}
		ALResult IsReached( bool& isReached, Tr2RenderContextAL& )
		{
			if( !m_owner || m_frameNumber == NOT_PUT )
			{
				return E_INVALIDCALL;
			}
			isReached = m_owner->GetRenderedFrameNumber() >= m_frameNumber;
			return S_OK;
		}
		ALResult Wait( Tr2RenderContextAL& )
		{
			if( !m_owner )
			{
				return E_INVALIDCALL;
			}
			// FlushAndSyncVulkan raises the rendered frame number to the frame being
			// recorded, so an IsReached immediately after this returns true.
			CR_RETURN_HR( m_owner->FlushAndSyncVulkan() );
			return S_OK;
		}

		bool operator==( const Tr2FenceAL& other ) const
		{
			return this == &other;
		}

		Tr2ALMemoryType GetMemoryClass() const { return AL_MEMORY_VIDEO; }
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const
		{
			description["type"] = "Tr2FenceAL";
			description["name"] = m_name;
		}
		ALResult SetName( const char* name )
		{
			m_name = name;
			return S_OK;
		}

	private:
		Tr2FenceAL( const Tr2FenceAL& ) /* = delete */;
		Tr2FenceAL& operator=( const Tr2FenceAL& ) /* = delete */;

		static const uint64_t NOT_PUT = 0xffffffffffffffffull;

		uint64_t m_frameNumber;
		Tr2PrimaryRenderContextAL* m_owner;
		std::string m_name;
	};
}

#endif
