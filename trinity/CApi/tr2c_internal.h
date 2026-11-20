// Copyright © 2026 CCP ehf.
//
// Shared helpers of the tr2c engine C interface (tr2c.h). Internal to trinity.

#pragma once
#ifndef tr2c_internal_h
#define tr2c_internal_h

#include "tr2c.h"

#include <vector>

class Tr2Sprite2dScene;
class Tr2CSpriteHook;

namespace tr2c
{
// Records the error text for this thread and returns status.
tr2c_status Fail( tr2c_status status, const char* fmt, ... );
// Clears this thread's error text and returns TR2C_OK.
tr2c_status Ok();

// TR2C_E_STATE before init, TR2C_E_THREAD off the init thread, else TR2C_OK (error text set).
tr2c_status Guard( const char* fn );

// Counted by Tr2CSpriteHook::ReleaseResources.
void BumpDeviceGeneration();
uint32_t DeviceGeneration();

// UTF-8 to codepoints. Rejects invalid sequences (overlong, surrogates, > U+10FFFF,
// truncated) and U+0000.
tr2c_status DecodeUtf8( const char* fn, const char* utf8, uint32_t len, std::vector<uint32_t>& out );
}

// The frame a gather callback receives. Lives on the stack of Tr2CSpriteHook::GatherSprites.
struct tr2c_frame
{
	enum Push : uint8_t
	{
		PUSH_TRANSLATION,
		PUSH_TRANSFORM,
		PUSH_CLIP
	};

	Tr2Sprite2dScene* scene = nullptr;
	Tr2CSpriteHook* hook = nullptr;
	int32_t effect = 0;           // last effect set on the scene through this frame
	float savedAlpha = 1.0f;      // scene's accumulated alpha before the callback, restored after
	bool capturing = false;       // between capture_begin and capture_end
	std::vector<uint8_t> pushes;  // LIFO of Push, unwound after the callback if left open
};

namespace tr2c
{
// TR2C_E_STATE unless f is the frame of the gather callback running now.
tr2c_status CheckFrame( const char* fn, tr2c_frame* f );
// Called by Tr2CSpriteHook::GatherSprites around the callback.
void BeginFrame( tr2c_frame* f );
void EndFrame( tr2c_frame* f );
}

// Function table entries implemented outside tr2c_api.cpp.
namespace tr2c
{
tr2c_status TextureLoad( const char* res_path, uint32_t len, tr2c_texture** out );
tr2c_status TextureCreateBgra( uint32_t w, uint32_t h, const void* pixels, uint32_t pitch, tr2c_texture** out );
tr2c_status TextureState( tr2c_texture* t, int32_t* out_state, uint32_t* out_w, uint32_t* out_h );
tr2c_status TextureRelease( tr2c_texture* t );

tr2c_status TextCreate( tr2c_text** out );
tr2c_status TextSetFont( tr2c_text* t, const char* res_path, uint32_t len, float size_px );
tr2c_status TextMetrics( tr2c_text* t, tr2c_font_metrics* out );
tr2c_status TextMeasure( tr2c_text* t, const char* utf8, uint32_t len, float* out_adv, uint32_t cap, uint32_t* out_count );
tr2c_status TextReset( tr2c_text* t );
tr2c_status TextAddLine( tr2c_text* t, const char* utf8, uint32_t len, int32_t x, int32_t y );
tr2c_status TextDraw( tr2c_frame* f, tr2c_text* t, float r, float g, float b, float a );
tr2c_status TextRelease( tr2c_text* t );
}

#endif // tr2c_internal_h
