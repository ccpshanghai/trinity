// Copyright © 2026 CCP ehf.
#include "StdAfx.h"
#if defined( __ANDROID__ )
#include "RenderWindow.h"
#include "AndroidTestHost.h"
#include <android/native_window.h>

// The one window belongs to the activity, which outlives every test; RenderWindow
// is a non-owning view onto it, never an owner. (R9 -- see the M6 gtest teardown
// crash this replaced: giving every test-local RenderWindow instance its own
// acquire()/release() on the *shared* activity window meant each one's
// SetBuffersGeometry/release calls reshaped and then reset the live display
// surface underneath the primary render context's swapchain -- a screenshot
// taken mid-soak showed exactly that, a black band where a 640x480 buffer
// geometry had been stretched onto the real display. The window extent on
// Android is display-dictated (GetSwapChainExtent always takes currentExtent),
// so a test asking for a specific size can't be honoured, and pretending to
// honour it is what corrupted the surface.) No acquire, no release, no
// buffer-geometry mutation anywhere in this file.

RenderWindow::RenderWindow( uint32_t /*width*/, uint32_t /*height*/ )
{
	ANativeWindow* window = AndroidTestHost::WaitForWindow();
	m_handle = reinterpret_cast<Tr2WindowHandle>( window );
}

RenderWindow::~RenderWindow()
{
	// Nothing to release: the handle was never ours to own.
}

void RenderWindow::AdoptWindow( ANativeWindow* window )
{
	// A non-owning swap -- just point at the new handle.
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

bool RenderWindow::Resize( uint32_t /*width*/, uint32_t /*height*/ )
{
	// No-op (R9): the surface is the real display, sized by the framework, not
	// by the app. See SwapChainResizing in the M6 inventory -- that suite passes
	// on Android without exercising a resize, because an app can't resize its
	// own window on this platform.
	return true;
}

#endif
