// Copyright © 2026 CCP ehf.

#include "stdafx.h"
#if _WIN32
#include "EffectCompilerVulkan.h"
#include "Macro.h"
#include "Platforms.h"

#include "CompileMessageQueue.h"
extern CompileMessageQueue g_messages;

bool EffectCompilerVulkan::Create()
{
	ZoneScoped;

	return m_compiler.Create();
}

bool EffectCompilerVulkan::CompileEffect( const char* source, size_t sourceLength, const std::vector<Macro>& defines, EffectData& result, IWorkQueue* workQueue )
{
	std::vector<Macro> newDefines = defines;
	if( auto define = FindMacro( newDefines, "PLATFORM" ) )
	{
		define->value = GetPlatformIdString( PLATFORM_VULKAN );
	}
	else
	{
		newDefines.push_back( { "PLATFORM", GetPlatformIdString( PLATFORM_VULKAN ) } );
	}
	return m_compiler.CompileEffect( source, sourceLength, newDefines, result, { "6_0", true, false, true }, workQueue );
}
#endif
