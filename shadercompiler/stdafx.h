// Copyright © 2023 CCP ehf.

// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#ifndef _WIN32_WINNT // Allow use of features specific to Windows XP or later.
#define _WIN32_WINNT 0x0501 // Change this to the appropriate value to target other versions of Windows.
#endif

#if _WIN32

#include <tchar.h>

#define NOMINMAX
#include <windows.h>
#include <atlbase.h>

#include <D3Dcompiler.h>
#include <d3d11.h>
#include <dxcapi.h>

#include <io.h>
#include <stdio.h>

#else

#include <cstdint>
#include <unistd.h>

// dxc's own portable Windows shims, plus the one D3D reflection header its
// DirectX-Headers submodule carries. Until 2026-09-08 this branch hand-rolled the
// handful of Windows types the Metal path needed and no more; the Vulkan path needs
// COM as well -- CComPtr, IUnknown, IID_PPV_ARGS, ID3D12ShaderReflection -- and
// hand-rolling those is not on offer. Everything below that WinAdapter.h or
// d3dcommon.h already defines has therefore been deleted from this file rather than
// guarded: two definitions of BOOL (int here, bool there) or of struct _FILETIME are
// a hard error, not a warning.
//
// Order matters. WinAdapter.h must precede d3d12shader.h because it defines
// COM_NO_WINDOWS_H, which is what stops d3dcommon.h reaching for <windows.h> and
// <ole2.h>. The pragma and the `interface` define around the include are dxc's own
// recipe, from its include/dxc/Support/D3DReflection.h; it is copied rather than
// included because the vcpkg port installs these headers flat into
// include/directx-dxc/ rather than under a dxc/ prefix.
#include <WinAdapter.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#define interface struct
#include <d3d12shader.h>
#undef interface
#pragma GCC diagnostic pop

// WinAdapter.h also maps four CRT "secure" names onto POSIX functions of a DIFFERENT
// ARITY (sprintf_s -> snprintf, strcpy_s( d, n, s ) -> strncpy( d, s, n ), ...) and
// maps _strnicmp onto strnicmp, which macOS does not have at all. This file has
// defined the _stricmp/_strnicmp pair as strcasecmp/strncasecmp since the Metal port
// and InlineString.h and EffectCompilerMetal.cpp call them on this path, so: take the
// types and the COM support from WinAdapter, and keep our own string shims. Nothing
// in dxc's headers uses the four undefined below after their definition.
#undef _strnicmp
#undef sprintf_s
#undef strcpy_s
#undef strcat_s
#undef vsnprintf_s

#define _stricmp strcasecmp
#define _strnicmp strncasecmp

#include <dxcapi.h>

#define MAX_PATH 260

// Not in WinAdapter.h, and used by FXAnalyzer's render-state table.
#define CONST const
typedef float FLOAT;

#define D3D11_FILTER_REDUCTION_TYPE_MASK ( 0x3 )
#define D3D11_FILTER_REDUCTION_TYPE_SHIFT ( 7 )
#define D3D11_FILTER_TYPE_MASK ( 0x3 )
#define D3D11_MIN_FILTER_SHIFT ( 4 )
#define D3D11_MAG_FILTER_SHIFT ( 2 )
#define D3D11_MIP_FILTER_SHIFT ( 0 )
#define D3D11_ANISOTROPIC_FILTERING_BIT ( 0x40 )

#define D3D11_DECODE_MIN_FILTER( d3d11Filter ) \
	( (D3D11_FILTER_TYPE)( ( ( d3d11Filter ) >> D3D11_MIN_FILTER_SHIFT ) & D3D11_FILTER_TYPE_MASK ) )
#define D3D11_DECODE_MAG_FILTER( d3d11Filter ) \
	( (D3D11_FILTER_TYPE)( ( ( d3d11Filter ) >> D3D11_MAG_FILTER_SHIFT ) & D3D11_FILTER_TYPE_MASK ) )
#define D3D11_DECODE_MIP_FILTER( d3d11Filter ) \
	( (D3D11_FILTER_TYPE)( ( ( d3d11Filter ) >> D3D11_MIP_FILTER_SHIFT ) & D3D11_FILTER_TYPE_MASK ) )
#define D3D11_DECODE_FILTER_REDUCTION( d3d11Filter ) \
	( (D3D11_FILTER_REDUCTION_TYPE)( ( ( d3d11Filter ) >> D3D11_FILTER_REDUCTION_TYPE_SHIFT ) & D3D11_FILTER_REDUCTION_TYPE_MASK ) )
#define D3D11_DECODE_IS_COMPARISON_FILTER( d3d11Filter ) \
	( D3D11_DECODE_FILTER_REDUCTION( d3d11Filter ) == D3D11_FILTER_REDUCTION_TYPE_COMPARISON )
#define D3D11_DECODE_IS_ANISOTROPIC_FILTER( d3d11Filter )                       \
	( ( ( d3d11Filter ) & D3D11_ANISOTROPIC_FILTERING_BIT ) &&                  \
	  ( D3D11_FILTER_TYPE_LINEAR == D3D11_DECODE_MIN_FILTER( d3d11Filter ) ) && \
	  ( D3D11_FILTER_TYPE_LINEAR == D3D11_DECODE_MAG_FILTER( d3d11Filter ) ) && \
	  ( D3D11_FILTER_TYPE_LINEAR == D3D11_DECODE_MIP_FILTER( d3d11Filter ) ) )

