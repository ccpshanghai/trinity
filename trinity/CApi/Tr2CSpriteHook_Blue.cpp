// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#include "Tr2CSpriteHook.h"

BLUE_DEFINE( Tr2CSpriteHook );

const Be::ClassInfo* Tr2CSpriteHook::ExposeToBlue()
{
	EXPOSURE_BEGIN( Tr2CSpriteHook, "Sprite object that calls back into the tr2c engine C interface (carbon-ui-core) when the scene gathers its sprites" )
		MAP_INTERFACE( ITr2SpriteObject )
		MAP_INTERFACE( Tr2CSpriteHook )
	EXPOSURE_CHAINTO( Tr2SpriteObjectBase )
}
