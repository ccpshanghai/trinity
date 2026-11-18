// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#include "WithValidRenderContextFixture.h"

using namespace Tr2RenderContextEnum;

// Lock, write, Unlock, draw -- twice in one frame, on ONE constant buffer. Both draws must see
// the value that was written for them.
//
// That is how trinity feeds every object its constants: Tr2PerObjectData writes each object's
// matrices into the same Tr2ConstantBufferAL and draws, object after object, inside a single
// frame (FillAndSetConstants). D3D11 renames the buffer on every Lock (MAP_WRITE_DISCARD); dx12
// and metal copy the bytes into a per-frame ring at SetConstants. A backend whose Lock maps the
// same memory in place hands every draw of the frame the LAST object's constants when the GPU
// finally runs them -- and a hull drawn with a booster's matrix, or with the shadow cascade's
// projection, is a hull drawn nowhere. Every draw in the EVE scene on the S23 counted and not one
// pixel appeared (2026-09-10/11); this is the reason.
//
// Two draws with two x offsets: the triangle lands on the LEFT for the first and the RIGHT for
// the second, and both halves have to be red. An in-place Lock draws both on the right.
struct ConstantBufferRename : public WithValidRenderContext
{
};

namespace
{
const uint32_t RT_WIDTH = 64;
const uint32_t RT_HEIGHT = 8;
const uint32_t CLEAR_ARGB = 0xff000000u;
const uint32_t RED_ARGB = 0xffff0000u;

struct PerObjectData
{
	float x;
	float y;
	float padding[2];
};

ALResult CreatePerObjectVS( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( PositionOnlyWithPerObjectData.vs )
	};
	auto input = Tr2ShaderSignatureAL()
					 .Add( Tr2VertexDefinition::POSITION, 0, 0, Tr2ShaderPipelineInputAL::FLOAT, 3 )
					 .Add( Tr2ShaderRegisterAL::CONSTANT_BUFFER, 0 );
	return shader.Create( VERTEX_SHADER, bytecode, input, "", renderContext );
}

ALResult CreateConstantColorPSForRename( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( ConstantColor.ps )
	};
	return shader.Create( PIXEL_SHADER, bytecode, Tr2ShaderSignatureAL(), "", renderContext );
}

// A small triangle around the origin, x from -0.4 to 0.4; the constant moves it by +-0.5.
const float VERTICES[] = {
	-0.4f, -1.0f, 0.0f,
	 0.0f,  1.0f, 0.0f,
	 0.4f, -1.0f, 0.0f,
};
const uint32_t VB_STRIDE = 3 * sizeof( float );

uint32_t PixelAt( const uint8_t* data, uint32_t pitch, uint32_t x, uint32_t y )
{
	const uint8_t* row = data + size_t( y ) * pitch;
	return 0xff000000u | ( uint32_t( row[x * 4 + 2] ) << 16 ) | ( uint32_t( row[x * 4 + 1] ) << 8 ) | row[x * 4 + 0];
}
}

TEST_F( ConstantBufferRename, TwoDrawsInOneFrameEachSeeTheirOwnConstants )
{
	ENSURE_GPU_OR_SKIP

	Tr2ShaderAL vs, ps;
	ASSERT_HRESULT_SUCCEEDED( CreatePerObjectVS( vs, *renderContext ) );
	ASSERT_HRESULT_SUCCEEDED( CreateConstantColorPSForRename( ps, *renderContext ) );
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

	Tr2ConstantBufferAL cb;
	ASSERT_HRESULT_SUCCEEDED( cb.Create( sizeof( PerObjectData ), *renderContext ) );

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
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, vb, 0, VB_STRIDE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetVertexLayout( vertexLayout ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( sp ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ZENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ALPHABLENDENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_CULLMODE, CULLMODE_NONE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetTopology( TOP_TRIANGLES ) );

	// Object one: shifted left. Lock, write, Unlock, bind, draw -- FillAndSetConstants' exact
	// sequence.
	PerObjectData* data = nullptr;
	ASSERT_HRESULT_SUCCEEDED( cb.Lock( reinterpret_cast<void**>( &data ), *renderContext ) );
	ASSERT_NE( nullptr, data );
	data->x = -0.5f;
	data->y = 0.0f;
	ASSERT_HRESULT_SUCCEEDED( cb.Unlock( *renderContext ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetConstants( cb, VERTEX_SHADER, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->DrawPrimitive( 0, 1 ) );

	// Object two: the same buffer, shifted right.
	ASSERT_HRESULT_SUCCEEDED( cb.Lock( reinterpret_cast<void**>( &data ), *renderContext ) );
	ASSERT_NE( nullptr, data );
	data->x = 0.5f;
	data->y = 0.0f;
	ASSERT_HRESULT_SUCCEEDED( cb.Unlock( *renderContext ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetConstants( cb, VERTEX_SHADER, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->DrawPrimitive( 0, 1 ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->PopDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );

	const void* mapped = nullptr;
	uint32_t pitch = 0;
	ASSERT_HRESULT_SUCCEEDED( rt.MapForReading( Tr2TextureSubresource( 0 ), mapped, pitch, *renderContext ) );
	ASSERT_NE( nullptr, mapped );
	const uint8_t* pixels = static_cast<const uint8_t*>( mapped );
	// The triangle's base spans the bottom row; sample there, at the two centres it was moved to.
	const uint32_t y = RT_HEIGHT - 1;
	EXPECT_EQ( RED_ARGB, PixelAt( pixels, pitch, RT_WIDTH / 4, y ) )
		<< "the FIRST draw did not land at its own x: it saw the second object's constants";
	EXPECT_EQ( RED_ARGB, PixelAt( pixels, pitch, 3 * RT_WIDTH / 4, y ) )
		<< "the SECOND draw did not land at its own x";
	rt.UnmapForReading( *renderContext );

	ASSERT_HRESULT_SUCCEEDED( renderContext->SetConstants( Tr2ConstantBufferAL(), VERTEX_SHADER, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, Tr2BufferAL(), 0, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( Tr2ShaderProgramAL() ) );
}
