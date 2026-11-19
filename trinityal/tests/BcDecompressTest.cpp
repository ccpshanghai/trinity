// Copyright © 2026 CCP ehf.
#include "StdAfx.h"
#include "../BcDecompress.h"

#include <cstring>
#include <vector>

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

namespace
{
// 4096 BC6H blocks from xorshift64 (13, 7, 17) seeded 0x9E3779B97F4A7C15: every mode, the
// reserved ones included, about a hundred blocks each.
std::vector<uint8_t> XorshiftBlocks()
{
	std::vector<uint8_t> blocks( 4096 * 16 );
	uint64_t x = 0x9E3779B97F4A7C15ull;
	for( size_t i = 0; i < blocks.size(); i += 8 )
	{
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		std::memcpy( &blocks[i], &x, 8 );
	}
	return blocks;
}

uint64_t Fnv1a( const uint8_t* data, size_t size )
{
	uint64_t hash = 0xcbf29ce484222325ull;
	for( size_t i = 0; i < size; ++i )
	{
		hash = ( hash ^ data[i] ) * 0x100000001b3ull;
	}
	return hash;
}

uint64_t DecodeXorshiftImage( Tr2RenderContextEnum::PixelFormat format )
{
	const uint32_t width = 512;
	const uint32_t height = 128;
	std::vector<uint8_t> blocks = XorshiftBlocks();
	Tr2SubresourceData src;
	src.m_sysMem = blocks.data();
	src.m_sysMemPitch = width / 4 * 16;
	src.m_sysMemSlicePitch = uint32_t( blocks.size() );
	std::unique_ptr<uint8_t[]> out;
	EXPECT_TRUE( BcDecompress( width, height, 1, format, src, out ) );
	return Fnv1a( out.get(), size_t( width ) * height * 8 );
}
}

// The digests are of the RGBA16Float image an Apple M1 Max's Metal BC6H sampler returns for the
// same blocks: the decoder must match a GPU bit for bit, signed zeros included.
TEST( BcDecompressDirect, DecodesBc6hUnsignedAsTheGpuDoes )
{
	EXPECT_EQ( 0x35df1dbed9acbb5dull, DecodeXorshiftImage( Tr2RenderContextEnum::PIXEL_FORMAT_BC6H_UF16 ) );
}

TEST( BcDecompressDirect, DecodesBc6hSignedAsTheGpuDoes )
{
	EXPECT_EQ( 0x2af378d61478b6f6ull, DecodeXorshiftImage( Tr2RenderContextEnum::PIXEL_FORMAT_BC6H_SF16 ) );
}

TEST( BcDecompressDirect, NamesTheFormatItWrites )
{
	EXPECT_EQ( Tr2RenderContextEnum::PIXEL_FORMAT_B8G8R8A8_UNORM, BcDecompressedFormat( Tr2RenderContextEnum::PIXEL_FORMAT_BC3_UNORM ) );
	EXPECT_EQ( Tr2RenderContextEnum::PIXEL_FORMAT_R16G16B16A16_FLOAT, BcDecompressedFormat( Tr2RenderContextEnum::PIXEL_FORMAT_BC6H_SF16 ) );
	EXPECT_EQ( Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN, BcDecompressedFormat( Tr2RenderContextEnum::PIXEL_FORMAT_BC7_UNORM ) );
}
