// Copyright © 2026 CCP ehf.

#include "StdAfx.h"
#include "TriStepBeginRenderPass.h"
#include "TriRenderStep.h"


BLUE_DEFINE( TriStepBeginRenderPass );

const Be::ClassInfo* TriStepBeginRenderPass::ExposeToBlue()
{
	EXPOSURE_BEGIN( TriStepBeginRenderPass, "" )

		MAP_INTERFACE( TriRenderStep )
		MAP_INTERFACE( TriStepBeginRenderPass )

	EXPOSURE_CHAINTO( TriRenderStep )
}
