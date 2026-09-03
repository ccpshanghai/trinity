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
// True once the current window has been destroyed (onNativeWindowDestroyed),
// in any run mode -- not soak-only. R11 added gtest/smoke fixtures as readers:
// WithWindow::SetUpTestCase and WithValidRenderContext::SetUpTestCase check
// this before ever touching the window, because nothing outside a live soak
// cycle will recreate it, and WaitForWindow() would otherwise block forever.
bool WindowLost();
// The render side has released every object referencing the lost window;
// unblocks the waiting onNativeWindowDestroyed callback.
void AckWindowReleased();
// Marks the deliberate end-of-run teardown: RUN_ALL_TESTS has returned, every
// fixture (and therefore every Vulkan object that referenced the window) is
// already gone, and ANativeActivity_finish is about to be called. Once set,
// onNativeWindowDestroyed returns promptly instead of waiting out (and
// potentially timing out) a release handshake nothing will ever answer.
void MarkFinishing();
// Intent extras, parsed in onCreate.
int SoakCycles();          // -e cycles N, default 20
const char* GtestFilter(); // -e gtest_filter ..., may be empty
}

#endif
