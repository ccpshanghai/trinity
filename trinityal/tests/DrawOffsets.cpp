// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#include "WithValidRenderContextFixture.h"

using namespace Tr2RenderContextEnum;

// startIndex, base vertex and startVertex, asserted on pixels.
//
// Every other draw in this suite passes 0 for all three, which is how the Vulkan backend came to
// drop startIndex and startVertex on the floor and both it and metal came to drop the base
// vertex. None of that is reachable until a caller binds a buffer bigger than the geometry it
// wants and expresses the offset in the draw -- which is exactly what a Tr2SuballocatedBuffer
// caller does: TriGeometryRes::Draw binds the whole 32 MB shared buffer and passes
// `indices.GetStartIndex() + area.m_firstIndex`, and Tr2MeshBase::GetBatch passes
// `vertexAllocation.GetOffset() / GetStride()` as the base vertex. Zeros there draw somebody
// else's triangles, or nobody's.
//
// The shape of each test: one vertex buffer holding two quads, one over the LEFT half of the
// render target and one over the RIGHT. Ask for the right-hand one by offset alone, then assert
// that the left half is still the clear colour and the right half is the shader's red. A backend
// that ignores the offset draws the left quad instead and fails on both halves at once, so the
// assertion cannot be satisfied by an accident of clipping or of winding.
struct DrawOffsets : public WithValidRenderContext
{
};

namespace
{
const uint32_t RT_WIDTH = 64;
const uint32_t RT_HEIGHT = 8;
const uint32_t CLEAR_ARGB = 0xff000000u;   // opaque black, a colour the shader cannot produce
const uint32_t DRAWN_ARGB = 0xffff0000u;   // ConstantColor.ps returns float4( 1, 0, 0, 1 )

ALResult CreatePositionOnlyVSForOffsets( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( PositionOnly.vs )
	};
	auto input = Tr2ShaderSignatureAL().Add( Tr2VertexDefinition::POSITION, 0, 0, Tr2ShaderPipelineInputAL::FLOAT, 3 );
	return shader.Create( VERTEX_SHADER, bytecode, input, "", renderContext );
}

ALResult CreateConstantColorPSForOffsets( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( ConstantColor.ps )
	};
	return shader.Create( PIXEL_SHADER, bytecode, Tr2ShaderSignatureAL(), "", renderContext );
}

// Six vertices: 0..2 one triangle pinned to the LEFT edge, 3..5 its mirror on the RIGHT. Each
// spans x from its own edge to a fifth of the way past the middle at y = 0, so at the row this
// file samples the left triangle covers x = -0.75 and nothing near +0.75, and the right one the
// other way round. Neither is a full-screen triangle -- the first draft used the (-1,-1)/(-1,3)/
// (3,-1) trick for both and every test failed identically, because that shape covers the whole
// viewport and both halves were red whatever the offset did.
const float VERTICES[] = {
	// 0..2 -- the left triangle: x from -1 to -0.2 at y = 0
	-1.0f, -1.0f, 0.0f,
	-1.0f,  1.0f, 0.0f,
	-0.2f,  0.0f, 0.0f,
	// 3..5 -- the right triangle, the same shape mirrored about x = 0
	 1.0f, -1.0f, 0.0f,
	 1.0f,  1.0f, 0.0f,
	 0.2f,  0.0f, 0.0f,
};
const uint32_t VB_STRIDE = 3 * sizeof( float );

// 0..2 draws the left triangle, 3..5 the right. A backend that ignores startIndex draws 0..2 for
// both, which is the failure this file exists to catch.
const uint16_t INDICES[] = { 0, 1, 2, 3, 4, 5 };

uint32_t PixelAt( const void* data, uint32_t pitch, uint32_t x, uint32_t y )
{
	const uint8_t* row = static_cast<const uint8_t*>( data ) + size_t( y ) * pitch;
	// B8G8R8A8_UNORM: blue, green, red, alpha in memory, so a little-endian uint32 spelled
	// 0xAARRGGBB is exactly those bytes. Same spelling as CLEAR_ARGB and DRAWN_ARGB.
	const uint8_t b = row[x * 4 + 0];
	const uint8_t g = row[x * 4 + 1];
	const uint8_t r = row[x * 4 + 2];
	return 0xff000000u | ( uint32_t( r ) << 16 ) | ( uint32_t( g ) << 8 ) | b;
}
}

