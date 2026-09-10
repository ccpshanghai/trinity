// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

// Vulkan-only for now. DX12 sizes its descriptor ranges by arrayCount but writes one descriptor
// per register (Tr2ResourceSetALDx12.cpp walks m_srvRegisters, one entry per register), so this
// test would fail there for a reason nobody on this branch can build to see; the gap is recorded
// in the Plan 1 ledger rather than hidden behind a passing guard.
#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "WithValidRenderContextFixture.h"

using namespace Tr2RenderContextEnum;

// Item 12: a resource array declared `Texture2D maps[4] : register( t0 )` is ONE binding with
// four descriptors, and element k is bound with SetSrv( stage, 0 + k, texture ). Four sites had
// to agree for that to hold -- the layout's descriptorCount, the pool's size, the descriptor
// write, and the shared register map that names element k at all -- and before this test only
// element 0 was ever written. Undefined descriptors are legal to record and wrong to sample, so
// the assertion is on pixels: four solid textures, four columns, each column must show its own.
struct ResourceArray : public WithValidRenderContext
{
};

namespace
{
const uint32_t RT_WIDTH = 64;
const uint32_t RT_HEIGHT = 8;

ALResult CreateTexCoordAndPositionVSForArray( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( TexCoordAndPosition.vs )
	};
	auto input = Tr2ShaderSignatureAL()
					 .Add( Tr2VertexDefinition::TEXCOORD, 0, 0, Tr2ShaderPipelineInputAL::FLOAT, 2 )
					 .Add( Tr2VertexDefinition::POSITION, 0, 1, Tr2ShaderPipelineInputAL::FLOAT, 3 );
	return shader.Create( VERTEX_SHADER, bytecode, input, "", renderContext );
}

ALResult CreateSampleTextureArrayPS( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( SampleTextureArrayFromTexCoord.ps )
	};
	// One register at t0 with arrayCount 4 -- not four registers. This is the declaration the
	// SPIR-V carries (ShaderBindingABI pins that), and what the AL has to turn into a layout
	// binding of four descriptors.
	auto input = Tr2ShaderSignatureAL()
					 .Add( Tr2ShaderRegisterAL::SRV_TEXTURE2D, 0, 0, 4 )
					 .Add( Tr2ShaderRegisterAL::SAMPLER, 0 );
	return shader.Create( PIXEL_SHADER, bytecode, input, "", renderContext );
}

// A 1x1 solid texture. B8G8R8A8_UNORM is blue, green, red, alpha in memory, so a little-endian
// uint32 spelled 0xAARRGGBB is exactly those four bytes -- the same spelling the readback uses.
ALResult CreateSolid( Tr2TextureAL& tex, uint32_t argb, Tr2PrimaryRenderContextAL& renderContext )
{
	Tr2SubresourceData data;
	data.m_sysMem = &argb;
	data.m_sysMemPitch = 4;
	data.m_sysMemSlicePitch = 4;
	return tex.Create( Tr2BitmapDimensions( 1, 1, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::SHADER_RESOURCE, &data, renderContext );
}
}

