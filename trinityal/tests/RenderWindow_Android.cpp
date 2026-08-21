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
	m_handle = reinterpret_cast<Tr2WindowHandle>( window );
	Resize( width, height );
}

RenderWindow::~RenderWindow()
{
	// Restore the native extent so the next test starts from screen size.
	ANativeWindow_setBuffersGeometry( reinterpret_cast<ANativeWindow*>( m_handle ), 0, 0, 0 );
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
