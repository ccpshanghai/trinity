// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#include "WithValidRenderContextFixture.h"

#include <HostBitmap.h>
#include <Tr2ImageHandler.h>

#include <vector>

using namespace Tr2RenderContextEnum;

// Phase 5's gate (spec §9): a KTX2/ASTC file loads through imageio, becomes an AL texture, and
// is sampled. Three tests, deliberately at three different depths, because the interesting
// failures are at the seams and not in the middle:
//
//   1. the container reads -- no device, runs on every backend and every machine
//   2. the capability answers -- no upload, says whether test 3 is meaningful here
//   3. the texture samples -- the whole path, and the only one that needs a GPU
//
// The fixture is bytes in a header, not a file on disk: the suite's own idiom (Dxt1Image.h) and
// the reason this can run under `simctl spawn` with no bundle around it.

struct Ktx2Astc : public WithValidRenderContext
{
};

#define ENSURE_ASTC_OR_SKIP                                                              \
	if( !renderContext->GetCaps().SupportsAstcTextures() )                               \
	{                                                                                    \
		GTEST_SKIP() << "Test Skipped: renderContext->GetCaps().SupportsAstcTextures() " \
						"is false on this device.";                                       \
	}

namespace
{

const uint8_t s_twoToneKtx2[] = {
#include "Astc6x6TwoTone.h"
};

// The fixture's shape, stated here rather than read back out of the file, so that a test asserting
// "24 wide" is asserting something and not echoing the header.
const uint32_t FIXTURE_WIDTH = 24;
const uint32_t FIXTURE_HEIGHT = 24;
const uint32_t FIXTURE_LEVELS = 5;
const uint32_t FIXTURE_LEVEL0_BYTES = 4 * 4 * 16; // ceil(24/6) blocks each way, 16 bytes a block

struct MemoryReadStream : public ICcpStream
{
	MemoryReadStream( const uint8_t* data, ptrdiff_t size ) : m_data( data ), m_size( size )
	{
	}

	ptrdiff_t Read( void* dest, ptrdiff_t count ) override
	{
		const ptrdiff_t available = m_size - m_offset;
		const ptrdiff_t take = count < available ? count : available;
		memcpy( dest, m_data + m_offset, size_t( take ) );
		m_offset += take;
		return take;
	}

	ptrdiff_t Write( const void*, size_t ) override
	{
		return -1;
	}

	ptrdiff_t Seek( ptrdiff_t distance, SeekOrigin method ) override
	{
		const ptrdiff_t base = method == SO_BEGIN ? 0 : ( method == SO_CURRENT ? m_offset : m_size );
		m_offset = base + distance;
		return m_offset;
	}

	ptrdiff_t GetPosition() override
	{
		return m_offset;
	}

	ptrdiff_t GetSize() override
	{
		return m_size;
	}

	const uint8_t* m_data;
	ptrdiff_t m_size;
	ptrdiff_t m_offset = 0;
};

// Through ImageIO::ReadImage and not Ktx::ReadImage: the extension dispatch is half of what makes
// the engine able to open these files at all, so the test that proves the container also proves
// the handler is registered.
::testing::AssertionResult LoadTheFixture( ImageIO::HostBitmap& bitmap )
{
	MemoryReadStream stream( s_twoToneKtx2, sizeof( s_twoToneKtx2 ) );
	const ImageIO::Result result =
		ImageIO::ReadImage( stream, ImageIO::LoadParameters( L"twotone.ktx2" ), bitmap, nullptr );
	if( !result )
	{
		return ::testing::AssertionFailure() << "ImageIO::ReadImage refused the fixture: "
											<< result.GetErrorMessage();
	}
	return ::testing::AssertionSuccess();
}

ALResult CreateTexCoordAndPositionVS( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( TexCoordAndPosition.vs )
	};

	auto input = Tr2ShaderSignatureAL()
					 .Add( Tr2VertexDefinition::TEXCOORD, 0, 0, Tr2ShaderPipelineInputAL::FLOAT, 2 )
					 .Add( Tr2VertexDefinition::POSITION, 0, 1, Tr2ShaderPipelineInputAL::FLOAT, 3 );

	return shader.Create( VERTEX_SHADER, bytecode, input, "", renderContext );
}

