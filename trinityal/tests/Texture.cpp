// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include "WithValidRenderContextFixture.h"
#include "WithRenderContextFixture.h"
#include "../metal/Tr2TextureALMetal.h"
#include "../metal/MetalUtils.h"
#include <cstdlib>
#include <cstring>

using namespace Tr2RenderContextEnum;

#if ( TRINITY_PLATFORM == TRINITY_METAL )
namespace
{
// Queries the same capability Create() consults, on the default device --
// mirrors what Tr2TextureALMetal.mm asks internally (MetalDeviceSupportsBC),
// including the TRINITY_FORCE_BC_DECOMPRESS override, so the test always
// agrees with the production code path it is exercising (spec D8).
bool MetalTestDeviceSupportsBC()
{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	return TrinityALImpl::MetalDeviceSupportsBC( device );
}

// Forces the decompress path for exactly the scope of one test, regardless of
// the process-wide environment the suite was launched with or the real
// device's BC capability. MetalDeviceSupportsBC() re-reads the env var on
// every call (nothing caches it), so setenv/unsetenv here is enough -- no
// dependency on how the binary itself was invoked.
struct ScopedForceBcDecompress
{
	ScopedForceBcDecompress()
	{
		setenv( "TRINITY_FORCE_BC_DECOMPRESS", "1", 1 );
	}
	~ScopedForceBcDecompress()
	{
		unsetenv( "TRINITY_FORCE_BC_DECOMPRESS" );
	}
};
}
#endif

struct Texture : public WithValidRenderContext
{
};

TEST_F( Texture, TextureIsInvalidBeforeCreation )
{
	Tr2TextureAL tex;
	EXPECT_FALSE( tex.IsValid() );
}

TEST_F( WithRenderContext, Creating2DTextureWithoutRenderContextFails )
{
	Tr2TextureAL tex;
	ASSERT_HRESULT_FAILED( tex.Create( Tr2BitmapDimensions( 128, 128, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
}

TEST_F( WithRenderContext, CreatingCubeTextureWithoutRenderContextFails )
{
	Tr2TextureAL tex;
	ASSERT_HRESULT_FAILED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_CUBE, PIXEL_FORMAT_B8G8R8A8_UNORM, 128, 128, 1, 1 ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
}

TEST_F( WithRenderContext, CreatingVolumeTextureWithoutRenderContextFails )
{
	uint32_t pixels[4 * 4 * 4] = { 0 };
	Tr2SubresourceData initialData;
	initialData.m_sysMemPitch = 4 * 4;
	initialData.m_sysMemSlicePitch = 4 * 4 * 4;
	initialData.m_sysMem = pixels;

	Tr2TextureAL tex;
	ASSERT_HRESULT_FAILED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_3D, PIXEL_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 1 ), Tr2GpuUsage::SHADER_RESOURCE, &initialData, *renderContext ) );
}

TEST_F( Texture, CreatingImmutable2DTextureWithoutInitialDataFails )
{
	Tr2TextureAL tex;
	ASSERT_HRESULT_FAILED( tex.Create( Tr2BitmapDimensions( 128, 128, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::SHADER_RESOURCE, nullptr, *renderContext ) );
}

TEST_F( Texture, CreatingImmutableCubeTextureWithoutInitialDataFails )
{
	Tr2TextureAL tex;
	ASSERT_HRESULT_FAILED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_CUBE, PIXEL_FORMAT_B8G8R8A8_UNORM, 128, 128, 1, 1 ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::NONE, *renderContext ) );
}

TEST_F( Texture, CreatingVolumeTextureWithoutInitialDataFails )
{
	Tr2TextureAL tex;
	ASSERT_HRESULT_FAILED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_3D, PIXEL_FORMAT_B8G8R8A8_UNORM, 128, 128, 128, 1 ), Tr2GpuUsage::SHADER_RESOURCE, nullptr, *renderContext ) );
}

