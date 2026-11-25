// Copyright © 2026 CCP ehf.

#pragma once
#ifndef Tr2CSpriteHook_h
#define Tr2CSpriteHook_h

#include "Sprite2d/Tr2SpriteObject.h"
#include "Tr2DeviceResource.h"
#include "tr2c.h"
#include "tr2c_internal.h"

BLUE_DECLARE( Tr2CSpriteHook );
BLUE_DECLARE( Tr2Sprite2dScene );

// The sprite object behind tr2c's hook_create (tr2c.h). It sits in a Tr2Sprite2dScene's
// children and, when the scene gathers its sprites, calls the tr2c_gather_fn it was
// created with. The scene gathers every render; the callback runs only when the hook is
// dirty (hook_set_dirty, creation, display on, device reset). On a clean gather the hook
// issues the frame calls it recorded from the last callback again instead. As a device resource it counts tr2c's device generation: a device reset
// releases every display list, so the C side must re-record them.
//
// Tr2DeviceResource is not the first base (see Tr2DeviceResource.h), as in
// EveSprite2dBracketRenderer.
class Tr2CSpriteHook : public Tr2SpriteObjectBase,
					   public Tr2DeviceResource
{
public:
	EXPOSE_TO_BLUE();
	Tr2CSpriteHook( IRoot* lockobj = nullptr );
	~Tr2CSpriteHook();

	void SetCallback( tr2c_gather_fn fn, void* user );

	// Gathers since creation that called back, and that replayed the recording instead.
	uint32_t GetCallbackCount() const { return m_callbacks; }
	uint32_t GetReplayCount() const { return m_replays; }

	// The scene this hook was attached to by tr2c (one reference held).
	Tr2Sprite2dScene* GetScene() const;
	void SetScene( Tr2Sprite2dScene* scene );

	//////////////////////////////////////////////////////////////////////////
	// ITr2SpriteObject
	void GatherSprites( Tr2Sprite2dScene* renderer ) override;
	ITr2SpriteObject* PickPoint( float x, float y, Tr2Sprite2dScene* renderer ) override;

	//////////////////////////////////////////////////////////////////////////
	// ITriDeviceResource
	void ReleaseResources( TriStorage s ) override;

private:
	bool OnPrepareResources() override;

private:
	tr2c_gather_fn m_fn;
	void* m_user;
	Tr2Sprite2dScenePtr m_scene;
	tr2c::Recording m_recording;
	uint32_t m_callbacks = 0;
	uint32_t m_replays = 0;
};

TYPEDEF_BLUECLASS( Tr2CSpriteHook );

#endif // Tr2CSpriteHook_h
