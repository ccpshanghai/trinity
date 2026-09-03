// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "Tr2RenderContextVulkan.h"
#include "Tr2BufferALVulkan.h"
#include "Tr2ShaderProgramALVulkan.h"
#include "Tr2PrimaryRenderContextVulkan.h"
#include "Tr2ResourceSetALVulkan.h"
#include "Tr2VertexLayoutALVulkan.h"
#include "Tr2TextureALVulkan.h"
#include "Tr2ConstantBufferALVulkan.h"
#include "Tr2ShaderBindingABIVulkan.h"
#include "UtilitiesVulkan.h"
#include "VkResult.h"
#include "../include/Tr2RtTopLevelAccelerationStructureAL.h"

bool g_gatherPipelineStatistics = false;

// The AL owns this counter on every backend -- dx11, dx12, metal and the stub all declare it
// in their render-context translation unit, while TriStepRenderFps.cpp and Tr2InstancedMesh.cpp
// say CCP_STATS_DECLARED_ELSEWHERE and read it. The engine does not link without a definition,
// so it is the AL's to provide. The draws below feed it: a declared counter nobody adds to
// reads a confident zero, which is worse than no counter at all.
CCP_STATS_DECLARE( vertexCount, "Trinity/AL/vertexCount", true, CST_COUNTER_HIGH, "Vertex count in DrawPrimitive calls." );

namespace
{

	std::pair<uint32_t, uint32_t> s_primitiveToVertexCount[] = {
		std::make_pair( 0, 0 ),
		std::make_pair( 3, 0 ),
		std::make_pair( 1, 2 ),
		std::make_pair( 0, 0 ),
		std::make_pair( 2, 0 ),
		std::make_pair( 1, 1 ),
		std::make_pair( 1, 0 ),
	};

	// Trinity's viewport has y down from the top left; Vulkan's has y up, and this backend
	// flips it by giving the viewport a negative height and moving the origin to the bottom
	// edge -- which is what VK_KHR_MAINTENANCE1, in the device extension list, exists to
	// allow. One function because there are two places that need the flip and they must not
	// be able to disagree: SetPass, which opens a pass, and SetViewport, which changes the
	// dynamic state inside one already open.
	VkViewport FlippedViewport( float x, float y, float width, float height, float minZ, float maxZ )
	{
		VkViewport viewport = { x, y + height, width, -height, minZ, maxZ };
		return viewport;
	}

}

size_t Tr2RenderContextAL::RenderPassSource::GetHash() const
{
	return CcpHashFNV1( this, sizeof( *this ) );
}

size_t Tr2RenderContextAL::PipelineSource::GetHash() const
{
	return CcpHashFNV1( this, sizeof( *this ) );
}

Tr2RenderContextAL::Tr2RenderContextAL() throw( )
	:m_dirtyPso( true ),
	m_dirtyPass( true ),
	m_owner( nullptr ),
	m_readOnlyDepth( false ),
	m_srgbWrite( false ),
	m_renderingActive( false ),
	m_commandBuffer( VK_NULL_HANDLE ),
	m_constantPool( VK_NULL_HANDLE ),
	m_constantSet( VK_NULL_HANDLE ),
	m_boundConstantLayout( VK_NULL_HANDLE ),
	m_constantsDirty( false ),
	m_constantPoolLastUse( 0 ),
	m_computePipeline( VK_NULL_HANDLE ),
	m_computePipelineLayout( VK_NULL_HANDLE ),
	m_primitiveToVertexCount( 0, 0 ),
	m_topology( Tr2RenderContextEnum::TOP_TRIANGLES ),
	m_separateAlphaBlend( false ),
	m_viewportSet( false )
{
	memset( &m_pipelineSource, 0, sizeof( m_pipelineSource ) );
	memset( &m_renderPassSource, 0, sizeof( m_renderPassSource ) );

	m_pipelineSource.m_depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	// The memset above leaves depthCompareOp as VK_COMPARE_OP_NEVER, which discards every
	// fragment the moment depth testing is switched on. LESS_OR_EQUAL is what the D3D-
	// shaped AL above this defaults RS_ZFUNC to, so a caller that enables depth without
	// naming a function gets what it expects rather than a black screen.
	m_pipelineSource.m_depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	m_pipelineSource.m_rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	m_pipelineSource.m_rasterizationState.lineWidth = 1;
	// Clockwise is the front face, because that is what the D3D-shaped AL above this means by
	// one: dx12 builds every pipeline from a rasterizer whose FrontCounterClockwise is FALSE
	// (PsoDescription.cpp s_defaultRasterizer), and *no render state on either backend ever
	// changes it* -- there is no RS_ for winding, so whatever is set here is what ships. The
	// memset above leaves VK_FRONT_FACE_COUNTER_CLOCKWISE, the opposite of it.
	//
	// What that cost, and why it was hard to see: RS_CULLMODE still arrived and was still
	// mapped correctly, so exactly one half of every mesh was culled -- the wrong half. A
	// closed mesh has the same silhouette whichever half survives, so the primitives cube
	// rendered at precisely the right size, place and perspective, matching the dx12 readback
	// in outline. Every visible face was a back face, its normal pointing away from the
	// camera, and SolidsWithZ shades with colour * saturate( dot( worldNormal, viewForward ) )
	// -- zero for all of them. A perfectly shaped black cube.
	//
	// CLOCKWISE is the value that reproduces dx12, verified against its readback rather than
	// reasoned about: this backend also flips the viewport in y (FlippedViewport), which
	// reverses the winding the rasterizer sees, so the two conventions compose and arguing
	// from either one alone gets the answer wrong half the time.
	m_pipelineSource.m_rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;

	VkPipelineColorBlendAttachmentState defaultAttachment = {
		VK_FALSE,                                                     // VkBool32                                       blendEnable
		VK_BLEND_FACTOR_ONE,                                          // VkBlendFactor                                  srcColorBlendFactor
		VK_BLEND_FACTOR_ZERO,                                         // VkBlendFactor                                  dstColorBlendFactor
		VK_BLEND_OP_ADD,                                              // VkBlendOp                                      colorBlendOp
		VK_BLEND_FACTOR_ONE,                                          // VkBlendFactor                                  srcAlphaBlendFactor
		VK_BLEND_FACTOR_ZERO,                                         // VkBlendFactor                                  dstAlphaBlendFactor
		VK_BLEND_OP_ADD,                                              // VkBlendOp                                      alphaBlendOp
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |         // VkColorComponentFlags                          colorWriteMask
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
	};

	for( uint32_t i = 0; i < _countof( m_pipelineSource.m_attachmentBlend ); ++i )
	{
		m_pipelineSource.m_attachmentBlend[i] = defaultAttachment;
	}

	VkPipelineColorBlendStateCreateInfo defaultColorBlend = {
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,     // VkStructureType                                sType
		nullptr,                                                      // const void                                    *pNext
		0,                                                            // VkPipelineColorBlendStateCreateFlags           flags
		VK_FALSE,                                                     // VkBool32                                       logicOpEnable
		VK_LOGIC_OP_COPY,                                             // VkLogicOp                                      logicOp
		1,                                                            // uint32_t                                       attachmentCount
		m_pipelineSource.m_attachmentBlend,                                // const VkPipelineColorBlendAttachmentState     *pAttachments
		{ 0.0f, 0.0f, 0.0f, 0.0f }                                    // float                                          blendConstants[4]
	};
	m_pipelineSource.m_colorBlendState = defaultColorBlend;

}

Tr2RenderContextAL::~Tr2RenderContextAL() throw( )
{
	Destroy();
}

void Tr2RenderContextAL::Destroy() throw( )
{
	// Release device resources held by this render context while the device is
	// still live. m_pipelineSource.m_shaderProgram is a member destroyed by the
	// base-class destructor, which runs after Tr2PrimaryRenderContextAL::Destroy()
	// has already vkDestroyDevice'd -- and Tr2ShaderAL::Destroy() destroys its
	// VkShaderModule directly against m_owner->m_device, so leaving the program
	// for the destructor means destroying the module against a null device.
	m_pipelineSource.m_shaderProgram = Tr2ShaderProgramAL();
	m_resourceSet = Tr2ResourceSetAL();
	m_drawUPHelper.Destroy();

	// The bound attachments and the push/pop stacks hold texture references too, and for
	// the same reason they have to be let go here rather than in the destructor. A test
	// that fails between SetDepthStencil and PopDepthStencil -- an ASSERT returning early
	// is enough -- leaves its depth buffer bound to a render context that outlives it, and
	// four of those turned up at vkDestroyDevice as twelve leaked objects the moment
	// binding a depth stencil started working.
	for( uint32_t i = 0; i < RENDER_TARGET_COUNT; ++i )
	{
		m_boundRenderTargets[i] = Tr2TextureAL();
		m_rtStack[i].clear();
	}
	m_boundDepthStencil = Tr2TextureAL();
	m_dsStack.clear();

	if( m_constantPool != VK_NULL_HANDLE )
	{
		m_owner->DestroyLaterVulkan( m_constantPool, vkDestroyDescriptorPool );
		m_constantPool = VK_NULL_HANDLE;
	}
	m_constantSet = VK_NULL_HANDLE;
	m_boundConstantLayout = VK_NULL_HANDLE;
	m_constantBuffers.clear();
	m_constantsDirty = false;

	if( m_computePipeline )
	{
		m_owner->DestroyLaterVulkan( m_computePipeline, vkDestroyPipeline );
		m_computePipeline = VK_NULL_HANDLE;
	}
	m_computePipelineLayout = VK_NULL_HANDLE;
}

