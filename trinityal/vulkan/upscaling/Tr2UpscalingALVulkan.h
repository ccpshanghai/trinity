// Copyright © 2026 CCP ehf.

#pragma once
#include "../include/upscaling/Tr2UpscalingAL.h"

#if TRINITY_PLATFORM == TRINITY_VULKAN
namespace TrinityALImpl
{
static const std::vector<Tr2UpscalingAL::Technique> AVAILABLE_UPSCALING_TECHNIQUES = {};
Tr2UpscalingTechniqueAL* CreateUpscalingTechnique( Tr2UpscalingAL::Technique technique, Tr2UpscalingAL::Setting setting, bool frameGeneration, uint32_t adapter );

}
#endif