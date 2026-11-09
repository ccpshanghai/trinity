// Copyright © 2026 CCP ehf.

#include "StdAfx.h"

// The whole file is Vulkan-only, the way ShaderBindingABI.cpp is: dx11, dx12 and metal
// compile the same `_SOURCES` list, and the rule this asserts is Vulkan's alone. No GPU,
// no VkDevice, no window -- it is arithmetic.
#if TRINITY_PLATFORM == TRINITY_VULKAN

#include "vulkan/UtilitiesVulkan.h"

using namespace Tr2RenderContextEnum;
using TrinityALImpl::GetCopyTexelExtentVulkan;

namespace
{

	Tr2BitmapDimensions Texture2D( PixelFormat format, uint32_t width, uint32_t height )
	{
		// mipCount 0 means "the full chain", which is what a .dds with mips arrives as.
		return Tr2BitmapDimensions( TEX_TYPE_2D, format, width, height, 1, 0 );
	}

}

// `VkBufferImageCopy::imageExtent` is counted in TEXELS of the destination subresource, and
// VUID-vkCmdCopyBufferToImage-imageSubresource-07971/07972 require it to fit *inside* that
// subresource. The spec's block-compressed allowance (VUID-07274/07275) is the other way
// round from what it looks like: a copy region may end on a partial block only when it ends
// exactly at the mip's edge, so the correct value at the edge is the mip's real texel size,
// never the size rounded up to a whole block.
//
// `BitmapDimensions::GetMipWidth` rounds compressed mips UP to a whole block, on purpose --
// it is the *storage* question ("how many bytes does this mip occupy"), and a 2x2 BC mip does
// occupy a full 4x4 block. Using that storage answer as the API's texel extent is the defect
// this file exists to prevent coming back. It cost 168 validation errors in one shippreview
// frame, all of them on the tail mips of BC2 cube maps, and DX12 never showed it because
// D3D12_SUBRESOURCE_FOOTPRINT wants exactly the block-aligned number that Vulkan forbids.

TEST( TextureCopyExtentVulkan, CompressedTailMipsAreTexelSizedNotBlockSized )
{
	// The shape that actually failed: a 128x128 BC2 cube face. Every mip down to 4x4 is a
	// whole number of blocks and is therefore unchanged; 2x2 and 1x1 are the two that the
	// storage-rounded accessor reports as 4x4.
	const Tr2BitmapDimensions desc = Texture2D( PIXEL_FORMAT_BC2_UNORM, 128, 128 );

	const uint32_t expected[] = { 128, 64, 32, 16, 8, 4, 2, 1 };
	for( uint32_t level = 0; level < 8; ++level )
	{
		const VkExtent3D extent = GetCopyTexelExtentVulkan( desc, level );
		EXPECT_EQ( expected[level], extent.width ) << "level " << level;
		EXPECT_EQ( expected[level], extent.height ) << "level " << level;
		EXPECT_EQ( 1u, extent.depth ) << "level " << level;
	}

	// And the accessor it must NOT be, stated as an assertion rather than as a comment, so
	// that a well-meaning "simplify" back to GetMipWidth fails here rather than in a frame.
	EXPECT_EQ( 4u, desc.GetMipWidth( 6 ) );
	EXPECT_EQ( 4u, desc.GetMipWidth( 7 ) );
}

TEST( TextureCopyExtentVulkan, UncompressedExtentsAreUnchanged )
{
	// The generalisation has to be free: for an uncompressed format the helper must agree
	// with GetMipWidth/GetMipHeight everywhere, or it has changed behaviour for the common
	// path while fixing the rare one.
	const Tr2BitmapDimensions desc = Texture2D( PIXEL_FORMAT_B8G8R8A8_UNORM, 64, 16 );

	for( uint32_t level = 0; level < desc.GetTrueMipCount(); ++level )
	{
		const VkExtent3D extent = GetCopyTexelExtentVulkan( desc, level );
		EXPECT_EQ( desc.GetMipWidth( level ), extent.width ) << "level " << level;
		EXPECT_EQ( desc.GetMipHeight( level ), extent.height ) << "level " << level;
		EXPECT_EQ( desc.GetMipDepth( level ), extent.depth ) << "level " << level;
	}
}

TEST( TextureCopyExtentVulkan, NonSquareCompressedMipsClampPerAxis )
{
	// Width and height run out at different levels, so a fix that clamps one axis by the
	// other's block count passes the square case and fails here. 64x8 BC1: height reaches
	// 4 at level 1 and 2 at level 2, while width is still 32 and 16.
	const Tr2BitmapDimensions desc = Texture2D( PIXEL_FORMAT_BC1_UNORM, 64, 8 );

	const VkExtent3D level2 = GetCopyTexelExtentVulkan( desc, 2 );
	EXPECT_EQ( 16u, level2.width );
	EXPECT_EQ( 2u, level2.height );

	const VkExtent3D level4 = GetCopyTexelExtentVulkan( desc, 4 );
	EXPECT_EQ( 4u, level4.width );
	EXPECT_EQ( 1u, level4.height );
}

TEST( TextureCopyExtentVulkan, LevelPastTheChainIsZero )
{
	// GetMipWidth's own convention for an out-of-range level, kept: a caller that loops on
	// GetMipCount rather than GetTrueMipCount gets a zero extent it can test, not a block.
	const Tr2BitmapDimensions desc = Texture2D( PIXEL_FORMAT_BC2_UNORM, 128, 128 );

	const VkExtent3D past = GetCopyTexelExtentVulkan( desc, desc.GetTrueMipCount() );
	EXPECT_EQ( 0u, past.width );
	EXPECT_EQ( 0u, past.height );
}

#endif
