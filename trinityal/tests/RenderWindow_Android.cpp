// Copyright © 2026 CCP ehf.
#include "StdAfx.h"
#if defined( __ANDROID__ )
#include "RenderWindow.h"
#include "AndroidTestHost.h"
#include <android/native_window.h>

// The one window belongs to the activity; every RenderWindow wraps it. The
// requested size is advisory: the surface is the screen. Resize() maps to
// ANativeWindow_setBuffersGeometry, which changes the buffer extent the
// swapchain sees via currentExtent.

RenderWindow::RenderWindow( uint32_t width, uint32_t height )
{
	ANativeWindow* window = AndroidTestHost::WaitForWindow();
	ANativeWindow_acquire( window );
	m_handle = reinterpret_cast<Tr2WindowHandle>( window );
	Resize( width, height );
}

RenderWindow::~RenderWindow()
{
	ANativeWindow* window = reinterpret_cast<ANativeWindow*>( m_handle );
	if( !window )
	{
		return;
	}
	// The soak path replaces the window; do not touch a handle that is no longer
	// the live surface, and do not call into a window the framework has taken back.
	if( !AndroidTestHost::WindowLost() && window == AndroidTestHost::LiveWindow() )
	{
		ANativeWindow_setBuffersGeometry( window, 0, 0, 0 );
	}
	ANativeWindow_release( window );
}

void RenderWindow::AdoptWindow( ANativeWindow* window )
{
	ANativeWindow* old = reinterpret_cast<ANativeWindow*>( m_handle );
	if( old == window )
	{
		return;
	}
	if( old )
	{
		ANativeWindow_release( old );
	}
	if( window )
	{
		ANativeWindow_acquire( window );
	}
	m_handle = reinterpret_cast<Tr2WindowHandle>( window );
}

uint32_t RenderWindow::GetClientWidth() const
{
	return (uint32_t)ANativeWindow_getWidth( reinterpret_cast<ANativeWindow*>( m_handle ) );
}

uint32_t RenderWindow::GetClientHeight() const
{
	return (uint32_t)ANativeWindow_getHeight( reinterpret_cast<ANativeWindow*>( m_handle ) );
}

bool RenderWindow::Resize( uint32_t width, uint32_t height )
{
	return ANativeWindow_setBuffersGeometry(
		       reinterpret_cast<ANativeWindow*>( m_handle ), (int32_t)width, (int32_t)height, 0 ) == 0;
}

#endif
