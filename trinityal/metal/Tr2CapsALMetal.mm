// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#if ( TRINITY_PLATFORM == TRINITY_METAL )

#include "Tr2CapsALMetal.h"

bool Tr2CapsAL::SupportsFloat16() const
{
	return true;
}

bool Tr2CapsAL::SupportsGpuBuffer() const
{
	return true;
}

bool Tr2CapsAL::SupportsStandaloneSwapChain() const
{
	return true;
}

bool Tr2CapsAL::SupportsVertexShaderTextures() const
{
	return true;
}

bool Tr2CapsAL::SupportsVariableRefreshRate() const
{
	return false;
}

bool Tr2CapsAL::SupportsRaytracing() const
{
	return m_supportsRaytracing;
}

bool Tr2CapsAL::SupportsAstcTextures() const
{
	return m_supportsAstcTextures;
}

Tr2CapsAL::Tr2CapsAL() : m_supportsRaytracing( false ), m_supportsAstcTextures( false )
{
}

Tr2CapsAL::Tr2CapsAL( const Tr2CapsAL& other ) :
	m_supportsRaytracing( other.m_supportsRaytracing ),
	m_supportsAstcTextures( other.m_supportsAstcTextures )
{
}

Tr2CapsAL& Tr2CapsAL::operator=( const Tr2CapsAL& other )
{
	m_supportsRaytracing = other.m_supportsRaytracing;
	m_supportsAstcTextures = other.m_supportsAstcTextures;
	return *this;
}

#endif
