// Copyright © 2024 CCP ehf.

#include "StdAfx.h"
#if ( TRINITY_PLATFORM == TRINITY_METAL )
#include <TargetConditionals.h>

#include "Tr2UpscalingALMetal.h"
#include "Tr2MetalFxUpscaling.h"
#include "../../include/upscaling/Tr2Fsr1Upscaling.h"

namespace TrinityALImpl
{
Tr2UpscalingTechniqueAL* CreateUpscalingTechnique( Tr2RenderContextAL& renderContext,
												   Tr2UpscalingAL::Technique technique,
												   Tr2UpscalingAL::Setting setting,
												   bool frameGeneration,
												   uint32_t adapter )
{
	Tr2UpscalingTechniqueAL* tech = nullptr;
	switch( technique )
	{
#if !TARGET_OS_SIMULATOR
	// No MetalFX framework on the iOS Simulator SDK (see Tr2MetalFxUpscaling.h) --
	// METALFX falls through to the default case there and CreateUpscalingTechnique
	// returns nullptr, same as any other technique IsAvailable() rejects.
	case Tr2UpscalingAL::Technique::METALFX:
		tech = new Tr2MetalFxUpscalingTechnique( renderContext, technique, setting, frameGeneration, adapter );
		break;
#endif
	case Tr2UpscalingAL::Technique::FSR1:
		tech = new Tr2Fsr1UpscalingTechnique( renderContext, technique, setting, frameGeneration, adapter );
		break;
	default:
		break;
	}

	if( tech && !tech->IsAvailable() )
	{
		delete tech;
		tech = nullptr;
	}
	return tech;
}
}


#endif
