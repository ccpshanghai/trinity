// Copyright © 2026 CCP ehf.
//
// tr2c engine C interface (tr2c.h): the function table, thread checks, error text, the
// hook and the frame calls. Textures are in tr2c_texture.cpp, text in tr2c_text.cpp.

#include "StdAfx.h"
#include "tr2c_internal.h"
#include "Tr2CSpriteHook.h"
#include "Sprite2d/Tr2Sprite2dScene.h"
#include "Sprite2d/Tr2Sprite2dDisplayList.h"
#include "Font/Tr2FontManager.h"
#include "Tr2TextureAtlasMan.h"
#include "Tr2TextureAtlas.h"

#include <IBlueRtti.h>
#include <IList.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <stddef.h>
#include <string>
#include <thread>

using namespace Tr2RenderContextEnum;

//////////////////////////////////////////////////////////////////////////
// The C enums equal trinity's, so the shim casts. Pinned here so an upstream change fails the build.

static_assert( int( TR2C_EFFECT_FILL ) == int( TR2_SFX_FILL ), "tr2c effect mapping" );
static_assert( int( TR2C_EFFECT_FILL_AA ) == int( TR2_SFX_FILL_AA ), "tr2c effect mapping" );
static_assert( int( TR2C_EFFECT_COPY ) == int( TR2_SFX_COPY ), "tr2c effect mapping" );
static_assert( int( TR2C_EFFECT_FONT ) == int( TR2_SFX_FONT ), "tr2c effect mapping" );
static_assert( int( TR2C_EFFECT_MODULATE ) == int( TR2_SFX_MODULATE ), "tr2c effect mapping" );
static_assert( int( TR2C_EFFECT_MASK ) == int( TR2_SFX_MASK ), "tr2c effect mapping" );
static_assert( int( TR2C_BLEND_NONE ) == int( TR2_SBM_NONE ), "tr2c blend mapping" );
static_assert( int( TR2C_BLEND_BLEND ) == int( TR2_SBM_BLEND ), "tr2c blend mapping" );
static_assert( int( TR2C_BLEND_ADD ) == int( TR2_SBM_ADD ), "tr2c blend mapping" );
static_assert( int( TR2C_TEX_NONE ) == int( S2D_TS_NONE ), "tr2c texture settings mapping" );
static_assert( int( TR2C_TEX_TILE_X ) == int( S2D_TS_TILE_X ), "tr2c texture settings mapping" );
static_assert( int( TR2C_TEX_TILE_Y ) == int( S2D_TS_TILE_Y ), "tr2c texture settings mapping" );

// tr2c_vertex is passed to PrepareTriangleVerts as a Tr2Sprite2dVertexBase.
static_assert( sizeof( tr2c_vertex ) == sizeof( Tr2Sprite2dVertexBase ), "tr2c vertex layout" );
static_assert( offsetof( tr2c_vertex, x ) == offsetof( Tr2Sprite2dVertexBase, position ), "tr2c vertex layout" );
static_assert( offsetof( tr2c_vertex, r ) == offsetof( Tr2Sprite2dVertexBase, color ), "tr2c vertex layout" );
static_assert( offsetof( tr2c_vertex, u0 ) == offsetof( Tr2Sprite2dVertexBase, texCoord ), "tr2c vertex layout" );
static_assert( sizeof( Matrix ) == 16 * sizeof( float ), "tr2c transform layout" );

