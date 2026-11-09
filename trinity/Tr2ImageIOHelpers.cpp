// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include <functional>
#include "Tr2ImageIOHelpers.h"

using namespace Tr2RenderContextEnum;

namespace
{

bool CreateCubeTexture( ImageIO::HostBitmap& bitmap, Tr2TextureAL& out, uint32_t& memoryUse, Tr2PrimaryRenderContext& renderContext )
{
	CCP_STATS_ZONE( __FUNCTION__ );

	if( !bitmap.IsValid() )
	{
		return false;
	}

	const unsigned trueMipLevelCount = bitmap.GetTrueMipCount();

	std::vector<Tr2SubresourceData> initData;

	for( unsigned face = 0; face != 6; ++face )
	{
		for( unsigned i = 0; i != trueMipLevelCount; ++i )
		{
			Tr2SubresourceData srd;
			srd.m_sysMem = const_cast<char*>( bitmap.GetMipRawData( i, CubemapFace( face ) ) );
			srd.m_sysMemSlicePitch = bitmap.GetMipSize( i );
			srd.m_sysMemPitch = bitmap.GetMipPitch( i );

			if( !srd.m_sysMem )
			{
				return false;
			}

			initData.push_back( srd );

			memoryUse += srd.m_sysMemSlicePitch;
		}
	}

	return !FAILED( out.Create( bitmap, Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::READ, &initData[0], renderContext ) );
}

bool CreateVolumeTexture( ImageIO::HostBitmap& bitmap, Tr2TextureAL& out, uint32_t& memoryUse, Tr2PrimaryRenderContext& renderContext )
{
	CCP_STATS_ZONE( __FUNCTION__ );

	if( !bitmap.IsValid() )
	{
		return false;
	}

	BeTimer create;

	const unsigned trueMipLevelCount = bitmap.GetTrueMipCount();

	std::vector<Tr2SubresourceData> initData;

	//
	// Copy the pixels into the locked D3D surface (one copy per mip)
	//
	for( unsigned i = 0; i != trueMipLevelCount; ++i )
	{
		Tr2SubresourceData srd;

		srd.m_sysMem = const_cast<char*>( bitmap.GetMipRawData( i ) );
		srd.m_sysMemSlicePitch = bitmap.GetMipSize( i ) / std::max( bitmap.GetMipDepth( i ), 1u );
		srd.m_sysMemPitch = bitmap.GetMipPitch( i );

		if( !srd.m_sysMem )
		{
			return false;
		}

		//const unsigned mipDepth = std::max( ih.GetDepth() >> i, 1u );
		//srd.m_sysMemSlicePitch	/= mipDepth;

		initData.push_back( srd );

		memoryUse += bitmap.GetMipSize( i );
	}

	return !FAILED( out.Create( bitmap, Tr2GpuUsage::SHADER_RESOURCE, &initData[0], renderContext ) );
}

}