TEST_F( Texture, Texture2DIsValidAfterCreation )
{
	ENSURE_GPU_OR_SKIP
	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( 128, 128, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
	EXPECT_TRUE( tex.IsValid() );
	EXPECT_EQ( TEX_TYPE_2D, tex.GetType() );
}

#if TRINITY_PLATFORM_SUPPORTS_TEXTURE_ARRAYS
TEST_F( Texture, Texture2DArrayIsValidAfterCreation )
{
	ENSURE_GPU_OR_SKIP
	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_2D, PIXEL_FORMAT_B8G8R8A8_UNORM, 128, 128, 1, 1, 2 ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
	EXPECT_TRUE( tex.IsValid() );
	EXPECT_EQ( TEX_TYPE_2D, tex.GetType() );
	EXPECT_EQ( 2, tex.GetArraySize() );
}
#else
TEST_F( Texture, Texture2DArrayFailsOnUnsupportingPlatforms )
{
	Tr2TextureAL tex;
	ASSERT_HRESULT_FAILED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_2D, PIXEL_FORMAT_B8G8R8A8_UNORM, 128, 128, 1, 1, 2 ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
}
#endif

TEST_F( Texture, TextureCubeIsValidAfterCreation )
{
	ENSURE_GPU_OR_SKIP
	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_CUBE, PIXEL_FORMAT_B8G8R8A8_UNORM, 128, 128, 1, 1 ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
	EXPECT_TRUE( tex.IsValid() );
	EXPECT_EQ( TEX_TYPE_CUBE, tex.GetType() );
}

TEST_F( Texture, TextureVolumeIsValidAfterCreation )
{
	ENSURE_GPU_OR_SKIP
	uint32_t pixels[4 * 4 * 4 * 4] = { 0 };
	Tr2SubresourceData initialData;
	initialData.m_sysMemPitch = 4 * 4;
	initialData.m_sysMemSlicePitch = 4 * 4 * 4;
	initialData.m_sysMem = pixels;

	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_3D, PIXEL_FORMAT_B8G8R8A8_UNORM, 4, 4, 4, 1 ), Tr2GpuUsage::SHADER_RESOURCE, &initialData, *renderContext ) );
	EXPECT_TRUE( tex.IsValid() );
	EXPECT_EQ( TEX_TYPE_3D, tex.GetType() );
}

