// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#include "WithValidRenderContextFixture.h"

using namespace Tr2RenderContextEnum;

// MapForWriting, write, UnmapForWriting, draw -- twice in one frame, on ONE vertex buffer. Both
// draws must land where the vertices they were given put them.
//
// ConstantBufferRename's argument, for the other buffer kind. A buffer created WRITE or
// WRITE_OFTEN without NON_SYNCRONIZED_WRITE is mapped with D3D11's MAP_WRITE_DISCARD contract:
// the caller writes the whole buffer, and no draw already recorded against the old contents --
// this frame's or an earlier frame's still on the GPU -- sees the new ones. metal hands out a
// staging buffer and blits it at unmap; dx12 spells it Tr2LockType::SYNCHRONIZED. Tr2Blitter
// relies on it a dozen times a frame: one four-vertex screen quad, rewritten per blit.
//
// A backend that maps the same memory in place gives every draw of the frame the LAST quad
// written when the GPU gets to them -- and gives a draw from a frame still in flight whatever
// the CPU is writing at that moment. On the S23 Ultra that was the shippreview flicker: one
// frame in ~250 the final upscale fetched a quad that was not the full-screen one and drew
// nothing over the cleared back buffer (docs/spikes/artifacts/m6-android-flicker-20260914.txt).
//
// Two draws, two triangles: the first is written pinned to the LEFT edge, the second to the
// RIGHT, and both halves have to be red. An in-place map draws both on the right.
struct VertexBufferRename : public WithValidRenderContext
{
};

namespace
{
const uint32_t RT_WIDTH = 64;
const uint32_t RT_HEIGHT = 8;
const uint32_t CLEAR_ARGB = 0xff000000u;
const uint32_t RED_ARGB = 0xffff0000u;

ALResult CreatePositionOnlyVSForRename( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( PositionOnly.vs )
	};
	auto input = Tr2ShaderSignatureAL().Add( Tr2VertexDefinition::POSITION, 0, 0, Tr2ShaderPipelineInputAL::FLOAT, 3 );
	return shader.Create( VERTEX_SHADER, bytecode, input, "", renderContext );
}

ALResult CreateConstantColorPSForVbRename( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( ConstantColor.ps )
	};
	return shader.Create( PIXEL_SHADER, bytecode, Tr2ShaderSignatureAL(), "", renderContext );
}

// A triangle whose base spans the bottom row from `left` to `right`; the apex is above the
// middle of that span. The two writes below place it against each edge in turn.
void WriteTriangle( float* vertices, float left, float right )
{
	const float apexX = 0.5f * ( left + right );
	const float triangle[] = {
		left,  -1.0f, 0.0f,
		apexX,  1.0f, 0.0f,
		right, -1.0f, 0.0f,
	};
	memcpy( vertices, triangle, sizeof( triangle ) );
}
const uint32_t VB_STRIDE = 3 * sizeof( float );
const uint32_t VB_VERTICES = 3;

uint32_t PixelAt( const uint8_t* data, uint32_t pitch, uint32_t x, uint32_t y )
{
	const uint8_t* row = data + size_t( y ) * pitch;
	return 0xff000000u | ( uint32_t( row[x * 4 + 2] ) << 16 ) | ( uint32_t( row[x * 4 + 1] ) << 8 ) | row[x * 4 + 0];
}
}

