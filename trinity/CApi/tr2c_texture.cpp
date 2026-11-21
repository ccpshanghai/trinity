// Copyright © 2026 CCP ehf.
//
// tr2c textures (tr2c.h). A tr2c_texture* is a Tr2AtlasTexture* holding one reference.

#include "StdAfx.h"
#include "tr2c_internal.h"
#include "Tr2TextureAtlasMan.h"
#include "Tr2TextureAtlas.h"
#include "Tr2AtlasTexture.h"

#include <cstring>
#include <string>

using namespace Tr2RenderContextEnum;

namespace tr2c
{
namespace
{
Tr2AtlasTexture* TexOf( tr2c_texture* t )
{
	return reinterpret_cast<Tr2AtlasTexture*>( t );
}
}

tr2c_status TextureLoad( const char* res_path, uint32_t len, tr2c_texture** out )
{
	if( tr2c_status s = Guard( "texture_load" ) )
	{
		return s;
	}
	if( !out || !res_path || !len )
	{
		return Fail( TR2C_E_INVALID_ARG, "texture_load: null or empty argument" );
	}
	*out = nullptr;
	const std::string path( res_path, len );
	if( path.find( '\0' ) != std::string::npos )
	{
		return Fail( TR2C_E_INVALID_ARG, "texture_load: NUL in path" );
	}
	// Asynchronous: texture_state reports LOADING until the resource manager is done.
	Tr2AtlasTexturePtr tex;
	BeResMan->GetResource( path, "atlas", tex );
	if( !tex )
	{
		return Fail( TR2C_E_NOT_FOUND, "texture_load: no resource for '%s'", path.c_str() );
	}
	*out = reinterpret_cast<tr2c_texture*>( tex.Detach() );
	return Ok();
}

tr2c_status TextureCreateBgra( uint32_t w, uint32_t h, const void* pixels, uint32_t pitch, tr2c_texture** out )
{
	if( tr2c_status s = Guard( "texture_create_bgra" ) )
	{
		return s;
	}
	if( !out || !pixels || !w || !h )
	{
		return Fail( TR2C_E_INVALID_ARG, "texture_create_bgra: null argument or empty size" );
	}
	*out = nullptr;
	if( uint64_t( pitch ) < uint64_t( w ) * 4 )
	{
		return Fail( TR2C_E_INVALID_ARG, "texture_create_bgra: pitch %u is less than width * 4 (%u)", pitch, w * 4 );
	}
	// Same path as Tr2FontManager's glyph upload.
	Tr2TextureAtlas* atlas = g_textureAtlasMan ? g_textureAtlasMan->FindAtlas( PIXEL_FORMAT_B8G8R8A8_UNORM ) : nullptr;
	if( !atlas )
	{
		return Fail( TR2C_E_ENGINE, "texture_create_bgra: no BGRA texture atlas" );
	}
	Tr2AtlasTexturePtr tex;
	atlas->CreateTexture( w, h, Tr2TextureAtlas::ATT_DEFAULT, &tex );
	if( !tex )
	{
		return Fail( TR2C_E_ENGINE, "texture_create_bgra: CreateTexture(%u, %u) failed", w, h );
	}
	void* dest = nullptr;
	unsigned int destPitch = 0;
	if( !tex->LockBuffer( dest, destPitch ) || !dest )
	{
		return Fail( TR2C_E_ENGINE, "texture_create_bgra: LockBuffer failed" );
	}
	const size_t rowBytes = size_t( w ) * 4;
	if( destPitch < rowBytes )
	{
		tex->UnlockBuffer();
		return Fail( TR2C_E_ENGINE, "texture_create_bgra: locked pitch %u is less than %zu", destPitch, rowBytes );
	}
	for( uint32_t y = 0; y < h; ++y )
	{
		std::memcpy( static_cast<uint8_t*>( dest ) + size_t( y ) * destPitch, static_cast<const uint8_t*>( pixels ) + size_t( y ) * pitch, rowBytes );
	}
	tex->UnlockBuffer();
	*out = reinterpret_cast<tr2c_texture*>( tex.Detach() );
	return Ok();
}

tr2c_status TextureState( tr2c_texture* t, int32_t* out_state, uint32_t* out_w, uint32_t* out_h )
{
	if( tr2c_status s = Guard( "texture_state" ) )
	{
		return s;
	}
	if( !t || !out_state )
	{
		return Fail( TR2C_E_INVALID_ARG, "texture_state: null argument" );
	}
	Tr2AtlasTexture* tex = TexOf( t );
	uint32_t w = 0;
	uint32_t h = 0;
	if( tex->IsLoading() )
	{
		*out_state = TR2C_TEXTURE_LOADING;
	}
	else if( tex->IsGood() || ( tex->GetTexture() && tex->GetWidth() ) )
	{
		// A texture made with texture_create_bgra has its pixels without going through a load.
		*out_state = TR2C_TEXTURE_READY;
		w = tex->GetWidth();
		h = tex->GetHeight();
	}
	else
	{
		*out_state = TR2C_TEXTURE_FAILED;
	}
	if( out_w )
	{
		*out_w = w;
	}
	if( out_h )
	{
		*out_h = h;
	}
	return Ok();
}

tr2c_status TextureRelease( tr2c_texture* t )
{
	if( tr2c_status s = Guard( "texture_release" ) )
	{
		return s;
	}
	if( t )
	{
		Tr2AtlasTexturePtr tex;
		tex.Attach( TexOf( t ) ); // drops the handle's reference
	}
	return Ok();
}
}