namespace tr2c
{
namespace
{
thread_local std::string t_lastError;
std::atomic<bool> g_initialized{ false };
std::thread::id g_thread;
std::atomic<uint32_t> g_deviceGeneration{ 0 };
uint32_t g_contentEpoch = 0; // render thread only

// The frame of the gather callback running now (render thread only).
tr2c_frame* g_currentFrame = nullptr;

const unsigned short s_quadIndices[6] = { 0, 1, 3, 3, 1, 2 };
}

tr2c_status Fail( tr2c_status status, const char* fmt, ... )
{
	char buf[512];
	va_list args;
	va_start( args, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, args );
	va_end( args );
	try
	{
		t_lastError = buf;
	}
	catch( ... )
	{
		// Out of memory for the text itself: keep the status, drop the text.
		t_lastError.clear();
	}
	return status;
}

tr2c_status Ok()
{
	t_lastError.clear();
	return TR2C_OK;
}

tr2c_status Guard( const char* fn )
{
	if( !g_initialized.load() )
	{
		return Fail( TR2C_E_STATE, "%s: tr2c is not initialised (call init first)", fn );
	}
	if( std::this_thread::get_id() != g_thread )
	{
		return Fail( TR2C_E_THREAD, "%s: called off the thread that called init", fn );
	}
	return TR2C_OK;
}

void BumpDeviceGeneration()
{
	g_deviceGeneration.fetch_add( 1 );
}

uint32_t DeviceGeneration()
{
	return g_deviceGeneration.load();
}

uint32_t ContentEpoch()
{
	return g_contentEpoch;
}

void SpoilRecording()
{
	if( g_currentFrame && g_currentFrame->rec )
	{
		g_currentFrame->recSpoiled = true;
	}
}

void NoteChanged( const void* obj )
{
	// Spoils the recordings of other hooks (and of this hook's last clean frame); the
	// recording being made now stays valid unless it already drew from obj.
	++g_contentEpoch;
	if( obj && g_currentFrame && g_currentFrame->rec && g_currentFrame->rec->References( obj ) )
	{
		g_currentFrame->recSpoiled = true;
	}
}

tr2c_status DecodeUtf8( const char* fn, const char* utf8, uint32_t len, std::vector<uint32_t>& out )
{
	out.clear();
	if( len && !utf8 )
	{
		return Fail( TR2C_E_INVALID_ARG, "%s: null text with length %u", fn, len );
	}
	out.reserve( len );
	const auto* s = reinterpret_cast<const unsigned char*>( utf8 );
	uint32_t i = 0;
	while( i < len )
	{
		const uint32_t c0 = s[i];
		uint32_t cp;
		uint32_t n;
		uint32_t min;
		if( c0 < 0x80 )
		{
			cp = c0;
			n = 0;
			min = 0;
		}
		else if( ( c0 & 0xE0 ) == 0xC0 )
		{
			cp = c0 & 0x1F;
			n = 1;
			min = 0x80;
		}
		else if( ( c0 & 0xF0 ) == 0xE0 )
		{
			cp = c0 & 0x0F;
			n = 2;
			min = 0x800;
		}
		else if( ( c0 & 0xF8 ) == 0xF0 )
		{
			cp = c0 & 0x07;
			n = 3;
			min = 0x10000;
		}
		else
		{
			return Fail( TR2C_E_INVALID_ARG, "%s: invalid UTF-8 lead byte 0x%02x at %u", fn, c0, i );
		}
		if( n > len - i - 1 )
		{
			return Fail( TR2C_E_INVALID_ARG, "%s: truncated UTF-8 sequence at %u", fn, i );
		}
		for( uint32_t k = 1; k <= n; ++k )
		{
			const uint32_t c = s[i + k];
			if( ( c & 0xC0 ) != 0x80 )
			{
				return Fail( TR2C_E_INVALID_ARG, "%s: invalid UTF-8 continuation byte at %u", fn, i + k );
			}
			cp = ( cp << 6 ) | ( c & 0x3F );
		}
		if( cp < min || cp > 0x10FFFF || ( cp >= 0xD800 && cp <= 0xDFFF ) )
		{
			return Fail( TR2C_E_INVALID_ARG, "%s: invalid UTF-8 sequence (overlong, surrogate or out of range) at %u", fn, i );
		}
		if( cp == 0 )
		{
			return Fail( TR2C_E_INVALID_ARG, "%s: U+0000 in text at %u", fn, i );
		}
		out.push_back( cp );
		i += n + 1;
	}
	return TR2C_OK;
}

tr2c_status CheckFrame( const char* fn, tr2c_frame* f )
{
	if( tr2c_status s = Guard( fn ) )
	{
		return s;
	}
	if( !f || f != g_currentFrame )
	{
		return Fail( TR2C_E_STATE, "%s: frame calls are only valid inside the gather callback", fn );
	}
	return TR2C_OK;
}

void BeginFrame( tr2c_frame* f )
{
	CCP_ASSERT( !g_currentFrame );
	g_currentFrame = f;

	// A known starting state, as each sprite object sets its own render state.
	f->effect = TR2C_EFFECT_FILL;
	f->scene->SetSpriteEffect( TR2_SFX_FILL );
	f->scene->SetBlendmode( TR2_SBM_BLEND );
	f->scene->SetColor( Color( 1.0f, 1.0f, 1.0f, 1.0f ) );
	f->savedAlpha = f->scene->GetAccumulatedAlpha();
}

void EndFrame( tr2c_frame* f )
{
	// Unwind what the callback left open, so the scene's stacks stay balanced.
	if( f->capturing )
	{
		Tr2Sprite2dDisplayList* dl = f->scene->EndCapture( nullptr );
		if( dl )
		{
			CCP_DELETE dl;
		}
		f->capturing = false;
		Fail( TR2C_E_STATE, "gather: callback returned inside capture_begin; the capture was dropped" );
		CCP_LOGERR( "tr2c: gather callback returned inside capture_begin; the capture was dropped" );
	}
	if( !f->pushes.empty() )
	{
		CCP_LOGERR( "tr2c: gather callback returned with %u unpopped push(es); popped them", unsigned( f->pushes.size() ) );
		Fail( TR2C_E_STATE, "gather: callback returned with %u unpopped push(es)", unsigned( f->pushes.size() ) );
		while( !f->pushes.empty() )
		{
			switch( f->pushes.back() )
			{
			case tr2c_frame::PUSH_TRANSLATION: f->scene->PopTranslation(); break;
			case tr2c_frame::PUSH_TRANSFORM: f->scene->PopTransform(); break;
			case tr2c_frame::PUSH_CLIP: f->scene->PopClipRectangle(); break;
			}
			f->pushes.pop_back();
		}
	}
	f->scene->SetAccumulatedAlpha( f->savedAlpha );
	g_currentFrame = nullptr;
}
}