ALResult CreateSampleTextureFromTexCoordPS( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( SampleTextureFromTexCoord.ps )
	};

	auto input = Tr2ShaderSignatureAL()
					 .Add( Tr2ShaderRegisterAL::SRV_TEXTURE2D, 0 )
					 .Add( Tr2ShaderRegisterAL::SAMPLER, 0 );

	return shader.Create( PIXEL_SHADER, bytecode, input, "", renderContext );
}

} // namespace


TEST( Ktx2AstcContainer, TheFixtureLoadsThroughImageIo )
{
	// No fixture class and no ENSURE_GPU_OR_SKIP: this is the container, and it has to be provable
	// on a machine with no adapter at all -- including a Windows CI box whose backend will skip
	// every test below.
	ImageIO::HostBitmap bitmap;
	ASSERT_TRUE( LoadTheFixture( bitmap ) );

	EXPECT_EQ( PIXEL_FORMAT_ASTC_6x6_UNORM_SRGB, bitmap.GetFormat() );
	EXPECT_EQ( FIXTURE_WIDTH, bitmap.GetWidth() );
	EXPECT_EQ( FIXTURE_HEIGHT, bitmap.GetHeight() );
	ASSERT_EQ( FIXTURE_LEVELS, bitmap.GetTrueMipCount() );

	// The block arithmetic, at the two levels where a hardcoded 4 gives a different answer than
	// the format's own footprint. Level 2 is 6 texels: one block, 16 bytes. A `(x + 3) & ~3`
	// rounding calls that 8 and charges 64.
	EXPECT_EQ( FIXTURE_LEVEL0_BYTES, bitmap.GetMipSize( 0 ) );
	EXPECT_EQ( 2u * 2u * 16u, bitmap.GetMipSize( 1 ) );
	EXPECT_EQ( 16u, bitmap.GetMipSize( 2 ) );
	EXPECT_EQ( 16u, bitmap.GetMipSize( FIXTURE_LEVELS - 1 ) );
}

TEST_F( Ktx2Astc, TheCapabilityAnswersForThisDevice )
{
	ENSURE_GPU_OR_SKIP

	// Not an assertion about the answer -- it is false on four of the five backends by design, and
	// false on an Intel Mac. What is asserted is that the query is answerable and consistent
	// between two calls, so the test below skips for a stated reason rather than by accident.
	const bool supported = renderContext->GetCaps().SupportsAstcTextures();
	EXPECT_EQ( supported, renderContext->GetCaps().SupportsAstcTextures() );

#if TRINITY_PLATFORM_SUPPORTS_ASTC_TEXTURES == 0
	// The compile-time macro is the platform class; the runtime query is the device. A backend
	// whose format table has no ASTC rows must never answer yes, whatever device it is on.
	EXPECT_FALSE( supported );
#endif
}

