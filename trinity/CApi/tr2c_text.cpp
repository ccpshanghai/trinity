// Copyright © 2026 CCP ehf.
//
// tr2c text (tr2c.h) over Tr2FontMeasurer, using the methods added to it for tr2c
// (AddCodepoints, GetPendingAdvances, GetFontMetrics, SetCursorX, SetLimit, SetLetterSpace).

#include "StdAfx.h"
#include "tr2c_internal.h"
#include "Font/Tr2FontMeasurer.h"
#include "Sprite2d/Tr2Sprite2dScene.h"

#include <cmath>
#include <memory>
#include <new>
#include <string>
#include <vector>

// A tr2c_text owns one reference to its OTr2FontMeasurer plus what text_draw needs to know
// to prepare sprites only when something changed.
struct tr2c_text
{
	Tr2FontMeasurerPtr measurer;
	bool dirty = true;              // lines, font or reset since the last PrepareSprites
	float color[4] = { 0, 0, 0, 0 }; // colour of the last PrepareSprites
	uint32_t generation = 0;        // device generation of the last PrepareSprites
};

namespace tr2c
{
namespace
{
tr2c_status CheckText( const char* fn, tr2c_text* t )
{
	if( tr2c_status s = Guard( fn ) )
	{
		return s;
	}
	if( !t || !t->measurer )
	{
		return Fail( TR2C_E_INVALID_ARG, "%s: null text", fn );
	}
	return TR2C_OK;
}

thread_local std::vector<uint32_t> t_codepoints;
}

tr2c_status TextCreate( tr2c_text** out )
{
	if( tr2c_status s = Guard( "text_create" ) )
	{
		return s;
	}
	if( !out )
	{
		return Fail( TR2C_E_INVALID_ARG, "text_create: null out" );
	}
	*out = nullptr;
	std::unique_ptr<tr2c_text> t( new( std::nothrow ) tr2c_text );
	if( !t )
	{
		return Fail( TR2C_E_OUT_OF_MEMORY, "text_create: out of memory" );
	}
	// O-type: constructed with one reference, which the wrapper owns. No extra Lock.
	// If this throws, t is freed and the table entry's catch reports TR2C_E_OUT_OF_MEMORY.
	t->measurer.Attach( new OTr2FontMeasurer );
	if( !t->measurer )
	{
		return Fail( TR2C_E_OUT_OF_MEMORY, "text_create: out of memory" ); // Windows: new returns nullptr
	}
	// m_limit is not initialised by the constructor; letter space as CarbonUI and Chrome (0).
	t->measurer->SetLimit( 0 );
	t->measurer->SetLetterSpace( 0 );
	t->generation = DeviceGeneration();
	*out = t.release();
	return Ok();
}

tr2c_status TextSetFont( tr2c_text* t, const char* res_path, uint32_t len, float size_px )
{
	if( tr2c_status s = CheckText( "text_set_font", t ) )
	{
		return s;
	}
	if( !res_path || !len )
	{
		return Fail( TR2C_E_INVALID_ARG, "text_set_font: null or empty font path" );
	}
	if( !std::isfinite( size_px ) || size_px < 0.5f || size_px > 4096.0f )
	{
		return Fail( TR2C_E_INVALID_ARG, "text_set_font: font size %g out of range", double( size_px ) );
	}
	const std::string path( res_path, len );
	if( path.find( '\0' ) != std::string::npos )
	{
		return Fail( TR2C_E_INVALID_ARG, "text_set_font: NUL in font path" );
	}
	NoteChanged( t );
	t->measurer->SetFont( path );
	t->measurer->SetFontSize( int( std::lround( size_px ) ) );
	t->dirty = true;
	return Ok();
}

tr2c_status TextMetrics( tr2c_text* t, tr2c_font_metrics* out )
{
	if( tr2c_status s = CheckText( "text_metrics", t ) )
	{
		return s;
	}
	if( !out )
	{
		return Fail( TR2C_E_INVALID_ARG, "text_metrics: null out" );
	}
	int ascender = 0;
	int descender = 0;
	if( !t->measurer->GetFontMetrics( ascender, descender ) )
	{
		return Fail( TR2C_E_NOT_FOUND, "text_metrics: font '%s' (and the fallback) not found", t->measurer->GetFont().c_str() );
	}
	out->ascender = float( ascender );
	out->descender = float( descender );
	out->line_height = float( ascender - descender );
	return Ok();
}

tr2c_status TextMeasure( tr2c_text* t, const char* utf8, uint32_t len, float* out_adv, uint32_t cap, uint32_t* out_count )
{
	if( tr2c_status s = CheckText( "text_measure", t ) )
	{
		return s;
	}
	if( !out_count || ( cap && !out_adv ) )
	{
		return Fail( TR2C_E_INVALID_ARG, "text_measure: null argument" );
	}
	*out_count = 0;
	if( tr2c_status s = DecodeUtf8( "text_measure", utf8, len, t_codepoints ) )
	{
		return s;
	}
	if( t_codepoints.empty() || !cap )
	{
		return Ok();
	}
	Tr2FontMeasurer* m = t->measurer;
	// Committed lines are untouched; the run is cancelled again (which restores the cursor).
	if( m->HasUncommittedText() )
	{
		// Not reached through tr2c (add_line commits, measure cancels). CancelLastText could
		// not tell our run from that one, so refuse.
		return Fail( TR2C_E_STATE, "text_measure: the measurer has uncommitted text" );
	}
	m->SetCursorX( 0 );
	m->AddCodepoints( t_codepoints.data(), uint32_t( t_codepoints.size() ) );
	// AddText can return before it builds a run (no face found), so check before reading.
	if( m->HasUncommittedText() )
	{
		*out_count = m->GetPendingAdvances( out_adv, cap );
		m->CancelLastText();
	}
	return Ok();
}

tr2c_status TextReset( tr2c_text* t )
{
	if( tr2c_status s = CheckText( "text_reset", t ) )
	{
		return s;
	}
	NoteChanged( t );
	t->measurer->Reset();
	t->dirty = true;
	return Ok();
}

tr2c_status TextAddLine( tr2c_text* t, const char* utf8, uint32_t len, int32_t x, int32_t y )
{
	if( tr2c_status s = CheckText( "text_add_line", t ) )
	{
		return s;
	}
	if( tr2c_status s = DecodeUtf8( "text_add_line", utf8, len, t_codepoints ) )
	{
		return s;
	}
	if( t_codepoints.empty() )
	{
		return Ok();
	}
	NoteChanged( t );
	Tr2FontMeasurer* m = t->measurer;
	// CommitText does not reset the cursor: without this every line after the first moves right.
	m->SetCursorX( 0 );
	m->AddCodepoints( t_codepoints.data(), uint32_t( t_codepoints.size() ) );
	if( m->HasUncommittedText() )
	{
		// y is the baseline (line top + ascender).
		m->CommitText( x, y );
	}
	t->dirty = true;
	return Ok();
}

tr2c_status TextDraw( tr2c_frame* f, tr2c_text* t, float r, float g, float b, float a )
{
	if( tr2c_status s = CheckFrame( "text_draw", f ) )
	{
		return s;
	}
	if( !t || !t->measurer )
	{
		return Fail( TR2C_E_INVALID_ARG, "text_draw: null text" );
	}
	Tr2FontMeasurer* m = t->measurer;
	const uint32_t generation = DeviceGeneration();
	if( generation != t->generation )
	{
		// A device reset clears the glyph atlas: drop the prepared sprites (as
		// Tr2Sprite2dTextObject::ReleaseResources does).
		m->ClearSprites();
		t->generation = generation;
		t->dirty = true;
	}
	if( t->dirty || t->color[0] != r || t->color[1] != g || t->color[2] != b || t->color[3] != a )
	{
		// Position comes from push_translation: the scene applies it in ProcessVertices.
		m->PrepareSprites(
			f->scene,
			Vector2( 0.0f, 0.0f ),
			Color( r, g, b, a ),
			TR2_SFX_FONT,
			TR2_SBM_BLEND,
			Tr2SpriteTarget::COLOR,
			1.0f,
			false,
			Vector2( 0.0f, 0.0f ),
			Color( 0.0f, 0.0f, 0.0f, 0.0f ),
			TR2_SFX_FONT );
		t->color[0] = r;
		t->color[1] = g;
		t->color[2] = b;
		t->color[3] = a;
		t->dirty = false;
	}
	// The glyph sprites are single-texture FONT sprites; leave the scene in that state.
	f->scene->SetSpriteEffect( TR2_SFX_FONT );
	f->effect = TR2C_EFFECT_FONT;
	m->SubmitSprites( f->scene );
	if( f->rec )
	{
		// Inside a capture the glyphs went into the display list; only the effect change is
		// seen outside it.
		Recording::Op& op = f->rec->ops.emplace_back();
		op.kind = f->capturing ? Recording::SET_EFFECT : Recording::TEXT_DRAW;
		op.u = TR2C_EFFECT_FONT;
		op.p = t;
		if( f->capturing )
		{
			op.p = nullptr;
		}
		op.v[0] = r;
		op.v[1] = g;
		op.v[2] = b;
		op.v[3] = a;
	}
	return Ok();
}

tr2c_status TextRelease( tr2c_text* t )
{
	if( tr2c_status s = Guard( "text_release" ) )
	{
		return s;
	}
	NoteChanged( t );
	delete t; // drops the measurer's reference
	return Ok();
}
}