bool Tr2RenderContextAL::IsValid() const throw( )
{
	return m_owner != nullptr;
}

ALResult Tr2RenderContextAL::BeginScene() throw( )
{
	return S_OK;
}

ALResult Tr2RenderContextAL::EndScene()
{
	return S_OK;
}

ALResult Tr2RenderContextAL::Clear(
	uint32_t clearFlags,
	uint32_t color,
	float depth,
	uint32_t stencil,
	uint32_t slot ) throw( )
{
	if( clearFlags & Tr2RenderContextEnum::CLEARFLAGS_TARGET )
	{
		if( m_boundRenderTargets[slot].IsValid() )
		{
			float f = 1.0f / 255.0f;
			VkClearColorValue clearColor = {
				{
					f * (float)(uint8_t)( color >> 16 ),
					f * (float)(uint8_t)( color >> 8 ),
					f * (float)(uint8_t)( color >> 0 ),
					f * (float)(uint8_t)( color >> 24 )
				} 
			};

			// vkCmdClearColorImage is a transfer command, so it needs the image in a
			// transfer layout and needs to be outside any render pass. The barrier that
			// used to be spelled out here assumed the image was in
			// COLOR_ATTACHMENT_OPTIMAL, which was only true if a pass had just been open;
			// the tracker knows where it actually is.
			EndRenderPassVulkan();
			m_boundRenderTargets[slot].m_texture->TransitionForTransferWriteVulkan( m_commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

			VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdClearColorImage( m_commandBuffer, m_boundRenderTargets[slot].m_texture->GetImageVulkan(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &subresourceRange );

			m_dirtyPass = true;
		}
		else
		{
			return E_INVALIDCALL;
		}
	}
	if( clearFlags & ( Tr2RenderContextEnum::CLEARFLAGS_ZBUFFER | Tr2RenderContextEnum::CLEARFLAGS_STENCIL ) )
	{
		if( !m_boundDepthStencil.IsValid() )
		{
			return E_INVALIDCALL;
		}

		// One vkCmdClearDepthStencilImage for both flags rather than two calls: the aspect
		// mask is what selects them, and clearing the same image twice in a row is a
		// write-after-write hazard the sync validator would rightly complain about.
		//
		// The requested aspects are intersected with what the format actually has, because
		// asking to clear the stencil of a depth-only image is a VUID, and callers pass
		// CLEARFLAGS_ZBUFFER | CLEARFLAGS_STENCIL habitually.
		const VkImageAspectFlags available = TrinityALImpl::GetAspectMaskVulkan( m_boundDepthStencil.m_texture->m_format );
		VkImageAspectFlags aspect = 0;
		if( clearFlags & Tr2RenderContextEnum::CLEARFLAGS_ZBUFFER )
		{
			aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
		}
		if( clearFlags & Tr2RenderContextEnum::CLEARFLAGS_STENCIL )
		{
			aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
		aspect &= available;
		if( aspect == 0 )
		{
			return E_INVALIDARG;
		}

		EndRenderPassVulkan();
		m_boundDepthStencil.m_texture->TransitionForTransferWriteVulkan( m_commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

		VkClearDepthStencilValue clearValue = { depth, stencil };
		VkImageSubresourceRange subresourceRange = { aspect, 0, 1, 0, 1 };
		vkCmdClearDepthStencilImage( m_commandBuffer, m_boundDepthStencil.m_texture->GetImageVulkan(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &subresourceRange );

		m_dirtyPass = true;
	}

	return S_OK;
}

ALResult Tr2RenderContextAL::SetStreamSource( uint32_t stream, const Tr2BufferAL & buffer, uint32_t offset, uint32_t stride ) throw( )
{
	VkDeviceSize deviceOffset = offset;
	if( !buffer.IsValid() )
	{
		auto buf = m_owner->GetZeroBufferVulkan();
		vkCmdBindVertexBuffers( m_commandBuffer, stream, 1, &buf, &deviceOffset );
		return S_OK;
	}
	m_pipelineSource.m_streams[stream].stride = stride;
	vkCmdBindVertexBuffers( m_commandBuffer, stream, 1, &buffer.m_buffer->m_buffer, &deviceOffset );
	return S_OK;
}

ALResult Tr2RenderContextAL::SetIndices( const Tr2BufferAL & buffer ) throw( )
{
	if( !buffer.IsValid() )
	{
		vkCmdBindIndexBuffer( m_commandBuffer, m_owner->GetZeroBufferVulkan(), 0, VK_INDEX_TYPE_UINT16 );
		return S_OK;
	}
	vkCmdBindIndexBuffer( m_commandBuffer, buffer.m_buffer->m_buffer, 0, buffer.GetDesc().stride == 4 ?  VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16 );
	return S_OK;
}

// The caller-supplied-stride form. The overload above reads the stride out of the buffer
// description; Tr2EffectStateManager tracks index stride separately and passes it, which
// matters for a buffer bound with a stride other than the one it was created with.
ALResult Tr2RenderContextAL::SetIndices( const Tr2BufferAL & buffer, uint32_t stride ) throw( )
{
	if( !buffer.IsValid() )
	{
		vkCmdBindIndexBuffer( m_commandBuffer, m_owner->GetZeroBufferVulkan(), 0, VK_INDEX_TYPE_UINT16 );
		return S_OK;
	}
	vkCmdBindIndexBuffer( m_commandBuffer, buffer.m_buffer->m_buffer, 0, stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32 );
	return S_OK;
}


ALResult Tr2RenderContextAL::SetVertexLayout( const Tr2VertexLayoutAL& layout ) throw( )
{
	if( !( m_pipelineSource.m_layout == layout ) )
	{
		m_pipelineSource.m_layout = layout;
		m_dirtyPso = true;
	}
	return S_OK;
}

namespace
{
	VkPrimitiveTopology s_topologyMap[] = {
		VK_PRIMITIVE_TOPOLOGY_MAX_ENUM,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
		VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
		VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
	};
}

ALResult Tr2RenderContextAL::SetTopology( Tr2RenderContextEnum::Topology topology ) throw( )
{
	m_topology = topology;
	auto top = s_topologyMap[topology];
	if( top != m_pipelineSource.m_topology )
	{
		m_primitiveToVertexCount = s_primitiveToVertexCount[topology];
		m_pipelineSource.m_topology = top;
		m_dirtyPso = true;
	}
	return S_OK;
}

ALResult Tr2RenderContextAL::SetShaderProgram( const Tr2ShaderProgramAL& shader ) throw( )
{
	if( !( m_pipelineSource.m_shaderProgram == shader ) )
	{
		// The constant descriptor set/pool belong to the old program's layout, which
		// is destroyed when the program is released (and the driver may reuse the
		// handle). Invalidate them so the next SetPipeline reallocates against the
		// new program's layout rather than comparing stale layout handles.
		if( m_constantPool )
		{
			m_owner->DestroyLaterVulkan( m_constantPool, vkDestroyDescriptorPool );
			m_constantPool = VK_NULL_HANDLE;
		}
		m_constantSet = VK_NULL_HANDLE;
		m_boundConstantLayout = VK_NULL_HANDLE;
		m_constantsDirty = true;

		m_pipelineSource.m_shaderProgram = shader;
		m_dirtyPso = true;
	}
	return S_OK;
}

ALResult Tr2RenderContextAL::SetResourceSet( const Tr2ResourceSetAL& resourceSet ) throw( )
{
	m_resourceSet = resourceSet;
	return S_OK;
}

ALResult Tr2RenderContextAL::SetConstants( const Tr2ConstantBufferAL& buffer, Tr2RenderContextEnum::ShaderType constantType, uint32_t registerIndex, uint32_t ) throw( )
{
	if( !m_owner )
	{
		return E_INVALIDCALL;
	}

	const uint32_t binding = Tr2VulkanBindingABI::BindingNumber( Tr2ShaderRegisterAL::CONSTANT_BUFFER, registerIndex, constantType );

	if( !buffer.IsValid() )
	{
		m_constantBuffers.erase( binding );
	}
	else
	{
		m_constantBuffers[binding] = buffer.m_buffer->GetBufferVulkan();
	}
	m_constantsDirty = true;
	return S_OK;
}


namespace
{
	// Tr2RenderContextEnum::BlendMode is D3D9's D3DBLEND, numbered from 1. dx12 stores it
	// into D3D12_BLEND unchanged because the two enumerations happen to agree;
	// VkBlendFactor does not agree with either, so it needs a real table. Index 0 is unused
	// and maps to ZERO so that a zero-initialised state is at least defined.
	const VkBlendFactor s_blendFactorMap[] = {
		VK_BLEND_FACTOR_ZERO,                        // (unused, BlendMode starts at 1)
		VK_BLEND_FACTOR_ZERO,                        // BM_ZERO
		VK_BLEND_FACTOR_ONE,                         // BM_ONE
		VK_BLEND_FACTOR_SRC_COLOR,                   // BM_SRCCOLOR
		VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,         // BM_INVSRCCOLOR
		VK_BLEND_FACTOR_SRC_ALPHA,                   // BM_SRCALPHA
		VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,         // BM_INVSRCALPHA
		VK_BLEND_FACTOR_DST_ALPHA,                   // BM_DESTALPHA
		VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,         // BM_INVDESTALPHA
		VK_BLEND_FACTOR_DST_COLOR,                   // BM_DESTCOLOR
		VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,         // BM_INVDESTCOLOR
		VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,          // BM_SRCALPHASAT
		VK_BLEND_FACTOR_SRC_ALPHA,                   // BM_BOTHSRCALPHA, a D3D9 legacy pair
		VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,         // BM_BOTHINVSRCALPHA, likewise
		VK_BLEND_FACTOR_CONSTANT_COLOR,              // BM_BLENDFACTOR
		VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR     // BM_INVBLENDFACTOR
	};

	// What a colour factor becomes when it is applied to the alpha channel. D3D9 derives
	// the alpha factors from the colour ones unless separate alpha blending is switched
	// on, and dx12 does the same remap in GetPipelineState; this is that table, in
	// VkBlendFactor terms.
	VkBlendFactor AlphaFactorFor( VkBlendFactor colorFactor )
	{
		switch( colorFactor )
		{
		case VK_BLEND_FACTOR_SRC_COLOR:               return VK_BLEND_FACTOR_SRC_ALPHA;
		case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:     return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case VK_BLEND_FACTOR_DST_COLOR:               return VK_BLEND_FACTOR_DST_ALPHA;
		case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:     return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case VK_BLEND_FACTOR_CONSTANT_COLOR:          return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		default:                                      return colorFactor;
		}
	}

	// BO_DISABLE is 0 and has no Vulkan equivalent -- disabling is blendEnable, not an
	// operation -- so it maps to ADD, which is what a disabled attachment ignores anyway.
	const VkBlendOp s_blendOpMap[] = {
		VK_BLEND_OP_ADD,                             // BO_DISABLE
		VK_BLEND_OP_ADD,                             // BO_ADD
		VK_BLEND_OP_SUBTRACT,                        // BO_SUBTRACT
		VK_BLEND_OP_REVERSE_SUBTRACT,                // BO_REVSUBTRACT
		VK_BLEND_OP_MIN,                             // BO_MIN
		VK_BLEND_OP_MAX                              // BO_MAX
	};
}

ALResult Tr2RenderContextAL::SetRenderState( Tr2RenderContextEnum::RenderState state, uint32_t value ) throw( )
{
	switch( state )
	{
	case Tr2RenderContextEnum::RS_ZENABLE:
		m_pipelineSource.m_depthStencilState.depthTestEnable = value != 0;
		m_dirtyPso = true;
		return S_OK;
	case Tr2RenderContextEnum::RS_ZWRITEENABLE:
		m_pipelineSource.m_depthStencilState.depthWriteEnable = value != 0;
		m_dirtyPso = true;
		return S_OK;
	case Tr2RenderContextEnum::RS_ZFUNC:
		// Tr2RenderContextEnum's comparisons are the D3D ones, numbered from 1;
		// VkCompareOp is the same order numbered from 0. Same off-by-one the cullMode
		// case above relies on.
		if( value < Tr2RenderContextEnum::CMP_NEVER || value > Tr2RenderContextEnum::CMP_ALWAYS )
		{
			return E_INVALIDARG;
		}
		m_pipelineSource.m_depthStencilState.depthCompareOp = VkCompareOp( value - 1 );
		m_dirtyPso = true;
		return S_OK;
	case Tr2RenderContextEnum::RS_CULLMODE:
		m_pipelineSource.m_rasterizationState.cullMode = value - 1;
		m_dirtyPso = true;
		return S_OK;
	case Tr2RenderContextEnum::RS_ALPHABLENDENABLE:
		m_pipelineSource.m_attachmentBlend[0].blendEnable = value != 0;
		m_pipelineSource.m_attachmentBlend[1].blendEnable = value != 0;
		m_pipelineSource.m_attachmentBlend[2].blendEnable = value != 0;
		m_pipelineSource.m_attachmentBlend[3].blendEnable = value != 0;
		m_dirtyPso = true;
		return S_OK;

	// The blend factors and the operation apply to every attachment, which is what
	// RS_ALPHABLENDENABLE above already assumes. Per-attachment blending would need the
	// RS_COLORWRITEENABLE1..3 family, and nothing asks for it yet.
	case Tr2RenderContextEnum::RS_SRCBLEND:
	case Tr2RenderContextEnum::RS_DESTBLEND:
	case Tr2RenderContextEnum::RS_SRCBLENDALPHA:
	case Tr2RenderContextEnum::RS_DESTBLENDALPHA:
	{
		if( value >= _countof( s_blendFactorMap ) )
		{
			return E_INVALIDARG;
		}
		const VkBlendFactor factor = s_blendFactorMap[value];
		const bool isAlphaState = ( state == Tr2RenderContextEnum::RS_SRCBLENDALPHA ) ||
								  ( state == Tr2RenderContextEnum::RS_DESTBLENDALPHA );
		const bool isSource = ( state == Tr2RenderContextEnum::RS_SRCBLEND ) ||
							  ( state == Tr2RenderContextEnum::RS_SRCBLENDALPHA );
		if( isAlphaState )
		{
			// An explicit alpha factor means the caller wants the two channels to differ,
			// so stop deriving one from the other. dx12 tracks the same thing.
			m_separateAlphaBlend = true;
		}
		for( uint32_t i = 0; i < RENDER_TARGET_COUNT; ++i )
		{
			VkPipelineColorBlendAttachmentState& blend = m_pipelineSource.m_attachmentBlend[i];
			if( isAlphaState )
			{
				( isSource ? blend.srcAlphaBlendFactor : blend.dstAlphaBlendFactor ) = factor;
			}
			else
			{
				( isSource ? blend.srcColorBlendFactor : blend.dstColorBlendFactor ) = factor;
				if( !m_separateAlphaBlend )
				{
					( isSource ? blend.srcAlphaBlendFactor : blend.dstAlphaBlendFactor ) = AlphaFactorFor( factor );
				}
			}
		}
		m_dirtyPso = true;
		return S_OK;
	}

	case Tr2RenderContextEnum::RS_BLENDOP:
	case Tr2RenderContextEnum::RS_BLENDOPALPHA:
	{
		if( value >= _countof( s_blendOpMap ) )
		{
			return E_INVALIDARG;
		}
		const VkBlendOp op = s_blendOpMap[value];
		const bool isAlphaState = ( state == Tr2RenderContextEnum::RS_BLENDOPALPHA );
		for( uint32_t i = 0; i < RENDER_TARGET_COUNT; ++i )
		{
			VkPipelineColorBlendAttachmentState& blend = m_pipelineSource.m_attachmentBlend[i];
			if( isAlphaState )
			{
				blend.alphaBlendOp = op;
			}
			else
			{
				blend.colorBlendOp = op;
				if( !m_separateAlphaBlend )
				{
					blend.alphaBlendOp = op;
				}
			}
		}
		m_dirtyPso = true;
		return S_OK;
	}

	case Tr2RenderContextEnum::RS_COLORWRITEENABLE:
		// D3D's channel bits are R=1, G=2, B=4, A=8, and VkColorComponentFlagBits uses the
		// same values, so the mask carries over unchanged. dx12 masks with 0xf for the same
		// reason.
		for( uint32_t i = 0; i < RENDER_TARGET_COUNT; ++i )
		{
			m_pipelineSource.m_attachmentBlend[i].colorWriteMask = value & 0xf;
		}
		m_dirtyPso = true;
		return S_OK;
	case Tr2RenderContextEnum::RS_SRGBWRITEENABLE:
		// Honest degradation, not a stub (section 24e): without
		// VK_KHR_swapchain_mutable_format the default back buffer has no sRGB view to
		// write through, and there is no second implementation to fall back to.
		if( value != 0 && ( !m_owner || !m_owner->m_swapChainMutableFormat ) )
		{
			return E_NOTIMPL;
		}
		if( m_srgbWrite != ( value != 0 ) )
		{
			m_srgbWrite = value != 0;
			m_dirtyPass = true;
			m_dirtyPso = true;
		}
		return S_OK;
	default:
		return E_NOTIMPL;
	}
}

uint32_t Tr2RenderContextAL::SrgbWriteMaskVulkan() const
{
	if( !m_srgbWrite )
	{
		return 0;
	}
	uint32_t mask = 0;
	for( uint32_t i = 0; i < RENDER_TARGET_COUNT; ++i )
	{
		if( m_boundRenderTargets[i].IsValid() && m_boundRenderTargets[i].m_texture->HasSrgbAttachmentViewVulkan() )
		{
			mask |= 1u << i;
		}
	}
	return mask;
}

ALResult Tr2RenderContextAL::SetRenderStates( const uint32_t* stateValuePairs, uint32_t count ) throw( )
{
	// Two locals, deliberately. Both increments used to sit in one call as
	// `SetRenderState( RenderState( *p++ ), *p++ )`, and the order in which a function's
	// arguments are evaluated is unspecified -- MSVC evaluates right to left, so every
	// pair arrived with the state and the value swapped. Rendering.CanUseViewport failed
	// on E_NOTIMPL for a state it had not asked for, while the same two states set
	// individually worked. dx12's version has always used locals.
	while( count-- )
	{
		const uint32_t state = *stateValuePairs++;
		const uint32_t value = *stateValuePairs++;
		// Deliberately not FORWARD_HR, and dx12 is the reference for that: its SetRenderStates
		// calls SetRenderState and discards the result. A state block is applied best-effort, so
		// one state this backend has no case for must not drop the states after it. It used to,
		// and the cost was the whole primitives frame. SolidsWithZ sends CULLMODE FILLMODE
		// ALPHABLENDENABLE ALPHATESTENABLE ZENABLE ZWRITEENABLE ZFUNC COLORWRITEENABLE DEPTHBIAS
		// SLOPESCALEDEPTHBIAS SEPARATEALPHABLENDENABLE; FILLMODE is the second entry and is
		// E_NOTIMPL here, so the nine after it never arrived. ZFUNC was among them -- the
		// inverted-depth override had just mapped it to GREATEREQUAL -- so the pipeline kept its
		// LESS_OR_EQUAL default and every fragment failed against a depth buffer cleared to the
		// reversed-Z far plane of 0. A blank frame, from a state nobody asked about.
		//
		// Five states in that one block are still E_NOTIMPL here: FILLMODE, ALPHATESTENABLE,
		// DEPTHBIAS, SLOPESCALEDEPTHBIAS and SEPARATEALPHABLENDENABLE. Each is asked for with the
		// value this backend already defaults to, which is why skipping them is right and not
		// merely survivable. Implementing them is AL work, and it is not what this fixes.
		SetRenderState( Tr2RenderContextEnum::RenderState( state ), value );
	}
	return S_OK;
}

ALResult Tr2RenderContextAL::SetRenderTarget( const Tr2TextureAL& renderTarget, uint32_t slot, uint32_t slice ) throw()
{
	m_boundRenderTargets[slot] = renderTarget;

	if( renderTarget.IsValid() )
	{
		VkAttachmentDescription attachment = {
			0,
			renderTarget.m_texture->m_format,
			VkSampleCountFlagBits( renderTarget.GetMsaaDesc().samples ),
			VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_STORE_OP_DONT_CARE,
			// COLOR_ATTACHMENT_OPTIMAL both ends. It used to be UNDEFINED in and
			// TRANSFER_DST_OPTIMAL out, which was wrong twice: an initialLayout of
			// UNDEFINED discards the contents the LOAD_OP_LOAD above is asking to keep
			// (VUID-VkAttachmentDescription-format-06699), and a finalLayout of
			// TRANSFER_DST_OPTIMAL left every render target resting in a transfer layout,
			// so sampling one contradicted its own descriptor
			// (VUID-vkCmdDraw-imageLayout-00344). SetPass now transitions the attachment
			// into this layout before the pass opens, so declaring it is truthful.
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};
		if( memcmp( &m_renderPassSource.m_rt[slot + 1], &attachment, sizeof( attachment ) ) != 0 )
		{
			m_renderPassSource.m_rt[slot + 1] = attachment;
			m_dirtyPass = true;
		}
	}
	else
	{
		VkAttachmentDescription attachment = {
			0,
			VK_FORMAT_UNDEFINED,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_UNDEFINED,
		};
		if( memcmp( &m_renderPassSource.m_rt[slot + 1], &attachment, sizeof( attachment ) ) != 0 )
		{
			m_renderPassSource.m_rt[slot + 1] = attachment;
			m_dirtyPass = true;
		}
	}
	return S_OK;
}

ALResult Tr2RenderContextAL::SetDepthStencil( const Tr2TextureAL& depthStencil ) throw()
{
	m_boundDepthStencil = depthStencil;

	// Slot 0 of the render pass source is the depth attachment; CreateRenderPass has
	// always read it that way and nothing ever wrote it, which is why binding a depth
	// buffer was E_NOTIMPL rather than wrong.
	VkAttachmentDescription attachment = {};
	if( depthStencil.IsValid() )
	{
		attachment.format = depthStencil.m_texture->m_format;
		attachment.samples = VkSampleCountFlagBits( depthStencil.GetMsaaDesc().samples );
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		// Spelled out rather than left DONT_CARE: for a combined format the stencil ops
		// are what govern the stencil aspect, and DONT_CARE there would discard it on
		// every pass while the depth aspect survived.
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}
	else
	{
		attachment.format = VK_FORMAT_UNDEFINED;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	if( memcmp( &m_renderPassSource.m_rt[0], &attachment, sizeof( attachment ) ) != 0 )
	{
		m_renderPassSource.m_rt[0] = attachment;
		m_dirtyPass = true;
	}
	return S_OK;
}

ALResult Tr2RenderContextAL::PushRenderTarget( uint32_t slot ) throw()
{
	if( slot >= RENDER_TARGET_COUNT )
	{
		return E_INVALIDARG;
	}
	m_rtStack[slot].push_back( m_boundRenderTargets[slot] );
	return S_OK;
}

ALResult Tr2RenderContextAL::PopRenderTarget( uint32_t slot ) throw()
{
	if( slot >= RENDER_TARGET_COUNT )
	{
		return E_INVALIDARG;
	}
	if( m_rtStack[slot].empty() )
	{
		return E_INVALIDCALL;
	}
	Tr2TextureAL rt = m_rtStack[slot].back();
	m_rtStack[slot].pop_back();
	return SetRenderTarget( rt, slot );
}

ALResult Tr2RenderContextAL::PushDepthStencil() throw()
{
	m_dsStack.push_back( m_boundDepthStencil );
	return S_OK;
}

ALResult Tr2RenderContextAL::PopDepthStencil() throw()
{
	if( m_dsStack.empty() )
	{
		return E_INVALIDCALL;
	}
	Tr2TextureAL ds = m_dsStack.back();
	m_dsStack.pop_back();
	return SetDepthStencil( ds );
}

ALResult Tr2RenderContextAL::SetViewport( const Tr2Viewport& viewport ) throw()
{
	m_viewport = viewport;

	// A degenerate rectangle is not a viewport. TriDevice::Render pushes mViewport on every
	// frame (TriDevice.cpp:1165), and mViewport is 0x0 for the whole life of a device created
	// as CreateWindowedDevice( hwnd, 0, 0 ) -- "render to the entire window area" -- because
	// mWidth/mHeight are set from the *arguments* (TriDevice.cpp:286) and nothing back-fills
	// them from the swap chain. DX11 and DX12 hand that straight to the API, which accepts a
	// zero-extent viewport and simply draws nothing; Vulkan rejects it outright
	// (VUID-VkViewport-width-01770). So it is read here as "no viewport chosen", which is the
	// state the context starts in and whose fallback is the whole render target.
	m_viewportSet = viewport.m_width > 0.0f && viewport.m_height > 0.0f;

	// Recorded here *as well as* in SetPass, and that is not a duplicate. The viewport is
	// dynamic state, so a change made while a pass is open has to be recorded into that pass:
	// SetPass returns early when !m_dirtyPass, so without this the value the pass opened with
	// stands for every draw inside it. That is what the primitives sample hit -- the pass
	// opened with the 0x0 viewport above, Tr2EffectStateManager set the real one immediately
	// afterwards, and nothing ever reached the command buffer. Keeping it in SetPass too is
	// what survives a pass restart, which the clear path and the query pools both cause.
	if( m_renderingActive && m_viewportSet )
	{
		VkViewport vp = FlippedViewport( m_viewport.m_x, m_viewport.m_y, m_viewport.m_width,
			m_viewport.m_height, m_viewport.m_minZ, m_viewport.m_maxZ );
		vkCmdSetViewport( m_commandBuffer, 0, 1, &vp );
	}
	return S_OK;
}

ALResult Tr2RenderContextAL::GetViewport( Tr2Viewport& viewport ) throw()
{
	if( m_viewportSet )
	{
		viewport = m_viewport;
		return S_OK;
	}

	// Nobody has set one, so report the default: the whole of render target 0. Returning a
	// stored copy instead would be wrong the moment the render target changed size.
	if( !m_boundRenderTargets[0].IsValid() )
	{
		return E_FAIL;
	}
	viewport = Tr2Viewport( m_boundRenderTargets[0].GetWidth(), m_boundRenderTargets[0].GetHeight() );
	return S_OK;
}

ALResult Tr2RenderContextAL::DrawIndexedPrimitive(
	uint32_t numVertices,
	uint32_t startIndex,
	uint32_t primitiveCount,
	uint32_t minimumIndex ) throw( )
{
	SetPipeline();

	auto vc = m_primitiveToVertexCount.first * primitiveCount + m_primitiveToVertexCount.second;
	CCP_STATS_ADD( vertexCount, vc );

	vkCmdDrawIndexed( m_commandBuffer, vc, 1, 0, 0, 0 );
	return S_OK;
}

ALResult Tr2RenderContextAL::DrawPrimitive( uint32_t startVertex, uint32_t primitiveCount ) throw( )
{
	SetPipeline();

	auto vc = m_primitiveToVertexCount.first * primitiveCount + m_primitiveToVertexCount.second;
	CCP_STATS_ADD( vertexCount, vc );

	vkCmdDraw( m_commandBuffer, vc, 1, 0, 0 );
	return S_OK;
}

ALResult Tr2RenderContextAL::DrawIndexedInstanced(
	uint32_t numVertices,
	uint32_t startIndex,
	uint32_t primitiveCount,
	uint32_t numInstances ) throw( )
{
	SetPipeline();

	auto vc = m_primitiveToVertexCount.first * primitiveCount + m_primitiveToVertexCount.second;
	CCP_STATS_ADD( vertexCount, vc * numInstances );

	// startIndex is honoured here even though DrawIndexedPrimitive above still ignores it.
	// Passing it is free and leaving it out would be a second copy of that defect.
	vkCmdDrawIndexed(
		m_commandBuffer,
		vc,
		numInstances,
		startIndex,
		0,
		0 );
	return S_OK;
}

ALResult Tr2RenderContextAL::DrawIndexedInstanced(
	uint32_t indexCountPerInstance,
	uint32_t instanceCount,
	uint32_t startIndexLocation,
	int32_t baseVertexLocation,
	uint32_t startInstanceLocation ) throw( )
{
	SetPipeline();

	CCP_STATS_ADD( vertexCount, indexCountPerInstance * instanceCount );

	vkCmdDrawIndexed(
		m_commandBuffer,
		indexCountPerInstance,
		instanceCount,
		startIndexLocation,
		baseVertexLocation,
		startInstanceLocation );
	return S_OK;
}

ALResult Tr2RenderContextAL::DrawInstanced(
	uint32_t vertexCountPerInstance,
	uint32_t instanceCount,
	uint32_t startVertexLocation,
	uint32_t startInstanceLocation ) throw( )
{
	SetPipeline();

	CCP_STATS_ADD( vertexCount, vertexCountPerInstance * instanceCount );

	vkCmdDraw(
		m_commandBuffer,
		vertexCountPerInstance,
		instanceCount,
		startVertexLocation,
		startInstanceLocation );
	return S_OK;
}

ALResult Tr2RenderContextAL::DrawInstancedIndirect( Tr2BufferAL& params, uint32_t offset ) throw( )
{
	if( !params.IsValid() )
	{
		return E_INVALIDARG;
	}
	SetPipeline();

	// drawCount 1, so the stride is never read; passing 0 for it is the documented way of
	// saying so rather than an oversight.
	vkCmdDrawIndirect( m_commandBuffer, params.m_buffer->GetBufferVulkan(), offset, 1, 0 );
	return S_OK;
}

ALResult Tr2RenderContextAL::DrawIndexedInstancedIndirect( Tr2BufferAL& params, uint32_t offset ) throw( )
{
	if( !params.IsValid() )
	{
		return E_INVALIDARG;
	}
	SetPipeline();

	vkCmdDrawIndexedIndirect( m_commandBuffer, params.m_buffer->GetBufferVulkan(), offset, 1, 0 );
	return S_OK;
}

ALResult Tr2RenderContextAL::DrawIndexedPrimitiveUP(
	uint32_t numVertices,
	uint32_t primitiveCount,
	const uint32_t* indexData,
	const void* vertexStreamZeroData,
	uint32_t vertexStreamZeroStride ) throw( )
{
	return m_drawUPHelper.DrawIndexedPrimitiveUP( m_topology, numVertices, primitiveCount, indexData, vertexStreamZeroData, vertexStreamZeroStride, *this, *m_owner );
}

ALResult Tr2RenderContextAL::DrawIndexedPrimitiveUP(
	uint32_t numVertices,
	uint32_t primitiveCount,
	const uint16_t* indexData,
	const void* vertexStreamZeroData,
	uint32_t vertexStreamZeroStride ) throw( )
{
	return m_drawUPHelper.DrawIndexedPrimitiveUP( m_topology, numVertices, primitiveCount, indexData, vertexStreamZeroData, vertexStreamZeroStride, *this, *m_owner );
}

ALResult Tr2RenderContextAL::DrawPrimitiveUP(
	uint32_t primitiveCount,
	const void* vertexStreamZeroData,
	uint32_t VertexStreamZeroStride ) throw( )
{
	return m_drawUPHelper.DrawPrimitiveUP( m_topology, primitiveCount, vertexStreamZeroData, VertexStreamZeroStride, *this, *m_owner );
}

namespace
{
	// A UAV is bound to its descriptor as VK_IMAGE_LAYOUT_GENERAL, and GENERAL is one of
	// the layouts vkCmdClearColorImage accepts, so clearing there avoids a transition out
	// and straight back again for an image that is about to be written by a shader.
	const VkImageLayout UAV_CLEAR_LAYOUT = VK_IMAGE_LAYOUT_GENERAL;
}

ALResult Tr2RenderContextAL::ClearUav( const Tr2TextureAL& rt, uint32_t mip, const float values[4] ) throw( )
{
	if( !rt.IsValid() )
	{
		return E_INVALIDARG;
	}
	VkClearColorValue clearColor;
	memcpy( clearColor.float32, values, sizeof( clearColor.float32 ) );

	EndRenderPassVulkan();
	rt.m_texture->TransitionForTransferWriteVulkan( m_commandBuffer, UAV_CLEAR_LAYOUT );

	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, VK_REMAINING_ARRAY_LAYERS };
	vkCmdClearColorImage( m_commandBuffer, rt.m_texture->GetImageVulkan(), UAV_CLEAR_LAYOUT, &clearColor, 1, &range );
	return S_OK;
}

ALResult Tr2RenderContextAL::ClearUav( const Tr2TextureAL& rt, uint32_t mip, const uint32_t values[4] ) throw( )
{
	if( !rt.IsValid() )
	{
		return E_INVALIDARG;
	}
	VkClearColorValue clearColor;
	memcpy( clearColor.uint32, values, sizeof( clearColor.uint32 ) );

	EndRenderPassVulkan();
	rt.m_texture->TransitionForTransferWriteVulkan( m_commandBuffer, UAV_CLEAR_LAYOUT );

	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, VK_REMAINING_ARRAY_LAYERS };
	vkCmdClearColorImage( m_commandBuffer, rt.m_texture->GetImageVulkan(), UAV_CLEAR_LAYOUT, &clearColor, 1, &range );
	return S_OK;
}

ALResult Tr2RenderContextAL::ClearUav( const Tr2BufferAL& buffer, const float values[4] ) throw( )
{
	if( !buffer.IsValid() )
	{
		return E_INVALIDARG;
	}
	uint32_t bits;
	memcpy( &bits, &values[0], sizeof( bits ) );

	EndRenderPassVulkan();
	vkCmdFillBuffer( m_commandBuffer, buffer.m_buffer->GetBufferVulkan(), 0, VK_WHOLE_SIZE, bits );
	return S_OK;
}

ALResult Tr2RenderContextAL::ClearUav( const Tr2BufferAL& buffer, const uint32_t values[4] ) throw( )
{
	if( !buffer.IsValid() )
	{
		return E_INVALIDARG;
	}

	EndRenderPassVulkan();
	vkCmdFillBuffer( m_commandBuffer, buffer.m_buffer->GetBufferVulkan(), 0, VK_WHOLE_SIZE, values[0] );
	return S_OK;
}

ALResult Tr2RenderContextAL::SetPass()
{
	if( !m_dirtyPass )
	{
		return S_OK;
	}

	// A pass can still be open: SetRenderTarget and SetViewport only mark the source
	// dirty, they do not close anything. vkCmdBeginRenderPass inside an active pass is
	// VUID-vkCmdBeginRenderPass-renderpass, and it only became reachable once a second
	// render target could be bound. Closing here rather than at the rebind keeps the
	// pass open for as long as possible, which is what a tiler wants.
	EndRenderPassVulkan();

	// Everything the pass is about to touch has to be in the layout the pass declares,
	// and the barriers that get it there are illegal once the pass is open -- so this is
	// the only place they can go. A texture that is already in the right layout costs
	// nothing here; TransitionVulkan emits no barrier in that case.
	// Resource set first, attachments second, and the order is load-bearing. A texture can
	// appear in both -- a depth buffer sampled in one pass and rendered into the next --
	// and whichever runs last decides the layout the pass actually begins in. The pass
	// declares its attachment layouts, so the attachments have to win, or
	// vkCmdBeginRenderPass finds SHADER_READ_ONLY_OPTIMAL where it promised
	// DEPTH_STENCIL_ATTACHMENT_OPTIMAL (VUID-vkCmdBeginRenderPass-initialLayout-00900).
	if( m_resourceSet.IsValid() )
	{
		m_resourceSet.m_resourceSet->TransitionImagesVulkan( m_commandBuffer );
	}
	if( m_boundDepthStencil.IsValid() )
	{
		m_boundDepthStencil.m_texture->TransitionVulkan( m_commandBuffer, m_readOnlyDepth
			? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
			: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );
	}
	for( uint32_t i = 0; i < RENDER_TARGET_COUNT; ++i )
	{
		if( m_boundRenderTargets[i].IsValid() )
		{
			m_boundRenderTargets[i].m_texture->TransitionVulkan( m_commandBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL );
		}
	}

	// Dynamic rendering: the attachments are named directly, no VkRenderPass or
	// VkFramebuffer objects and no cache keyed on either. Slot 0 of the source is depth,
	// slots 1-4 the colour slots; colorAttachmentCount is the highest bound slot plus one,
	// with any gap below it declared as an unused attachment (null imageView, format
	// UNDEFINED in the pipeline), because a fragment shader's output locations index this
	// array positionally.
	//
	// The loadOp/storeOp discipline is unchanged from the render-pass version -- LOAD and
	// STORE, taken from the same attachment descriptions SetRenderTarget and
	// SetDepthStencil have always written. On a tiler those ops are where bandwidth goes,
	// and 1.3 relocates them into VkRenderingAttachmentInfo without changing what they
	// mean.
	const uint32_t srgbMask = SrgbWriteMaskVulkan();
	VkRenderingAttachmentInfo colorAttachments[RENDER_TARGET_COUNT];
	uint32_t colorAttachmentCount = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	for( uint32_t i = 0; i < RENDER_TARGET_COUNT; ++i )
	{
		VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		attachment.resolveMode = VK_RESOLVE_MODE_NONE;
		if( m_renderPassSource.m_rt[i + 1].format != VK_FORMAT_UNDEFINED && m_boundRenderTargets[i].IsValid() )
		{
			// Mip 0, slice 0: nothing in the AL binds a render target at another level yet,
			// and the slice argument to SetRenderTarget is not plumbed through either. Both
			// belong with mip generation, which is a separate cluster.
			attachment.imageView = m_boundRenderTargets[i].m_texture->GetAttachmentViewVulkan( 0, 0, ( srgbMask & ( 1u << i ) ) != 0 );
			attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachment.loadOp = m_renderPassSource.m_rt[i + 1].loadOp;
			attachment.storeOp = m_renderPassSource.m_rt[i + 1].storeOp;
			colorAttachmentCount = i + 1;
			if( width == 0 )
			{
				width = m_boundRenderTargets[i].GetWidth();
				height = m_boundRenderTargets[i].GetHeight();
			}
		}
		colorAttachments[i] = attachment;
	}

	VkRenderingAttachmentInfo depthAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	depthAttachment.resolveMode = VK_RESOLVE_MODE_NONE;
	VkRenderingAttachmentInfo stencilAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	stencilAttachment.resolveMode = VK_RESOLVE_MODE_NONE;
	bool haveDepth = false;
	bool haveStencil = false;
	if( m_renderPassSource.m_rt[0].format != VK_FORMAT_UNDEFINED && m_boundDepthStencil.IsValid() )
	{
		const VkImageAspectFlags aspect = TrinityALImpl::GetAspectMaskVulkan( m_renderPassSource.m_rt[0].format );
		const VkImageLayout depthLayout = m_readOnlyDepth
			? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
			: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		if( aspect & VK_IMAGE_ASPECT_DEPTH_BIT )
		{
			depthAttachment.imageView = m_boundDepthStencil.m_texture->GetAttachmentViewVulkan( 0, 0 );
			depthAttachment.imageLayout = depthLayout;
			depthAttachment.loadOp = m_renderPassSource.m_rt[0].loadOp;
			depthAttachment.storeOp = m_renderPassSource.m_rt[0].storeOp;
			haveDepth = true;
		}
		if( aspect & VK_IMAGE_ASPECT_STENCIL_BIT )
		{
			// The stencil aspect is governed by the stencil ops, exactly as the
			// VkAttachmentDescription's stencilLoadOp/stencilStoreOp governed it before.
			stencilAttachment.imageView = m_boundDepthStencil.m_texture->GetAttachmentViewVulkan( 0, 0 );
			stencilAttachment.imageLayout = depthLayout;
			stencilAttachment.loadOp = m_renderPassSource.m_rt[0].stencilLoadOp;
			stencilAttachment.storeOp = m_renderPassSource.m_rt[0].stencilStoreOp;
			haveStencil = true;
		}
		if( width == 0 )
		{
			width = m_boundDepthStencil.GetWidth();
			height = m_boundDepthStencil.GetHeight();
		}
	}

	if( colorAttachmentCount == 0 && !haveDepth && !haveStencil )
	{
		// Nothing bound at all. The render-pass version reached vkCmdBeginRenderPass with a
		// null framebuffer here, which no draw survives either; saying so is strictly
		// better than recording an invalid begin.
		return E_INVALIDCALL;
	}

	VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	renderingInfo.renderArea = { { 0, 0 }, { width, height } };
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = colorAttachmentCount;
	renderingInfo.pColorAttachments = colorAttachmentCount ? colorAttachments : nullptr;
	renderingInfo.pDepthAttachment = haveDepth ? &depthAttachment : nullptr;
	renderingInfo.pStencilAttachment = haveStencil ? &stencilAttachment : nullptr;

	m_owner->m_vkCmdBeginRendering( m_commandBuffer, &renderingInfo );
	m_renderingActive = true;

	// FlippedViewport applies the y flip; only the source of the rectangle differs. m_viewportSet
	// is false both before any SetViewport and after one with a degenerate rectangle, and the
	// fallback for both is the whole render target.
	const bool haveViewport = m_viewportSet;
	VkViewport viewport = haveViewport
		? FlippedViewport( m_viewport.m_x, m_viewport.m_y, m_viewport.m_width,
			m_viewport.m_height, m_viewport.m_minZ, m_viewport.m_maxZ )
		: FlippedViewport( 0.0f, 0.0f, float( width ), float( height ), 0.0f, 1.0f );

	VkRect2D scissor = {
		{ 0, 0 },
		{ width, height }
	};

	vkCmdSetViewport( m_commandBuffer, 0, 1, &viewport );
	vkCmdSetScissor( m_commandBuffer, 0, 1, &scissor );
	m_dirtyPass = false;
	m_dirtyPso = true;
	return S_OK;
}

ALResult Tr2RenderContextAL::SetPipeline()
{
	// Checked before the m_dirtyPass test rather than folded into SetPass, because a
	// resource set can change without the pass source changing at all. Asking the set
	// first means a pass only restarts when an image genuinely has to move -- marking the
	// pass dirty on every SetResourceSet would restart it on almost every draw.
	if( m_resourceSet.IsValid() && m_resourceSet.m_resourceSet->NeedsTransitionVulkan() )
	{
		EndRenderPassVulkan();
	}

	if( m_dirtyPass )
	{
		SetPass();
	}
	if( !m_dirtyPso )
	{
		return S_OK;
	}

	// The attachments are part of the key, not just the pipeline state. A pipeline is
	// created against specific attachment formats -- VkPipelineRenderingCreateInfo under
	// dynamic rendering (VUID-vkCmdDraw-dynamicRenderingUnusedAttachments-08911 and
	// friends) exactly as VkRenderPass compatibility was before -- and PipelineSource does
	// not describe the attachments at all. Without this, the first pipeline built for a
	// colour-only pass was handed straight back for a pass that also had depth.
	auto hash = m_pipelineSource.GetHash() ^ ( m_renderPassSource.GetHash() * 0x9e3779b9u )
		// The sRGB write mask changes the attachment formats the pipeline is created
		// against without touching either source struct, so it has to be in the key.
		^ ( size_t( SrgbWriteMaskVulkan() ) * 0x85ebca6bu );
	auto found = m_owner->m_pipelines.find( hash );
	VkPipeline pipeline;
	if( found == m_owner->m_pipelines.end() )
	{
		FORWARD_HR( CreatePipeline( pipeline ) );
		m_owner->m_pipelines[hash] = pipeline;
	}
	else
	{
		pipeline = found->second;
	}
	vkCmdBindPipeline( m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );

	auto* program = m_pipelineSource.m_shaderProgram.m_program.get();
	// The layout comparison is load-bearing: the AL's state machine carries a bound
	// resource set across shader program changes, and a set is only legal with the
	// layout it was allocated against. Drawing CanSampleVolumeTexture's set with
	// CanSampleUnassignedTexture's 2D-sampling program was this suite's one flaky test.
	if( m_resourceSet.IsValid() && m_resourceSet.m_resourceSet->m_descriptorSet && program && program->m_resourceLayout
		&& m_resourceSet.m_resourceSet->m_layout == program->m_resourceLayout )
	{
		vkCmdBindDescriptorSets( 
			m_commandBuffer, 
			VK_PIPELINE_BIND_POINT_GRAPHICS, 
			program->m_pipelineLayout, 
			1, 
			1,
			&m_resourceSet.m_resourceSet->m_descriptorSet, 0, nullptr );
	}
	else if( program && program->m_resourceLayout )
	{
		// The program statically uses set 1 and the caller bound nothing. Without this
		// the draw consumed whatever the recycled pool last held -- a stale set from an
		// unrelated test, surfacing as viewType-07752 / layout-08600 at a rate that made
		// Rendering.CanSampleUnassignedTexture the suite's one flaky test. The default
		// set binds the dummies, so sampling an unassigned slot reads opaque black.
		VkDescriptorSet defaultSet = program->GetDefaultResourceSetVulkan( *m_owner );
		if( defaultSet != VK_NULL_HANDLE )
		{
			vkCmdBindDescriptorSets(
				m_commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				program->m_pipelineLayout,
				1,
				1,
				&defaultSet, 0, nullptr );
		}
	}

	FORWARD_HR( BindConstantBuffers( VK_PIPELINE_BIND_POINT_GRAPHICS ) );
	return S_OK;
}

void Tr2RenderContextAL::InvalidateAttachmentsVulkan()
{
	m_dirtyPass = true;
}

void Tr2RenderContextAL::EndRenderPassVulkan()
{
	if( m_renderingActive )
	{
		m_owner->m_vkCmdEndRendering( m_commandBuffer );
		m_renderingActive = false;
		m_dirtyPass = true;
	}
}

void Tr2RenderContextAL::ResetConstantPoolVulkan()
{
	if( m_constantPool == VK_NULL_HANDLE || !m_owner )
	{
		return;
	}

	// Waiting on this frame slot's fence proves the work from VIRTUAL_FRAMES ago is done,
	// not that the last two frames are -- and they allocated from this same pool.
	// vkResetDescriptorPool on a pool a live command buffer still references is
	// VUID-vkResetDescriptorPool-descriptorPool-00313. The frame numbers added for
	// Tr2FenceAL answer exactly this question, so ask them rather than adding a second
	// mechanism. If the answer is no, the sets simply survive another frame.
	if( m_owner->GetRenderedFrameNumber() < m_constantPoolLastUse )
	{
		return;
	}

	vkResetDescriptorPool( m_owner->m_device, m_constantPool, 0 );
	m_constantSet = VK_NULL_HANDLE;
	m_constantsDirty = true;
}

ALResult Tr2RenderContextAL::BindConstantBuffers( VkPipelineBindPoint bindPoint )
{
	auto* program = m_pipelineSource.m_shaderProgram.m_program.get();
	if( !program || !program->m_constantLayout || m_constantBuffers.empty() )
	{
		return S_OK;
	}
	if( m_boundConstantLayout != program->m_constantLayout )
	{
		m_boundConstantLayout = program->m_constantLayout;
		if( m_constantPool )
		{
			m_owner->DestroyLaterVulkan( m_constantPool, vkDestroyDescriptorPool );
			m_constantPool = VK_NULL_HANDLE;
		}
		m_constantSet = VK_NULL_HANDLE;
	}

	if( !m_constantSet )
	{
		if( !m_constantPool )
		{
			// One set per SetConstants that actually changes something, for a whole
			// frame, so maxSets is a frame's budget rather than 1. The pool is reset in
			// BeginFrame, after the frame fence has been waited on -- which is the only
			// moment nothing in flight can still reference a set from it.
			VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CONSTANT_SETS_PER_FRAME * 4 };
			VkDescriptorPoolCreateInfo poolInfo = {
				VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				nullptr,
				0,
				CONSTANT_SETS_PER_FRAME,
				1,
				&poolSize
			};
			CR_RETURN_HR( Vk2Al( vkCreateDescriptorPool( m_owner->m_device, &poolInfo, nullptr, &m_constantPool ) ) );
		}

		VkDescriptorSetAllocateInfo allocateInfo = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			nullptr,
			m_constantPool,
			1,
			&program->m_constantLayout
		};
		CR_RETURN_HR( Vk2Al( vkAllocateDescriptorSets( m_owner->m_device, &allocateInfo, &m_constantSet ) ) );
		m_constantsDirty = true;
	}

	if( m_constantsDirty )
	{
		// A fresh set rather than a rewrite of the bound one. vkUpdateDescriptorSets on a
		// set that a command buffer has already bound invalidates that command buffer --
		// "destroyed or updated without UPDATE_AFTER_BIND" -- and every command recorded
		// afterwards is rejected. Nothing reached this before, because it needs two draws
		// with different constants in one frame, which needed depth states to get to.
		VkDescriptorSetAllocateInfo reallocateInfo = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			nullptr,
			m_constantPool,
			1,
			&program->m_constantLayout
		};
		VkDescriptorSet fresh = VK_NULL_HANDLE;
		if( SUCCEEDED( Vk2Al( vkAllocateDescriptorSets( m_owner->m_device, &reallocateInfo, &fresh ) ) ) )
		{
			m_constantSet = fresh;
			m_constantPoolLastUse = m_owner->GetRecordingFrameNumber();
		}
		// If the pool is exhausted the old set is rewritten, which is what used to happen
		// always. Better a validation error than a dropped draw.

		std::vector<VkWriteDescriptorSet> writes;
		std::vector<VkDescriptorBufferInfo> bufferInfos;
		writes.reserve( m_constantBuffers.size() );
		bufferInfos.reserve( m_constantBuffers.size() );
		for( auto it = begin( m_constantBuffers ); it != end( m_constantBuffers ); ++it )
		{
			// Only the bindings this program's layout declares. m_constantBuffers holds
			// whatever the caller has set, which outlives any one shader program, and
			// writing a descriptor to a binding the layout does not have is
			// VUID-VkWriteDescriptorSet-dstBinding-00315.
			if( program->m_constantBindings.find( it->first ) == program->m_constantBindings.end() )
			{
				continue;
			}
			VkDescriptorBufferInfo bufferInfo = { it->second, 0, VK_WHOLE_SIZE };
			bufferInfos.push_back( bufferInfo );
			VkWriteDescriptorSet write = {
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				nullptr,
				m_constantSet,
				it->first,
				0,
				1,
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				nullptr,
				&bufferInfos.back(),
				nullptr
			};
			writes.push_back( write );
		}
		vkUpdateDescriptorSets( m_owner->m_device, uint32_t( writes.size() ), writes.data(), 0, nullptr );

		m_constantsDirty = false;
	}

	vkCmdBindDescriptorSets(
		m_commandBuffer,
		bindPoint,
		program->m_pipelineLayout,
		0,
		1,
		&m_constantSet,
		0,
		nullptr );
	return S_OK;
}

ALResult Tr2RenderContextAL::RunComputeShader( unsigned groupDimX, unsigned groupDimY, unsigned groupDimZ ) throw( )
{
	auto* program = m_pipelineSource.m_shaderProgram.m_program.get();
	if( !program || !program->m_pipelineLayout || program->m_shaderInfo.empty() )
	{
		return E_INVALIDCALL;
	}

	// The compute pipeline is determined by the shader module and pipeline layout,
	// both owned by the program. Cache it keyed by the layout, invalidating on
	// program change (the same handle-reuse concern as the constant descriptor set).
	if( m_computePipelineLayout != program->m_pipelineLayout )
	{
		m_computePipelineLayout = program->m_pipelineLayout;
		if( m_computePipeline )
		{
			m_owner->DestroyLaterVulkan( m_computePipeline, vkDestroyPipeline );
			m_computePipeline = VK_NULL_HANDLE;
		}
	}

	if( !m_computePipeline )
	{
		VkComputePipelineCreateInfo pipelineInfo = {
			VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			nullptr,
			0,
			program->m_shaderInfo[0],
			program->m_pipelineLayout,
			VK_NULL_HANDLE,
			0
		};
		CR_RETURN_HR( Vk2Al( vkCreateComputePipelines( m_owner->m_device, m_owner->m_pipelineCache, 1, &pipelineInfo, nullptr, &m_computePipeline ) ) );
	}

	vkCmdBindPipeline( m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline );

	if( m_resourceSet.IsValid() && m_resourceSet.m_resourceSet->m_descriptorSet && program->m_resourceLayout )
	{
		vkCmdBindDescriptorSets(
			m_commandBuffer,
			VK_PIPELINE_BIND_POINT_COMPUTE,
			program->m_pipelineLayout,
			1,
			1,
			&m_resourceSet.m_resourceSet->m_descriptorSet, 0, nullptr );
	}

	FORWARD_HR( BindConstantBuffers( VK_PIPELINE_BIND_POINT_COMPUTE ) );

	vkCmdDispatch( m_commandBuffer, groupDimX, groupDimY, groupDimZ );

	return S_OK;
}

ALResult Tr2RenderContextAL::CreatePipeline( VkPipeline& pipeline )
{
	std::vector<VkVertexInputAttributeDescription> layout;
	m_pipelineSource.m_layout.m_layout->PopulateInputLayoutVulkan( layout, m_pipelineSource.m_shaderProgram.m_program->m_shaderInputs );

	// m_pipelineSource.m_streams carries only the stride, because SetStreamSource is the
	// only thing that writes it and a stride is all it knows. binding and inputRate were
	// left at whatever the memset put there, which is 0 -- so with two streams bound both
	// descriptions claimed binding 0 and nothing described binding 1
	// (VUID-VkPipelineVertexInputStateCreateInfo-pVertexBindingDescriptions-00616 and
	// -binding-00615). The vertex layout has computed the per-stream input rate since
	// before anything read it; instanced data was being fetched per vertex.
	const uint32_t streamCount = m_pipelineSource.m_layout.m_layout->m_streamCount;
	VkVertexInputBindingDescription bindings[MAX_VERTEX_STREAMS];
	for( uint32_t i = 0; i < streamCount && i < MAX_VERTEX_STREAMS; ++i )
	{
		bindings[i].binding = i;
		bindings[i].stride = m_pipelineSource.m_streams[i].stride;
		bindings[i].inputRate = m_pipelineSource.m_layout.m_layout->m_streamRates[i];
	}

	VkPipelineVertexInputStateCreateInfo vertexInput = {
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		nullptr,
		0,
		streamCount < MAX_VERTEX_STREAMS ? streamCount : MAX_VERTEX_STREAMS,
		bindings,
		uint32_t( layout.size() ),
		layout.data()
	};

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		nullptr,
		0,
		m_pipelineSource.m_topology,
		VK_FALSE
	};

	VkPipelineViewportStateCreateInfo viewport = {
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		nullptr,
		0,
		1,
		nullptr,
		1,
		nullptr
	};

	// The pipeline's sample count has to match the attachments the render pass declares.
	// It was hardcoded to one, so any pipeline used with a multisampled render target was
	// VUID-VkGraphicsPipelineCreateInfo-multisampledRenderToSingleSampled-06853. The count
	// comes from the same place the attachment description takes it from, so the two
	// cannot disagree.
	VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
	if( m_boundRenderTargets[0].IsValid() && m_boundRenderTargets[0].GetMsaaDesc().samples > 1 )
	{
		sampleCount = VkSampleCountFlagBits( m_boundRenderTargets[0].GetMsaaDesc().samples );
	}

	VkPipelineMultisampleStateCreateInfo msaa = {
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		nullptr,
		0,
		sampleCount,
		VK_FALSE,
		1.0f,
		nullptr,
		VK_FALSE,
		VK_FALSE
	};

	VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, };

	VkPipelineDynamicStateCreateInfo dynamicInfo = {
		VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		nullptr,
		0,
		2,
		dynamicStates
	};

	// Dynamic rendering: the pipeline is created against attachment formats, not a
	// VkRenderPass. The formats come from the same attachment descriptions SetPass builds
	// its VkRenderingAttachmentInfo from, so the two cannot disagree; the array is
	// positional with UNDEFINED marking an unbound slot, matching SetPass's unused
	// attachments. The stencil format is set only when the depth format actually has a
	// stencil aspect (VUID-VkGraphicsPipelineCreateInfo-renderPass-06054's dynamic-
	// rendering counterpart works per aspect).
	const uint32_t srgbMask = SrgbWriteMaskVulkan();
	VkFormat colorFormats[RENDER_TARGET_COUNT];
	uint32_t colorAttachmentCount = 0;
	for( uint32_t i = 0; i < RENDER_TARGET_COUNT; ++i )
	{
		colorFormats[i] = m_renderPassSource.m_rt[i + 1].format;
		if( ( srgbMask & ( 1u << i ) ) != 0 )
		{
			// The same decision SetPass made when it picked the attachment view; the
			// pipeline must declare the view's format, not the image's.
			colorFormats[i] = TrinityALImpl::GetSrgbCounterpartVulkan( colorFormats[i] );
		}
		if( colorFormats[i] != VK_FORMAT_UNDEFINED )
		{
			colorAttachmentCount = i + 1;
		}
	}
	const VkFormat depthStencilFormat = m_renderPassSource.m_rt[0].format;
	const VkImageAspectFlags depthStencilAspect = depthStencilFormat != VK_FORMAT_UNDEFINED
		? TrinityALImpl::GetAspectMaskVulkan( depthStencilFormat )
		: 0;

	VkPipelineRenderingCreateInfo renderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	renderingCreateInfo.viewMask = 0;
	renderingCreateInfo.colorAttachmentCount = colorAttachmentCount;
	renderingCreateInfo.pColorAttachmentFormats = colorAttachmentCount ? colorFormats : nullptr;
	renderingCreateInfo.depthAttachmentFormat = ( depthStencilAspect & VK_IMAGE_ASPECT_DEPTH_BIT ) ? depthStencilFormat : VK_FORMAT_UNDEFINED;
	renderingCreateInfo.stencilAttachmentFormat = ( depthStencilAspect & VK_IMAGE_ASPECT_STENCIL_BIT ) ? depthStencilFormat : VK_FORMAT_UNDEFINED;

	// The blend state's attachmentCount has to equal colorAttachmentCount under dynamic
	// rendering (VUID-VkGraphicsPipelineCreateInfo-renderPass-06060). The stored state is
	// written by SetRenderState with a fixed count of one, so it is corrected on a local
	// copy here rather than at every write site; pAttachments already points at all four
	// per-attachment blends.
	VkPipelineColorBlendStateCreateInfo colorBlendState = m_pipelineSource.m_colorBlendState;
	colorBlendState.attachmentCount = colorAttachmentCount;
	colorBlendState.pAttachments = m_pipelineSource.m_attachmentBlend;

	VkGraphicsPipelineCreateInfo pipelineCreateInfo = {
		VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		&renderingCreateInfo,
		0,
		static_cast<uint32_t>( m_pipelineSource.m_shaderProgram.m_program->m_shaderInfo.size() ),
		m_pipelineSource.m_shaderProgram.m_program->m_shaderInfo.data(),
		&vertexInput,
		&inputAssembly,
		nullptr,
		&viewport,
		&m_pipelineSource.m_rasterizationState,
		&msaa,
		// pDepthStencilState was null unconditionally, which is legal only while no
		// pass has a depth attachment -- true for as long as SetDepthStencil could not
		// bind one. With one bound it is VUID-VkGraphicsPipelineCreateInfo-renderPass-09028.
		// The state itself has been carried in m_pipelineSource since before this backend
		// could use it; SetRenderState is what fills it in.
		m_renderPassSource.m_rt[0].format != VK_FORMAT_UNDEFINED ? &m_pipelineSource.m_depthStencilState : nullptr,
		&colorBlendState,
		&dynamicInfo,
		m_pipelineSource.m_shaderProgram.m_program->m_pipelineLayout,
		VK_NULL_HANDLE,
		0,
		VK_NULL_HANDLE,
		-1
	};
	return Vk2Al( vkCreateGraphicsPipelines( m_owner->m_device, m_owner->m_pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipeline ) );
}

void Tr2RenderContextAL::RenderPassHint( const Tr2ColorAttachment&, const Tr2DepthAttachment& )
{
}

void Tr2RenderContextAL::RenderPassHint( const Tr2ColorAttachment&, const Tr2ColorAttachment&, const Tr2DepthAttachment& )
{
}

ALResult Tr2RenderContextAL::UseTextures( Tr2GpuUsage::Type, size_t, Tr2TextureAL* )
{
	return S_OK;
}

ALResult Tr2RenderContextAL::UseAccelerationStructure( Tr2RtTopLevelAccelerationStructureAL tlas )
{
    return S_OK;
}


#endif
