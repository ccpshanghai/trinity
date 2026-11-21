// Copyright © 2026 CCP ehf.

#pragma once
#ifndef Tr2CSpriteHook_h
#define Tr2CSpriteHook_h

#include "Sprite2d/Tr2SpriteObject.h"
#include "Tr2DeviceResource.h"
#include "tr2c.h"

BLUE_DECLARE( Tr2CSpriteHook );
BLUE_DECLARE( Tr2Sprite2dScene );

// The sprite object behind tr2c's hook_create (tr2c.h). It sits in a Tr2Sprite2dScene's
// children and, when the scene gathers its sprites, calls the tr2c_gather_fn it was
// created with. As a device resource it counts tr2c's device generation: a device reset
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
};

TYPEDEF_BLUECLASS( Tr2CSpriteHook );

#endif // Tr2CSpriteHook_h
