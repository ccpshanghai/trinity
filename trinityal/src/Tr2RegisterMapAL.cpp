// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include "../include/Tr2RegisterMapAL.h"
#include "../include/Tr2ShaderAL.h"


namespace
{
// An array register occupies registerIndex .. registerIndex + arrayCount - 1, and every element
// gets its own slot, so that SetSrv( stage, registerIndex + k, ... ) can name element k. Upstream
// maps only registerIndex, so no backend can bind element k of an array through the register map,
// whatever its own layout does with arrayCount (fork item 12, site 4 of 4; carried here from the
// fork's Tr2ResourceSetAL.cpp, which upstream #90 removed). Elements past the table's end are not
// mapped rather than written out of bounds.
template <typename Table, typename Count>
void MapElements( Table& table, uint32_t stage, const Tr2ShaderRegisterAL& reg, Count& count )
{
	const uint32_t elements = reg.arrayCount ? reg.arrayCount : 1;
	for( uint32_t k = 0; k < elements && reg.registerIndex + k < Tr2RegisterMapAL::MAX_RESOURCES_IN_STAGE; ++k )
	{
		table[stage][reg.registerIndex + k] = uint8_t( count++ );
	}
}
}

Tr2RegisterMapAL::Tr2RegisterMapAL() : // cppcheck-suppress uninitMemberVar
	srvCount( 0 ),
	uavCount( 0 ),
	samplerCount( 0 )
{
}

Tr2RegisterMapAL::Tr2RegisterMapAL( Tr2RenderContextEnum::ShaderType stage, const Tr2ShaderSignatureAL& signature ) :
	srvCount( 0 ),
	uavCount( 0 ),
	samplerCount( 0 )
{
	memset( srvs, -1, sizeof( srvs ) );
	memset( uavs, -1, sizeof( uavs ) );
	memset( samplers, -1, sizeof( samplers ) );

	for( auto it = begin( signature.registers ); it != end( signature.registers ); ++it )
	{
		if( it->IsSrv() )
		{
			MapElements( srvs, stage, *it, srvCount );
		}
		else if( it->IsUav() )
		{
			MapElements( uavs, stage, *it, uavCount );
		}
		else if( it->registerType == Tr2ShaderRegisterAL::SAMPLER )
		{
			MapElements( samplers, stage, *it, samplerCount );
		}
	}
}

Tr2RegisterMapAL::Tr2RegisterMapAL( const Tr2ShaderAL* shaders, size_t shaderCount ) :
	srvCount( 0 ),
	uavCount( 0 ),
	samplerCount( 0 )
{
	memset( srvs, -1, sizeof( srvs ) );
	memset( uavs, -1, sizeof( uavs ) );
	memset( samplers, -1, sizeof( samplers ) );

	for( size_t i = 0; i < shaderCount; ++i )
	{
		auto shaderType = shaders[i].GetType();
		auto& signature = shaders[i].GetSignature();
		for( auto it = begin( signature.registers ); it != end( signature.registers ); ++it )
		{
			if( it->IsSrv() )
			{
				MapElements( srvs, shaderType, *it, srvCount );
			}
			else if( it->IsUav() )
			{
				MapElements( uavs, shaderType, *it, uavCount );
			}
			else if( it->registerType == Tr2ShaderRegisterAL::SAMPLER )
			{
				MapElements( samplers, shaderType, *it, samplerCount );
			}
		}
	}
}

Tr2RegisterMapAL::Tr2RegisterMapAL( const Tr2RenderContextEnum::ShaderType* shaders, const Tr2ShaderSignatureAL* signatures, size_t signatureCount ) :
	srvCount( 0 ),
	uavCount( 0 ),
	samplerCount( 0 )
{
	memset( srvs, -1, sizeof( srvs ) );
	memset( uavs, -1, sizeof( uavs ) );
	memset( samplers, -1, sizeof( samplers ) );

	for( size_t i = 0; i < signatureCount; ++i )
	{
		for( auto it = begin( signatures[i].registers ); it != end( signatures[i].registers ); ++it )
		{
			if( it->IsSrv() )
			{
				MapElements( srvs, shaders[i], *it, srvCount );
			}
			else if( it->IsUav() )
			{
				MapElements( uavs, shaders[i], *it, uavCount );
			}
			else if( it->registerType == Tr2ShaderRegisterAL::SAMPLER )
			{
				MapElements( samplers, shaders[i], *it, samplerCount );
			}
		}
	}
}

bool Tr2RegisterMapAL::operator==( const Tr2RegisterMapAL& other ) const
{
	if( srvCount != other.srvCount || uavCount != other.uavCount || samplerCount != other.samplerCount )
	{
		return false;
	}
	if( srvCount > 0 && memcmp( srvs, other.srvs, sizeof( srvs ) ) )
	{
		return false;
	}
	if( uavCount > 0 && memcmp( uavs, other.uavs, sizeof( uavs ) ) )
	{
		return false;
	}
	if( samplerCount > 0 && memcmp( samplers, other.samplers, sizeof( samplers ) ) )
	{
		return false;
	}
	return true;
}

bool Tr2RegisterMapAL::operator!=( const Tr2RegisterMapAL& other ) const
{
	return !( *this == other );
}
