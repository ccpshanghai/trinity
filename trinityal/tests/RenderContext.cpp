// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include "WithValidRenderContextFixture.h"
#include "WithRenderContextFixture.h"

using namespace Tr2RenderContextEnum;

struct RenderContext : public WithValidRenderContext
{
};

struct PrimaryRenderContext : public WithValidRenderContext
{
};


TEST_F( RenderContext, CanSetViewport )
{
	ENSURE_GPU_OR_SKIP
	Tr2Viewport viewport( 123, 67 );
	viewport.m_x = 30;
	viewport.m_y = 18;
	viewport.m_minZ = 0.1f;
	viewport.m_maxZ = 0.6f;
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetViewport( viewport ) );
	Tr2Viewport gotViewport;
	ASSERT_HRESULT_SUCCEEDED( renderContext->GetViewport( gotViewport ) );
	EXPECT_EQ( viewport.m_x, gotViewport.m_x );
	EXPECT_EQ( viewport.m_y, gotViewport.m_y );
	EXPECT_EQ( viewport.m_width, gotViewport.m_width );
	EXPECT_EQ( viewport.m_height, gotViewport.m_height );
	EXPECT_EQ( viewport.m_minZ, gotViewport.m_minZ );
	EXPECT_EQ( viewport.m_maxZ, gotViewport.m_maxZ );
}

TEST_F( PrimaryRenderContext, CanGetBackbufferFormat )
{
	ENSURE_GPU_OR_SKIP
	EXPECT_NE( Tr2RenderContextEnum::PIXEL_FORMAT_UNKNOWN, renderContext->GetBackBufferFormat() );
}

TEST_F( PrimaryRenderContext, CanGetBackbufferSize )
{
	ENSURE_GPU_OR_SKIP
	uint32_t width = 0xDeadBeef;
	uint32_t height = 0xDeadBeef;
	ASSERT_HRESULT_SUCCEEDED( renderContext->GetRenderTargetSize( width, height ) );
	EXPECT_NE( 0xDeadBeef, width );
	EXPECT_NE( 0xDeadBeef, height );
}

