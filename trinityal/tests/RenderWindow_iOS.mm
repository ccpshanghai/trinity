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

#include "RenderWindow.h"
#import <QuartzCore/CAMetalLayer.h>

// No UIWindow, no UIKit: nextDrawable works on a standalone CAMetalLayer with a
// drawableSize, which is what lets the suite run as a console binary under
// `simctl spawn` (M3 spec §8.2). Anything that turns out to genuinely need a
// window is skipped-and-named on the simulator and runs on the device instead.

bool s_keyPressed = false;   // referenced by the fixture; nothing sets it here

RenderWindow::RenderWindow( uint32_t width, uint32_t height )
{
	CAMetalLayer* layer = [CAMetalLayer layer];
	layer.frame = CGRectMake( 0, 0, width, height );
	layer.drawableSize = CGSizeMake( width, height );
	m_handle = layer;
}

RenderWindow::~RenderWindow()
{
	m_handle = nullptr;
}

uint32_t RenderWindow::GetClientWidth() const
{
	return uint32_t( ( (CAMetalLayer*)m_handle ).drawableSize.width );
}

uint32_t RenderWindow::GetClientHeight() const
{
	return uint32_t( ( (CAMetalLayer*)m_handle ).drawableSize.height );
}

bool RenderWindow::Resize( uint32_t width, uint32_t height )
{
	( (CAMetalLayer*)m_handle ).frame = CGRectMake( 0, 0, width, height );
	( (CAMetalLayer*)m_handle ).drawableSize = CGSizeMake( width, height );
	return true;
}

Tr2WindowHandle RenderWindow::GetHandle() const
{
	// Already the CAMetalLayer — the same contract RenderWindow_Macos now
	// serves by unwrapping its content view (D6: the AL never sees a view).
	return m_handle;
}

#endif
