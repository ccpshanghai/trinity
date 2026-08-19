// Copyright © 2026 CCP ehf.

#pragma once


#if TRINITY_PLATFORM == TRINITY_VULKAN

#include <map>

#include "../Tr2HalHelperStructures.h"
#include "../Tr2RenderContextEnum.h"
#include "../Tr2DrawUPHelper.h"
#include "../include/Tr2ConstantBufferAL.h"
#include "../include/Tr2ResourceSetAL.h"
#include "../include/Tr2TextureAL.h"
#include "../include/Tr2VertexLayoutAL.h"
#include "../include/Tr2ShaderProgramAL.h"
#include "../include/Tr2RenderPassAL.h"
#include "../include/upscaling/Tr2UpscalingAL.h"


class Tr2ConstantBufferAL;
class Tr2VertexLayoutAL;
struct ITr2RenderContextEvents;

class Tr2ShaderAL;
class Tr2SamplerStateAL;
class Tr2BufferAL;
struct Tr2Viewport;
class Tr2RtShaderTableAL;
class Tr2RtTopLevelAccelerationStructureAL;


// -------------------------------------------------------------
// Description:
//   See http://carbon/wiki/Tr2RenderContext
// -------------------------------------------------------------
class Tr2RenderContextAL
{
public:
	Tr2RenderContextAL() throw( );
	~Tr2RenderContextAL() throw( );

	void Destroy() throw( );
	bool IsValid() const throw( );

	// A no-op, as it is on dx12: every render context already knows its owning device, so
	// there is nothing for the caller to tell it. The tests call it, which is why it
	// exists at all.
	static void SetPrimaryRenderContext( Tr2PrimaryRenderContextAL* )
	{

	}

	// These were returning a null reference and a null pointer. Dereferencing the first is
	// undefined behaviour at the call site, not at the return -- it happened to survive
	// only because nothing had called it yet.
	Tr2PrimaryRenderContextAL& GetPrimaryRenderContext()
	{
		return *m_owner;
	}
	Tr2PrimaryRenderContextAL* GetPrimaryRenderContextPointer()
	{
		return m_owner;
	}

	ALResult BeginScene() throw( );
	ALResult EndScene();


	void ReleaseDeviceResources() throw( )
	{

	}


	ALResult SetStreamSource( uint32_t stream, const Tr2BufferAL & buffer, uint32_t offset, uint32_t stride ) throw( );
	ALResult SetIndices( const Tr2BufferAL & buffer ) throw( );

	ALResult ClearUav( Tr2BufferAL& buffer, const float values[4] ) throw( )
	{
		return E_NOTIMPL;
	}
	ALResult ClearUav( Tr2BufferAL& buffer, const uint32_t values[4] ) throw( )
	{
		return E_NOTIMPL;
	}

	ALResult CopySubBuffer(
		Tr2BufferAL& dest,
		uint32_t destOffset,
		Tr2BufferAL& src,
		uint32_t offset,
		uint32_t length )
	{
		return E_NOTIMPL;
	}

	ALResult SetTopology( Tr2RenderContextEnum::Topology topology ) throw( );
	ALResult SetVertexLayout( const Tr2VertexLayoutAL& layout ) throw( );
	ALResult SetShaderProgram( const Tr2ShaderProgramAL& shader ) throw( );

	ALResult ClearUav( Tr2TextureAL& rt, uint32_t mip, const float values[4] ) throw( )
	{
		return E_NOTIMPL;
	}
	ALResult ClearUav( Tr2TextureAL& rt, uint32_t mip, const uint32_t values[4] ) throw( )
	{
		return E_NOTIMPL;
	}

	ALResult SetResourceSet( const Tr2ResourceSetAL& resourceSet ) throw( );
	
	ALResult DrawIndexedPrimitive(
		uint32_t numVertices,
		uint32_t startIndex,
		uint32_t primitiveCount,
		uint32_t minimumIndex = 0 ) throw( );
	ALResult DrawPrimitive( uint32_t startVertex, uint32_t primitiveCount ) throw( );

	ALResult DrawIndexedInstanced(
		uint32_t numVertices,
		uint32_t startIndex,
		uint32_t primitiveCount,
		uint32_t numInstances ) throw( );

	ALResult DrawIndexedInstancedIndirect( Tr2BufferAL& params, uint32_t offset ) throw( );
	ALResult DrawInstancedIndirect( Tr2BufferAL& params, uint32_t offset ) throw( );

