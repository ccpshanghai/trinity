// Copyright © 2023 CCP ehf.

#pragma once

#ifndef TRINITY_AL_WITH_BLUE_EXPOSURE
#define TRINITY_AL_WITH_BLUE_EXPOSURE 1
#endif

#define TRINITY_DIRECTX11 2
#define TRINITY_STUB 5
#define TRINITY_DIRECTX12 6
#define TRINITY_METAL 10
#define TRINITY_VULKAN 20

#ifndef TRINITY_PLATFORM
#error TRINITY_PLATFORM must be set
#endif


#ifdef _MSC_VER
#pragma warning( push, 3 )
#endif

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <atlbase.h>

#endif

#include <cstdint>
#include <algorithm>
#include <set>

#include <CcpCore.h>
#include <PixelFormat.h>
#include <CubemapFace.h>
#include <TextureType.h>
#include <BitmapDimensions.h>

#ifdef _MSC_VER
#pragma warning( pop )
#endif

#if TRINITY_PLATFORM != TRINITY_DIRECTX11 && TRINITY_PLATFORM != TRINITY_DIRECTX12 && TRINITY_PLATFORM != TRINITY_VULKAN
#define TRINITY_PLATFORM_HAS_PRIMARY_CONTEXT 0
#define Tr2PrimaryRenderContextAL Tr2RenderContextAL
#else
#define TRINITY_PLATFORM_HAS_PRIMARY_CONTEXT 1
#endif

#if TRINITY_PLATFORM == TRINITY_DIRECTX11

#define TRINITY_PLATFORM_SYMBOL dx11
#define TRINITY_PLATFORM_SYMBOL_SUFFIX Dx11
#define TRINITY_PLATFORM_NAME "dx11"

#include <D3D11.h>
#include <DXGI.h>

#elif ( TRINITY_PLATFORM == TRINITY_STUB )

#define TRINITY_PLATFORM_SYMBOL stub
#define TRINITY_PLATFORM_SYMBOL_SUFFIX Stub
#define TRINITY_PLATFORM_NAME "dx11" // In order to use the dx11 platform specific res files as our own

#elif TRINITY_PLATFORM == TRINITY_DIRECTX12

#define TRINITY_PLATFORM_SYMBOL dx12
#define TRINITY_PLATFORM_SYMBOL_SUFFIX Dx12
#define TRINITY_PLATFORM_NAME "dx12"

#include <d3d12.h>
#include <dxgi1_5.h>

#elif TRINITY_PLATFORM == TRINITY_VULKAN

#define TRINITY_PLATFORM_SYMBOL vulkan
#define TRINITY_PLATFORM_SYMBOL_SUFFIX Vulkan
#define TRINITY_PLATFORM_NAME "vulkan"

#pragma warning( push )
#pragma warning( disable: 4005 )
#include <vulkan/vulkan.h>
#pragma warning( pop )

#elif TRINITY_PLATFORM == TRINITY_METAL

#define TRINITY_PLATFORM_SYMBOL metal
#define TRINITY_PLATFORM_SYMBOL_SUFFIX Metal
#define TRINITY_PLATFORM_NAME "metal"

#else

#error Missing TrinityAL platform description

#endif

// The directory the compiled effects live in, as a suffix: res:/graphics/effect.<this>/.
//
// Separate from TRINITY_PLATFORM_NAME because they answer different questions. That one is the
// renderer's IDENTITY -- Tr2PlatformInfo returns it, TriStepRenderFps prints it on the HUD, and
// "metal" is the right answer to "which renderer is this". This one is a PATH, and on Apple one
// renderer needs three of them: a metallib is compiled per SDK and the three are mutually
// unloadable, so macosx, iphoneos and iphonesimulator cannot share a directory.
//
// M3 spec trap 7 is exactly this ("one mtl identity for three mutually-unloadable AIR targets"),
// and it left open whether the answer was a new platform id or a path dimension. The staged tree
// took the path dimension -- tools/m1-stage-render-closure.py produces effect.metal-iphonesimulator
// and effect.metal-macosx -- and this is the half that tells the engine. Overloading
// TRINITY_PLATFORM_NAME instead would have made "which renderer is this" answer
// "metal-iphonesimulator", which is not a renderer.
//
// Everything except Metal is one target per renderer, so the two names coincide there.
#if TRINITY_PLATFORM == TRINITY_METAL
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#define TRINITY_EFFECT_PLATFORM_NAME "metal-macosx"
#elif TARGET_OS_SIMULATOR
// Simulator before device: TARGET_OS_IPHONE is 1 for both, TARGET_OS_SIMULATOR only for the
// simulator, so testing it first is what keeps them apart.
#define TRINITY_EFFECT_PLATFORM_NAME "metal-iphonesimulator"
#elif TARGET_OS_IPHONE
#define TRINITY_EFFECT_PLATFORM_NAME "metal-iphoneos"
#else
#error Metal on an Apple platform this effect-path mapping does not know
#endif
#else
#define TRINITY_EFFECT_PLATFORM_NAME TRINITY_PLATFORM_NAME
#endif

// clang-format off
#define TRINITY_AL_PLATFORM_INCLUDE( className ) CCP_STRINGIZE(../TRINITY_PLATFORM_SYMBOL/CCP_CONCATENATE( className, TRINITY_PLATFORM_SYMBOL_SUFFIX ).h )
// clang-format on