TEST_F( Ktx2Astc, TheLoadedTextureSamplesAsTheImageItWas )
{
	ENSURE_GPU_OR_SKIP
	ENSURE_ASTC_OR_SKIP

	ImageIO::HostBitmap bitmap;
	ASSERT_TRUE( LoadTheFixture( bitmap ) );

	// imageio's bitmap to the AL's upload, the way TriTextureRes::CreateDescription does it: one
	// Tr2SubresourceData per mip, pitch and slice pitch from the bitmap rather than computed here.
	std::vector<Tr2SubresourceData> initialData;
	for( uint32_t level = 0; level < bitmap.GetTrueMipCount(); ++level )
	{
		Tr2SubresourceData srd;
		srd.m_sysMem = const_cast<char*>( bitmap.GetMipRawData( level ) );
		srd.m_sysMemPitch = bitmap.GetMipPitch( level );
		srd.m_sysMemSlicePitch = bitmap.GetMipSize( level );
		initialData.push_back( srd );
	}

	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create(
		Tr2BitmapDimensions( bitmap.GetType(), bitmap.GetFormat(), bitmap.GetWidth(), bitmap.GetHeight(),
			1, bitmap.GetTrueMipCount(), 1 ),
		Tr2GpuUsage::SHADER_RESOURCE, initialData.data(), *renderContext ) );
	EXPECT_EQ( PIXEL_FORMAT_ASTC_6x6_UNORM_SRGB, tex.GetFormat() );

	Tr2ShaderAL vs;
	ASSERT_HRESULT_SUCCEEDED( CreateTexCoordAndPositionVS( vs, *renderContext ) );
	Tr2ShaderAL ps;
	ASSERT_HRESULT_SUCCEEDED( CreateSampleTextureFromTexCoordPS( ps, *renderContext ) );
	Tr2ShaderAL shaders[] = { vs, ps };
	Tr2ShaderProgramAL sp;
	ASSERT_HRESULT_SUCCEEDED( sp.Create( shaders, 2, *renderContext ) );

	// A quad over the whole viewport with u running 0 to 1 left to right, so a render target the
	// size of the texture maps one pixel to one texel and the readback below can name columns.
	// The existing CanSampleBc1Texture quad covers the middle half with its u mirrored, which is
	// fine for a screenshot and useless for an assertion.
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

	// Point, and mip 0 only. Any filtering would blend the two halves across the seam and turn a
	// per-column assertion into a tolerance argument.
	Tr2SamplerStateAL sampl;
	ASSERT_HRESULT_SUCCEEDED( sampl.Create(
		Tr2SamplerDescription( TF_POINT, TA_CLAMP, 1, 0.0f, 0.0f ), *renderContext ) );

	Tr2ResourceSetDescriptionAL desc( sp );
	desc.SetSrv( PIXEL_SHADER, 0, tex );
	desc.SetSampler( PIXEL_SHADER, 0, sampl );
	Tr2ResourceSetAL resourceSet;
	ASSERT_HRESULT_SUCCEEDED( resourceSet.Create( desc, sp, *renderContext ) );

	Tr2TextureAL rt;
	ASSERT_HRESULT_SUCCEEDED( rt.Create(
		Tr2BitmapDimensions( FIXTURE_WIDTH, FIXTURE_HEIGHT, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ),
		Tr2GpuUsage::RENDER_TARGET, Tr2CpuUsage::READ, *renderContext ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( rt ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetDepthStencil( Tr2TextureAL() ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetViewport( Tr2Viewport( FIXTURE_WIDTH, FIXTURE_HEIGHT ) ) );
	// Opaque green, a colour the fixture does not contain: a pixel the quad failed to cover is
	// then a named failure rather than something that happens to look plausible.
	ASSERT_HRESULT_SUCCEEDED( renderContext->Clear( CLEARFLAGS_TARGET, 0xff00ff00, 1.0f ) );
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

	// Pure red and pure blue on purpose: both survive the sRGB decode unchanged (0 and 1 are the
	// transfer function's fixed points), so the expected values are exact and no tolerance has to
	// be argued for. A mid-grey fixture would have needed one.
	uint32_t red = 0, blue = 0, other = 0;
	uint32_t firstBadX = FIXTURE_WIDTH, firstBadY = FIXTURE_HEIGHT;
	for( uint32_t y = 0; y < FIXTURE_HEIGHT; ++y )
	{
		const uint8_t* row = static_cast<const uint8_t*>( data ) + size_t( y ) * pitch;
		for( uint32_t x = 0; x < FIXTURE_WIDTH; ++x )
		{
			// B8G8R8A8: blue, green, red, alpha in memory order.
			const uint8_t b = row[x * 4 + 0];
			const uint8_t g = row[x * 4 + 1];
			const uint8_t r = row[x * 4 + 2];
			const bool isRed = r > 200 && g < 64 && b < 64;
			const bool isBlue = b > 200 && g < 64 && r < 64;
			const bool wantRed = x < FIXTURE_WIDTH / 2;
			if( ( wantRed && isRed ) || ( !wantRed && isBlue ) )
			{
				wantRed ? ++red : ++blue;
				continue;
			}
			++other;
			if( firstBadX == FIXTURE_WIDTH )
			{
				firstBadX = x;
				firstBadY = y;
			}
		}
	}
	rt.UnmapForReading( *renderContext );

	const uint32_t half = FIXTURE_WIDTH / 2 * FIXTURE_HEIGHT;
	EXPECT_EQ( half, red ) << "red texels in the left half";
	EXPECT_EQ( half, blue ) << "blue texels in the right half";
	EXPECT_EQ( 0u, other ) << "first wrong pixel at (" << firstBadX << ", " << firstBadY << ")";
}