	// The DrawXxxUP family is a DX9 shape -- geometry passed by pointer with no buffer --
	// and Tr2DrawUPHelper already emulates it on top of SetStreamSource, SetIndices and
	// the ordinary draws, entirely in terms of the AL. dx12 uses the same helper, so
	// nothing here is Vulkan-specific and all three are one line.
	ALResult DrawIndexedPrimitiveUP(
		uint32_t numVertices,
		uint32_t primitiveCount,
		const uint32_t* indexData,
		const void* vertexStreamZeroData,
		uint32_t vertexStreamZeroStride ) throw( );

	ALResult DrawIndexedPrimitiveUP(
		uint32_t numVertices,
		uint32_t primitiveCount,
		const uint16_t* indexData,
		const void* vertexStreamZeroData,
		uint32_t vertexStreamZeroStride ) throw( );

	ALResult DrawPrimitiveUP(
		uint32_t primitiveCount,
		const void* vertexStreamZeroData,
		uint32_t VertexStreamZeroStride ) throw( );

	ALResult RunComputeShader( unsigned groupDimX, unsigned groupDimY, unsigned groupDimZ ) throw( );
	ALResult RunComputeShaderIndirect( Tr2BufferAL& indirectParams, unsigned offset ) throw( )
	{
		return E_NOTIMPL;
	}

	ALResult DispatchRays( Tr2RtPipelineStateAL&, Tr2RtShaderTableAL&, const wchar_t*, uint32_t, uint32_t, uint32_t )
	{
		return E_FAIL;
	}

	ALResult SetRenderState( Tr2RenderContextEnum::RenderState state, uint32_t value ) throw( );
	ALResult SetRenderStates( const uint32_t* stateValuePairs, uint32_t count ) throw( );

	ALResult SetConstants(
		const Tr2ConstantBufferAL& buffer,
		Tr2RenderContextEnum::ShaderType constantType,
		uint32_t registerIndex,
		uint32_t unusedArgument = 0 ) throw( );

	ALResult SetDepthStencil( const Tr2TextureAL& depthStencil ) throw( );
	void SetReadOnlyDepth( bool enable ) throw( )
	{

	}
	bool GetReadOnlyDepth() const
	{
		return false;
	}
	ALResult SetRenderTarget( const Tr2TextureAL& renderTarget, uint32_t slot = 0, uint32_t slice = 0 ) throw();

	void RenderPassHint( const Tr2ColorAttachment& rt0, const Tr2DepthAttachment& depth );
	void RenderPassHint( const Tr2ColorAttachment& rt0, const Tr2ColorAttachment& rt1, const Tr2DepthAttachment& depth );

	static void DestroyMainThreadRenderContext()
	{

	}

	// Helper function to clear the current primary backbuffer, depth and/or stencil.
	ALResult Clear(
		uint32_t clearFlags,
		uint32_t color,
		float depth,
		uint32_t stencil = 0,
		uint32_t slot = 0 ) throw( );

	ALResult SetViewport( const Tr2Viewport& viewport ) throw( );
	ALResult GetViewport( Tr2Viewport& viewport ) throw( );

	// Save/restore of what is bound, nothing more -- the same shape as dx12's. Pop goes
	// back through SetRenderTarget rather than assigning m_boundRenderTargets directly,
	// so that the render pass source is marked dirty exactly as it would be on any other
	// rebind.
	ALResult PushRenderTarget( uint32_t slot = 0 ) throw( );
	ALResult PopRenderTarget( uint32_t slot = 0 ) throw( );
	ALResult PushDepthStencil() throw( );
	ALResult PopDepthStencil() throw( );

	ALResult GetRenderTargetSize( uint32_t& width, uint32_t& height, uint32_t slot = 0 ) throw( )
	{
		if( slot >= RENDER_TARGET_COUNT || !m_boundRenderTargets[slot].IsValid() )
		{
			return E_FAIL;
		}
		width = m_boundRenderTargets[slot].GetWidth();
		height = m_boundRenderTargets[slot].GetHeight();
		return S_OK;
	}

	Tr2RenderContextEnum::PixelFormat GetBackBufferFormat() const throw( )
	{
		return Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN;
	}
	