TEST_F( RenderContext, CanGetRenderTargetSize )
{
	ENSURE_GPU_OR_SKIP
	uint32_t width = 0xDeadBeef;
	uint32_t height = 0xDeadBeef;

	Tr2TextureAL rt;
	ASSERT_HRESULT_SUCCEEDED( rt.Create( Tr2BitmapDimensions( 128, 64, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::RENDER_TARGET, *renderContext ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( rt ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->GetRenderTargetSize( width, height ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget() );

	EXPECT_EQ( rt.GetWidth(), width );
	EXPECT_EQ( rt.GetHeight(), height );
}

TEST_F( RenderContext, CanGetRenderTargetSizeForNonZeroSlot )
{
	ENSURE_GPU_OR_SKIP
	uint32_t width = 0xDeadBeef;
	uint32_t height = 0xDeadBeef;
	const uint32_t slot = 2;

	Tr2TextureAL rt;
	ASSERT_HRESULT_SUCCEEDED( rt.Create( Tr2BitmapDimensions( 128, 64, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ), Tr2GpuUsage::RENDER_TARGET, *renderContext ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget( 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( Tr2TextureAL() ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget( slot ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( rt, slot ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->GetRenderTargetSize( width, height, slot ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget( slot ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget( 0 ) );

	EXPECT_EQ( rt.GetWidth(), width );
	EXPECT_EQ( rt.GetHeight(), height );
}

TEST_F( RenderContext, GetRenderTargetSizeFailsWithNoRenderTarget )
{
	uint32_t width = 0xDeadBeef;
	uint32_t height = 0xDeadBeef;
	const uint32_t slot = 1;

	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget( slot ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( Tr2TextureAL(), slot ) );
	ASSERT_HRESULT_FAILED( renderContext->GetRenderTargetSize( width, height, slot ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget( slot ) );

	EXPECT_EQ( 0xDeadBeef, width );
	EXPECT_EQ( 0xDeadBeef, height );
}

TEST_F( WithRenderContext, InvalidRenderContextHasInvalidBackBuffer )
{
	EXPECT_FALSE( renderContext->GetDefaultBackBuffer().IsValid() );
}

TEST_F( PrimaryRenderContext, ValidRenderContextHasValidBackBuffer )
{
	ENSURE_GPU_OR_SKIP
	EXPECT_TRUE( renderContext->GetDefaultBackBuffer().IsValid() );
	EXPECT_EQ( 1, renderContext->GetDefaultBackBuffer().GetMipCount() );
	EXPECT_EQ( renderContext->GetBackBufferFormat(), renderContext->GetDefaultBackBuffer().GetFormat() );
}

TEST( RenderContextEnum, CanConvertPixelFormatToTypeless )
{
	Tr2RenderContextEnum::PixelFormat formats[] = {
		PIXEL_FORMAT_UNKNOWN, PIXEL_FORMAT_UNKNOWN, PIXEL_FORMAT_R32G32B32A32_TYPELESS, PIXEL_FORMAT_R32G32B32A32_TYPELESS, PIXEL_FORMAT_R32G32B32A32_FLOAT, PIXEL_FORMAT_R32G32B32A32_TYPELESS, PIXEL_FORMAT_R32G32B32A32_UINT, PIXEL_FORMAT_R32G32B32A32_TYPELESS, PIXEL_FORMAT_R32G32B32A32_SINT, PIXEL_FORMAT_R32G32B32A32_TYPELESS, PIXEL_FORMAT_R32G32B32_TYPELESS, PIXEL_FORMAT_R32G32B32_TYPELESS, PIXEL_FORMAT_R32G32B32_FLOAT, PIXEL_FORMAT_R32G32B32_TYPELESS, PIXEL_FORMAT_R32G32B32_UINT, PIXEL_FORMAT_R32G32B32_TYPELESS, PIXEL_FORMAT_R32G32B32_SINT, PIXEL_FORMAT_R32G32B32_TYPELESS, PIXEL_FORMAT_R16G16B16A16_TYPELESS, PIXEL_FORMAT_R16G16B16A16_TYPELESS, PIXEL_FORMAT_R16G16B16A16_FLOAT, PIXEL_FORMAT_R16G16B16A16_TYPELESS, PIXEL_FORMAT_R16G16B16A16_UNORM, PIXEL_FORMAT_R16G16B16A16_TYPELESS, PIXEL_FORMAT_R16G16B16A16_UINT, PIXEL_FORMAT_R16G16B16A16_TYPELESS, PIXEL_FORMAT_R16G16B16A16_SNORM, PIXEL_FORMAT_R16G16B16A16_TYPELESS, PIXEL_FORMAT_R16G16B16A16_SINT, PIXEL_FORMAT_R16G16B16A16_TYPELESS, PIXEL_FORMAT_R32G32_TYPELESS, PIXEL_FORMAT_R32G32_TYPELESS, PIXEL_FORMAT_R32G32_FLOAT, PIXEL_FORMAT_R32G32_TYPELESS, PIXEL_FORMAT_R32G32_UINT, PIXEL_FORMAT_R32G32_TYPELESS, PIXEL_FORMAT_R32G32_SINT, PIXEL_FORMAT_R32G32_TYPELESS, PIXEL_FORMAT_R32G8X24_TYPELESS, PIXEL_FORMAT_R32G8X24_TYPELESS, PIXEL_FORMAT_D32_FLOAT_S8X24_UINT, PIXEL_FORMAT_D32_FLOAT_S8X24_UINT, PIXEL_FORMAT_R32_FLOAT_X8X24_TYPELESS, PIXEL_FORMAT_R32_FLOAT_X8X24_TYPELESS, PIXEL_FORMAT_X32_TYPELESS_G8X24_UINT, PIXEL_FORMAT_X32_TYPELESS_G8X24_UINT, PIXEL_FORMAT_R10G10B10A2_TYPELESS, PIXEL_FORMAT_R10G10B10A2_TYPELESS, PIXEL_FORMAT_R10G10B10A2_UNORM, PIXEL_FORMAT_R10G10B10A2_TYPELESS, PIXEL_FORMAT_R10G10B10A2_UINT, PIXEL_FORMAT_R10G10B10A2_TYPELESS, PIXEL_FORMAT_R11G11B10_FLOAT, PIXEL_FORMAT_R11G11B10_FLOAT, PIXEL_FORMAT_R8G8B8A8_TYPELESS, PIXEL_FORMAT_R8G8B8A8_TYPELESS, PIXEL_FORMAT_R8G8B8A8_UNORM, PIXEL_FORMAT_R8G8B8A8_TYPELESS, PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB, PIXEL_FORMAT_R8G8B8A8_TYPELESS, PIXEL_FORMAT_R8G8B8A8_UINT, PIXEL_FORMAT_R8G8B8A8_TYPELESS, PIXEL_FORMAT_R8G8B8A8_SNORM, PIXEL_FORMAT_R8G8B8A8_TYPELESS, PIXEL_FORMAT_R8G8B8A8_SINT, PIXEL_FORMAT_R8G8B8A8_TYPELESS, PIXEL_FORMAT_R16G16_TYPELESS, PIXEL_FORMAT_R16G16_TYPELESS, PIXEL_FORMAT_R16G16_FLOAT, PIXEL_FORMAT_R16G16_TYPELESS, PIXEL_FORMAT_R16G16_UNORM, PIXEL_FORMAT_R16G16_TYPELESS, PIXEL_FORMAT_R16G16_UINT, PIXEL_FORMAT_R16G16_TYPELESS, PIXEL_FORMAT_R16G16_SNORM, PIXEL_FORMAT_R16G16_TYPELESS, PIXEL_FORMAT_R16G16_SINT, PIXEL_FORMAT_R16G16_TYPELESS, PIXEL_FORMAT_R32_TYPELESS, PIXEL_FORMAT_R32_TYPELESS, PIXEL_FORMAT_D32_FLOAT, PIXEL_FORMAT_R32_TYPELESS, PIXEL_FORMAT_R32_FLOAT, PIXEL_FORMAT_R32_TYPELESS, PIXEL_FORMAT_R32_UINT, PIXEL_FORMAT_R32_TYPELESS, PIXEL_FORMAT_R32_SINT, PIXEL_FORMAT_R32_TYPELESS, PIXEL_FORMAT_R24G8_TYPELESS, PIXEL_FORMAT_R24G8_TYPELESS, PIXEL_FORMAT_D24_UNORM_S8_UINT, PIXEL_FORMAT_R24G8_TYPELESS, PIXEL_FORMAT_R24_UNORM_X8_TYPELESS, PIXEL_FORMAT_R24_UNORM_X8_TYPELESS, PIXEL_FORMAT_X24_TYPELESS_G8_UINT, PIXEL_FORMAT_X24_TYPELESS_G8_UINT, PIXEL_FORMAT_R8G8_TYPELESS, PIXEL_FORMAT_R8G8_TYPELESS, PIXEL_FORMAT_R8G8_UNORM, PIXEL_FORMAT_R8G8_TYPELESS, PIXEL_FORMAT_R8G8_UINT, PIXEL_FORMAT_R8G8_TYPELESS, PIXEL_FORMAT_R8G8_SNORM, PIXEL_FORMAT_R8G8_TYPELESS, PIXEL_FORMAT_R8G8_SINT, PIXEL_FORMAT_R8G8_TYPELESS, PIXEL_FORMAT_R16_TYPELESS, PIXEL_FORMAT_R16_TYPELESS, PIXEL_FORMAT_R16_FLOAT, PIXEL_FORMAT_R16_TYPELESS, PIXEL_FORMAT_D16_UNORM, PIXEL_FORMAT_D16_UNORM, PIXEL_FORMAT_R16_UNORM, PIXEL_FORMAT_R16_TYPELESS, PIXEL_FORMAT_R16_UINT, PIXEL_FORMAT_R16_TYPELESS, PIXEL_FORMAT_R16_SNORM, PIXEL_FORMAT_R16_TYPELESS, PIXEL_FORMAT_R16_SINT, PIXEL_FORMAT_R16_TYPELESS, PIXEL_FORMAT_R8_TYPELESS, PIXEL_FORMAT_R8_TYPELESS, PIXEL_FORMAT_R8_UNORM, PIXEL_FORMAT_R8_TYPELESS, PIXEL_FORMAT_R8_UINT, PIXEL_FORMAT_R8_TYPELESS, PIXEL_FORMAT_R8_SNORM, PIXEL_FORMAT_R8_TYPELESS, PIXEL_FORMAT_R8_SINT, PIXEL_FORMAT_R8_TYPELESS, PIXEL_FORMAT_A8_UNORM, PIXEL_FORMAT_A8_UNORM, PIXEL_FORMAT_R1_UNORM, PIXEL_FORMAT_R1_UNORM, PIXEL_FORMAT_R9G9B9E5_SHAREDEXP, PIXEL_FORMAT_R9G9B9E5_SHAREDEXP, PIXEL_FORMAT_R8G8_B8G8_UNORM, PIXEL_FORMAT_R8G8_B8G8_UNORM, PIXEL_FORMAT_G8R8_G8B8_UNORM, PIXEL_FORMAT_G8R8_G8B8_UNORM, PIXEL_FORMAT_BC1_TYPELESS, PIXEL_FORMAT_BC1_TYPELESS, PIXEL_FORMAT_BC1_UNORM, PIXEL_FORMAT_BC1_TYPELESS, PIXEL_FORMAT_BC1_UNORM_SRGB, PIXEL_FORMAT_BC1_TYPELESS, PIXEL_FORMAT_BC2_TYPELESS, PIXEL_FORMAT_BC2_TYPELESS, PIXEL_FORMAT_BC2_UNORM, PIXEL_FORMAT_BC2_TYPELESS, PIXEL_FORMAT_BC2_UNORM_SRGB, PIXEL_FORMAT_BC2_TYPELESS, PIXEL_FORMAT_BC3_TYPELESS, PIXEL_FORMAT_BC3_TYPELESS, PIXEL_FORMAT_BC3_UNORM, PIXEL_FORMAT_BC3_TYPELESS, PIXEL_FORMAT_BC3_UNORM_SRGB, PIXEL_FORMAT_BC3_TYPELESS, PIXEL_FORMAT_BC4_TYPELESS, PIXEL_FORMAT_BC4_TYPELESS, PIXEL_FORMAT_BC4_UNORM, PIXEL_FORMAT_BC4_TYPELESS, PIXEL_FORMAT_BC4_SNORM, PIXEL_FORMAT_BC4_TYPELESS, PIXEL_FORMAT_BC5_TYPELESS, PIXEL_FORMAT_BC5_TYPELESS, PIXEL_FORMAT_BC5_UNORM, PIXEL_FORMAT_BC5_TYPELESS, PIXEL_FORMAT_BC5_SNORM, PIXEL_FORMAT_BC5_TYPELESS, PIXEL_FORMAT_B5G6R5_UNORM, PIXEL_FORMAT_B5G6R5_UNORM, PIXEL_FORMAT_B5G5R5A1_UNORM, PIXEL_FORMAT_B5G5R5A1_UNORM, PIXEL_FORMAT_B8G8R8A8_UNORM, PIXEL_FORMAT_B8G8R8A8_TYPELESS, PIXEL_FORMAT_B8G8R8X8_UNORM, PIXEL_FORMAT_B8G8R8X8_TYPELESS, PIXEL_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, PIXEL_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, PIXEL_FORMAT_B8G8R8A8_TYPELESS, PIXEL_FORMAT_B8G8R8A8_TYPELESS, PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB, PIXEL_FORMAT_B8G8R8A8_TYPELESS, PIXEL_FORMAT_B8G8R8X8_TYPELESS, PIXEL_FORMAT_B8G8R8X8_TYPELESS, PIXEL_FORMAT_B8G8R8X8_UNORM_SRGB, PIXEL_FORMAT_B8G8R8X8_TYPELESS, PIXEL_FORMAT_BC6H_TYPELESS, PIXEL_FORMAT_BC6H_TYPELESS, PIXEL_FORMAT_BC6H_UF16, PIXEL_FORMAT_BC6H_TYPELESS, PIXEL_FORMAT_BC6H_SF16, PIXEL_FORMAT_BC6H_TYPELESS, PIXEL_FORMAT_BC7_TYPELESS, PIXEL_FORMAT_BC7_TYPELESS, PIXEL_FORMAT_BC7_UNORM, PIXEL_FORMAT_BC7_TYPELESS, PIXEL_FORMAT_BC7_UNORM_SRGB, PIXEL_FORMAT_BC7_TYPELESS
	};

	for( uint32_t i = 0; i < sizeof( formats ) / sizeof( formats[0] ); i += 2 )
	{
		EXPECT_EQ( formats[i + 1], Tr2RenderContextEnum::MakeTypeless( formats[i] ) );
	}
}

TEST( RenderContextEnum, CanConvertPixelFormatTosRgb )
{
	Tr2RenderContextEnum::PixelFormat formats[] = {
		PIXEL_FORMAT_UNKNOWN,
		PIXEL_FORMAT_UNKNOWN,
		PIXEL_FORMAT_R32G32B32A32_TYPELESS,
		PIXEL_FORMAT_R32G32B32A32_TYPELESS,
		PIXEL_FORMAT_R32G32B32A32_FLOAT,
		PIXEL_FORMAT_R32G32B32A32_FLOAT,
		PIXEL_FORMAT_R32G32B32A32_UINT,
		PIXEL_FORMAT_R32G32B32A32_UINT,
		PIXEL_FORMAT_R32G32B32A32_SINT,
		PIXEL_FORMAT_R32G32B32A32_SINT,
		PIXEL_FORMAT_R32G32B32_TYPELESS,
		PIXEL_FORMAT_R32G32B32_TYPELESS,
		PIXEL_FORMAT_R32G32B32_FLOAT,
		PIXEL_FORMAT_R32G32B32_FLOAT,
		PIXEL_FORMAT_R32G32B32_UINT,
		PIXEL_FORMAT_R32G32B32_UINT,
		PIXEL_FORMAT_R32G32B32_SINT,
		PIXEL_FORMAT_R32G32B32_SINT,
		PIXEL_FORMAT_R16G16B16A16_TYPELESS,
		PIXEL_FORMAT_R16G16B16A16_TYPELESS,
		PIXEL_FORMAT_R16G16B16A16_FLOAT,
		PIXEL_FORMAT_R16G16B16A16_FLOAT,
		PIXEL_FORMAT_R16G16B16A16_UNORM,
		PIXEL_FORMAT_R16G16B16A16_UNORM,
		PIXEL_FORMAT_R16G16B16A16_UINT,
		PIXEL_FORMAT_R16G16B16A16_UINT,
		PIXEL_FORMAT_R16G16B16A16_SNORM,
		PIXEL_FORMAT_R16G16B16A16_SNORM,
		PIXEL_FORMAT_R16G16B16A16_SINT,
		PIXEL_FORMAT_R16G16B16A16_SINT,
		PIXEL_FORMAT_R32G32_TYPELESS,
		PIXEL_FORMAT_R32G32_TYPELESS,
		PIXEL_FORMAT_R32G32_FLOAT,
		PIXEL_FORMAT_R32G32_FLOAT,
		PIXEL_FORMAT_R32G32_UINT,
		PIXEL_FORMAT_R32G32_UINT,
		PIXEL_FORMAT_R32G32_SINT,
		PIXEL_FORMAT_R32G32_SINT,
		PIXEL_FORMAT_R32G8X24_TYPELESS,
		PIXEL_FORMAT_R32G8X24_TYPELESS,
		PIXEL_FORMAT_D32_FLOAT_S8X24_UINT,
		PIXEL_FORMAT_D32_FLOAT_S8X24_UINT,
		PIXEL_FORMAT_R32_FLOAT_X8X24_TYPELESS,
		PIXEL_FORMAT_R32_FLOAT_X8X24_TYPELESS,
		PIXEL_FORMAT_X32_TYPELESS_G8X24_UINT,
		PIXEL_FORMAT_X32_TYPELESS_G8X24_UINT,
		PIXEL_FORMAT_R10G10B10A2_TYPELESS,
		PIXEL_FORMAT_R10G10B10A2_TYPELESS,
		PIXEL_FORMAT_R10G10B10A2_UNORM,
		PIXEL_FORMAT_R10G10B10A2_UNORM,
		PIXEL_FORMAT_R10G10B10A2_UINT,
		PIXEL_FORMAT_R10G10B10A2_UINT,
		PIXEL_FORMAT_R11G11B10_FLOAT,
		PIXEL_FORMAT_R11G11B10_FLOAT,
		PIXEL_FORMAT_R8G8B8A8_TYPELESS,
		PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB,
		PIXEL_FORMAT_R8G8B8A8_UNORM,
		PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB,
		PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB,
		PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB,
		PIXEL_FORMAT_R8G8B8A8_UINT,
		PIXEL_FORMAT_R8G8B8A8_UINT,
		PIXEL_FORMAT_R8G8B8A8_SNORM,
		PIXEL_FORMAT_R8G8B8A8_SNORM,
		PIXEL_FORMAT_R8G8B8A8_SINT,
		PIXEL_FORMAT_R8G8B8A8_SINT,
		PIXEL_FORMAT_R16G16_TYPELESS,
		PIXEL_FORMAT_R16G16_TYPELESS,
		PIXEL_FORMAT_R16G16_FLOAT,
		PIXEL_FORMAT_R16G16_FLOAT,
		PIXEL_FORMAT_R16G16_UNORM,
		PIXEL_FORMAT_R16G16_UNORM,
		PIXEL_FORMAT_R16G16_UINT,
		PIXEL_FORMAT_R16G16_UINT,
		PIXEL_FORMAT_R16G16_SNORM,
		PIXEL_FORMAT_R16G16_SNORM,
		PIXEL_FORMAT_R16G16_SINT,
		PIXEL_FORMAT_R16G16_SINT,
		PIXEL_FORMAT_R32_TYPELESS,
		PIXEL_FORMAT_R32_TYPELESS,
		PIXEL_FORMAT_D32_FLOAT,
		PIXEL_FORMAT_D32_FLOAT,
		PIXEL_FORMAT_R32_FLOAT,
		PIXEL_FORMAT_R32_FLOAT,
		PIXEL_FORMAT_R32_UINT,
		PIXEL_FORMAT_R32_UINT,
		PIXEL_FORMAT_R32_SINT,
		PIXEL_FORMAT_R32_SINT,
		PIXEL_FORMAT_R24G8_TYPELESS,
		PIXEL_FORMAT_R24G8_TYPELESS,
		PIXEL_FORMAT_D24_UNORM_S8_UINT,
		PIXEL_FORMAT_D24_UNORM_S8_UINT,
		PIXEL_FORMAT_R24_UNORM_X8_TYPELESS,
		PIXEL_FORMAT_R24_UNORM_X8_TYPELESS,
		PIXEL_FORMAT_X24_TYPELESS_G8_UINT,
		PIXEL_FORMAT_X24_TYPELESS_G8_UINT,
		PIXEL_FORMAT_R8G8_TYPELESS,
		PIXEL_FORMAT_R8G8_TYPELESS,
		PIXEL_FORMAT_R8G8_UNORM,
		PIXEL_FORMAT_R8G8_UNORM,
		PIXEL_FORMAT_R8G8_UINT,
		PIXEL_FORMAT_R8G8_UINT,
		PIXEL_FORMAT_R8G8_SNORM,
		PIXEL_FORMAT_R8G8_SNORM,
		PIXEL_FORMAT_R8G8_SINT,
		PIXEL_FORMAT_R8G8_SINT,
		PIXEL_FORMAT_R16_TYPELESS,
		PIXEL_FORMAT_R16_TYPELESS,
		PIXEL_FORMAT_R16_FLOAT,
		PIXEL_FORMAT_R16_FLOAT,
		PIXEL_FORMAT_D16_UNORM,
		PIXEL_FORMAT_D16_UNORM,
		PIXEL_FORMAT_R16_UNORM,
		PIXEL_FORMAT_R16_UNORM,
		PIXEL_FORMAT_R16_UINT,
		PIXEL_FORMAT_R16_UINT,
		PIXEL_FORMAT_R16_SNORM,
		PIXEL_FORMAT_R16_SNORM,
		PIXEL_FORMAT_R16_SINT,
		PIXEL_FORMAT_R16_SINT,
		PIXEL_FORMAT_R8_TYPELESS,
		PIXEL_FORMAT_R8_TYPELESS,
		PIXEL_FORMAT_R8_UNORM,
		PIXEL_FORMAT_R8_UNORM,
		PIXEL_FORMAT_R8_UINT,
		PIXEL_FORMAT_R8_UINT,
		PIXEL_FORMAT_R8_SNORM,
		PIXEL_FORMAT_R8_SNORM,
		PIXEL_FORMAT_R8_SINT,
		PIXEL_FORMAT_R8_SINT,
		PIXEL_FORMAT_A8_UNORM,
		PIXEL_FORMAT_A8_UNORM,
		PIXEL_FORMAT_R1_UNORM,
		PIXEL_FORMAT_R1_UNORM,
		PIXEL_FORMAT_R9G9B9E5_SHAREDEXP,
		PIXEL_FORMAT_R9G9B9E5_SHAREDEXP,
		PIXEL_FORMAT_R8G8_B8G8_UNORM,
		PIXEL_FORMAT_R8G8_B8G8_UNORM,
		PIXEL_FORMAT_G8R8_G8B8_UNORM,
		PIXEL_FORMAT_G8R8_G8B8_UNORM,
		PIXEL_FORMAT_BC1_TYPELESS,
		PIXEL_FORMAT_BC1_UNORM_SRGB,
		PIXEL_FORMAT_BC1_UNORM,
		PIXEL_FORMAT_BC1_UNORM_SRGB,
		PIXEL_FORMAT_BC1_UNORM_SRGB,
		PIXEL_FORMAT_BC1_UNORM_SRGB,
		PIXEL_FORMAT_BC2_TYPELESS,
		PIXEL_FORMAT_BC2_UNORM_SRGB,
		PIXEL_FORMAT_BC2_UNORM,
		PIXEL_FORMAT_BC2_UNORM_SRGB,
		PIXEL_FORMAT_BC2_UNORM_SRGB,
		PIXEL_FORMAT_BC2_UNORM_SRGB,
		PIXEL_FORMAT_BC3_TYPELESS,
		PIXEL_FORMAT_BC3_UNORM_SRGB,
		PIXEL_FORMAT_BC3_UNORM,
		PIXEL_FORMAT_BC3_UNORM_SRGB,
		PIXEL_FORMAT_BC3_UNORM_SRGB,
		PIXEL_FORMAT_BC3_UNORM_SRGB,
		PIXEL_FORMAT_BC4_TYPELESS,
		PIXEL_FORMAT_BC4_TYPELESS,
		PIXEL_FORMAT_BC4_UNORM,
		PIXEL_FORMAT_BC4_UNORM,
		PIXEL_FORMAT_BC4_SNORM,
		PIXEL_FORMAT_BC4_SNORM,
		PIXEL_FORMAT_BC5_TYPELESS,
		PIXEL_FORMAT_BC5_TYPELESS,
		PIXEL_FORMAT_BC5_UNORM,
		PIXEL_FORMAT_BC5_UNORM,
		PIXEL_FORMAT_BC5_SNORM,
		PIXEL_FORMAT_BC5_SNORM,
		PIXEL_FORMAT_B5G6R5_UNORM,
		PIXEL_FORMAT_B5G6R5_UNORM,
		PIXEL_FORMAT_B5G5R5A1_UNORM,
		PIXEL_FORMAT_B5G5R5A1_UNORM,
		PIXEL_FORMAT_B8G8R8A8_UNORM,
		PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB,
		PIXEL_FORMAT_B8G8R8X8_UNORM,
		PIXEL_FORMAT_B8G8R8X8_UNORM_SRGB,
		PIXEL_FORMAT_R10G10B10_XR_BIAS_A2_UNORM,
		PIXEL_FORMAT_R10G10B10_XR_BIAS_A2_UNORM,
		PIXEL_FORMAT_B8G8R8A8_TYPELESS,
		PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB,
		PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB,
		PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB,
		PIXEL_FORMAT_B8G8R8X8_TYPELESS,
		PIXEL_FORMAT_B8G8R8X8_UNORM_SRGB,
		PIXEL_FORMAT_B8G8R8X8_UNORM_SRGB,
		PIXEL_FORMAT_B8G8R8X8_UNORM_SRGB,
		PIXEL_FORMAT_BC6H_TYPELESS,
		PIXEL_FORMAT_BC6H_TYPELESS,
		PIXEL_FORMAT_BC6H_UF16,
		PIXEL_FORMAT_BC6H_UF16,
		PIXEL_FORMAT_BC6H_SF16,
		PIXEL_FORMAT_BC6H_SF16,
		PIXEL_FORMAT_BC7_TYPELESS,
		PIXEL_FORMAT_BC7_UNORM_SRGB,
		PIXEL_FORMAT_BC7_UNORM,
		PIXEL_FORMAT_BC7_UNORM_SRGB,
		PIXEL_FORMAT_BC7_UNORM_SRGB,
		PIXEL_FORMAT_BC7_UNORM_SRGB,
	};

	for( uint32_t i = 0; i < sizeof( formats ) / sizeof( formats[0] ); i += 2 )
	{
		EXPECT_EQ( formats[i + 1], Tr2RenderContextEnum::MakeSrgb( formats[i] ) );
	}
}

TEST_F( RenderContext, PrimaryRenderContextIsSetCorrectly )
{
	ENSURE_GPU_OR_SKIP
	EXPECT_EQ( renderContext, &renderContext->GetPrimaryRenderContext() );
}

TEST_F( RenderContext, CanBeginAndEndScene )
{
	ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );
}

#if ( TRINITY_PLATFORM == TRINITY_METAL )
TEST_F( RenderContext, NativeHandlesObserveTheFrame )
{
	ENSURE_GPU_OR_SKIP
	// Device and queue exist from creation on; they are what ui_init needs.
	EXPECT_NE( 0u, renderContext->GetNativeDevice() );
	EXPECT_NE( 0u, renderContext->GetNativeCommandQueue() );
	// The encoder is only valid inside a pass; outside one, 0 is the documented
	// value — the same contract the DX12 command-list getter carries (S6: the
	// call site is the guard, so the honest answer out-of-frame is "not now").
	EXPECT_EQ( 0u, renderContext->GetNativeRenderEncoder() );
	// The DX12 names that have no Metal meaning stay 0 — spec D3 forbids
	// smuggling a command buffer through a heap getter.
	EXPECT_EQ( 0u, renderContext->GetNativeSrvHeap() );
	EXPECT_EQ( 0u, renderContext->GetNativeSamplerHeap() );
}

TEST_F( RenderContext, NativeCommandBufferExistsAfterEndScene )
{
	ENSURE_GPU_OR_SKIP
	// Unlike the encoder, the command buffer has a happy path this fixture can
	// reach without opening a render pass: EndScene's FlushOutstandingOperations
	// creates one unconditionally when the frame recorded nothing ("The tests
	// just do a present with no render" -- MetalWorkQueue::FlushOutstandingOperations),
	// and ResetFrame never clears it. Same BeginScene/EndScene sequence as
	// CanBeginAndEndScene above; the getter only reads what that sequence
	// already produced.
	ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );
	EXPECT_NE( 0u, renderContext->GetNativeCommandBuffer() );
}
#endif

#if ( TRINITY_PLATFORM == TRINITY_VULKAN )
#include "vulkan/UtilitiesVulkan.h"
#include <vulkan/vulkan.h>

namespace
{

// The two shaders the side-effect test below needs, in the shape Rendering.cpp uses them.
// Duplicated rather than shared because the alternative is a test-support header for two
// functions, and Shaders.vulkan already carries both .psh/.vsh for this target.
ALResult CreatePositionOnlyVSForScissorTest( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( PositionOnly.vs )
	};

	auto input = Tr2ShaderSignatureAL().Add( Tr2VertexDefinition::POSITION, 0, 0, Tr2ShaderPipelineInputAL::FLOAT, 3 );

	return shader.Create( VERTEX_SHADER, bytecode, input, "", renderContext );
}

ALResult CreateConstantColorPSForScissorTest( Tr2ShaderAL& shader, Tr2PrimaryRenderContextAL& renderContext )
{
	uint8_t bytecode[] = {
#include INCLUDE_SHADER_CODE( ConstantColor.ps )
	};

	return shader.Create( PIXEL_SHADER, bytecode, Tr2ShaderSignatureAL(), "", renderContext );
}

}

TEST_F( RenderContext, VulkanNativeHandlesExistFromCreation )
{
	ENSURE_GPU_OR_SKIP
	// Everything ImGui_ImplVulkan_InitInfo needs, and the moment it needs it: ui_init runs
	// before the first frame, so none of these may depend on a scene being open (spec §7.3).
	EXPECT_NE( 0u, renderContext->GetNativeInstance() );
	EXPECT_NE( 0u, renderContext->GetNativePhysicalDevice() );
	EXPECT_NE( 0u, renderContext->GetNativeDevice() );
	EXPECT_NE( 0u, renderContext->GetNativeCommandQueue() );
	// A family index, not a handle: 0 is legal, so the assertion is that it is a plausible
	// index rather than that it is non-zero. 64 is far above any device's family count and
	// well below the 0xffffffff the AL uses internally for "none", so this catches the
	// mistake of passing that through (which would abort ImGui's init inside the driver).
	EXPECT_LT( renderContext->GetNativeQueueFamily(), 64u );
	// The DX12 names have no Vulkan meaning and stay 0 (spec D3).
	EXPECT_EQ( 0u, renderContext->GetNativeCommandList() );
	EXPECT_EQ( 0u, renderContext->GetNativeSrvHeap() );
	EXPECT_EQ( 0u, renderContext->GetNativeSamplerHeap() );
	EXPECT_EQ( 0u, renderContext->GetNativeRenderEncoder() );
}

TEST_F( RenderContext, NativeCommandBufferOpensTheScopeAndSurvivesForeignScissor )
{
	ENSURE_GPU_OR_SKIP
	// Spec §7.4, both side effects, in the arrangement that makes them fail if absent.
	//
	// A 16x16 target cleared black; then a "hosted UI" (this test) takes the command buffer
	// and leaves a 1x1 scissor behind; then the AL draws a full-screen white quad. The far
	// corner must be white. If GetNativeCommandBuffer did not open a rendering scope, the
	// vkCmdSetScissor below is recorded outside one and the validation layer says so. If it
	// did not mark the state dirty, the AL's draw inherits the 1x1 scissor, paints one pixel
	// and leaves the corner black -- with no validation error at all, which is why this is a
	// pixel assertion and not a state one.
	Tr2ShaderAL vs;
	ASSERT_HRESULT_SUCCEEDED( CreatePositionOnlyVSForScissorTest( vs, *renderContext ) );
	Tr2ShaderAL ps;
	ASSERT_HRESULT_SUCCEEDED( CreateConstantColorPSForScissorTest( ps, *renderContext ) );
	Tr2ShaderAL shaders[] = { vs, ps };
	Tr2ShaderProgramAL sp;
	ASSERT_HRESULT_SUCCEEDED( sp.Create( shaders, 2, *renderContext ) );

	// A strip that covers the whole target in clip space.
	float vertices[] = {
		-1.0f, -1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f,
		 1.0f, -1.0f, 0.0f,
		 1.0f,  1.0f, 0.0f,
	};
	const uint32_t vbStride = 3 * sizeof( float );
	Tr2BufferAL vb;
	ASSERT_HRESULT_SUCCEEDED( vb.Create( vbStride, sizeof( vertices ) / vbStride,
		Tr2GpuUsage::VERTEX_BUFFER, Tr2CpuUsage::NONE, vertices, *renderContext ) );

	Tr2VertexDefinition definition;
	definition.Add( Tr2VertexDefinition::FLOAT32_3, Tr2VertexDefinition::POSITION );
	Tr2VertexLayoutAL vertexLayout;
	ASSERT_HRESULT_SUCCEEDED( vertexLayout.Create( definition, *renderContext ) );

	Tr2TextureAL rt;
	ASSERT_HRESULT_SUCCEEDED( rt.Create( Tr2BitmapDimensions( 16, 16, 1, PIXEL_FORMAT_B8G8R8A8_UNORM ),
		Tr2GpuUsage::RENDER_TARGET, Tr2CpuUsage::READ, *renderContext ) );

	ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderTarget( rt ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PushDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetDepthStencil( Tr2TextureAL() ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetViewport( Tr2Viewport( 16, 16 ) ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->Clear( CLEARFLAGS_TARGET, 0xff000000, 1.0f ) );

	const uint64_t handle = renderContext->GetNativeCommandBuffer();
	ASSERT_NE( 0u, handle );
	VkRect2D tiny = { { 0, 0 }, { 1, 1 } };
	vkCmdSetScissor( reinterpret_cast<VkCommandBuffer>( static_cast<uintptr_t>( handle ) ), 0, 1, &tiny );

	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, vb, 0, vbStride ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetVertexLayout( vertexLayout ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( sp ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ZENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_ALPHABLENDENABLE, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetRenderState( RS_CULLMODE, CULLMODE_NONE ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetTopology( TOP_TRIANGLE_STRIP ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->DrawPrimitive( 0, 2 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopDepthStencil() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->PopRenderTarget() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );

	const void* data = nullptr;
	uint32_t pitch = 0;
	ASSERT_HRESULT_SUCCEEDED( rt.MapForReading( Tr2TextureSubresource( 0 ), data, pitch, *renderContext ) );
	ASSERT_NE( nullptr, data );
	// BGRA, and ConstantColor.ps writes float4( 1, 0, 0, 1 ) -- red, not white. The
	// discriminator is the red channel: the clear is black, so a corner that stayed black
	// means the quad never covered it, which on this arrangement can only be the foreign
	// 1x1 scissor still in force. Asserted on all four channels because "red" is also what
	// says the draw itself was correct rather than something happening to be non-zero.
	const uint8_t* corner = static_cast<const uint8_t*>( data ) + 15 * pitch + 15 * 4;
	EXPECT_EQ( 0xff, corner[2] ) << "the far corner is not red: the AL's draw inherited the foreign 1x1 scissor";
	EXPECT_EQ( 0x00, corner[0] );
	EXPECT_EQ( 0x00, corner[1] );
	EXPECT_EQ( 0xff, corner[3] );
	rt.UnmapForReading( *renderContext );

	ASSERT_HRESULT_SUCCEEDED( renderContext->SetStreamSource( 0, Tr2BufferAL(), 0, 0 ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->SetShaderProgram( Tr2ShaderProgramAL() ) );
}

TEST_F( RenderContext, NativeBackBufferFormatIsTheSwapchainsVkFormat )
{
	ENSURE_GPU_OR_SKIP
	// ui_init's 'color_format' on Vulkan: the VkFormat the swapchain images carry, which is
	// what ImGui_ImplVulkan's pipeline must declare (spec §7.3). 0 would mean "no device".
	const uint64_t native = renderContext->GetNativeBackBufferFormat();
	EXPECT_NE( 0u, native );
	EXPECT_EQ( static_cast<uint64_t>( TrinityALImpl::GetVulkanFormat( renderContext->GetBackBufferFormat() ) ), native );
	// The Metal-only encoder and the DX12-only heaps stay 0 here (spec D3).
	EXPECT_EQ( 0u, renderContext->GetNativeRenderEncoder() );
	EXPECT_EQ( 0u, renderContext->GetNativeSrvHeap() );
	EXPECT_EQ( 0u, renderContext->GetNativeSamplerHeap() );
}
#endif
