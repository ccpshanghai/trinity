// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include "Tr2DriverUtilities.h"
#include "Tr2VideoAdapterInfoAL.h"

#include <cstdio>

namespace
{

struct DriverInfo
{
	Tr2VideoDriverInfo info;
	ALResult fetchResult;
};

TrackableStdUnorderedMap<uint32_t, DriverInfo> s_fetchedDriverInfo( "Tr2DriverUtilities::s_fetchedDriverInfo" );

#ifdef _WIN32

bool IsOptimus()
{
	static bool initialized = false;
	static bool isOptimus = false;
	if( !initialized )
	{
		initialized = true;
		isOptimus = GetModuleHandleW( L"nvd3d9wrap.dll" ) != nullptr;
	}
	return isOptimus;
}

bool GetHexIdFromDeviceId( const char* deviceId, uint32_t& deviceIdHex )
{
	const char* deviceIdPrefix = "DEV_";

	auto found = strstr( deviceId, deviceIdPrefix );
	if( !found )
	{
		return false;
	}
	return sscanf_s( found + strlen( deviceIdPrefix ), "%x", &deviceIdHex ) == 1;
}

const char* GetRegistryPathToLocalMachine( const char* registryPath )
{
	const char* rootPath = "\\Registry\\Machine\\";
	if( strncmp( registryPath, rootPath, strlen( rootPath ) ) == 0 )
	{
		return registryPath + strlen( rootPath );
	}
	else
	{
		return registryPath;
	}
}

bool GetDeviceRegistryKey( uint32_t deviceId, std::string& keyPath )
{
	DISPLAY_DEVICE dd;
	dd.cb = sizeof( DISPLAY_DEVICE );

	for( int i = 0; EnumDisplayDevices( nullptr, i, &dd, 0 ); ++i )
	{
		uint32_t device;
		if( GetHexIdFromDeviceId( dd.DeviceID, device ) && device == deviceId )
		{
			keyPath = GetRegistryPathToLocalMachine( dd.DeviceKey );
			return true;
		}
	}
	return false;
}

bool GetRegistryValue( HKEY key, const char* name, std::string& value )
{
	char buffer[256];
	DWORD dwcb_data = sizeof( buffer );

	LONG result = RegQueryValueEx( key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>( buffer ), &dwcb_data );
	if( result == ERROR_SUCCESS )
	{
		value = buffer;
		return true;
	}
	value = "";
	return false;
}

bool DriverVersionToInt64( const char* driverVersion, int64_t& intVersion )
{
	unsigned parts[4];
	if( sscanf_s( driverVersion, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3] ) == 4 )
	{
		intVersion = ( int64_t( parts[0] ) << 48 ) | ( int64_t( parts[1] ) << 32 ) | ( int64_t( parts[2] ) << 16 ) | int64_t( parts[3] );
		return true;
	}
	return false;
}

ALResult DoGetDriverVersion( uint32_t deviceId, Tr2VideoDriverInfo& info )
{
	std::string keyPath;
	if( !GetDeviceRegistryKey( deviceId, keyPath ) )
	{
		return E_FAIL;
	}

	HKEY key;
	LONG result = RegOpenKeyEx( HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_QUERY_VALUE, &key );
	if( result != ERROR_SUCCESS )
	{
		return E_FAIL;
	}
	ON_BLOCK_EXIT_WITH_UNUSED( [&] { RegCloseKey( key ); } );

	if( GetRegistryValue( key, "DriverVersion", info.driverVersionString ) )
	{
		DriverVersionToInt64( info.driverVersionString.c_str(), info.driverVersion );
	}
	GetRegistryValue( key, "DriverDate", info.driverDate );
	if( GetRegistryValue( key, "ProviderName", info.driverVendor ) )
	{
		info.isAmdDynamicSwitchable = info.driverVendor == "Advanced Micro Devices, Inc." || info.driverVendor == "ATI Technologies Inc.";
	}

	info.isOptimus = IsOptimus();
	info.isAmdDynamicSwitchable = false;

	return S_OK;
}

#elif ( TRINITY_PLATFORM == TRINITY_VULKAN )

// Tr2VideoAdapterInfo::GetAdapterInfo already threads VkPhysicalDeviceProperties::
// driverVersion up to Tr2AdapterInfo (Tr2VideoAdapterInfoALVulkan.cpp) -- that is
// the "right source" R7 asks for, and reaching it through the generic AL header is
// the existing seam every platform's adapter info goes through, not a new one.
// Getting from here to a VkPhysicalDevice directly would have meant inventing
// exactly that seam, which is the thing to avoid.
//
// driverDate and driverVendor have no Vulkan query to answer them (unlike the
// Windows registry, which hands both over for free), so they stay empty rather
// than inventing a value; isOptimus/isAmdDynamicSwitchable are Windows-only
// concepts and are simply false here.
ALResult DoGetDriverVersion( uint32_t deviceId, Tr2VideoDriverInfo& info )
{
	uint32_t count = 0;
	FORWARD_HR( Tr2VideoAdapterInfo::GetAdapterCount( count ) );

	for( uint32_t i = 0; i < count; ++i )
	{
		Tr2AdapterInfo adapter;
		if( FAILED( Tr2VideoAdapterInfo::GetAdapterInfo( i, adapter ) ) || adapter.deviceID != deviceId )
		{
			continue;
		}

		info.driverVersion = adapter.driverVersion;

		// The standard Vulkan encoding (VK_VERSION_MAJOR/MINOR/PATCH: a 10/10/12-bit
		// major.minor.patch packing) is what every target of this backend today
		// (Adreno) reports. NVIDIA packs driverVersion in its own four-part scheme
		// instead, which this would decode wrong -- not a concern for Android.
		const uint32_t raw = static_cast<uint32_t>( adapter.driverVersion );
		char buffer[32];
		snprintf( buffer, sizeof( buffer ), "%u.%u.%u", VK_VERSION_MAJOR( raw ), VK_VERSION_MINOR( raw ), VK_VERSION_PATCH( raw ) );
		info.driverVersionString = buffer;

		info.driverDate = "";
		info.driverVendor = "";
		info.isOptimus = false;
		info.isAmdDynamicSwitchable = false;
		return S_OK;
	}
	return E_FAIL;
}

#else

ALResult DoGetDriverVersion( uint32_t deviceId, Tr2VideoDriverInfo& info )
{
	return E_FAIL;
}

#endif

}

namespace Tr2DriverUtilities
{
#if ( TRINITY_PLATFORM == TRINITY_STUB )
ALResult GetDriverVersion( uint32_t deviceId, Tr2VideoDriverInfo& info )
{
	if( deviceId == 0xffffffff )
	{
		return E_FAIL;
	}
	info.driverDate = "01/01/01";
	info.driverVendor = "Stub";
	info.driverVersion = 31337;
	info.driverVersionString = "31337";
	info.isAmdDynamicSwitchable = false;
	info.isOptimus = false;
	return S_OK;
}
#else
ALResult GetDriverVersion( uint32_t deviceId, Tr2VideoDriverInfo& info )
{
	auto found = s_fetchedDriverInfo.insert( std::make_pair( deviceId, DriverInfo() ) );
	if( found.second )
	{
		found.first->second.fetchResult = DoGetDriverVersion( deviceId, found.first->second.info );
	}

	info = found.first->second.info;
	return found.first->second.fetchResult;
}
#endif
}