enum D3D11_FILTER
{
	D3D11_FILTER_MIN_MAG_MIP_POINT = 0,
	D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR = 0x1,
	D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x4,
	D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR = 0x5,
	D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT = 0x10,
	D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x11,
	D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT = 0x14,
	D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0x15,
	D3D11_FILTER_ANISOTROPIC = 0x55,
	D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT = 0x80,
	D3D11_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR = 0x81,
	D3D11_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x84,
	D3D11_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR = 0x85,
	D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT = 0x90,
	D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x91,
	D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT = 0x94,
	D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR = 0x95,
	D3D11_FILTER_COMPARISON_ANISOTROPIC = 0xd5,

	D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT = 0x100,
	D3D11_FILTER_MINIMUM_MIN_MAG_POINT_MIP_LINEAR = 0x101,
	D3D11_FILTER_MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x104,
	D3D11_FILTER_MINIMUM_MIN_POINT_MAG_MIP_LINEAR = 0x105,
	D3D11_FILTER_MINIMUM_MIN_LINEAR_MAG_MIP_POINT = 0x110,
	D3D11_FILTER_MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x111,
	D3D11_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT = 0x114,
	D3D11_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR = 0x115,
	D3D11_FILTER_MINIMUM_ANISOTROPIC = 0x155,
	D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_POINT = 0x180,
	D3D11_FILTER_MAXIMUM_MIN_MAG_POINT_MIP_LINEAR = 0x181,
	D3D11_FILTER_MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x184,
	D3D11_FILTER_MAXIMUM_MIN_POINT_MAG_MIP_LINEAR = 0x185,
	D3D11_FILTER_MAXIMUM_MIN_LINEAR_MAG_MIP_POINT = 0x190,
	D3D11_FILTER_MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x191,
	D3D11_FILTER_MAXIMUM_MIN_MAG_LINEAR_MIP_POINT = 0x194,
	D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR = 0x195,
	D3D11_FILTER_MAXIMUM_ANISOTROPIC = 0x1d5
};

enum D3D11_COMPARISON_FUNC
{
	D3D11_COMPARISON_NEVER = 1,
	D3D11_COMPARISON_LESS = 2,
	D3D11_COMPARISON_EQUAL = 3,
	D3D11_COMPARISON_LESS_EQUAL = 4,
	D3D11_COMPARISON_GREATER = 5,
	D3D11_COMPARISON_NOT_EQUAL = 6,
	D3D11_COMPARISON_GREATER_EQUAL = 7,
	D3D11_COMPARISON_ALWAYS = 8
};

enum D3D11_TEXTURE_ADDRESS_MODE
{
	D3D11_TEXTURE_ADDRESS_WRAP = 1,
	D3D11_TEXTURE_ADDRESS_MIRROR = 2,
	D3D11_TEXTURE_ADDRESS_CLAMP = 3,
	D3D11_TEXTURE_ADDRESS_BORDER = 4,
	D3D11_TEXTURE_ADDRESS_MIRROR_ONCE = 5
};

enum D3D11_FILTER_TYPE
{
	D3D11_FILTER_TYPE_POINT,
	D3D11_FILTER_TYPE_LINEAR
};

enum D3D11_FILTER_REDUCTION_TYPE
{
	D3D11_FILTER_REDUCTION_TYPE_STANDARD,
	D3D11_FILTER_REDUCTION_TYPE_COMPARISON,
	D3D11_FILTER_REDUCTION_TYPE_MINIMUM,
	D3D11_FILTER_REDUCTION_TYPE_MAXIMUM
};

enum D3D11_BLEND
{
	D3D11_BLEND_ZERO = 1,
	D3D11_BLEND_ONE = 2,
	D3D11_BLEND_SRC_COLOR = 3,
	D3D11_BLEND_INV_SRC_COLOR = 4,
	D3D11_BLEND_SRC_ALPHA = 5,
	D3D11_BLEND_INV_SRC_ALPHA = 6,
	D3D11_BLEND_DEST_ALPHA = 7,
	D3D11_BLEND_INV_DEST_ALPHA = 8,
	D3D11_BLEND_DEST_COLOR = 9,
	D3D11_BLEND_INV_DEST_COLOR = 10,
	D3D11_BLEND_SRC_ALPHA_SAT = 11,
	D3D11_BLEND_BLEND_FACTOR = 14,
	D3D11_BLEND_INV_BLEND_FACTOR = 15,
	D3D11_BLEND_SRC1_COLOR = 16,
	D3D11_BLEND_INV_SRC1_COLOR = 17,
	D3D11_BLEND_SRC1_ALPHA = 18,
	D3D11_BLEND_INV_SRC1_ALPHA = 19
};

enum D3D11_BLEND_OP
{
	D3D11_BLEND_OP_ADD = 1,
	D3D11_BLEND_OP_SUBTRACT = 2,
	D3D11_BLEND_OP_REV_SUBTRACT = 3,
	D3D11_BLEND_OP_MIN = 4,
	D3D11_BLEND_OP_MAX = 5
};

enum D3D11_FILL_MODE
{
	D3D11_FILL_WIREFRAME = 2,
	D3D11_FILL_SOLID = 3
};

#endif

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <sstream>
#include <vector>
#include <regex>

#include <mutex>
#include <condition_variable>
#include <optional>
#include <atomic>
#include <thread>
#include <functional>

#if !NDEBUG
#undef CCP_TELEMETRY_ENABLED
#endif

#if CCP_TELEMETRY_ENABLED
#include <tracy/Tracy.hpp>
#else
#define ZoneScoped
#define ZoneScopedN( x )
#endif

#ifndef _WIN32

inline errno_t fopen_s( FILE** stream, char const* fileName, char const* mode )
{
	*stream = fopen( fileName, mode );
	if( !*stream )
	{
		auto error = errno;
		return error ? error : -1;
	}
	return 0;
}

#endif
