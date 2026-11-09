// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#include "WithValidRenderContextFixture.h"

using namespace Tr2RenderContextEnum;

// Rebinding a render target of the SAME format must move the draws.
//
// The Vulkan backend opened its rendering pass lazily and re-opened it only when the pass
// source changed -- and the pass source held the attachment's *description* (format, sample
// count, load/store ops, layouts), not its identity. Two images of one format compare equal
// there, so SetRenderTarget( B ) after SetRenderTarget( A ) left the pass open on A and every
// draw meant for B landed on A. Every other test in this suite renders into one target per
// pass, so nothing here ever switched between two of the same kind.
//
// What it cost: Tr2PostProcessRenderer creates "Final Result" with destination.GetFormat() --
// deliberately the back buffer's own format -- tonemaps into it, then binds the back buffer
// and draws it across. That last draw landed in Final Result. A whole EVE scene rendered,
// 43 draws and a quarter of a million vertices a frame, and not one pixel reached the swap
// chain; the frame was the clear colour and the HUD. (S23, 2026-09-10/11.)
struct RenderTargetSwitch : public WithValidRenderContext
{
};

namespace
{
const uint32_t RT_WIDTH = 32;
const uint32_t RT_HEIGHT = 8;
const uint32_t CLEAR_ARGB = 0xff000000u;   // opaque black: neither shader colour
const uint32_t RED_ARGB = 0xffff0000u;     // ConstantColor.ps

ALResult CreatePositionOnlyVSForSwitch( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( PositionOnly.vs )
	};
	auto input = Tr2ShaderSignatureAL().Add( Tr2VertexDefinition::POSITION, 0, 0, Tr2ShaderPipelineInputAL::FLOAT, 3 );
	return shader.Create( VERTEX_SHADER, bytecode, input, "", renderContext );
}

ALResult CreateConstantColorPSForSwitch( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( ConstantColor.ps )
	};
	return shader.Create( PIXEL_SHADER, bytecode, Tr2ShaderSignatureAL(), "", renderContext );
}

// One full-screen triangle.
const float VERTICES[] = {
	-1.0f, -1.0f, 0.0f,
	-1.0f,  3.0f, 0.0f,
	 3.0f, -1.0f, 0.0f,
};
const uint32_t VB_STRIDE = 3 * sizeof( float );

uint32_t CentrePixel( Tr2TextureAL& rt, Tr2PrimaryRenderContextAL& renderContext )
{
	const void* data = nullptr;
	uint32_t pitch = 0;
	if( FAILED( rt.MapForReading( Tr2TextureSubresource( 0 ), data, pitch, renderContext ) ) || !data )
	{
		return 0xdeadbeefu;
	}
	const uint8_t* row = static_cast<const uint8_t*>( data ) + size_t( RT_HEIGHT / 2 ) * pitch;
	const uint32_t x = RT_WIDTH / 2;
	const uint32_t got = 0xff000000u | ( uint32_t( row[x * 4 + 2] ) << 16 ) | ( uint32_t( row[x * 4 + 1] ) << 8 ) | row[x * 4 + 0];
	rt.UnmapForReading( renderContext );
	return got;
}
}

TEST_F( RenderTargetSwitch, DrawsFollowARebindToATargetOfTheSameFormat )
{
	ENSURE_GPU_OR_SKIP

	Tr2ShaderAL vs, ps;
	ASSERT_HRESULT_SUCCEEDED( CreatePositionOnlyVSForSwitch( vs, *renderContext ) );
	ASSERT_HRESULT_SUCCEEDED( CreateConstantColorPSForSwitch( ps, *renderContext ) );
	Tr2ShaderAL shaders[] = { vs, ps };
	Tr2ShaderProgramAL sp;
	ASSERT_HRESULT_SUCCEEDED( sp.Create( shaders, 2, *renderContext ) );

	Tr2BufferAL vb;
	ASSERT_HRESULT_SUCCEEDED( vb.Create( VB_STRIDE, sizeof( VERTICES ) / VB_STRIDE, Tr2GpuUsage::VERTEX_BUFFER,
		Tr2CpuUsage::NONE, VERTICES, *renderContext ) );
	Tr2VertexDefinition definition;
	definition.Add( Tr2VertexDefinition::FLOAT32_3, Tr2VertexDefinition::POSITION );
	Tr2VertexLayoutAL vertexLayout;
	ASSERT_HRESULT_SUCCEEDED( vertexLayout.Create( definition, *renderContext ) );

	// Identical in every way the attachment description can see.
	Tr2TextureAL a, b;
	ASSERT_HRESULT_SUCCEEDED( a.Create( Tr2BitmapDimensions( RT_WIDTH, RT_HEIGHT, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ),
		Tr2GpuUsage::RENDER_TARGET, Tr2CpuUsage::READ, *renderContext ) );
	ASSERT_HRESULT_SUCCEEDED( b.Create( Tr2BitmapDimensions( RT_WIDTH, RT_HEIGHT, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ),
		Tr2GpuUsage::RENDER_TARGET, Tr2CpuUsage::READ, *renderContext ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetDepthStencil( Tr2TextureAL() ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetViewport( Tr2Viewport( RT_WIDTH, RT_HEIGHT ) ) );

	// Both cleared up front, so that the pass below is opened by a draw and not by a clear --
	// a clear closes the pass itself, which would hide exactly the fault under test.
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( a ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->Clear( CLEARFLAGS_TARGET, CLEAR_ARGB, 1.0f ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( b ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->Clear( CLEARFLAGS_TARGET, CLEAR_ARGB, 1.0f ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, vb, 0, VB_STRIDE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetVertexLayout( vertexLayout ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( sp ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ZENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ALPHABLENDENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_CULLMODE, CULLMODE_NONE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetTopology( TOP_TRIANGLES ) );

	// Open the pass on A with a draw that is *masked off*, so A stays black; then rebind B and
	// draw for real. Nothing but the target changes between the two draws.
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( a ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_COLORWRITEENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->DrawPrimitive( 0, 1 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_COLORWRITEENABLE, 0xf ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( b ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->DrawPrimitive( 0, 1 ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->PopDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );

	EXPECT_EQ( CLEAR_ARGB, CentrePixel( a, *renderContext ) )
		<< "A was drawn into after B was bound -- the rebind did not move the pass";
	EXPECT_EQ( RED_ARGB, CentrePixel( b, *renderContext ) )
		<< "B received nothing -- the draw meant for it went elsewhere";

	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, Tr2BufferAL(), 0, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( Tr2ShaderProgramAL() ) );
}