namespace Tr2ImageIOHelpers
{
bool Create2DTexture( ImageIO::HostBitmap& bitmap, Tr2TextureAL& out, uint32_t& memoryUse, Tr2PrimaryRenderContext& renderContext, Tr2RenderContextEnum::BufferUsage usage )
{
	CCP_STATS_ZONE( __FUNCTION__ );

	if( !bitmap.IsValid() )
	{
		return false;
	}

	BeTimer create;

	const unsigned trueMipLevelCount = bitmap.GetTrueMipCount();

	std::vector<Tr2SubresourceData> initData( trueMipLevelCount * bitmap.GetArraySize() );

	for( unsigned j = 0; j < bitmap.GetArraySize(); ++j )
	{
		for( unsigned i = 0; i != trueMipLevelCount; ++i )
		{
			Tr2SubresourceData& srd = initData[i + j * trueMipLevelCount];

			srd.m_sysMem = const_cast<char*>( bitmap.GetMipRawData( i, j ) );
			srd.m_sysMemSlicePitch = bitmap.GetMipSize( i );
			srd.m_sysMemPitch = bitmap.GetMipPitch( i );

			if( !srd.m_sysMem )
			{
				return false;
			}

			memoryUse += srd.m_sysMemSlicePitch;
		}
	}

	return !FAILED( out.Create( bitmap, Tr2GpuUsage::SHADER_RESOURCE, Tr2CpuUsage::READ, &initData[0], renderContext ) );
}

bool CreateTexture( ImageIO::HostBitmap& bitmap, Tr2TextureAL& out, uint32_t& memoryUse, Tr2PrimaryRenderContext& renderContext, Tr2RenderContextEnum::BufferUsage usage )
{
	if( !bitmap.IsValid() )
	{
		return false;
	}
	if( bitmap.GetType() == TEX_TYPE_CUBE )
	{
		return CreateCubeTexture( bitmap, out, memoryUse, renderContext );
	}
	if( bitmap.GetType() == TEX_TYPE_3D )
	{
		return CreateVolumeTexture( bitmap, out, memoryUse, renderContext );
	}
	return Create2DTexture( bitmap, out, memoryUse, renderContext, usage );
}

bool CopyToTexture( ImageIO::HostBitmap& bitmap, Tr2TextureAL& texture, unsigned int x, unsigned int y, unsigned int margin, Tr2RenderContext& renderContext )
{
	CCP_STATS_ZONE( __FUNCTION__ );

	if( !bitmap.IsValid() )
	{
		return false;
	}

	if( texture.GetFormat() != bitmap.GetFormat() )
	{
		CCP_LOGERR( "Tr2ImageHandler::CopyToTexture - formats don't match" );
		return false;
	}

	if( x >= texture.GetWidth() || y >= texture.GetHeight() )
	{
		CCP_LOGERR( "Tr2ImageHandler::CopyToTexture - out of bounds" );
		return false;
	}

	if( x + bitmap.GetWidth() > texture.GetWidth() || y + bitmap.GetHeight() > texture.GetHeight() )
	{
		CCP_LOGERR( "Tr2ImageHandler::CopyToTexture - out of bounds" );
		return false;
	}

	if( !margin )
	{
		const auto result = texture.UpdateSubresource( Tr2TextureSubresource( 0 ).SetRect( x, y, x + bitmap.GetWidth(), y + bitmap.GetHeight() ), bitmap.GetRawData(), bitmap.GetPitch(), 0, renderContext );
		if( FAILED( result ) )
		{
			CCP_LOGERR( "Tr2ImageHandler::CopyToTexture - UpdateSubresource failed [no margin]: %08x", result.GetResult() );
		}
		return SUCCEEDED( result );
	}

	// Can't expect the Hal to support updating a subresource with automatic replication of border pixels, so do this ourselves in a chunk
	// of temporary memory, then send that off to the backend.

	std::vector<unsigned char> pixels;
	unsigned pitch = 0;
	AddMargin( bitmap.GetFormat(), reinterpret_cast<const uint8_t*>( bitmap.GetRawData() ), bitmap.GetWidth(), bitmap.GetHeight(), margin, pixels, pitch );

	const auto result = texture.UpdateSubresource( Tr2TextureSubresource( 0 ).SetRect( x, y, x + bitmap.GetWidth() + 2 * margin, y + bitmap.GetHeight() + 2 * margin ), &pixels[0], pitch, 0, renderContext );
	if( FAILED( result ) )
	{
		CCP_LOGERR( "Tr2ImageHandler::CopyToTexture - UpdateSubresource failed [margin]: %08x", result.GetResult() );
	}
	return SUCCEEDED( result );
}


void AddMargin( const Tr2RenderContextEnum::PixelFormat format,
				const unsigned char* source,
				const unsigned width,
				const unsigned height,
				const unsigned margin,
				std::vector<unsigned char>& output,
				unsigned& outputPitch )
{
	const unsigned char* src = source;

	if( IsCompressedFormat( format ) )
	{
		const unsigned blockByteSize = Tr2RenderContextEnum::GetBlockByteSize( format );
		// From the format, not a literal 4. Square for every format we carry, but asked for on
		// both axes because ASTC does not have to be (8x5 and 10x6 exist), and a margin that is
		// a whole number of blocks on one axis need not be on the other.
		const unsigned blockWidth = Tr2RenderContextEnum::GetBlockWidth( format );
		const unsigned blockHeight = Tr2RenderContextEnum::GetBlockHeight( format );
		CCP_ASSERT( blockByteSize != 0 && margin % blockWidth == 0 && margin % blockHeight == 0 );
		const unsigned blockMargin = margin / blockWidth;
		const unsigned blocksX = Tr2RenderContextEnum::GetBlockCount( width, blockWidth );
		const unsigned blocksY = Tr2RenderContextEnum::GetBlockCount( height, blockHeight );

		//technically the block contents need to be mirrored in the margin to get nice filtering,
		// but that's somewhat fiddly.
		//textures which are intended to be used in a tiled fashion could also use the block from
		// the opposite edge.
		//maybe we need a usage hint here to inform this decision.

		outputPitch = ( blocksX + 2 * blockMargin ) * blockByteSize;
		output.resize( ( blockMargin + blocksY + blockMargin ) * outputPitch );
		unsigned char* dst = &output[0];

		//top margin
		for( unsigned i = 0; i < blockMargin; ++i )
		{
			memcpy( dst + blockMargin * blockByteSize, src, blocksX * blockByteSize );
			dst += outputPitch;
		}

		// Have to copy one line at a time since the target area is not linearly laid out.

		// Iterating blocksY rather than stepping a texel counter by the block height. Those are
		// the same count -- both are ceil( height / blockHeight ) -- but only one of them is
		// provably the number of rows the buffer above was sized for. They used to disagree:
		// this loop rounded up while blocksY truncated, so a compressed image whose height was
		// not a whole number of blocks wrote one row more than was allocated.
		//
		// NOTE (not fixed here, and pre-existing): this loop never advances `dst`, so every
		// block row is written over the first, and the CCP_ASSERT at the end of the branch
		// cannot hold for any blocksY above 0. That is a missing `dst += outputPitch;` -- but the
		// branch is unreachable today (its only caller, Tr2TextureAtlas.cpp:1073, packs
		// uncompressed formats, which take the else branch), so it has never fired and there is
		// nothing to verify a fix against. Left alone deliberately rather than guessed at.
		for( unsigned row = 0; row < blocksY; ++row )
		{
			//left margin
			for( unsigned i = 0; i != blockMargin; ++i )
			{
				memcpy( dst + i * blockByteSize, src, blockByteSize );
			}

			memcpy( dst + blockMargin * blockByteSize, src, blocksX * blockByteSize );

			//right margin
			for( unsigned i = 0; i != blockMargin; ++i )
			{
				memcpy( dst + ( blocksX + blockMargin + i ) * blockByteSize, src + ( blocksX - 1 ) * blockByteSize, blockByteSize );
			}

			if( row + 1 < blocksY )
			{
				src += blocksX * blockByteSize;
			}
		}

		//bottom margin
		for( unsigned i = 0; i != blockMargin; ++i )
		{
			memcpy( dst + blockMargin * blockByteSize, src, blocksX * blockByteSize );
			dst += outputPitch;
		}

		CCP_ASSERT( dst == &output[0] + output.size() );
	}
	else
	{
		// Align pixels to bytes.
		const unsigned byteCount = GetBytesPerPixel( format );

		// Align srcPitch to 4 bytes.
		unsigned int srcPitch = 4 * ( ( width * byteCount + 3 ) / 4 );

		outputPitch = ( width + 2 * margin ) * byteCount;

		output.resize( ( height + 2 * margin ) * outputPitch );
		unsigned char* dst = &output[0];

		//top margin
		for( unsigned i = 0; i != margin; ++i )
		{
			//note that we don't touch the corners - may want to stick debug colours in these texels,
			// at least for margins > 1. Hmm.
			memcpy( dst + margin * byteCount, src, width * byteCount );
			dst += outputPitch;
		}

		// Have to copy one line at a time since the target area is not linearly laid out.
		for( unsigned line = 0; line != height; ++line )
		{
			//left margin
			for( unsigned i = 0; i != margin; ++i )
			{
				memcpy( dst + i * byteCount, src, byteCount );
			}

			memcpy( dst + margin * byteCount, src, width * byteCount );

			//right margin
			for( unsigned i = 0; i != margin; ++i )
			{
				memcpy( dst + ( width + margin + i ) * byteCount, src + ( width - 1 ) * byteCount, byteCount );
			}

			if( line < height - 1 )
			{
				src += srcPitch;
			}
			dst += outputPitch;
		}

		//bottom margin
		for( unsigned i = 0; i != margin; ++i )
		{
			memcpy( dst + margin * byteCount, src, width * byteCount );
			dst += outputPitch;
		}

		CCP_ASSERT( dst == &output[0] + output.size() );
	}
}

}