using namespace tr2c;

namespace
{
//////////////////////////////////////////////////////////////////////////
// Init, errors

const char* LastError()
{
	return t_lastError.c_str();
}

tr2c_status Init()
{
	if( g_initialized.load() )
	{
		if( std::this_thread::get_id() != g_thread )
		{
			return Fail( TR2C_E_THREAD, "init: already initialised on another thread" );
		}
		return Ok();
	}
	if( !g_textureAtlasMan )
	{
		return Fail( TR2C_E_ENGINE, "init: no texture atlas manager" );
	}
	if( !g_textureAtlasMan->FindAtlas( PIXEL_FORMAT_B8G8R8A8_UNORM ) )
	{
		// Same atlas Tr2Sprite2dScene registers on construction.
		g_textureAtlasMan->AddAtlas( PIXEL_FORMAT_B8G8R8A8_UNORM, 2048, 2048 );
		if( !g_textureAtlasMan->FindAtlas( PIXEL_FORMAT_B8G8R8A8_UNORM ) )
		{
			return Fail( TR2C_E_ENGINE, "init: could not register a BGRA texture atlas" );
		}
	}
	if( !g_fontManager )
	{
		Tr2FontManager::Initialize();
		if( !g_fontManager )
		{
			return Fail( TR2C_E_ENGINE, "init: font manager failed to initialise" );
		}
	}
	g_thread = std::this_thread::get_id();
	g_initialized.store( true );
	return Ok();
}

//////////////////////////////////////////////////////////////////////////
// Hook

Tr2CSpriteHook* HookOf( tr2c_hook* h )
{
	return reinterpret_cast<Tr2CSpriteHook*>( h );
}

// Tr2CSpriteHook and Tr2Sprite2dScene reach IRoot through more than one base.
IRoot* RootOf( Tr2CSpriteHook* hook )
{
	return static_cast<ITr2SpriteObject*>( hook );
}

IRoot* RootOf( Tr2Sprite2dScene* scene )
{
	return static_cast<ITr2Scene*>( scene );
}

// The scene's Blue "children" list (Tr2Sprite2dScene::m_children), found the way Python's
// getattr finds it, so trinity's scene class is not changed.
IListPtr ChildrenOf( Tr2Sprite2dScene* scene )
{
	IRoot* base = RootOf( scene )->GetRootObject();
	IBlueRtti* rtti = BeClasses->GetRtti( base->ClassType() );
	BlueRttiValue* v = rtti ? rtti->FindAttribute( "children" ) : nullptr;
	if( !v || v->mType != BlueRttiValue::var || !v->mVar || v->mVar->mType != Be::IROOT )
	{
		return IListPtr();
	}
	IRoot* embedded = reinterpret_cast<IRoot*>( reinterpret_cast<uintptr_t>( base ) + v->mTotalOffset );
	IListPtr list = BlueCastPtr( embedded );
	return list;
}

tr2c_status HookCreate( void* scene, tr2c_gather_fn fn, void* user, tr2c_hook** out_hook )
{
	if( tr2c_status s = Guard( "hook_create" ) )
	{
		return s;
	}
	if( !out_hook || !scene || !fn )
	{
		return Fail( TR2C_E_INVALID_ARG, "hook_create: null argument" );
	}
	*out_hook = nullptr;

	Tr2Sprite2dScenePtr sceneObj = BlueCastPtr( static_cast<IRoot*>( scene ) );
	if( !sceneObj )
	{
		return Fail( TR2C_E_INVALID_ARG, "hook_create: the object is not a Tr2Sprite2dScene" );
	}
	IListPtr children = ChildrenOf( sceneObj );
	if( !children )
	{
		return Fail( TR2C_E_ENGINE, "hook_create: Tr2Sprite2dScene has no 'children' list" );
	}

	// O-type: constructed with one reference, which the handle owns. Keep the null check:
	// on Windows the global new is CCP_MALLOC, which returns nullptr instead of throwing.
	// Elsewhere new throws std::bad_alloc, which Shielded (below) turns into TR2C_E_OUT_OF_MEMORY.
	Tr2CSpriteHook* raw = new OTr2CSpriteHook;
	BluePtr<Tr2CSpriteHook> hook;
	hook.Attach( raw );
	if( !raw )
	{
		return Fail( TR2C_E_OUT_OF_MEMORY, "hook_create: out of memory" );
	}
	hook->SetCallback( fn, user );
	hook->SetScene( sceneObj );

	if( !children->Append( RootOf( raw ) ) )
	{
		hook->SetScene( nullptr );
		return Fail( TR2C_E_ENGINE, "hook_create: the scene's children list refused the hook" );
	}
	*out_hook = reinterpret_cast<tr2c_hook*>( hook.Detach() );
	return Ok();
}

tr2c_status HookSetDirty( tr2c_hook* h )
{
	if( tr2c_status s = Guard( "hook_set_dirty" ) )
	{
		return s;
	}
	if( !h )
	{
		return Fail( TR2C_E_INVALID_ARG, "hook_set_dirty: null hook" );
	}
	HookOf( h )->SetDirty();
	return Ok();
}

tr2c_status HookRelease( tr2c_hook* h )
{
	if( tr2c_status s = Guard( "hook_release" ) )
	{
		return s;
	}
	if( !h )
	{
		return Fail( TR2C_E_INVALID_ARG, "hook_release: null hook" );
	}
	Tr2CSpriteHook* hook = HookOf( h );
	if( g_currentFrame && g_currentFrame->hook == hook )
	{
		// The scene is iterating its children.
		return Fail( TR2C_E_STATE, "hook_release: called inside the hook's own gather callback" );
	}
	tr2c_status result = TR2C_OK;
	if( Tr2Sprite2dScene* scene = hook->GetScene() )
	{
		if( IListPtr children = ChildrenOf( scene ) )
		{
			const ssize_t key = children->FindKey( RootOf( hook ), 0 );
			if( key >= 0 && !children->Remove( key ) )
			{
				result = Fail( TR2C_E_ENGINE, "hook_release: could not remove the hook from scene.children" );
			}
		}
		hook->SetScene( nullptr );
	}
	hook->SetCallback( nullptr, nullptr );
	RootOf( hook )->Unlock();
	return result == TR2C_OK ? Ok() : result;
}

//////////////////////////////////////////////////////////////////////////
// Frame: state

// Appends an op to the recording being made, if any. Returns null when not recording.
Recording::Op* Record( tr2c_frame* f, Recording::Kind kind )
{
	if( !f->rec )
	{
		return nullptr;
	}
	f->rec->ops.emplace_back();
	Recording::Op* op = &f->rec->ops.back();
	op->kind = kind;
	return op;
}

// Draws made between capture_begin and capture_end land in the display list, not on screen,
// so they are not recorded (the replay of that list is). State calls are recorded either way.
Recording::Op* RecordDraw( tr2c_frame* f, Recording::Kind kind )
{
	return f->capturing ? nullptr : Record( f, kind );
}

tr2c_status PushTranslation( tr2c_frame* f, float x, float y )
{
	if( tr2c_status s = CheckFrame( "push_translation", f ) )
	{
		return s;
	}
	f->pushes.reserve( f->pushes.size() + 1 ); // so push_back below cannot throw
	f->scene->PushTranslation( Vector2( x, y ) );
	f->pushes.push_back( tr2c_frame::PUSH_TRANSLATION );
	if( Recording::Op* op = Record( f, Recording::PUSH_TRANSLATION ) )
	{
		op->v[0] = x;
		op->v[1] = y;
	}
	return Ok();
}

tr2c_status PopPush( tr2c_frame* f, const char* fn, tr2c_frame::Push kind )
{
	if( tr2c_status s = CheckFrame( fn, f ) )
	{
		return s;
	}
	if( f->pushes.empty() || f->pushes.back() != kind )
	{
		return Fail( TR2C_E_STATE, "%s: does not match the last push", fn );
	}
	switch( kind )
	{
	case tr2c_frame::PUSH_TRANSLATION: f->scene->PopTranslation(); break;
	case tr2c_frame::PUSH_TRANSFORM: f->scene->PopTransform(); break;
	case tr2c_frame::PUSH_CLIP: f->scene->PopClipRectangle(); break;
	}
	f->pushes.pop_back();
	switch( kind )
	{
	case tr2c_frame::PUSH_TRANSLATION: Record( f, Recording::POP_TRANSLATION ); break;
	case tr2c_frame::PUSH_TRANSFORM: Record( f, Recording::POP_TRANSFORM ); break;
	case tr2c_frame::PUSH_CLIP: Record( f, Recording::POP_CLIP ); break;
	}
	return Ok();
}

tr2c_status PopTranslation( tr2c_frame* f )
{
	return PopPush( f, "pop_translation", tr2c_frame::PUSH_TRANSLATION );
}

tr2c_status PushTransform( tr2c_frame* f, const float m[16] )
{
	if( tr2c_status s = CheckFrame( "push_transform", f ) )
	{
		return s;
	}
	if( !m )
	{
		return Fail( TR2C_E_INVALID_ARG, "push_transform: null matrix" );
	}
	Matrix mat;
	std::memcpy( static_cast<void*>( &mat ), m, sizeof( mat ) );
	f->pushes.reserve( f->pushes.size() + 1 ); // so push_back below cannot throw
	f->scene->PushTransform( mat );
	f->pushes.push_back( tr2c_frame::PUSH_TRANSFORM );
	if( Recording::Op* op = Record( f, Recording::PUSH_TRANSFORM ) )
	{
		op->off = uint32_t( f->rec->floats.size() );
		op->n = 16;
		f->rec->floats.insert( f->rec->floats.end(), m, m + 16 );
	}
	return Ok();
}

tr2c_status PopTransform( tr2c_frame* f )
{
	return PopPush( f, "pop_transform", tr2c_frame::PUSH_TRANSFORM );
}

tr2c_status PushClip( tr2c_frame* f, float x, float y, float w, float h )
{
	if( tr2c_status s = CheckFrame( "push_clip", f ) )
	{
		return s;
	}
	f->pushes.reserve( f->pushes.size() + 1 ); // so push_back below cannot throw
	f->scene->PushClipRectangle( x, y, w, h );
	f->pushes.push_back( tr2c_frame::PUSH_CLIP );
	if( Recording::Op* op = Record( f, Recording::PUSH_CLIP ) )
	{
		op->v[0] = x;
		op->v[1] = y;
		op->v[2] = w;
		op->v[3] = h;
	}
	return Ok();
}

tr2c_status PopClip( tr2c_frame* f )
{
	return PopPush( f, "pop_clip", tr2c_frame::PUSH_CLIP );
}

tr2c_status SetAlpha( tr2c_frame* f, float a )
{
	if( tr2c_status s = CheckFrame( "set_alpha", f ) )
	{
		return s;
	}
	f->scene->SetAccumulatedAlpha( a );
	if( Recording::Op* op = Record( f, Recording::SET_ALPHA ) )
	{
		op->v[0] = a;
	}
	return Ok();
}

tr2c_status SetEffect( tr2c_frame* f, int32_t effect )
{
	if( tr2c_status s = CheckFrame( "set_effect", f ) )
	{
		return s;
	}
	switch( effect )
	{
	case TR2C_EFFECT_FILL:
	case TR2C_EFFECT_FILL_AA:
	case TR2C_EFFECT_COPY:
	case TR2C_EFFECT_FONT:
	case TR2C_EFFECT_MODULATE:
	case TR2C_EFFECT_MASK:
		break;
	default:
		return Fail( TR2C_E_INVALID_ARG, "set_effect: unknown effect %d", effect );
	}
	f->scene->SetSpriteEffect( static_cast<Tr2SpriteObjectEffect>( effect ) );
	f->effect = effect;
	if( Recording::Op* op = Record( f, Recording::SET_EFFECT ) )
	{
		op->u = uint32_t( effect );
	}
	return Ok();
}

tr2c_status SetBlend( tr2c_frame* f, int32_t blend )
{
	if( tr2c_status s = CheckFrame( "set_blend", f ) )
	{
		return s;
	}
	if( blend != TR2C_BLEND_NONE && blend != TR2C_BLEND_BLEND && blend != TR2C_BLEND_ADD )
	{
		return Fail( TR2C_E_INVALID_ARG, "set_blend: unknown blend mode %d", blend );
	}
	f->scene->SetBlendmode( static_cast<Tr2SpriteObjectBlendMode>( blend ) );
	if( Recording::Op* op = Record( f, Recording::SET_BLEND ) )
	{
		op->u = uint32_t( blend );
	}
	return Ok();
}

tr2c_status SetColor( tr2c_frame* f, float r, float g, float b, float a )
{
	if( tr2c_status s = CheckFrame( "set_color", f ) )
	{
		return s;
	}
	f->scene->SetColor( Color( r, g, b, a ) );
	if( Recording::Op* op = Record( f, Recording::SET_COLOR ) )
	{
		op->v[0] = r;
		op->v[1] = g;
		op->v[2] = b;
		op->v[3] = a;
	}
	return Ok();
}

tr2c_status SetTexture( tr2c_frame* f, uint32_t slot, tr2c_texture* tex, uint32_t settings )
{
	if( tr2c_status s = CheckFrame( "set_texture", f ) )
	{
		return s;
	}
	if( slot > 1 )
	{
		return Fail( TR2C_E_INVALID_ARG, "set_texture: slot %u (0 or 1)", slot );
	}
	if( settings & ~uint32_t( TR2C_TEX_TILE_X | TR2C_TEX_TILE_Y ) )
	{
		return Fail( TR2C_E_INVALID_ARG, "set_texture: unknown settings 0x%x", settings );
	}
	Tr2AtlasTexturePtr t( reinterpret_cast<Tr2AtlasTexture*>( tex ) );
	f->scene->SetTexture( slot, t, static_cast<Tr2Sprite2dTextureSettings>( settings ) );
	if( Recording::Op* op = Record( f, Recording::SET_TEXTURE ) )
	{
		op->u = slot;
		op->n = settings;
		op->p = tex;
	}
	return Ok();
}

tr2c_status SetTextureWindow( tr2c_frame* f, uint32_t slot, float x, float y, float w, float h )
{
	if( tr2c_status s = CheckFrame( "set_texture_window", f ) )
	{
		return s;
	}
	if( slot > 1 )
	{
		return Fail( TR2C_E_INVALID_ARG, "set_texture_window: slot %u (0 or 1)", slot );
	}
	f->scene->SetTextureWindow( slot, x, y, w, h );
	if( Recording::Op* op = Record( f, Recording::SET_TEXTURE_WINDOW ) )
	{
		op->u = slot;
		op->v[0] = x;
		op->v[1] = y;
		op->v[2] = w;
		op->v[3] = h;
	}
	return Ok();
}

//////////////////////////////////////////////////////////////////////////
// Frame: drawing

tr2c_status DrawQuad( tr2c_frame* f, float x, float y, float w, float h, const float uv[8] )
{
	if( tr2c_status s = CheckFrame( "draw_quad", f ) )
	{
		return s;
	}
	// Recorded even when the textures are not ready yet, so a replay draws it once they are.
	if( Recording::Op* op = RecordDraw( f, Recording::DRAW_QUAD ) )
	{
		op->v[0] = x;
		op->v[1] = y;
		op->v[2] = w;
		op->v[3] = h;
		if( uv )
		{
			op->off = uint32_t( f->rec->floats.size() );
			op->n = 8;
			f->rec->floats.insert( f->rec->floats.end(), uv, uv + 8 );
		}
	}
	Tr2Sprite2dD3DVertex verts[4];
	const auto sfx = static_cast<Tr2SpriteObjectEffect>( f->effect );
	if( !f->scene->PrepareSpriteVerts( verts, Vector2( x, y ), w, h, sfx ) )
	{
		// Textures not ready yet: nothing drawn, not an error (as Tr2Sprite2d).
		return Ok();
	}
	if( uv )
	{
		// PrepareSpriteVerts wrote the corners SetSpriteVerticesUVs gives for this size (texture
		// window, tiling and transform applied). Map the caller's unit-square uvs through the same
		// affine map: base0 + u * (base1 - base0) + v * (base3 - base0).
		const int textures = f->effect >= int( TR2_SFX_TWO_TEXTURES ) ? 2 : ( f->effect >= int( TR2_SFX_ONE_TEXTURE ) ? 1 : 0 );
		for( int t = 0; t < textures; ++t )
		{
			const Vector2 b0 = verts[0].texCoord[t];
			const Vector2 du = verts[1].texCoord[t] - b0;
			const Vector2 dv = verts[3].texCoord[t] - b0;
			for( int i = 0; i < 4; ++i )
			{
				const float u = uv[i * 2];
				const float v = uv[i * 2 + 1];
				verts[i].texCoord[t] = b0 + du * u + dv * v;
			}
		}
	}
	f->scene->RenderTriangleVerts( verts, 4, const_cast<unsigned short*>( s_quadIndices ), 6 );
	return Ok();
}

tr2c_status DrawTriangles( tr2c_frame* f, const tr2c_vertex* v, uint32_t vcount, const uint32_t* idx, uint32_t icount )
{
	if( tr2c_status s = CheckFrame( "draw_triangles", f ) )
	{
		return s;
	}
	if( ( vcount && !v ) || ( icount && !idx ) )
	{
		return Fail( TR2C_E_INVALID_ARG, "draw_triangles: null array" );
	}
	if( icount % 3 )
	{
		return Fail( TR2C_E_INVALID_ARG, "draw_triangles: index count %u is not a multiple of 3", icount );
	}
	for( uint32_t i = 0; i < icount; ++i )
	{
		if( idx[i] >= vcount )
		{
			return Fail( TR2C_E_INVALID_ARG, "draw_triangles: index %u out of range (%u vertices)", idx[i], vcount );
		}
	}
	if( !icount )
	{
		return Ok();
	}
	if( Recording::Op* op = RecordDraw( f, Recording::DRAW_TRIANGLES ) )
	{
		op->off = uint32_t( f->rec->verts.size() );
		op->n = vcount;
		op->off2 = uint32_t( f->rec->indices.size() );
		op->n2 = icount;
		f->rec->verts.insert( f->rec->verts.end(), v, v + vcount );
		f->rec->indices.insert( f->rec->indices.end(), idx, idx + icount );
	}

	const uint32_t maxV = std::max( 3u, f->scene->GetMaxVertexCountPerDrawCall() );
	const uint32_t maxI = std::max( 3u, f->scene->GetMaxIndexCountPerDrawCall() - f->scene->GetMaxIndexCountPerDrawCall() % 3 );

	static std::vector<Tr2Sprite2dD3DVertex> s_dest;
	auto* base = reinterpret_cast<Tr2Sprite2dVertexBase*>( const_cast<tr2c_vertex*>( v ) );

	if( vcount <= maxV && icount <= maxI )
	{
		s_dest.resize( vcount );
		if( !f->scene->PrepareTriangleVerts( s_dest.data(), base, sizeof( tr2c_vertex ), vcount ) )
		{
			return Ok(); // textures not ready
		}
		f->scene->RenderTriangleVerts( s_dest.data(), vcount, const_cast<uint32_t*>( idx ), icount );
		return Ok();
	}

	// Too big for one draw call: split by triangles, remapping vertices per chunk.
	static std::vector<Tr2Sprite2dVertexBase> s_src;
	static std::vector<uint32_t> s_idx;
	static std::vector<uint32_t> s_remap;
	static std::vector<uint32_t> s_stamp;
	s_remap.assign( vcount, 0 );
	s_stamp.assign( vcount, 0 );
	uint32_t stamp = 1;
	s_src.clear();
	s_idx.clear();

	auto flush = [&]() -> bool {
		if( s_idx.empty() )
		{
			return true;
		}
		s_dest.resize( s_src.size() );
		if( !f->scene->PrepareTriangleVerts( s_dest.data(), s_src.data(), sizeof( Tr2Sprite2dVertexBase ), uint32_t( s_src.size() ) ) )
		{
			return false;
		}
		f->scene->RenderTriangleVerts( s_dest.data(), uint32_t( s_dest.size() ), s_idx.data(), uint32_t( s_idx.size() ) );
		s_src.clear();
		s_idx.clear();
		++stamp;
		return true;
	};

	for( uint32_t t = 0; t < icount; t += 3 )
	{
		uint32_t fresh = 0;
		for( uint32_t k = 0; k < 3; ++k )
		{
			fresh += s_stamp[idx[t + k]] != stamp ? 1 : 0;
		}
		if( s_src.size() + fresh > maxV || s_idx.size() + 3 > maxI )
		{
			if( !flush() )
			{
				return Ok(); // textures not ready
			}
		}
		for( uint32_t k = 0; k < 3; ++k )
		{
			const uint32_t i = idx[t + k];
			if( s_stamp[i] != stamp )
			{
				s_stamp[i] = stamp;
				s_remap[i] = uint32_t( s_src.size() );
				s_src.push_back( base[i] );
			}
			s_idx.push_back( s_remap[i] );
		}
	}
	flush();
	return Ok();
}

//////////////////////////////////////////////////////////////////////////
// Frame: display lists

tr2c_status CaptureBegin( tr2c_frame* f )
{
	if( tr2c_status s = CheckFrame( "capture_begin", f ) )
	{
		return s;
	}
	if( f->capturing || f->scene->IsCapturing() )
	{
		return Fail( TR2C_E_STATE, "capture_begin: already capturing (no nesting)" );
	}
	if( !f->scene->StartCapture( f->hook ) )
	{
		return Fail( TR2C_E_ENGINE, "capture_begin: StartCapture failed" );
	}
	f->capturing = true;
	return Ok();
}

tr2c_status CaptureEnd( tr2c_frame* f, tr2c_display_list* prev, tr2c_display_list** out )
{
	if( tr2c_status s = CheckFrame( "capture_end", f ) )
	{
		return s;
	}
	if( !out )
	{
		return Fail( TR2C_E_INVALID_ARG, "capture_end: null out" );
	}
	*out = nullptr;
	if( !f->capturing )
	{
		return Fail( TR2C_E_STATE, "capture_end: not capturing" );
	}
	f->capturing = false;
	// EndCapture may move prev's buffers into the new list; prev stays the caller's to destroy.
	Tr2Sprite2dDisplayList* dl = f->scene->EndCapture( reinterpret_cast<Tr2Sprite2dDisplayList*>( prev ) );
	*out = reinterpret_cast<tr2c_display_list*>( dl );
	return Ok();
}

tr2c_status Replay( tr2c_frame* f, const tr2c_display_list* dl )
{
	if( tr2c_status s = CheckFrame( "replay", f ) )
	{
		return s;
	}
	if( !dl )
	{
		return Fail( TR2C_E_INVALID_ARG, "replay: null display list" );
	}
	if( f->capturing )
	{
		return Fail( TR2C_E_STATE, "replay: not allowed while capturing" );
	}
	f->scene->ReplayCapture( const_cast<Tr2Sprite2dDisplayList*>( reinterpret_cast<const Tr2Sprite2dDisplayList*>( dl ) ) );
	if( Recording::Op* op = RecordDraw( f, Recording::REPLAY ) )
	{
		op->p = dl;
	}
	return Ok();
}

tr2c_status DisplayListDestroy( tr2c_display_list* dl )
{
	if( tr2c_status s = Guard( "display_list_destroy" ) )
	{
		return s;
	}
	if( dl )
	{
		NoteChanged( dl );
		Tr2Sprite2dDisplayList* p = reinterpret_cast<Tr2Sprite2dDisplayList*>( dl );
		CCP_DELETE p;
	}
	return Ok();
}

}