	static const uint32_t SHADER_TYPE_MASK = 
		( 1 << Tr2RenderContextEnum::VERTEX_SHADER ) |
		( 1 << Tr2RenderContextEnum::PIXEL_SHADER ) |
		( 1 << Tr2RenderContextEnum::COMPUTE_SHADER ) |
		( 1 << Tr2RenderContextEnum::GEOMETRY_SHADER ) |
		( 1 << Tr2RenderContextEnum::HULL_SHADER ) |
		( 1 << Tr2RenderContextEnum::DOMAIN_SHADER );

	// Debug helpers
	size_t GetStackSizeRT( uint32_t RT = 0 )	const { return 0; }
	size_t GetStackSizeDS()						const { return 0; }

	void	ResetCapturePlayback()
	{

	}

	// Set of variables that are the first thing we need in ApplyShadowState, keep them
	// close for the cache -- don't put renderstates, renderStateEmulation or m_esm
	// in between.

	void AddGpuMarker( const char* marker )
	{

	}
	void PushGpuMarker( const char* marker )
	{

	}

	void PopGpuMarker()
	{

	}
	ALResult GetGpuStateMarker( Tr2RenderContextEnum::RenderContextStatus& status, std::string& marker ) const
	{
		return E_NOTIMPL;
	}
	ALResult GetGpuPageFaultResource(
		Tr2RenderContextEnum::PixelFormat& format,
		uint64_t& size,
		uint32_t& width,
		uint32_t& height,
		uint32_t& depth,
		uint32_t& mips ) const
	{
		return E_NOTIMPL;
	}

	ALResult UseTextures( Tr2GpuUsage::Type usage, size_t count, Tr2TextureAL* textures );
    ALResult UseAccelerationStructure( Tr2RtTopLevelAccelerationStructureAL tlas );

    

	Tr2UpscalingAL::Result EnableUpscaling( Tr2UpscalingAL::Technique tech, Tr2UpscalingAL::Setting setting, bool framegeneration, uint32_t adapter )
	{
		return Tr2UpscalingAL::Result::OK;
	}

	Tr2UpscalingContextAL* GetUpscalingContext( uint32_t upscalingContextID )
	{
		return nullptr;
	}

	Tr2UpscalingContextAL* CreateUpscalingContext( uint32_t displayWidth, uint32_t displayHeight, Tr2RenderContextEnum::PixelFormat sourceFormat, Tr2RenderContextEnum::DepthStencilFormat depthFormat )
	{
		return nullptr;
	}

	void DeleteUpscalingContext( uint32_t contextID )
	{
	}

	std::vector<std::tuple<Tr2UpscalingAL::Technique, uint32_t, bool>> GetSupportedUpscalingTechniques( uint32_t adapter )
	{
		return std::vector<std::tuple<Tr2UpscalingAL::Technique, uint32_t, bool>>();
	}

	void GetUpscalingSetup( Tr2UpscalingAL::Technique& technique, Tr2UpscalingAL::Setting& setting, bool& framegeneration )
	{
		technique = Tr2UpscalingAL::Technique::NONE;
		setting = Tr2UpscalingAL::Setting::NATIVE;
		framegeneration = false;
	}

	Tr2UpscalingAL::UpscalingInfo GetUpscalingInfo( uint32_t upscalingContextID )
	{
		return Tr2UpscalingAL::UpscalingInfo();
	}

	void MarkFrameEvent( Tr2RenderContextEnum::FrameEvent frameEvent )
	{
	}

private:
	ALResult SetPass();
	ALResult CreateRenderPass( VkRenderPass& renderPass );
	void UpdateFramebuffer();

	ALResult SetPipeline();
	ALResult CreatePipeline( VkPipeline& pipeline );
	ALResult BindConstantBuffers( VkPipelineBindPoint bindPoint );

	// Four, matching Tr2VertexLayoutAL::m_streamRates. The two arrays are indexed by the
	// same stream number and CreatePipeline reads them together.
	static const uint32_t MAX_VERTEX_STREAMS = 4;

	struct PipelineSource
	{
		Tr2VertexLayoutAL m_layout;
		VkPrimitiveTopology m_topology;
		Tr2ShaderProgramAL m_shaderProgram;
		VkPipelineDepthStencilStateCreateInfo m_depthStencilState;
		VkPipelineRasterizationStateCreateInfo m_rasterizationState;

		VkPipelineColorBlendStateCreateInfo m_colorBlendState;
		VkPipelineColorBlendAttachmentState m_attachmentBlend[4];