TEST_F( ResourceArray, FourTexturesBehindOneArrayBindingEachReachTheirColumn )
{
	ENSURE_GPU_OR_SKIP

	Tr2ShaderAL vs, ps;
	ASSERT_HRESULT_SUCCEEDED( CreateTexCoordAndPositionVSForArray( vs, *renderContext ) );
	ASSERT_HRESULT_SUCCEEDED( CreateSampleTextureArrayPS( ps, *renderContext ) );
	Tr2ShaderAL shaders[] = { vs, ps };
	Tr2ShaderProgramAL sp;
	ASSERT_HRESULT_SUCCEEDED( sp.Create( shaders, 2, *renderContext ) );

	// A quad over the whole viewport with u running 0 to 1 left to right (Ktx2AstcRoundTrip's
	// quad), so the shader's four quarter-columns land on four known x ranges.
	const float vertices[] = {
		-1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
		-1.0f,  1.0f, 0.0f, 0.0f, 0.0f,
		 1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
		 1.0f,  1.0f, 0.0f, 1.0f, 0.0f,
	};
	const uint32_t vbStride = 5 * sizeof( float );
	Tr2BufferAL vb;
	ASSERT_HRESULT_SUCCEEDED( vb.Create( vbStride, sizeof( vertices ) / vbStride, Tr2GpuUsage::VERTEX_BUFFER,
		Tr2CpuUsage::NONE, vertices, *renderContext ) );

	Tr2VertexDefinition definition;
	definition.Add( Tr2VertexDefinition::FLOAT32_3, Tr2VertexDefinition::POSITION );
	definition.Add( Tr2VertexDefinition::FLOAT32_2, Tr2VertexDefinition::TEXCOORD );
	Tr2VertexLayoutAL vertexLayout;
	ASSERT_HRESULT_SUCCEEDED( vertexLayout.Create( definition, *renderContext ) );

	// Red, green, blue, white: no two share a channel pattern, so a column showing the wrong
	// element -- or the undefined descriptor's garbage -- cannot pass by coincidence.
	const uint32_t colours[4] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu };
	Tr2TextureAL tex[4];
	for( uint32_t k = 0; k < 4; ++k )
	{
		ASSERT_HRESULT_SUCCEEDED( CreateSolid( tex[k], colours[k], *renderContext ) ) << "texture " << k;
	}
	Tr2SamplerStateAL sampl;
	ASSERT_HRESULT_SUCCEEDED( sampl.Create( Tr2SamplerDescription( TF_POINT, TA_CLAMP, 1, 0.0f, 0.0f ), *renderContext ) );

	Tr2ResourceSetDescriptionAL desc( sp );
	for( uint32_t k = 0; k < 4; ++k )
	{
		// Element k of the array at t0 is register t(0 + k). That the description accepts k > 0
		// at all is site 4 (Tr2RegisterMapAL maps every element); before it, SetSrv returned
		// false here for k = 1..3.
		ASSERT_TRUE( desc.SetSrv( PIXEL_SHADER, k, tex[k] ) ) << "element " << k << " has no register slot";
	}
	ASSERT_TRUE( desc.SetSampler( PIXEL_SHADER, 0, sampl ) );
	Tr2ResourceSetAL resourceSet;
	ASSERT_HRESULT_SUCCEEDED( resourceSet.Create( desc, sp, *renderContext ) );

	Tr2TextureAL rt;
	ASSERT_HRESULT_SUCCEEDED( rt.Create( Tr2BitmapDimensions( RT_WIDTH, RT_HEIGHT, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ),
		Tr2GpuUsage::RENDER_TARGET, Tr2CpuUsage::READ, *renderContext ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( rt ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetDepthStencil( Tr2TextureAL() ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetViewport( Tr2Viewport( RT_WIDTH, RT_HEIGHT ) ) );
	// Opaque black: a colour none of the four textures carry, so an uncovered pixel is a
	// named failure and not a plausible-looking one.
	ASSERT_HRESULT_SUCCEEDED( renderContext->Clear( CLEARFLAGS_TARGET, 0xff000000, 1.0f ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, vb, 0, vbStride ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetVertexLayout( vertexLayout ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( sp ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ZENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ALPHABLENDENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_CULLMODE, CULLMODE_NONE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetResourceSet( resourceSet ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetTopology( TOP_TRIANGLE_STRIP ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->DrawPrimitive( 0, 2 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );

	const void* data = nullptr;
	uint32_t pitch = 0;
	ASSERT_HRESULT_SUCCEEDED( rt.MapForReading( Tr2TextureSubresource( 0 ), data, pitch, *renderContext ) );
	ASSERT_NE( nullptr, data );

	// One sample per quarter, on the middle row, at the middle of each quarter -- away from the
	// three boundaries, where the shader's own `x < 0.25` steps sit on a pixel edge.
	const uint8_t* row = static_cast<const uint8_t*>( data ) + size_t( RT_HEIGHT / 2 ) * pitch;
	const uint32_t xs[4] = { RT_WIDTH / 8, 3 * RT_WIDTH / 8, 5 * RT_WIDTH / 8, 7 * RT_WIDTH / 8 };
	for( uint32_t k = 0; k < 4; ++k )
	{
		const uint8_t b = row[xs[k] * 4 + 0];
		const uint8_t g = row[xs[k] * 4 + 1];
		const uint8_t r = row[xs[k] * 4 + 2];
		const uint32_t got = 0xff000000u | ( uint32_t( r ) << 16 ) | ( uint32_t( g ) << 8 ) | b;
		EXPECT_EQ( colours[k], got ) << "column " << k << " sampled the wrong texture -- descriptor " << k
									 << " of the array was not written";
	}
	rt.UnmapForReading( *renderContext );
}

#endif