TEST_F( Texture, CanCreateMipMapped2DTexture )
{
	ENSURE_GPU_OR_SKIP
	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( 128, 128, 0, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
	EXPECT_TRUE( tex.IsValid() );
	EXPECT_EQ( 8, tex.GetTrueMipCount() );
}

TEST_F( Texture, CanCreateMipMappedCubeTexture )
{
	ENSURE_GPU_OR_SKIP
	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_CUBE, PIXEL_FORMAT_B8G8R8A8_UNORM, 128, 128, 1, 0 ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
	EXPECT_TRUE( tex.IsValid() );
	EXPECT_EQ( 8, tex.GetTrueMipCount() );
}

TEST_F( Texture, TextureEqualsItself )
{
	ENSURE_GPU_OR_SKIP
	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( 128, 128, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
	EXPECT_TRUE( tex == tex );
}

TEST_F( Texture, DifferentTexturesAreNotEqual )
{
	ENSURE_GPU_OR_SKIP
	Tr2TextureAL tex1;
	ASSERT_HRESULT_SUCCEEDED( tex1.Create( Tr2BitmapDimensions( 128, 128, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
	Tr2TextureAL tex2;
	ASSERT_HRESULT_SUCCEEDED( tex2.Create( Tr2BitmapDimensions( 128, 128, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
	EXPECT_FALSE( tex1 == tex2 );
}

TEST_F( Texture, LockingInvalidTextureFails )
{
	Tr2TextureAL tex;
	const void* constData;
	void* data;
	uint32_t pitch;
	ASSERT_HRESULT_FAILED( tex.MapForReading( Tr2TextureSubresource( 0 ), constData, pitch, *renderContext ) );
	ASSERT_HRESULT_FAILED( tex.MapForWriting( Tr2TextureSubresource( 0 ), data, pitch, *renderContext ) );
}

TEST_F( Texture, TextureHasMemoryClass )
{
	ENSURE_GPU_OR_SKIP
	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( 128, 128, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::WRITE, *renderContext ) );
	auto memoryClass = tex.GetMemoryClass();
	EXPECT_TRUE( memoryClass == AL_MEMORY_VIDEO || memoryClass == AL_MEMORY_MANAGED );
}

TEST_F( Texture, CanCreateCompressed2DTexture )
{
	ENSURE_GPU_OR_SKIP
	const uint32_t width = 128;
	const uint32_t height = 128;
	uint32_t texturePixels0[] = {
#include "Dxt1Image.h"
	};
	Tr2SubresourceData textureData[1];
	textureData[0].m_sysMem = texturePixels0;
	textureData[0].m_sysMemPitch = sizeof( texturePixels0 ) / height * 4; // *4 because it's a compressed format
	textureData[0].m_sysMemSlicePitch = sizeof( texturePixels0 );

	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( width, height, 1, PIXEL_FORMAT_BC1_UNORM ),
										  Tr2GpuUsage::SHADER_RESOURCE, textureData, *renderContext ) );
	EXPECT_TRUE( tex.IsValid() );
	EXPECT_EQ( TEX_TYPE_2D, tex.GetType() );
#if ( TRINITY_PLATFORM == TRINITY_METAL )
	// Capability-aware (spec D8): where BC sampling exists the format round-trips;
	// where it does not, the create still succeeds THROUGH the decompress path and
	// GetFormat reports what was actually allocated. Both arms assert -- a test
	// that passes without knowing which path ran would be green by luck. The
	// DebugDecompressedOnCreate() half is what rules out a silent no-op: without
	// it, a stub that hardcoded BGRA8 without ever calling BcDecompress would
	// still make the format assertion pass.
	if( MetalTestDeviceSupportsBC() )
	{
		EXPECT_EQ( PIXEL_FORMAT_BC1_UNORM, tex.GetFormat() );
		EXPECT_FALSE( tex.TrinityALImpl_GetObject()->DebugDecompressedOnCreate() );
	}
	else
	{
		EXPECT_EQ( PIXEL_FORMAT_B8G8R8A8_UNORM, tex.GetFormat() );
		EXPECT_TRUE( tex.TrinityALImpl_GetObject()->DebugDecompressedOnCreate() );
	}
#else
	EXPECT_EQ( PIXEL_FORMAT_BC1_UNORM, tex.GetFormat() );
#endif
}

TEST_F( Texture, CanCreateCompressedCubeTexture )
{
	ENSURE_GPU_OR_SKIP
	const uint32_t width = 128;
	const uint32_t height = 128;
	uint32_t texturePixels0[] = {
#include "Dxt1Image.h"
	};
	Tr2SubresourceData textureData[6];
	for( auto& face : textureData )
	{
		face.m_sysMem = texturePixels0;
		face.m_sysMemPitch = sizeof( texturePixels0 ) / height * 4; // *4 because it's a compressed format
		face.m_sysMemSlicePitch = sizeof( texturePixels0 );
	}

	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_CUBE, PIXEL_FORMAT_BC1_UNORM, width, height, 1, 1 ),
										  Tr2GpuUsage::SHADER_RESOURCE, textureData, *renderContext ) );
	EXPECT_TRUE( tex.IsValid() );
	EXPECT_EQ( TEX_TYPE_CUBE, tex.GetType() );
#if ( TRINITY_PLATFORM == TRINITY_METAL )
	// Same capability-aware contract as CanCreateCompressed2DTexture above.
	if( MetalTestDeviceSupportsBC() )
	{
		EXPECT_EQ( PIXEL_FORMAT_BC1_UNORM, tex.GetFormat() );
		EXPECT_FALSE( tex.TrinityALImpl_GetObject()->DebugDecompressedOnCreate() );
	}
	else
	{
		EXPECT_EQ( PIXEL_FORMAT_B8G8R8A8_UNORM, tex.GetFormat() );
		EXPECT_TRUE( tex.TrinityALImpl_GetObject()->DebugDecompressedOnCreate() );
	}
#else
	EXPECT_EQ( PIXEL_FORMAT_BC1_UNORM, tex.GetFormat() );
#endif
}

#if ( TRINITY_PLATFORM == TRINITY_METAL )
// Regression coverage for a review finding on this same diff: the widened D7
// trigger fires for TEX_TYPE_CUBE (GetArraySize() == 6), unlike the old
// macOS-10.14-volume-only trigger where GetArraySize() was always 1. Under
// the bug, the decompression branch indexed initialData[mip] instead of
// initialData[index], so every face after face 0 silently decompressed face
// 0's bytes again. CanCreateCompressedCubeTexture above cannot catch this --
// it points all six faces at the same source image on purpose, so the wrong
// index and the right index read identical bytes.
//
// This test gives each face genuinely distinct source data and checks, per
// face, that the decompressor consumed THAT face's buffer. What it cannot
// check: the actual GPU-side decoded pixel content per face.
// MapForReading's Metal implementation hard-asserts
// `region.m_startFace == 0 && region.m_endFace == 1` (Tr2TextureALMetal.mm),
// i.e. it structurally only supports reading back face 0 of a cube texture --
// which is exactly the one face the bug never corrupts, so a GPU readback
// through this fixture could not have distinguished "fixed" from "buggy"
// either. DebugDecompressedSources() records, at the point of consumption,
// which source Create() actually fed to BcDecompress for each (slice, mip) --
// the narrowest observable signal that still fails under the original bug
// and passes once initialData[index] is used.
TEST_F( Texture, CompressedCubeTextureDecompressesEachFaceFromItsOwnSource )
{
	ENSURE_GPU_OR_SKIP
	// This test's entire point is the decompress branch; force it regardless
	// of this Mac's real BC capability or how the suite binary was invoked.
	ScopedForceBcDecompress forceDecompress;

	const uint32_t width = 128;
	const uint32_t height = 128;
	uint32_t templatePixels[] = {
#include "Dxt1Image.h"
	};
	constexpr size_t kFaceCount = 6;
	uint32_t facePixels[kFaceCount][sizeof( templatePixels ) / sizeof( templatePixels[0] )];
	Tr2SubresourceData textureData[kFaceCount];
	for( size_t face = 0; face < kFaceCount; ++face )
	{
		memcpy( facePixels[face], templatePixels, sizeof( templatePixels ) );
		// One word differing per face: genuinely distinct content per face,
		// not just distinct array addresses (matching a real skybox/cube map,
		// where every face is different art).
		facePixels[face][0] ^= ( 0x1000u + static_cast<uint32_t>( face ) );

		textureData[face].m_sysMem = facePixels[face];
		textureData[face].m_sysMemPitch = sizeof( facePixels[face] ) / height * 4; // *4: compressed format
		textureData[face].m_sysMemSlicePitch = sizeof( facePixels[face] );
	}

	Tr2TextureAL tex;
	ASSERT_HRESULT_SUCCEEDED( tex.Create( Tr2BitmapDimensions( TEX_TYPE_CUBE, PIXEL_FORMAT_BC1_UNORM, width, height, 1, 1 ),
										  Tr2GpuUsage::SHADER_RESOURCE, textureData, *renderContext ) );

	const auto& consumedSources = tex.TrinityALImpl_GetObject()->DebugDecompressedSources();
	ASSERT_EQ( kFaceCount, consumedSources.size() );
	for( size_t face = 0; face < kFaceCount; ++face )
	{
		EXPECT_EQ( textureData[face].m_sysMem, consumedSources[face] ) << "face " << face;
	}
}
#endif
