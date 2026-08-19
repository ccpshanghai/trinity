// Copyright © 2026 CCP ehf.

#pragma once

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "../include/Tr2OcclusionQueryAL.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "VkResult.h"

namespace TrinityALImpl
{
	class Tr2OcclusionQueryAL : public Tr2DeviceResourceAL<Tr2OcclusionQueryAL>
	{
	public:
		Tr2OcclusionQueryAL() :
			m_queryPool( VK_NULL_HANDLE ),
			m_frameNumber( NOT_ENDED ),
			m_owner( nullptr )
		{
		}
		~Tr2OcclusionQueryAL()
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

			VkQueryPoolCreateInfo createInfo = {
				VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
				nullptr,
				0,
				VK_QUERY_TYPE_OCCLUSION,
				1,
				0                                   // pipelineStatistics, occlusion ignores it
			};
			VkQueryPool queryPool = VK_NULL_HANDLE;
			CR_RETURN_HR( Vk2Al( vkCreateQueryPool( renderContext.m_device, &createInfo, nullptr, &queryPool ) ) );

			m_queryPool = queryPool;
			m_owner = &renderContext;
			return S_OK;
		}
		bool IsValid() const
		{
			return m_queryPool != VK_NULL_HANDLE;
		}
		void Destroy()
		{
			if( m_queryPool != VK_NULL_HANDLE && m_owner )
			{
				m_owner->DestroyLaterVulkan( m_queryPool, vkDestroyQueryPool );
			}
			m_queryPool = VK_NULL_HANDLE;
			m_frameNumber = NOT_ENDED;
			m_owner = nullptr;
		}

		ALResult Begin( Tr2RenderContextAL& renderContext )
		{
			if( !m_queryPool || !m_owner )
			{
				return E_INVALIDCALL;
			}
			if( !renderContext.m_commandBuffer )
			{
				return E_INVALIDARG;
			}

			// A query pool holds undefined results until it is reset, and on Vulkan 1.0
			// the only way to reset one is on a command buffer -- vkResetQueryPool is 1.2,
			// and this backend asks for 1.0. vkCmdResetQueryPool is also illegal inside a
			// render pass instance, hence the close: it costs a pass restart, which the
			// backend already does for clears, and SetPipeline re-begins the pass lazily.
			renderContext.EndRenderPassVulkan();
			vkCmdResetQueryPool( renderContext.m_commandBuffer, m_queryPool, 0, 1 );

			// Flags 0, not VK_QUERY_CONTROL_PRECISE_BIT: precise occlusion counts need the
			// occlusionQueryPrecise device feature, and this backend enables no features at
			// all (pEnabledFeatures is null in vkCreateDevice). Without it the result is
			// specified only as zero or non-zero, which is what GetPixelCount can promise.
			vkCmdBeginQuery( renderContext.m_commandBuffer, m_queryPool, 0, 0 );
			m_frameNumber = NOT_ENDED;
			return S_OK;
		}
		ALResult End( Tr2RenderContextAL& renderContext )
		{
			if( !m_queryPool || !m_owner )
			{
				return E_INVALIDCALL;
			}
			if( !renderContext.m_commandBuffer )
			{
				return E_INVALIDARG;
			}
			vkCmdEndQuery( renderContext.m_commandBuffer, m_queryPool, 0 );

			// The result belongs to the frame that recorded it, and cannot be read before
			// that frame retires. Same bookkeeping as dx12's.
			m_frameNumber = m_owner->GetRecordingFrameNumber();
			return S_OK;
		}
		ALResult GetPixelCount( Tr2RenderContextAL&, uint32_t& count, ::Tr2OcclusionQueryAL::WaitMode waitMode )
		{
			if( !m_queryPool || !m_owner )
			{
				return E_INVALIDCALL;
			}
			if( m_frameNumber == NOT_ENDED )
			{
				return E_INVALIDCALL;
			}
			if( waitMode == ::Tr2OcclusionQueryAL::WAIT )
			{
				CR_RETURN_HR( m_owner->FlushAndSyncVulkan() );
			}
			if( m_owner->GetRenderedFrameNumber() < m_frameNumber )
			{
				return S_FALSE;
			}

			// Read on the host rather than through vkCmdCopyQueryPoolResults and a readback
			// buffer: the work is known to be finished by the frame-number check above, so
			// there is nothing left for the GPU to do and a staging buffer would only add a
			// copy. dx12 needs its buffer because ResolveQueryData is the only way out of a
			// query heap there.
			uint64_t result = 0;
			VkResult vkResult = vkGetQueryPoolResults(
				m_owner->m_device,
				m_queryPool,
				0,
				1,
				sizeof( result ),
				&result,
				sizeof( result ),
				VK_QUERY_RESULT_64_BIT );
			if( vkResult == VK_NOT_READY )
			{
				return S_FALSE;
			}
			CR_RETURN_HR( Vk2Al( vkResult ) );

			count = uint32_t( result );
			return S_OK;
		}

		bool operator==( const Tr2OcclusionQueryAL& other ) const { return this == &other; }

		Tr2ALMemoryType GetMemoryClass() const { return AL_MEMORY_MANAGED; }
		void Describe( Tr2DeviceResourceDescriptionAL& description ) const
		{
			description["type"] = "Tr2OcclusionQueryAL";
			description["name"] = m_name;
		}
		ALResult SetName( const char* name )
		{
			m_name = name;
			return S_OK;
		}

	private:
		Tr2OcclusionQueryAL( const Tr2OcclusionQueryAL& ) /* = delete */;
		Tr2OcclusionQueryAL& operator=( const Tr2OcclusionQueryAL& ) /* = delete */;

		static const uint64_t NOT_ENDED = 0xffffffffffffffffull;

		VkQueryPool m_queryPool;
		uint64_t m_frameNumber;
		Tr2PrimaryRenderContextAL* m_owner;
		std::string m_name;
	};
}

#endif