		VkVertexInputBindingDescription m_streams[MAX_VERTEX_STREAMS];

		size_t GetHash() const;
	} m_pipelineSource;

	struct RenderPassSource
	{
		VkAttachmentDescription m_rt[5]; //  0 - ds

		size_t GetHash() const;
	} m_renderPassSource;
	// The topology in the AL's own terms. m_pipelineSource.m_topology is the translated
	// VkPrimitiveTopology, and Tr2DrawUPHelper needs the untranslated one to work out how
	// many vertices a primitive count means.
	Tr2RenderContextEnum::Topology m_topology;
	TrinityALImpl::Tr2DrawUPHelper m_drawUPHelper;

	bool m_dirtyPso;
	bool m_dirtyPass;
	std::pair<uint32_t, uint32_t> m_primitiveToVertexCount;

	VkFramebuffer m_framebuffer;

	Tr2ResourceSetAL m_resourceSet;

	// Constant-buffer bindings (descriptor set 0), keyed by binding number. Set
	// by SetConstants, written to a descriptor set and bound at SetPipeline time.
	std::map<uint32_t, VkBuffer> m_constantBuffers;
	bool m_constantsDirty;
	// How many distinct constant descriptor sets one frame may use before the pool is
	// exhausted. Reset every BeginFrame.
	static const uint32_t CONSTANT_SETS_PER_FRAME = 64;
	VkDescriptorPool m_constantPool;

	// The newest frame that allocated a set from m_constantPool. The pool cannot be reset
	// until that frame has retired -- a single pool is shared by every frame in flight, so
	// "this slot's fence has signalled" is not enough on its own.
	uint64_t m_constantPoolLastUse;
	VkDescriptorSet m_constantSet;
	VkDescriptorSetLayout m_boundConstantLayout;

	VkPipeline m_computePipeline;
	VkPipelineLayout m_computePipelineLayout;
public:
	// If you need this, you're probably doing something wrong :P
	//Tr2TextureAL&			GetDefaultBackBuffer()
protected:
	static const uint32_t RENDER_TARGET_COUNT = 4;
	Tr2TextureAL m_boundRenderTargets[RENDER_TARGET_COUNT];
	std::vector<Tr2TextureAL> m_rtStack[RENDER_TARGET_COUNT];

	// Tracked even though binding one is still E_NOTIMPL, so that the push/pop pairing is
	// already correct when SetDepthStencil grows a real implementation.
	Tr2TextureAL m_boundDepthStencil;
	std::vector<Tr2TextureAL> m_dsStack;

	// The viewport last handed to SetViewport, in Trinity's coordinates: y down from the
	// top left, height positive. m_viewportSet distinguishes "the caller chose this" from
	// "nobody has asked", because the default has to track the render target's size and a
	// stored copy would go stale the moment the target changes.
	Tr2Viewport m_viewport;
	bool m_viewportSet;
	Tr2PrimaryRenderContextAL* m_owner;
	VkRenderPass m_renderPass;

public:
	// Close any render pass instance that is open, so that a command which is illegal
	// inside one can be recorded. vkCmdResetQueryPool is the case that needs it; the
	// clear path in Tr2RenderContextVulkan.cpp does the same thing inline for
	// vkCmdClearColorImage. Setting m_dirtyPass is what makes SetPipeline re-open the
	// pass before the next draw -- without it the draw records outside a pass and fails.
	//
	// On a tiler this is a resolve and a reload, so it is not free; it is here because
	// Vulkan 1.0 has no host-side vkResetQueryPool, which arrived in 1.2.
	// Recycle the frame's constant descriptor sets. Only safe once the frame's fence has
	// been waited on, which is why the primary render context calls it from BeginFrame and
	// nothing else calls it at all.
	void ResetConstantPoolVulkan();

	void EndRenderPassVulkan()
	{
		if( m_renderPass )
		{
			vkCmdEndRenderPass( m_commandBuffer );
			m_renderPass = VK_NULL_HANDLE;
			m_dirtyPass = true;
		}
	}

	VkCommandBuffer m_commandBuffer;

private:	

	Tr2RenderContextAL( const Tr2RenderContextAL& ) /* = delete */;
	Tr2RenderContextAL& operator=( const Tr2RenderContextAL& ) /* = delete */;

};

#endif
