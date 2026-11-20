// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#include "Tr2CSpriteHook.h"
#include "tr2c_internal.h"
#include "Sprite2d/Tr2Sprite2dScene.h"

Tr2CSpriteHook::Tr2CSpriteHook( IRoot* lockobj /*= nullptr */ ) :
	m_fn( nullptr ),
	m_user( nullptr )
{
}

Tr2CSpriteHook::~Tr2CSpriteHook()
{
}

void Tr2CSpriteHook::SetCallback( tr2c_gather_fn fn, void* user )
{
	m_fn = fn;
	m_user = user;
}

Tr2Sprite2dScene* Tr2CSpriteHook::GetScene() const
{
	return m_scene;
}

void Tr2CSpriteHook::SetScene( Tr2Sprite2dScene* scene )
{
	m_scene = scene;
}

void Tr2CSpriteHook::GatherSprites( Tr2Sprite2dScene* renderer )
{
	if( !m_display || !m_fn || !renderer )
	{
		return;
	}

	tr2c_frame frame;
	frame.scene = renderer;
	frame.hook = this;

	// hook_release refuses to run inside this callback, so the hook outlives it.
	tr2c::BeginFrame( &frame );
	m_fn( &frame, m_user );
	tr2c::EndFrame( &frame );

	m_isDirty = false;
}

ITr2SpriteObject* Tr2CSpriteHook::PickPoint( float x, float y, Tr2Sprite2dScene* renderer )
{
	// carbon-ui-core does its own hit testing.
	return nullptr;
}

void Tr2CSpriteHook::ReleaseResources( TriStorage s )
{
	// Display lists recorded through this hook are gone (Tr2Sprite2dDisplayList::ReleaseResources),
	// and the font manager drops its glyph atlas textures: tell the C side.
	tr2c::BumpDeviceGeneration();
	SetDirty();
}

bool Tr2CSpriteHook::OnPrepareResources()
{
	return true;
}
