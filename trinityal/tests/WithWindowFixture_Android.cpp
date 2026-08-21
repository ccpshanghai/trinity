// Copyright © 2026 CCP ehf.
#include "StdAfx.h"
#include "WithWindowFixture.h"
#include "RenderWindow.h"

#if defined( __ANDROID__ )

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
