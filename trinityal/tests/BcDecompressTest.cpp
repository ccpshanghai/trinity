// Copyright © 2026 CCP ehf.
#include "StdAfx.h"
#include "../BcDecompress.h"

// BC1 is 8 bytes per 4x4 block. The embedded image is 128x128 (Dxt1Image.h is a
// bare comma-separated initializer list -- no macros -- so dimensions are given
// here, matching the only existing consumer's idiom, Rendering.cpp:2415-2425).
// Decoding must fill width*height*4 bytes and must not be a no-op -- a silent
// pass here is the "green test was the defect" failure mode (spec D8), so
// assert content, not just success. No fixture, no GPU: this proves the
// decoder itself, independent of any device, and must always run (no
// ENSURE_GPU_OR_SKIP).
TEST( BcDecompressDirect, DecodesTheEmbeddedDxt1Image )
{
	const uint32_t width = 128;
	const uint32_t height = 128;
	uint32_t texturePixels0[] = {
#include "Dxt1Image.h"
	};
	Tr2SubresourceData src;
	src.m_sysMem = texturePixels0;
	src.m_sysMemPitch = sizeof( texturePixels0 ) / height * 4; // *4 because it's a compressed format
	src.m_sysMemSlicePitch = sizeof( texturePixels0 );

	std::unique_ptr<uint8_t[]> out;
	ASSERT_TRUE( BcDecompress( width, height, 1, Tr2RenderContextEnum::PIXEL_FORMAT_BC1_UNORM, src, out ) );
	ASSERT_NE( nullptr, out.get() );
	bool anyNonZero = false;
	for( uint32_t i = 0; i < width * height * 4; ++i )
	{
		if( out[i] != 0 )
		{
			anyNonZero = true;
			break;
		}
	}
	EXPECT_TRUE( anyNonZero );
}

TEST( BcDecompressDirect, RefusesTheFormatsItCannotDecode )
{
	// BC4/5/7 return false by design; the caller must fail loudly, never
	// upload garbage. M3 ships no decoder for them -- ASTC supersedes (spec D10).
	uint8_t srcBytes[16] = {};
	Tr2SubresourceData src;
	src.m_sysMem = srcBytes;
	src.m_sysMemPitch = 16;
	src.m_sysMemSlicePitch = 16;

	std::unique_ptr<uint8_t[]> out;
	EXPECT_FALSE( BcDecompress( 4, 4, 1, Tr2RenderContextEnum::PIXEL_FORMAT_BC7_UNORM, src, out ) );
}