// The one thing that varies between the three tests is the draw call, so it is a parameter and
// everything around it is written once. `indexed` decides whether an index buffer is bound at
// all: SetIndices with a valid buffer is what a non-indexed draw must not need.
#define DRAW_OFFSET_BODY( indexed, drawCall )                                                                          \
	ENSURE_GPU_OR_SKIP                                                                                                 \
	Tr2ShaderAL vs, ps;                                                                                                \
	ASSERT_HRESULT_SUCCEEDED( CreatePositionOnlyVSForOffsets( vs, *renderContext ) );                                  \
	ASSERT_HRESULT_SUCCEEDED( CreateConstantColorPSForOffsets( ps, *renderContext ) );                                 \
	Tr2ShaderAL shaders[] = { vs, ps };                                                                                \
	Tr2ShaderProgramAL sp;                                                                                             \
	ASSERT_HRESULT_SUCCEEDED( sp.Create( shaders, 2, *renderContext ) );                                               \
                                                                                                                       \
	Tr2BufferAL vb;                                                                                                    \
	ASSERT_HRESULT_SUCCEEDED( vb.Create( VB_STRIDE, sizeof( VERTICES ) / VB_STRIDE, Tr2GpuUsage::VERTEX_BUFFER,         \
		Tr2CpuUsage::NONE, VERTICES, *renderContext ) );                                                               \
	Tr2BufferAL ib;                                                                                                    \
	if( indexed )                                                                                                      \
	{                                                                                                                  \
		ASSERT_HRESULT_SUCCEEDED( ib.Create( PIXEL_FORMAT_R16_UINT, sizeof( INDICES ) / sizeof( INDICES[0] ),           \
			Tr2GpuUsage::INDEX_BUFFER, Tr2CpuUsage::NONE, INDICES, *renderContext ) );                                  \
	}                                                                                                                  \
                                                                                                                       \
	Tr2VertexDefinition definition;                                                                                    \
	definition.Add( Tr2VertexDefinition::FLOAT32_3, Tr2VertexDefinition::POSITION );                                   \
	Tr2VertexLayoutAL vertexLayout;                                                                                    \
	ASSERT_HRESULT_SUCCEEDED( vertexLayout.Create( definition, *renderContext ) );                                     \
                                                                                                                       \
	Tr2TextureAL rt;                                                                                                   \
	ASSERT_HRESULT_SUCCEEDED( rt.Create( Tr2BitmapDimensions( RT_WIDTH, RT_HEIGHT, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ),    \
		Tr2GpuUsage::RENDER_TARGET, Tr2CpuUsage::READ, *renderContext ) );                                             \
                                                                                                                       \
	ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );                                                           \
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget() );                                                     \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( rt ) );                                                  \
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushDepthStencil() );                                                     \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetDepthStencil( Tr2TextureAL() ) );                                      \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetViewport( Tr2Viewport( RT_WIDTH, RT_HEIGHT ) ) );                       \
	ASSERT_HRESULT_SUCCEEDED( renderContext->Clear( CLEARFLAGS_TARGET, CLEAR_ARGB, 1.0f ) );                            \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, vb, 0, VB_STRIDE ) );                                 \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetVertexLayout( vertexLayout ) );                                        \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( sp ) );                                                 \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ZENABLE, 0 ) );                                        \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ALPHABLENDENABLE, 0 ) );                                \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_CULLMODE, CULLMODE_NONE ) );                            \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetTopology( TOP_TRIANGLES ) );                                            \
	if( indexed )                                                                                                      \
	{                                                                                                                  \
		ASSERT_HRESULT_SUCCEEDED( renderContext->SetIndices( ib ) );                                                    \
	}                                                                                                                  \
	ASSERT_HRESULT_SUCCEEDED( drawCall );                                                                              \
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopDepthStencil() );                                                      \
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget() );                                                      \
	ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );                                                             \
                                                                                                                       \
	const void* data = nullptr;                                                                                        \
	uint32_t pitch = 0;                                                                                                \
	ASSERT_HRESULT_SUCCEEDED( rt.MapForReading( Tr2TextureSubresource( 0 ), data, pitch, *renderContext ) );            \
	ASSERT_NE( nullptr, data );                                                                                        \
	const uint32_t y = RT_HEIGHT / 2;                                                                                   \
	EXPECT_EQ( CLEAR_ARGB, PixelAt( data, pitch, RT_WIDTH / 8, y ) )                                                    \
		<< "the LEFT half was drawn -- the draw's offset was ignored and it drew the first triangle";                  \
	EXPECT_EQ( DRAWN_ARGB, PixelAt( data, pitch, 7 * RT_WIDTH / 8, y ) )                                                \
		<< "the RIGHT half was not drawn -- the draw's offset reached nothing";                                        \
	rt.UnmapForReading( *renderContext );                                                                              \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, Tr2BufferAL(), 0, 0 ) );                               \
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( Tr2ShaderProgramAL() ) );                                \
	if( indexed )                                                                                                      \
	{                                                                                                                  \
		ASSERT_HRESULT_SUCCEEDED( renderContext->SetIndices( Tr2BufferAL() ) );                                         \
	}

// startIndex 3: indices 3, 4, 5, which name vertices 3, 4, 5 -- the right-hand triangle.
TEST_F( DrawOffsets, IndexedDrawStartsAtStartIndex )
{
	DRAW_OFFSET_BODY( true, renderContext->DrawIndexedPrimitive( 6, 3, 1 ) )
}

// startIndex 0 with a base vertex of 3: indices 0, 1, 2 plus 3, which is again vertices 3, 4, 5.
// The fourth parameter of DrawIndexedPrimitive is called minimumIndex after D3D9 and is the base
// vertex; dx11 names it baseVertexLocation.
TEST_F( DrawOffsets, IndexedDrawOffsetsItsVerticesByMinimumIndex )
{
	DRAW_OFFSET_BODY( true, renderContext->DrawIndexedPrimitive( 6, 0, 1, 3 ) )
}

// No index buffer: startVertex 3 draws vertices 3, 4, 5 directly.
TEST_F( DrawOffsets, NonIndexedDrawStartsAtStartVertex )
{
	DRAW_OFFSET_BODY( false, renderContext->DrawPrimitive( 3, 1 ) )
}
