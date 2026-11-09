// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN


#include "../include/Tr2GpuTimerAL.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "VkResult.h"


namespace TrinityALImpl
{
	class Tr2GpuTimerAL :
		public Tr2DeviceResourceAL<Tr2GpuTimerAL>
	{
	public:
		Tr2GpuTimerAL() :
			m_queryPool( VK_NULL_HANDLE ),
			m_frameNumber( 0 ),
			m_owner( nullptr ),
			m_lastTime( -1.0f ),
			m_state( UNINITIALIZED )
		{
		}
		~Tr2GpuTimerAL()
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

			// timestampPeriod is nanoseconds per tick and is 0 on a device that cannot
			// timestamp at all. Read it here rather than at GetTime so that Create is the
			// one place that can fail, which is what the caller can actually react to.
			VkPhysicalDeviceProperties properties;
			vkGetPhysicalDeviceProperties( renderContext.m_physicalDevice, &properties );
			if( properties.limits.timestampPeriod == 0.0f )
			{
				return E_NOTIMPL;
			}

			VkQueryPoolCreateInfo createInfo = {
				VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
				nullptr,
				0,
				VK_QUERY_TYPE_TIMESTAMP,
				2,                                  // one slot for Begin, one for End
				0
			};
			VkQueryPool queryPool = VK_NULL_HANDLE;
			CR_RETURN_HR( Vk2Al( vkCreateQueryPool( renderContext.m_device, &createInfo, nullptr, &queryPool ) ) );

			m_queryPool = queryPool;
			m_timestampPeriod = properties.limits.timestampPeriod;
			m_owner = &renderContext;
			m_state = READY;
			return S_OK;
		}

		void Destroy()
		{
			if( m_queryPool != VK_NULL_HANDLE && m_owner )
			{
				m_owner->DestroyLaterVulkan( m_queryPool, vkDestroyQueryPool );
			}
			m_queryPool = VK_NULL_HANDLE;
			m_owner = nullptr;
			m_lastTime = -1.0f;
			m_state = UNINITIALIZED;
		}

		bool Begin( Tr2RenderContextAL& renderContext )
		{
			if( !m_queryPool || m_state != READY || !renderContext.m_commandBuffer )
			{
				return false;
			}

			// Same Vulkan 1.0 reset rule as the occlusion query: the pool has to be reset
			// on a command buffer, outside a render pass instance. Both slots at once,
			// because a timestamp written into an unreset slot reads back as garbage.
			renderContext.EndRenderPassVulkan();
			vkCmdResetQueryPool( renderContext.m_commandBuffer, m_queryPool, 0, 2 );

			// TOP_OF_PIPE for the opening stamp and BOTTOM_OF_PIPE for the closing one, so
			// the pair brackets the work rather than sitting inside it.
			vkCmdWriteTimestamp( renderContext.m_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 0 );
			m_state = BEGIN_ISSUED;
			return true;
		}

		void End( Tr2RenderContextAL& renderContext )
		{
			if( !m_queryPool || m_state != BEGIN_ISSUED || !renderContext.m_commandBuffer || !m_owner )
			{
				return;
			}
			vkCmdWriteTimestamp( renderContext.m_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, 1 );
			m_frameNumber = m_owner->GetRecordingFrameNumber();
			m_state = END_ISSUED;
		}

		float GetTime( Tr2RenderContextAL& )
		{
			// Every early exit returns the previous reading rather than an error: this is
			// polled every frame by callers that want a number to display, and dx12's
			// behaves the same way.
			if( !m_queryPool || !m_owner || m_state != END_ISSUED )
			{
				return m_lastTime;
			}
			if( m_owner->GetRenderedFrameNumber() < m_frameNumber )
			{
				return m_lastTime;
			}

			uint64_t stamps[2] = { 0, 0 };
			if( vkGetQueryPoolResults(
					m_owner->m_device,
					m_queryPool,
					0,
					2,
					sizeof( stamps ),
					stamps,
					sizeof( uint64_t ),
					VK_QUERY_RESULT_64_BIT ) != VK_SUCCESS )
			{
				return m_lastTime;
			}

			// timestampPeriod is nanoseconds per tick; the AL reports seconds.
			m_lastTime = float( double( stamps[1] - stamps[0] ) * double( m_timestampPeriod ) / 1000000000.0 );
			m_state = READY;
			return m_lastTime;
		}

		bool IsValid() const
		{
			return m_queryPool != VK_NULL_HANDLE;
		}

		bool operator==( const Tr2GpuTimerAL& other ) const
		{
			return this == &other;
		}

		Tr2ALMemoryType GetMemoryClass() const { return AL_MEMORY_VIDEO; }
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const
		{
			description["type"] = "Tr2GpuTimerAL";
			description["name"] = m_name;
		}
		ALResult SetName( const char* name )
		{
			m_name = name;
			return S_OK;
		}

	private:
		Tr2GpuTimerAL( const Tr2GpuTimerAL& ) /* = delete */;
		Tr2GpuTimerAL& operator=( const Tr2GpuTimerAL& ) /* = delete */;

		enum State
		{
			UNINITIALIZED,
			READY,
			BEGIN_ISSUED,
			END_ISSUED
		};

		VkQueryPool m_queryPool;
		float m_timestampPeriod;
		uint64_t m_frameNumber;
		Tr2PrimaryRenderContextAL* m_owner;
		float m_lastTime;
		State m_state;
		std::string m_name;
	};
}


#endif
