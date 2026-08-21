// Copyright © 2026 CCP ehf.
#include "StdAfx.h"
#if defined( __ANDROID__ )
#include "WithValidRenderContextFixture.h"
#include "RenderWindow.h"
#include "AndroidTestHost.h"
#include <android/native_window.h>

struct AndroidBringup : public WithValidRenderContext
{
	// Drops swapchain + VkSurfaceKHR so onNativeWindowDestroyed can return.
	// Must run before AckWindowReleased: every Vulkan object that referenced
	// the old ANativeWindow has to be gone first.
	static ALResult ReleasePresentation();
	// Rebinds the primary context's presentation to `window` after surface loss
	// via SetPresentParameters (outputWindow carries the new ANativeWindow*).
	static ALResult RecreateOnWindow( ANativeWindow* window );
};

ALResult AndroidBringup::ReleasePresentation()
{
	presentParameters.outputWindow = 0;
	return renderContext->SetPresentParameters( 0, presentParameters );
}

ALResult AndroidBringup::RecreateOnWindow( ANativeWindow* window )
{
	presentParameters.outputWindow = reinterpret_cast<Tr2WindowHandle>( window );
	presentParameters.mode.width = (uint32_t)ANativeWindow_getWidth( window );
	presentParameters.mode.height = (uint32_t)ANativeWindow_getHeight( window );
	return renderContext->SetPresentParameters( 0, presentParameters );
}

TEST_F( AndroidBringup, Smoke )
{
	ENSURE_GPU_OR_SKIP

	unsigned count = 0;
	ASSERT_HRESULT_SUCCEEDED( Tr2VideoAdapterInfo::GetAdapterCount( count ) );
	ASSERT_GT( count, 0u );
	extern void PrintAllAdapterInfo();
	PrintAllAdapterInfo(); // reaches logcat via the stdio pump

	ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->Clear( Tr2RenderContextEnum::CLEARFLAGS_TARGET, 0xff00ff00, 1.0f ) );
	ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );
	ASSERT_HRESULT_SUCCEEDED( renderContext->Present() );
}

TEST_F( AndroidBringup, LifecycleSoak )
{
	ENSURE_GPU_OR_SKIP

	const int targetCycles = AndroidTestHost::SoakCycles();
	int completed = 0;
	int64_t framesSinceEvent = 0;
	uint32_t g = 0;

	while( completed < targetCycles )
	{
		// The driver script delivers background/foreground cycles; if it dies,
		// fail rather than hang. ~10 min of frames between events at 60 Hz.
		ASSERT_LT( framesSinceEvent++, 36000 ) << "soak driver stopped delivering lifecycle events at cycle " << completed;

		if( AndroidTestHost::WindowLost() )
		{
			ASSERT_HRESULT_SUCCEEDED( ReleasePresentation() );
			AndroidTestHost::AckWindowReleased();
			ANativeWindow* window = AndroidTestHost::WaitForWindow();
			ASSERT_HRESULT_SUCCEEDED( RecreateOnWindow( window ) );
			++completed;
			framesSinceEvent = 0;
			printf( "soak cycle %d/%d complete\n", completed, targetCycles );
			continue;
		}

		ASSERT_HRESULT_SUCCEEDED( renderContext->BeginScene() );
		ASSERT_HRESULT_SUCCEEDED( renderContext->Clear( Tr2RenderContextEnum::CLEARFLAGS_TARGET, 0xff000000 | ( ( g++ & 0xff ) << 8 ), 1.0f ) );
		ASSERT_HRESULT_SUCCEEDED( renderContext->EndScene() );
		ASSERT_HRESULT_SUCCEEDED( renderContext->Present() );
	}
}

#endif
