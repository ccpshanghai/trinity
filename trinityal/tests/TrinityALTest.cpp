// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include "WithWindowFixture.h"
#include "WithRenderContextFixture.h"
#include "WithValidRenderContextFixture.h"
#include <Tr2DriverUtilities.h>

// Needed by CcpCore
const char* g_moduleName = "TrinityALTest";

// Interactive test flag (set by --interactive option)
bool g_interactive = false;
// Make screenshots for the first frame of interactive tests (set by --screenshots option)
bool g_makeScreenShots = false;
// Compare with existing screenshots
bool g_compareScreenShots = false;
// Folder to save screenshots (set by --screenshotdir option)
std::string g_screenshotFolder = "screenshots/" TRINITY_PLATFORM_NAME;

void PrintAdapterInfo( unsigned index )
{
	Tr2AdapterInfo info;
	if( FAILED( Tr2VideoAdapterInfo::GetAdapterInfo( index, info ) ) )
	{
		fprintf( stderr, "Failed to get video adapter information for adapter %u\n", index );
		return;
	}

	printf(
		"Device name: %s\nDescription: %ls\nVendor ID: %u\nDevice ID: %u\n",
		info.deviceName.c_str(),
		info.description.c_str(),
		info.vendorID,
		info.deviceID );

	Tr2VideoDriverInfo driverInfo;
	if( FAILED( Tr2DriverUtilities::GetDriverVersion( info.deviceID, driverInfo ) ) )
	{
		fprintf( stderr, "Failed to get video driver information for adapter %u\n", index );
		return;
	}
	printf(
		"Driver version: %s\nDriver date: %s\nDriver vendor: %s\nIs Optimus: %s\nIs AMD Dynamic Switchable: %s\n\n",
		driverInfo.driverVersionString.c_str(),
		driverInfo.driverDate.c_str(),
		driverInfo.driverVendor.c_str(),
		driverInfo.isOptimus ? "yes" : "no",
		driverInfo.isAmdDynamicSwitchable ? "yes" : "no" );
}

void PrintAllAdapterInfo()
{
	unsigned count = 0;
	if( FAILED( Tr2VideoAdapterInfo::GetAdapterCount( count ) ) )
	{
		fprintf( stderr, "Failed to get video adapter count\n" );
		return;
	}
	for( unsigned i = 0; i < count; ++i )
	{
		PrintAdapterInfo( i );
	}
}

int main( int argc, char** argv )
{
	CCP::SetLogMainThreadId();

	for( int i = 1; i < argc; ++i )
	{
		if( strcmp( argv[i], "--interactive" ) == 0 )
		{
			g_interactive = true;
		}
		else if( strcmp( argv[i], "--screenshots" ) == 0 )
		{
			g_makeScreenShots = true;
		}
		else if( strcmp( argv[i], "--compare" ) == 0 )
		{
			g_compareScreenShots = true;
		}
		else if( strcmp( argv[i], "--screenshotdir" ) == 0 )
		{
			if( i + 1 >= argc )
			{
				printf( "Error parsing arguments: --screenshotdir should be followed by directory path" );
				return 1;
			}
			g_screenshotFolder = argv[++i];
			g_screenshotFolder += "/" TRINITY_PLATFORM_NAME;
		}
		else if( strcmp( argv[i], "--adapterinfo" ) == 0 )
		{
			if( i + 1 >= argc )
			{
				printf( "Error parsing arguments: --adapterinfo should be followed by adapter index or \"all\"" );
				return 1;
			}
			if( strcmp( argv[i + 1], "all" ) == 0 )
			{
				PrintAllAdapterInfo();
			}
			else
			{
				PrintAdapterInfo( atoi( argv[i + 1] ) );
			}
		}
	}
	::testing::InitGoogleTest( &argc, argv );
	int result = RUN_ALL_TESTS();
	return result;
}

#if defined( __ANDROID__ )

#include "AndroidTestHost.h"
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/log.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern bool g_requestDeviceDebugLayer;
// extern const char* g_pipelineCacheDirectory; // Task 9 wires this

ANativeActivity* g_androidActivity = nullptr;
ANativeWindow* g_androidWindow = nullptr; // read by RenderWindow_Android

