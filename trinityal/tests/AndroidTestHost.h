// Copyright © 2026 CCP ehf.
// The handshake between the NativeActivity callbacks (main thread) and the
// test runner thread. Implemented in TrinityALTest.cpp's __ANDROID__ block.
#pragma once
#ifndef AndroidTestHost_H
#define AndroidTestHost_H

struct ANativeWindow;

namespace AndroidTestHost
{
// Blocks until a live ANativeWindow exists, returns it. Never returns null.
ANativeWindow* WaitForWindow();
// Soak recreate wait. Returns null if no window arrives within timeoutSeconds.
ANativeWindow* WaitForWindow( int timeoutSeconds );
// The window currently stored by the host, or null. Not a wait.
ANativeWindow* LiveWindow();
// True when the framework has asked for the current window back (soak mode).
bool WindowLost();
// The render side has released every object referencing the lost window;
// unblocks the waiting onNativeWindowDestroyed callback.
void AckWindowReleased();
// Intent extras, parsed in onCreate.
int SoakCycles();          // -e cycles N, default 20
const char* GtestFilter(); // -e gtest_filter ..., may be empty
}

#endif
