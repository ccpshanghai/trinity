// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

// TARGET_OS_* is needed by the guard below, so the include cannot go inside it -- but it
// is an Apple header and Windows compiles this file too, so it is wrapped rather than
// left bare. Off Apple the undefined TARGET_OS_* evaluates to 0 and __APPLE__ already
// makes the guard false.
#if defined( __APPLE__ )
#include <TargetConditionals.h>
#endif

#if defined( __APPLE__ ) && TARGET_OS_IPHONE && ( TRINITY_PLATFORM != TRINITY_STUB )

#include "WithWindowFixture.h"
#include "RenderWindow.h"


extern bool s_keyPressed;

namespace
{
RenderWindow* s_wnd = nullptr;
}

void WithWindow::SetUpTestCase()
{
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
	s_keyPressed = false;
}

bool WithWindow::DoLoopProcessing()
{
	// No event pump: interactive mode has no meaning under simctl spawn.
	return !s_keyPressed;
}

Tr2WindowHandle WithWindow::GetWindowHandle()
{
	return s_wnd ? s_wnd->GetHandle() : Tr2WindowHandle( 0 );
}

RenderWindow* WithWindow::GetWindow()
{
	return s_wnd;
}

#endif
