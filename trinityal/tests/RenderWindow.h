// Copyright © 2023 CCP ehf.

#pragma once
#ifndef RenderWindow_H
#define RenderWindow_H

#if defined( __ANDROID__ )
struct ANativeWindow;
#endif

class RenderWindow
{
public:
	RenderWindow( uint32_t width = 128, uint32_t height = 64 );
	~RenderWindow();

#if !defined( __APPLE__ )
	Tr2WindowHandle GetHandle() const
	{
		return m_handle;
	}
#else
	Tr2WindowHandle GetHandle() const;
#endif

	operator Tr2WindowHandle() const
	{
		return GetHandle();
	}

	uint32_t GetClientWidth() const;
	uint32_t GetClientHeight() const;

	bool Resize( uint32_t width, uint32_t height );

#if defined( __ANDROID__ )
	// Soak hands a new ANativeWindow after surface loss. Releases the previous
	// acquire and takes one on `window`.
	void AdoptWindow( ANativeWindow* window );
#endif

private:
	Tr2WindowHandle m_handle;
};

#endif