//////////////////////////////////////////////////////////////////////////
// Replay of a recorded gather (Tr2CSpriteHook::GatherSprites)

void tr2c::Play( tr2c_frame* f, const Recording& rec )
{
	CCP_ASSERT( !f->rec );
	for( const Recording::Op& op : rec.ops )
	{
		switch( op.kind )
		{
		case Recording::PUSH_TRANSLATION: PushTranslation( f, op.v[0], op.v[1] ); break;
		case Recording::POP_TRANSLATION: PopTranslation( f ); break;
		case Recording::PUSH_TRANSFORM: PushTransform( f, rec.floats.data() + op.off ); break;
		case Recording::POP_TRANSFORM: PopTransform( f ); break;
		case Recording::PUSH_CLIP: PushClip( f, op.v[0], op.v[1], op.v[2], op.v[3] ); break;
		case Recording::POP_CLIP: PopClip( f ); break;
		case Recording::SET_ALPHA: SetAlpha( f, op.v[0] ); break;
		case Recording::SET_EFFECT: SetEffect( f, int32_t( op.u ) ); break;
		case Recording::SET_BLEND: SetBlend( f, int32_t( op.u ) ); break;
		case Recording::SET_COLOR: SetColor( f, op.v[0], op.v[1], op.v[2], op.v[3] ); break;
		case Recording::SET_TEXTURE:
			SetTexture( f, op.u, const_cast<tr2c_texture*>( static_cast<const tr2c_texture*>( op.p ) ), op.n );
			break;
		case Recording::SET_TEXTURE_WINDOW: SetTextureWindow( f, op.u, op.v[0], op.v[1], op.v[2], op.v[3] ); break;
		case Recording::DRAW_QUAD:
			DrawQuad( f, op.v[0], op.v[1], op.v[2], op.v[3], op.n ? rec.floats.data() + op.off : nullptr );
			break;
		case Recording::DRAW_TRIANGLES:
			DrawTriangles( f, rec.verts.data() + op.off, op.n, rec.indices.data() + op.off2, op.n2 );
			break;
		case Recording::REPLAY: Replay( f, static_cast<const tr2c_display_list*>( op.p ) ); break;
		case Recording::TEXT_DRAW:
			TextDraw( f, const_cast<tr2c_text*>( static_cast<const tr2c_text*>( op.p ) ), op.v[0], op.v[1], op.v[2], op.v[3] );
			break;
		}
	}
}

