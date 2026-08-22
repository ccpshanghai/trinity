// Copyright © 2023 CCP ehf.

#include "StdAfx.h"

#if ( TRINITY_PLATFORM == TRINITY_METAL )

#include "Tr2VideoAdapterInfoALMetal.h"
#include "Tr2AdapterStructures.h"
#include <cmath>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/graphics/IOGraphicsLib.h>
#endif
#if TARGET_OS_IPHONE
#import <UIKit/UIScreen.h>
#endif
#import <Metal/Metal.h>


using namespace Tr2RenderContextEnum;

namespace
{
struct Display
{
	// CGDirectDisplayID on macOS -- itself just a uint32_t -- so this stays a plain
	// integer instead of pulling the type into scope unconditionally. iOS has no
	// per-display id at all and leaves this 0 (see RefreshDisplays); GetAdapterMonitor
	// already turns that into a null monitor handle without any platform split.
	uint32_t displayID;
	std::string name;
	std::wstring description;
	uint32_t vendorID;
	uint32_t deviceID;

	std::vector<Tr2DisplayModeInfo> modes;
};
std::vector<Display> s_displays;

#if TARGET_OS_OSX

void GetDisplayModes( CGDirectDisplayID display, std::vector<Tr2DisplayModeInfo>& modes )
{
	const CFStringRef keys[] = { kCGDisplayShowDuplicateLowResolutionModes };
	const CFBooleanRef values[] = { kCFBooleanTrue };
	auto dict = CFDictionaryCreate( nullptr,
									(const void**)keys,
									(const void**)values,
									1,
									&kCFCopyStringDictionaryKeyCallBacks,
									&kCFTypeDictionaryValueCallBacks );

	auto allModes = CGDisplayCopyAllDisplayModes( display, dict );
	CFRelease( dict );

	auto count = CFArrayGetCount( allModes );

	for( CFIndex i = 0; i < count; ++i )
	{
		auto modeRef = (CGDisplayModeRef)CFArrayGetValueAtIndex( allModes, i );

		Tr2DisplayModeInfo mode;
		mode.format = PIXEL_FORMAT_B8G8R8X8_UNORM;
		mode.width = (uint32_t)CGDisplayModeGetPixelWidth( modeRef );
		mode.height = (uint32_t)CGDisplayModeGetPixelHeight( modeRef );
		mode.refreshRateDenominator = 1;
		mode.refreshRateNumerator = 1;
		mode.scaling = DISPLAY_SCALING_UNSPECIFIED;
		mode.scanlineOrdering = SCANLINE_ORDER_UNSPECIFIED;

		auto found = std::find_if( begin( modes ), end( modes ), [&mode]( auto& other ) {
			return mode.width == other.width && mode.height == other.height;
		} );
		if( found == end( modes ) )
		{
			modes.push_back( mode );
		}
	}

	std::sort( begin( modes ), end( modes ), []( auto& m1, auto& m2 ) {
		if( m1.width == m2.width )
		{
			return m1.height > m2.height;
		}
		return m1.width > m2.width;
	} );
	CFRelease( allModes );
}

uint32_t GetEntryProperty( io_registry_entry_t entry, CFStringRef propertyName )
{
	uint32_t value = 0;
	CFTypeRef property = IORegistryEntrySearchCFProperty( entry,
														  kIOServicePlane,
														  propertyName,
														  kCFAllocatorDefault,
														  kIORegistryIterateRecursively | kIORegistryIterateParents );
	if( property )
	{
		if( auto data = reinterpret_cast<const uint32_t*>( CFDataGetBytePtr( (CFDataRef)property ) ) )
		{
			value = *data;
		}
		CFRelease( property );
	}
	return value;
}

#endif // TARGET_OS_OSX

#if TARGET_OS_IPHONE

// The one screen's current pixel extent. UIScreen is iOS's screen descriptor --
// the role CGDisplayCopyDisplayMode plays in the macOS branch. The AL is
// deliberately window-less (spec D6: it receives a CAMetalLayer, never a view),
// so there is no window scene to look the screen up through; mainScreen is
// deprecated in iOS 26 in favor of that scene-based lookup, and the pragma
// silences exactly that deprecation.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
void GetIOSScreenExtent( uint32_t& width, uint32_t& height )
{
	width = 0;
	height = 0;
	UIScreen* screen = [UIScreen mainScreen];
	if( !screen )
	{
		return;
	}
	const CGRect bounds = screen.bounds;
	const CGFloat nativeScale = screen.nativeScale; // pixels, not points -- CGDisplayModeGetPixelWidth parity
	width = (uint32_t)lround( bounds.size.width * nativeScale );
	height = (uint32_t)lround( bounds.size.height * nativeScale );
}
#pragma clang diagnostic pop

#endif // TARGET_OS_IPHONE

std::string ToString( NSString* string )
{
	NSData* data = [string dataUsingEncoding:NSASCIIStringEncoding allowLossyConversion:YES];
	int32_t length = int32_t( [data length] );
	return std::string( reinterpret_cast<const char*>( [data bytes] ), length );
}

std::wstring ToWString( NSString* string )
{
	NSData* data = [string dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
	int32_t length = int32_t( [data length] ) / sizeof( wchar_t );
	return std::wstring( reinterpret_cast<const wchar_t*>( [data bytes] ), length );
}

#if TARGET_OS_OSX

io_service_t IOServicePortFromCGDisplayID( CGDirectDisplayID displayID )
{
	io_iterator_t iter;
	io_service_t serv, servicePort = 0;

	CFMutableDictionaryRef matching = IOServiceMatching( "IODisplayConnect" );
	if( IOServiceGetMatchingServices( kIOMasterPortDefault, matching, &iter ) )
	{
		return 0;
	}

	auto displayVendorID = CGDisplayVendorNumber( displayID );
	auto displayModelID = CGDisplayModelNumber( displayID );
	auto displaySerial = CGDisplaySerialNumber( displayID );

	while( ( serv = IOIteratorNext( iter ) ) != 0 )
	{
		CFIndex vendorID, productID, serialNumber;
		Boolean success;

		auto info = IODisplayCreateInfoDictionary( serv, kIODisplayOnlyPreferredName );
		ON_BLOCK_EXIT( [&] { CFRelease( info ); } );

		auto vendorIDRef = static_cast<CFNumberRef>( CFDictionaryGetValue( info, CFSTR( kDisplayVendorID ) ) );
		auto productIDRef = static_cast<CFNumberRef>( CFDictionaryGetValue( info, CFSTR( kDisplayProductID ) ) );
		// Serial number no longer supported
		//auto serialNumberRef = static_cast<CFNumberRef>( CFDictionaryGetValue( info, CFSTR( kDisplaySerialNumber ) ) );

		success = CFNumberGetValue( vendorIDRef, kCFNumberCFIndexType, &vendorID );
		success &= CFNumberGetValue( productIDRef, kCFNumberCFIndexType, &productID );

		if( !success )
		{
			continue;
		}

		if( displayVendorID != (uint32_t)vendorID || displayModelID != (uint32_t)productID )
		{
			continue;
		}

		servicePort = serv;
		break;
	}

	IOObjectRelease( iter );
	return servicePort;
}

#endif // TARGET_OS_OSX

void RefreshDisplays()
{
	s_displays.clear();

	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if( !device )
	{
		return;
	}
	if( @available( macOS 10.15, iOS 13.0, * ) )
	{
		// Admission: any Mac- or Apple-family GPU. The old check asked for Mac1,
		// which every iPhone answers no to — family detection, not capability,
		// which is why this is a list rather than a threshold (spec D7). The
		// iOS simulator's software GPU ("Apple iOS simulator GPU") reports only
		// Apple1/Apple2, so the list reaches down to Apple1 or the suite would
		// find zero adapters and skip every device-dependent test.
		const MTLGPUFamily families[] = { MTLGPUFamilyMac2,   MTLGPUFamilyApple9, MTLGPUFamilyApple8,
										  MTLGPUFamilyApple7, MTLGPUFamilyApple6, MTLGPUFamilyApple5,
										  MTLGPUFamilyApple4, MTLGPUFamilyApple3, MTLGPUFamilyApple2,
										  MTLGPUFamilyApple1 };
		bool supported = false;
		for( MTLGPUFamily family : families )
		{
			if( [device supportsFamily:family] )
			{
				supported = true;
				break;
			}
		}
		if( !supported )
		{
			return;
		}
	}

	std::wstring deviceDescription = ToWString( [device name] );

#if TARGET_OS_OSX
	uint32_t vendorID = 0;
	uint32_t deviceID = 0;
	if( uint64_t regID = [device respondsToSelector:@selector( registryID )] ? device.registryID : 0 )
	{
		if( io_registry_entry_t entry =
				IOServiceGetMatchingService( kIOMasterPortDefault, IORegistryEntryIDMatching( regID ) ) )
		{
			io_registry_entry_t parent;
			if( IORegistryEntryGetParentEntry( entry, kIOServicePlane, &parent ) == kIOReturnSuccess )
			{
				vendorID = GetEntryProperty( parent, CFSTR( "vendor-id" ) );
				deviceID = GetEntryProperty( parent, CFSTR( "device-id" ) );
				IOObjectRelease( parent );
			}
			IOObjectRelease( entry );
		}
	}
#else
	// One GPU, Apple's, no PCI identity to look up. Zero is the honest value —
	// the same convention PR #6 chose for the android monitor handle.
	uint32_t vendorID = 0;
	uint32_t deviceID = 0;
#endif

#if TARGET_OS_OSX
	uint32_t displayCount;
	CGGetOnlineDisplayList( 0, NULL, &displayCount );
	std::unique_ptr<CGDirectDisplayID[]> displays( new CGDirectDisplayID[displayCount] );
	CGGetOnlineDisplayList( displayCount, displays.get(), &displayCount );
	for( uint32_t i = 0; i < displayCount; ++i )
	{
		if( !CGDisplayIsActive( displays[i] ) )
		{
			continue;
		}

		Display display = { displays[i] };

		if( auto port = IOServicePortFromCGDisplayID( display.displayID ) )
		{
			auto info = IODisplayCreateInfoDictionary( port, kIODisplayOnlyPreferredName );
			if( auto names =
					static_cast<CFDictionaryRef>( CFDictionaryGetValue( info, CFSTR( kDisplayProductName ) ) ) )
			{
				CFStringRef value;
				if( CFDictionaryGetValueIfPresent( names, CFSTR( "en_US" ), (const void**)&value ) )
				{
					auto size =
						CFStringGetMaximumSizeForEncoding( CFStringGetLength( value ), kCFStringEncodingWindowsLatin1 );
					std::unique_ptr<char[]> name( new char[size + 1] );
					CFStringGetCString( value, name.get(), size + 1, kCFStringEncodingWindowsLatin1 );
					display.name = name.get();
				}
			}
			CFRelease( info );
		}

		display.description = deviceDescription;
		display.vendorID = vendorID;
		display.deviceID = deviceID;

		GetDisplayModes( displays[i], display.modes );

		if( CGDisplayIsMain( displays[i] ) )
		{
			s_displays.insert( s_displays.begin(), display );
		}
		else
		{
			s_displays.push_back( display );
		}
	}
#else
	// iOS: no CoreGraphics/IOKit display enumeration exists to ask, and there is
	// exactly one screen/GPU. The screen's pixel extent is asked of UIScreen the
	// way the macOS branch asks CGDisplay, so the adapter reports a real display
	// mode; the swapchain's actual extent still comes from the CAMetalLayer at
	// present time (D6).
	uint32_t screenWidth = 0;
	uint32_t screenHeight = 0;
	GetIOSScreenExtent( screenWidth, screenHeight );

	Display display = {};
	display.displayID = 0; // no per-display id on iOS; GetAdapterMonitor already
							// turns this into a null monitor handle, matching PR #6's
							// android convention for the same field.
	display.description = deviceDescription; // the GPU name -- the same field every
											   // macOS display carries too.
	display.vendorID = vendorID; // 0, set above
	display.deviceID = deviceID; // 0, set above
	// display.name is left empty: iOS has no "display product name" API (that is
	// what the macOS-only IOKit lookup above provides), so there is nothing honest
	// to put there.

	Tr2DisplayModeInfo mode = {};
	mode.format = PIXEL_FORMAT_B8G8R8A8_UNORM;
	mode.width = screenWidth;  // the one real screen's extent, queried above --
	mode.height = screenHeight; // a reported mode of 0x0 would be a lie.
	mode.refreshRateNumerator = 1;
	mode.refreshRateDenominator = 1;
	mode.scaling = DISPLAY_SCALING_UNSPECIFIED;
	mode.scanlineOrdering = SCANLINE_ORDER_UNSPECIFIED;
	display.modes.push_back( mode );

	s_displays.push_back( display );
#endif
}
}

ALResult Tr2VideoAdapterInfo::GetAdapterCount( unsigned& count )
{
	if( s_displays.empty() )
	{
		RefreshDisplays();
	}
	count = unsigned( s_displays.size() );
	return S_OK;
}

#define CHECK_ADAPTER                                                                                                  \
	if( s_displays.empty() )                                                                                           \
	{                                                                                                                  \
		RefreshDisplays();                                                                                             \
	}                                                                                                                  \
	if( adapterIndex >= s_displays.size() )                                                                            \
	{                                                                                                                  \
		return E_INVALIDARG;                                                                                           \
	}

ALResult Tr2VideoAdapterInfo::GetAdapterInfo( unsigned adapterIndex, Tr2AdapterInfo& info )
{
	CHECK_ADAPTER;

	auto& display = s_displays[adapterIndex];
#if TARGET_OS_OSX
	id<MTLDevice> device = CGDirectDisplayCopyCurrentMetalDevice( display.displayID ); // unused below; kept to match existing macOS behavior
#endif

	info.driver = "";
	info.driverVersion = 0;
	info.deviceName = display.name;
	info.description = display.description;
	info.vendorID = display.vendorID;
	info.deviceID = display.deviceID;
	info.subSystemID = 0;
	info.revision = 0;
	AdapterGuid giud;
	giud.data1 = 0;
	giud.data2 = 0;
	giud.data3 = 0;
	for( int i = 0; i < 8; i++ )
	{
		giud.data4[i] = 0;
	}
	info.deviceIdentifier = giud;
	return S_OK;
}

ALResult Tr2VideoAdapterInfo::GetAdapterMonitor( unsigned adapterIndex, void*& monitor )
{
	CHECK_ADAPTER;

	monitor = (void*)uintptr_t( s_displays[adapterIndex].displayID );
	return S_OK;
}

ALResult Tr2VideoAdapterInfo::GetAdapterDisplayMode( unsigned adapterIndex, Tr2DisplayModeInfo& mode )
{
	CHECK_ADAPTER;

#if TARGET_OS_OSX
	size_t w = 0;
	size_t h = 0;

	// Find native display mode (if available).
	bool nativeModeFound = false;
	if( CFArrayRef modes = CGDisplayCopyAllDisplayModes( s_displays[adapterIndex].displayID, NULL ) )
	{
		for( CFIndex i = 0, n = CFArrayGetCount( modes ); i < n; ++i )
		{
			CGDisplayModeRef currentMode = (CGDisplayModeRef)CFArrayGetValueAtIndex( modes, i );
			uint32_t ioFlags = CGDisplayModeGetIOFlags( currentMode );

			if( ioFlags & kDisplayModeNativeFlag )
			{
				w = CGDisplayModeGetPixelWidth( currentMode );
				h = CGDisplayModeGetPixelHeight( currentMode );

				nativeModeFound = true;
				break;
			}
		}
		CFRelease( modes );
	}
	// If native display mode is not available - use current (default) mode.
	if( !nativeModeFound )
	{
		CGDisplayModeRef currentMode = CGDisplayCopyDisplayMode( s_displays[adapterIndex].displayID );
		if( !currentMode )
		{
			return E_FAIL;
		}

		w = CGDisplayModeGetPixelWidth( currentMode );
		h = CGDisplayModeGetPixelHeight( currentMode );

		CGDisplayModeRelease( currentMode );
	}

	mode.format = PIXEL_FORMAT_B8G8R8A8_UNORM;
	mode.width = (uint32_t)w;
	mode.height = (uint32_t)h;
	mode.refreshRateDenominator = 1;
	mode.refreshRateNumerator = 1;
	mode.scaling = DISPLAY_SCALING_UNSPECIFIED;
	mode.scanlineOrdering = SCANLINE_ORDER_UNSPECIFIED;
#else
	// No CoreGraphics display-mode API on iOS; UIScreen is the one screen's
	// descriptor. Queried live, like the macOS branch queries CGDisplay.
	uint32_t width = 0;
	uint32_t height = 0;
	GetIOSScreenExtent( width, height );
	mode.format = PIXEL_FORMAT_B8G8R8A8_UNORM;
	mode.width = width;
	mode.height = height;
	mode.refreshRateDenominator = 1;
	mode.refreshRateNumerator = 1;
	mode.scaling = DISPLAY_SCALING_UNSPECIFIED;
	mode.scanlineOrdering = SCANLINE_ORDER_UNSPECIFIED;
#endif

	return S_OK;
}

ALResult Tr2VideoAdapterInfo::GetAdapterModeCount( unsigned adapterIndex,
												   Tr2RenderContextEnum::PixelFormat,
												   unsigned& count )
{
	CHECK_ADAPTER;

	count = unsigned( s_displays[adapterIndex].modes.size() );
	return S_OK;
}

ALResult Tr2VideoAdapterInfo::GetAdapterMode( unsigned adapterIndex,
											  Tr2RenderContextEnum::PixelFormat,
											  unsigned modeIndex,
											  Tr2DisplayModeInfo& mode )
{
	CHECK_ADAPTER;

	auto& modes = s_displays[adapterIndex].modes;

	if( modeIndex >= modes.size() )
	{
		return E_INVALIDARG;
	}

	mode = modes[modeIndex];
	return S_OK;
}

ALResult Tr2VideoAdapterInfo::GetAdapterMaxTextureWidth( unsigned adapterIndex, unsigned& maxWidth )
{
	CHECK_ADAPTER;

	maxWidth = 16384;
	return S_OK;
}

bool Tr2VideoAdapterInfo::SupportsBackBufferFormat( unsigned adapterIndex,
													Tr2RenderContextEnum::PixelFormat backBufferFormat )
{
	CHECK_ADAPTER;

	return backBufferFormat == PIXEL_FORMAT_B8G8R8A8_UNORM;
}

bool Tr2VideoAdapterInfo::SupportsRenderTargetFormat( unsigned adapterIndex, Tr2RenderContextEnum::PixelFormat format )
{
	CHECK_ADAPTER;
	switch( format )
	{
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8_UNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8_SNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8_UINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8_SINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16_UNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16_SNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16_UINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16_SINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16_FLOAT:

	case Tr2RenderContextEnum::PIXEL_FORMAT_R8G8_UNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8G8_SNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8G8_UINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8G8_SINT:

	case Tr2RenderContextEnum::PIXEL_FORMAT_R32_UINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R32_SINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R32_FLOAT:

	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16_UNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16_SNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16_UINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16_SINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16_FLOAT:

	case Tr2RenderContextEnum::PIXEL_FORMAT_R8G8B8A8_UNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8G8B8A8_SNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8G8B8A8_UINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R8G8B8A8_SINT:

	case Tr2RenderContextEnum::PIXEL_FORMAT_B8G8R8A8_UNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB:

	case Tr2RenderContextEnum::PIXEL_FORMAT_R10G10B10A2_UNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R10G10B10A2_UINT:

	case Tr2RenderContextEnum::PIXEL_FORMAT_R11G11B10_FLOAT:

	case Tr2RenderContextEnum::PIXEL_FORMAT_R32G32_SINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R32G32_UINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R32G32_FLOAT:

	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16B16A16_SINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16B16A16_UINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16B16A16_FLOAT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16B16A16_UNORM:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R16G16B16A16_SNORM:

	case Tr2RenderContextEnum::PIXEL_FORMAT_R32G32B32A32_SINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R32G32B32A32_UINT:
	case Tr2RenderContextEnum::PIXEL_FORMAT_R32G32B32A32_FLOAT:
		return true;
	default:
		return false;
	}
	return true;
}

bool Tr2VideoAdapterInfo::AreAdaptersDifferent( unsigned adapter1, unsigned adapter2 )
{
	if( adapter1 == adapter2 )
	{
		return false;
	}
	if( adapter1 >= s_displays.size() || adapter2 >= s_displays.size() )
	{
		return true;
	}

#if TARGET_OS_OSX
	id<MTLDevice> device1 = CGDirectDisplayCopyCurrentMetalDevice( s_displays[adapter1].displayID );
	id<MTLDevice> device2 = CGDirectDisplayCopyCurrentMetalDevice( s_displays[adapter2].displayID );

	return device1 != device2;
#else
	// RefreshDisplays only ever records one display on iOS, so two distinct valid
	// indices can't happen in practice; if they somehow did, report "different"
	// rather than assume they're the same GPU.
	return true;
#endif
}

ALResult Tr2VideoAdapterInfo::RefreshData()
{
	RefreshDisplays();
	return S_OK;
}

#endif