namespace
{
std::mutex s_windowMutex;
std::condition_variable s_windowCv;
ANativeWindow* s_window = nullptr;   // guarded by s_windowMutex
bool s_windowLost = false;           // guarded by s_windowMutex
bool s_windowReleased = true;        // guarded by s_windowMutex
std::string s_mode = "gtest";
std::string s_gtestFilter;
int s_soakCycles = 20;
bool s_testsStarted = false;

// printf/gtest output would otherwise vanish; pump it to logcat.
int s_stdioPipe[2];
void* StdioPumpThread( void* )
{
	char buf[1024];
	ssize_t n;
	while( ( n = read( s_stdioPipe[0], buf, sizeof( buf ) - 1 ) ) > 0 )
	{
		if( buf[n - 1] == '\n' ) --n;
		buf[n] = '\0';
		__android_log_write( ANDROID_LOG_INFO, "TrinityALTest", buf );
	}
	return nullptr;
}

void StartStdioToLogcat()
{
	setvbuf( stdout, nullptr, _IOLBF, 0 );
	setvbuf( stderr, nullptr, _IONBF, 0 );
	pipe( s_stdioPipe );
	dup2( s_stdioPipe[1], STDOUT_FILENO );
	dup2( s_stdioPipe[1], STDERR_FILENO );
	pthread_t t;
	pthread_create( &t, nullptr, StdioPumpThread, nullptr );
	pthread_detach( t );
}

// onCreate runs on the main thread, where activity->env is valid.
std::string GetIntentStringExtra( ANativeActivity* activity, const char* name )
{
	JNIEnv* env = activity->env;
	jobject me = activity->clazz;
	jclass acl = env->GetObjectClass( me );
	jmethodID giid = env->GetMethodID( acl, "getIntent", "()Landroid/content/Intent;" );
	jobject intent = env->CallObjectMethod( me, giid );
	jclass icl = env->GetObjectClass( intent );
	jmethodID gse = env->GetMethodID( icl, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;" );
	jstring jname = env->NewStringUTF( name );
	jstring jvalue = (jstring)env->CallObjectMethod( intent, gse, jname );
	std::string result;
	if( jvalue )
	{
		const char* c = env->GetStringUTFChars( jvalue, nullptr );
		result = c;
		env->ReleaseStringUTFChars( jvalue, c );
	}
	return result;
}
}

namespace AndroidTestHost
{
ANativeWindow* WaitForWindow()
{
	std::unique_lock<std::mutex> lock( s_windowMutex );
	s_windowCv.wait( lock, [] { return s_window != nullptr; } );
	g_androidWindow = s_window;
	return s_window;
}
bool WindowLost()
{
	std::lock_guard<std::mutex> lock( s_windowMutex );
	return s_windowLost;
}
void AckWindowReleased()
{
	std::lock_guard<std::mutex> lock( s_windowMutex );
	s_windowReleased = true;
	s_windowCv.notify_all();
}
int SoakCycles() { return s_soakCycles; }
const char* GtestFilter() { return s_gtestFilter.c_str(); }
}

static uint32_t mainThread( void* )
{
	AndroidTestHost::WaitForWindow();

	std::vector<char*> args;
	args.push_back( strdup( "TrinityALTest" ) );
	std::string filter = "--gtest_filter=";
	if( s_mode == "smoke" )
		filter += "AndroidBringup.Smoke";
	else if( s_mode == "soak" )
		filter += "AndroidBringup.LifecycleSoak";
	else
		filter += s_gtestFilter.empty() ? "-AndroidBringup.LifecycleSoak" : s_gtestFilter;
	args.push_back( strdup( filter.c_str() ) );
	std::string xml = std::string( "--gtest_output=xml:" ) + g_androidActivity->internalDataPath + "/gtest.xml";
	args.push_back( strdup( xml.c_str() ) );

	int result = main( (int)args.size(), args.data() );
	__android_log_print( ANDROID_LOG_INFO, "TrinityALTest", "TESTS_COMPLETE exit=%d", result );
	ANativeActivity_finish( g_androidActivity );
	return 0;
}

static void onNativeWindowCreated( ANativeActivity*, ANativeWindow* window )
{
	{
		std::lock_guard<std::mutex> lock( s_windowMutex );
		s_window = window;
		s_windowLost = false;
		s_windowReleased = false;
	}
	s_windowCv.notify_all();
	if( !s_testsStarted )
	{
		s_testsStarted = true;
		CcpCreateThread( &mainThread, nullptr, CCP_THREAD_PRIORITY_NORMAL );
	}
}

static void onNativeWindowDestroyed( ANativeActivity*, ANativeWindow* )
{
	// After this returns the window is gone; block until the render side lets go.
	std::unique_lock<std::mutex> lock( s_windowMutex );
	s_window = nullptr;
	s_windowLost = true;
	s_windowCv.wait_for( lock, std::chrono::seconds( 10 ), [] { return s_windowReleased; } );
	if( !s_windowReleased )
	{
		__android_log_write( ANDROID_LOG_ERROR, "TrinityALTest", "window release handshake timed out" );
	}
}

extern "C" __attribute__( ( visibility( "default" ) ) ) void ANativeActivity_onCreate( ANativeActivity* activity, void*, size_t )
{
	g_androidActivity = activity;
	StartStdioToLogcat();

	std::string mode = GetIntentStringExtra( activity, "mode" );
	if( !mode.empty() ) s_mode = mode;
	s_gtestFilter = GetIntentStringExtra( activity, "gtest_filter" );
	std::string cycles = GetIntentStringExtra( activity, "cycles" );
	if( !cycles.empty() ) s_soakCycles = atoi( cycles.c_str() );
	if( GetIntentStringExtra( activity, "validation" ) == "1" ) g_requestDeviceDebugLayer = true;
	// g_pipelineCacheDirectory = strdup( activity->internalDataPath ); // Task 9 wires this

	activity->callbacks->onNativeWindowCreated = onNativeWindowCreated;
	activity->callbacks->onNativeWindowDestroyed = onNativeWindowDestroyed;
	__android_log_print( ANDROID_LOG_INFO, "TrinityALTest", "onCreate mode=%s cycles=%d", s_mode.c_str(), s_soakCycles );
}

#endif