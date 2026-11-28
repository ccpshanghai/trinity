// Copyright © 2026 CCP ehf.
#include "StdAfx.h"
#include "WithWindowFixture.h"
#include "RenderWindow.h"
#include "AndroidTestHost.h"

#if defined( __ANDROID__ )

namespace
{
RenderWindow* s_wnd = nullptr;
}

void WithWindow::SetUpTestCase()
{
	// R11: once the window is lost, nothing in gtest mode will ever recreate
	// it, so RenderWindow's constructor -- AndroidTestHost::WaitForWindow(),
	// which never returns null -- would block forever instead of ever handing
	// back to the caller. Skip before constructing it at all, for every
	// fixture built on WithWindow (with or without a device on top): a hung
	// process loses the run's evidence exactly as thoroughly as a crash does.
	if( AndroidTestHost::WindowLost() )
	{
		GTEST_SKIP() << "Window lost before window creation; skipping suite.";
	}

	CCP_DELETE s_wnd;
	s_wnd = CCP_NEW( "WithWindowFixture/s_wnd" ) RenderWindow( 640, 480 );
}

void WithWindow::TearDownTestCase()
{
	CCP_DELETE s_wnd;
	s_wnd = nullptr;
}

void WithWindow::BeginLoopProcessing()
{
}

bool WithWindow::DoLoopProcessing()
{
	return true; // no message pump; lifecycle events arrive on the activity thread
}

Tr2WindowHandle WithWindow::GetWindowHandle()
{
	return *s_wnd;
}

RenderWindow* WithWindow::GetWindow()
{
	return s_wnd;
}

#endif
