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

namespace tr2c
{
// What one gather callback drew, as the frame calls it made, so Tr2CSpriteHook can draw an
// unchanged frame again without calling back (tr2c.h, hook_set_dirty). Pointers to C-side
// objects (texture, text, display list) stay valid while ContentEpoch() is unchanged: releasing
// or changing one bumps it.
struct Recording
{
	enum Kind : uint8_t
	{
		PUSH_TRANSLATION,
		POP_TRANSLATION,
		PUSH_TRANSFORM, // 16 floats at off
		POP_TRANSFORM,
		PUSH_CLIP,
		POP_CLIP,
		SET_ALPHA,
		SET_EFFECT,
		SET_BLEND,
		SET_COLOR,
		SET_TEXTURE,
		SET_TEXTURE_WINDOW,
		DRAW_QUAD,      // n = 8: uvs at off; n = 0: no uvs
		DRAW_TRIANGLES, // n vertices at off, n2 indices at off2
		REPLAY,
		TEXT_DRAW
	};
	struct Op
	{
		Kind kind;
		uint32_t u = 0;          // slot, effect, blend, texture settings
		float v[4] = {};         // x y w h, colour, translation, alpha
		const void* p = nullptr; // texture, text or display list
		uint32_t off = 0, n = 0, off2 = 0, n2 = 0;
	};

	std::vector<Op> ops;
	std::vector<float> floats;
	std::vector<tr2c_vertex> verts;
	std::vector<uint32_t> indices;
	bool valid = false;      // complete and replayable
	uint32_t epoch = 0;      // ContentEpoch() when recorded
	uint32_t generation = 0; // DeviceGeneration() when recorded

	void Clear()
	{
		ops.clear();
		floats.clear();
		verts.clear();
		indices.clear();
		valid = false;
	}
	bool References( const void* obj ) const
	{
		for( const Op& op : ops )
		{
			if( op.p == obj )
			{
				return true;
			}
		}
		return false;
	}
};

// Marks the recording being made (if any) as not replayable, e.g. after a frame call threw.
void SpoilRecording();
// Bumped whenever a texture, text or display list is released or changed.
uint32_t ContentEpoch();
// Called before obj (a tr2c texture, text or display list) is released or changed. Inside a
// gather callback that is recording, a change to an object it already drew from spoils the
// recording.
void NoteChanged( const void* obj );
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
	tr2c::Recording* rec = nullptr; // set while the callback's calls are being recorded
	bool recSpoiled = false;        // the recording cannot be replayed
};

namespace tr2c
{
// TR2C_E_STATE unless f is the frame of the gather callback running now.
tr2c_status CheckFrame( const char* fn, tr2c_frame* f );
// Called by Tr2CSpriteHook::GatherSprites around the callback.
void BeginFrame( tr2c_frame* f );
void EndFrame( tr2c_frame* f );
// Issues a recording's calls into f, between BeginFrame and EndFrame.
void Play( tr2c_frame* f, const Recording& rec );
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
