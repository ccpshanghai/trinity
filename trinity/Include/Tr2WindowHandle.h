// Copyright © 2013 CCP ehf.

#pragma once
#ifndef Tr2WindowHandle_h
#define Tr2WindowHandle_h

#ifdef _WIN32

typedef HWND Tr2WindowHandle;

#elif defined( __APPLE__ )

// objc/objc.h (not objc-runtime.h) is the one header this typedef needs and the
// one that ships in every Apple platform SDK -- objc-runtime.h is macOS-SDK-only
// and isn't present under iPhoneOS/iPhoneSimulator.
#include <objc/objc.h>
typedef id Tr2WindowHandle;

#else

typedef uintptr_t Tr2WindowHandle;

#endif


#endif // Tr2WindowHandle_h
