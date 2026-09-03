// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include <Tr2DriverUtilities.h>

using namespace Tr2RenderContextEnum;

namespace
{
// A platform can have no desktop, and then it has no display mode to report.
// Tr2VideoAdapterInfoALVulkan.cpp's GetAdapterDisplayMode has two arms: a Win32 one
// that reads the real mode through EnumDisplaySettings, and a fallback for everywhere
// else that sets width = height = 0 on purpose -- the surface dictates the extent
// (GetSwapChainExtent always takes currentExtent on Android), and a made-up 1080p
// there would only masquerade as information. GetAdapterModeCount and GetAdapterMode
// forward to that same fallback.
//
// The two display-mode tests below assume every platform has a real mode to
// enumerate. On Android that assumption is what is wrong, not the AL, so they skip by
// name here instead of asserting through it (M6 spec, section 4: skips named). The
// condition mirrors the AL's own rather than naming Android -- a Vulkan target
// without Win32 is exactly the arm that reports no mode.
//
// Counting note: GTEST_SKIP reports as Passed on the console. Skip counts come from
// the XML (result="skipped"), never from the console totals.
#if ( TRINITY_PLATFORM == TRINITY_VULKAN ) && !defined( _WIN32 )
constexpr bool s_hasDesktopDisplayMode = false;
#else
constexpr bool s_hasDesktopDisplayMode = true;
#endif

const char* const s_noDisplayModeReason =
	"No desktop on this platform, so no display mode to report: the Vulkan AL answers "
	"0x0 by design and the surface dictates the real extent.";
}

TEST( VideoAdapterInfo, HasAtLeastOneAdapter )
{
	unsigned count = 0;
	ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterCount( count ) );
	if( !count )
	{
		GTEST_SKIP() << "Test Skipped as no adapters present on machine.";
	}
	EXPECT_GT( count, 0u );
}

TEST( VideoAdapterInfo, CanGetDefaultAdapterInfo )
{
	unsigned count = 0;
	Tr2VideoAdapterInfo::GetAdapterCount( count );
	if( !count )
	{
		GTEST_SKIP() << "Test Skipped as no adapters present on machine.";
	}

	Tr2AdapterInfo info;
	ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterInfo( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, info ) );
}

TEST( VideoAdapterInfo, CanGetDefaultAdapterMonitor )
{
	unsigned count = 0;
	Tr2VideoAdapterInfo::GetAdapterCount( count );
	if( !count )
	{
		GTEST_SKIP() << "Test Skipped as no adapters present on machine.";
	}

	void* monitor;
	ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterMonitor( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, monitor ) );
}

TEST( VideoAdapterInfo, CanGetDefaultAdapterDisplayMode )
{
	unsigned count = 0;
	Tr2VideoAdapterInfo::GetAdapterCount( count );
	if( !count )
	{
		GTEST_SKIP() << "Test Skipped as no adapters present on machine.";
	}

	if( !s_hasDesktopDisplayMode )
	{
		GTEST_SKIP() << s_noDisplayModeReason;
	}

	Tr2DisplayModeInfo mode;
	memset( &mode, 0, sizeof( mode ) );
	ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterDisplayMode( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, mode ) );
	EXPECT_GT( mode.width, 0u );
	EXPECT_GT( mode.height, 0u );
	EXPECT_GT( mode.format, 0 );
}

TEST( VideoAdapterInfo, CanEnumerateModesForDefaultAdapter )
{
	unsigned adapter_count = 0;
	Tr2VideoAdapterInfo::GetAdapterCount( adapter_count );
	if( !adapter_count )
	{
		GTEST_SKIP() << "Test Skipped as no adapters present on machine.";
	}

	if( !s_hasDesktopDisplayMode )
	{
		GTEST_SKIP() << s_noDisplayModeReason;
	}

	Tr2DisplayModeInfo mode;
	memset( &mode, 0, sizeof( mode ) );
	ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterDisplayMode( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, mode ) );

	PixelFormat backBufferFormat = mode.format;
	unsigned count = 0;
	ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterModeCount( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, backBufferFormat, count ) );

	EXPECT_GT( count, 0u );

	for( unsigned i = 0; i < count; ++i )
	{
		memset( &mode, 0, sizeof( mode ) );
		ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterMode( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, backBufferFormat, i, mode ) );
		EXPECT_GT( mode.width, 0u );
		EXPECT_GT( mode.height, 0u );
		EXPECT_GT( mode.format, 0 );
	}
}

TEST( VideoAdapterInfo, DefaultAdapterSupportsItsCurrentBackBufferFormat )
{
	unsigned count = 0;
	Tr2VideoAdapterInfo::GetAdapterCount( count );
	if( !count )
	{
		GTEST_SKIP() << "Test Skipped as no adapters present on machine.";
	}

	Tr2DisplayModeInfo mode;
	memset( &mode, 0, sizeof( mode ) );
	ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterDisplayMode( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, mode ) );

	EXPECT_TRUE( Tr2VideoAdapterInfo::SupportsBackBufferFormat( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, mode.format ) );
}

TEST( VideoAdapterInfo, SameAdaptersAreNotDifferent )
{
	EXPECT_FALSE( Tr2VideoAdapterInfo::AreAdaptersDifferent( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, Tr2VideoAdapterInfo::DEFAULT_ADAPTER ) );
}

TEST( VideoAdapterInfo, DefaultAdapterSupports32bppRenderTarget )
{
	EXPECT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::SupportsRenderTargetFormat(
		Tr2VideoAdapterInfo::DEFAULT_ADAPTER,
		PIXEL_FORMAT_B8G8R8A8_UNORM ) );
}

#ifdef _WIN32
TEST( VideoAdapterInfo, CanGetDriverInfo )
{
	unsigned count = 0;
	Tr2VideoAdapterInfo::GetAdapterCount( count );
	if( !count )
	{
		GTEST_SKIP() << "Test Skipped as no adapters present on machine.";
	}

	Tr2AdapterInfo info;
	ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterInfo( Tr2VideoAdapterInfo::DEFAULT_ADAPTER, info ) );

	Tr2VideoDriverInfo driverInfo;
	ASSERT_HRESULT_SUCCEEDED( Tr2DriverUtilities::GetDriverVersion( info.deviceID, driverInfo ) );

	EXPECT_FALSE( driverInfo.driverVersionString.empty() );
	EXPECT_FALSE( driverInfo.driverVendor.empty() );
	EXPECT_FALSE( driverInfo.driverDate.empty() );
	EXPECT_GT( driverInfo.driverVersion, 0 );
}

TEST( VideoAdapterInfo, GettingDriverInfoForInvalidVendorFails )
{
	Tr2VideoDriverInfo driverInfo;
	ASSERT_HRESULT_FAILED( Tr2DriverUtilities::GetDriverVersion( 0xffffffff, driverInfo ) );
}

#endif