TEST_F( VertexBufferRename, TwoDrawsInOneFrameEachSeeTheirOwnVertices )
{
	ENSURE_GPU_OR_SKIP

	Tr2ShaderAL vs, ps;
	ASSERT_HRESULT_SUCCEEDED( CreatePositionOnlyVSForRename( vs, *renderContext ) );
	ASSERT_HRESULT_SUCCEEDED( CreateConstantColorPSForVbRename( ps, *renderContext ) );
	Tr2ShaderAL shaders[] = { vs, ps };
	Tr2ShaderProgramAL sp;
	ASSERT_HRESULT_SUCCEEDED( sp.Create( shaders, 2, *renderContext ) );

	// WRITE_OFTEN and nothing else: Tr2Blitter's screen quad, exactly.
	Tr2BufferAL vb;
	ASSERT_HRESULT_SUCCEEDED( vb.Create( VB_STRIDE, VB_VERTICES, Tr2GpuUsage::VERTEX_BUFFER,
		Tr2CpuUsage::WRITE_OFTEN, nullptr, *renderContext ) );
	Tr2VertexDefinition definition;
	definition.Add( Tr2VertexDefinition::FLOAT32_3, Tr2VertexDefinition::POSITION );
	Tr2VertexLayoutAL vertexLayout;
	ASSERT_HRESULT_SUCCEEDED( vertexLayout.Create( definition, *renderContext ) );

	Tr2TextureAL rt;
	ASSERT_HRESULT_SUCCEEDED( rt.Create( Tr2BitmapDimensions( RT_WIDTH, RT_HEIGHT, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ),
		Tr2GpuUsage::RENDER_TARGET, Tr2CpuUsage::READ, *renderContext ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( rt ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetDepthStencil( Tr2TextureAL() ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetViewport( Tr2Viewport( RT_WIDTH, RT_HEIGHT ) ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->Clear( CLEARFLAGS_TARGET, CLEAR_ARGB, 1.0f ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetVertexLayout( vertexLayout ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( sp ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ZENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ALPHABLENDENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_CULLMODE, CULLMODE_NONE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetTopology( TOP_TRIANGLES ) );

	// Blit one: the triangle against the left edge. Map, write, unmap, bind, draw -- Tr2Blitter's
	// exact sequence.
	void* data = nullptr;
	ASSERT_HRESULT_SUCCEEDED( vb.MapForWriting( data, *renderContext ) );
	ASSERT_NE( nullptr, data );
	WriteTriangle( static_cast<float*>( data ), -1.0f, -0.2f );
	vb.UnmapForWriting( *renderContext );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, vb, 0, VB_STRIDE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->DrawPrimitive( 0, 1 ) );

	// Blit two: the same buffer, the triangle against the right edge.
	ASSERT_HRESULT_SUCCEEDED( vb.MapForWriting( data, *renderContext ) );
	ASSERT_NE( nullptr, data );
	WriteTriangle( static_cast<float*>( data ), 0.2f, 1.0f );
	vb.UnmapForWriting( *renderContext );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, vb, 0, VB_STRIDE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->DrawPrimitive( 0, 1 ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->PopDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );

	const void* mapped = nullptr;
	uint32_t pitch = 0;
	ASSERT_HRESULT_SUCCEEDED( rt.MapForReading( Tr2TextureSubresource( 0 ), mapped, pitch, *renderContext ) );
	ASSERT_NE( nullptr, mapped );
	const uint8_t* pixels = static_cast<const uint8_t*>( mapped );
	// Both bases span the bottom row; sample each at the middle of its own span, and the
	// middle of the row, which neither triangle reaches.
	const uint32_t y = RT_HEIGHT - 1;
	EXPECT_EQ( RED_ARGB, PixelAt( pixels, pitch, RT_WIDTH / 8, y ) )
		<< "the FIRST draw did not land at the left edge: it read the second write's vertices";
	EXPECT_EQ( RED_ARGB, PixelAt( pixels, pitch, RT_WIDTH - RT_WIDTH / 8, y ) )
		<< "the SECOND draw did not land at the right edge";
	EXPECT_EQ( CLEAR_ARGB, PixelAt( pixels, pitch, RT_WIDTH / 2, y ) )
		<< "something covered the middle of the row, which neither triangle reaches";
	rt.UnmapForReading( *renderContext );

	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, Tr2BufferAL(), 0, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( Tr2ShaderProgramAL() ) );
}