namespace
{
// No C++ exception may cross the C ABI. Every table entry that can throw is wrapped:
// std::bad_alloc becomes TR2C_E_OUT_OF_MEMORY, anything else TR2C_E_ENGINE, both with error
// text. An exception inside a gather callback also spoils the recording being made, as the
// frame call may have stopped halfway. (On Windows the global new returns nullptr instead of
// throwing, so a failed vector growth there crashes before any catch; not handled here.)
template <typename Sig, Sig Fn>
struct Shield;

template <typename... A, tr2c_status ( *Fn )( A... )>
struct Shield<tr2c_status ( * )( A... ), Fn>
{
	static tr2c_status Call( A... a ) noexcept
	{
		try
		{
			return Fn( a... );
		}
		catch( const std::bad_alloc& )
		{
			SpoilRecording();
			return Fail( TR2C_E_OUT_OF_MEMORY, "tr2c: out of memory (std::bad_alloc)" );
		}
		catch( const std::exception& e )
		{
			SpoilRecording();
			return Fail( TR2C_E_ENGINE, "tr2c: C++ exception: %s", e.what() );
		}
		catch( ... )
		{
			SpoilRecording();
			return Fail( TR2C_E_ENGINE, "tr2c: unknown C++ exception" );
		}
	}
};

#define TR2C_SHIELD( fn ) Shield<decltype( &fn ), &fn>::Call

uint32_t DeviceGenerationFn()
{
	return DeviceGeneration();
}

const tr2c_api s_api = {
	sizeof( tr2c_api ),
	TR2C_API_VERSION,

	LastError, // noexcept: c_str()
	TR2C_SHIELD( Init ),

	TR2C_SHIELD( HookCreate ),
	TR2C_SHIELD( HookSetDirty ),
	TR2C_SHIELD( HookRelease ),

	TR2C_SHIELD( PushTranslation ),
	TR2C_SHIELD( PopTranslation ),
	TR2C_SHIELD( PushTransform ),
	TR2C_SHIELD( PopTransform ),
	TR2C_SHIELD( PushClip ),
	TR2C_SHIELD( PopClip ),
	TR2C_SHIELD( SetAlpha ),
	TR2C_SHIELD( SetEffect ),
	TR2C_SHIELD( SetBlend ),
	TR2C_SHIELD( SetColor ),
	TR2C_SHIELD( SetTexture ),
	TR2C_SHIELD( SetTextureWindow ),
	TR2C_SHIELD( DrawQuad ),
	TR2C_SHIELD( DrawTriangles ),
	TR2C_SHIELD( CaptureBegin ),
	TR2C_SHIELD( CaptureEnd ),
	TR2C_SHIELD( Replay ),
	TR2C_SHIELD( DisplayListDestroy ),
	DeviceGenerationFn, // noexcept: an atomic load

	TR2C_SHIELD( TextureLoad ),
	TR2C_SHIELD( TextureCreateBgra ),
	TR2C_SHIELD( TextureState ),
	TR2C_SHIELD( TextureRelease ),

	TR2C_SHIELD( TextCreate ),
	TR2C_SHIELD( TextSetFont ),
	TR2C_SHIELD( TextMetrics ),
	TR2C_SHIELD( TextMeasure ),
	TR2C_SHIELD( TextReset ),
	TR2C_SHIELD( TextAddLine ),
	TR2C_SHIELD( TextDraw ),
	TR2C_SHIELD( TextRelease ),
};
}

extern "C" TR2C_API const tr2c_api* tr2c_get_api( uint32_t version )
{
	if( version != TR2C_API_VERSION )
	{
		Fail( TR2C_E_VERSION, "tr2c_get_api: version %u is not supported (this build has %u)", version, TR2C_API_VERSION );
		return nullptr;
	}
	return &s_api;
}
