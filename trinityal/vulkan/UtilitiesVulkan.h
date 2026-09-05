// Copyright © 2026 CCP ehf.

#pragma once

#include "VkResult.h"
#include "../Tr2RenderContextEnum.h"

class Tr2PrimaryRenderContextAL;
struct Tr2MsaaDesc;

namespace TrinityALImpl
{

	// VK_INCOMPLETE means the array was too small and only partly filled. It is a Vulkan
	// *success* code, so Vk2Al maps it to S_FALSE and CR_RETURN_HR would carry on with a
	// truncated vector -- which is why the fill calls below test for VK_SUCCESS exactly
	// rather than going through Vk2Al. It happens when the count changes between the two
	// calls, which for physical devices or present modes is unlikely but not impossible.
	inline ALResult ExactSuccess( VkResult result )
	{
		return result == VK_SUCCESS ? ALResult( S_OK ) : ALResult( E_FAIL );
	}

	template <typename Func, typename A1, typename Result>
	ALResult QueryArray( Func func, A1 a1, std::vector<Result>& result )
	{
		result.clear();
		uint32_t count = 0;
		CR_RETURN_HR( Vk2Al( ( *func )( a1, &count, nullptr ) ) );
		if( count )
		{
			result.resize( count );
			CR_RETURN_HR( ExactSuccess( ( *func )( a1, &count, &result[0] ) ) );
		}
		return S_OK;
	}

	template <typename Func, typename A1, typename A2, typename Result>
	ALResult QueryArray( Func func, A1 a1, A2 a2, std::vector<Result>& result )
	{
		result.clear();
		uint32_t count = 0;
		CR_RETURN_HR( Vk2Al( ( *func )( a1, a2, &count, nullptr ) ) );
		if( count )
		{
			result.resize( count );
			CR_RETURN_HR( ExactSuccess( ( *func )( a1, a2, &count, &result[0] ) ) );
		}
		return S_OK;
	}

	template <typename Func, typename A1, typename Result>
	void QueryArrayNoFail( Func func, A1 a1, std::vector<Result>& result )
	{
		result.clear();
		uint32_t count = 0;
		( *func )( a1, &count, nullptr );
		if( count )
		{
			result.resize( count );
			( *func )( a1, &count, &result[0] );
		}
	}

	template <typename Func, typename A1, typename Result>
	ALResult QueryArrayNotEmpty( Func func, A1 a1, std::vector<Result>& result )
	{
		FORWARD_HR( QueryArray( func, a1, result ) );
		if( result.empty() )
		{
			return E_FAIL;
		}
		return S_OK;
	}

	template <typename Func, typename A1, typename A2, typename Result>
	ALResult QueryArrayNotEmpty( Func func, A1 a1, A2 a2, std::vector<Result>& result )
	{
		FORWARD_HR( QueryArray( func, a1, a2, result ) );
		if( result.empty() )
		{
			return E_FAIL;
		}
		return S_OK;
	}

	VkFormat GetVulkanFormat( Tr2RenderContextEnum::PixelFormat format );

	// The presentable formats, named back the other way. GetVulkanFormat is many-to-one --
	// TYPELESS and B8G8R8X8 collapse onto the same VkFormat -- so it has no inverse, and
	// this is not one: it covers only the formats a surface can actually hand back, and
	// returns PIXEL_FORMAT_UNKNOWN for anything else. Callers must check.
	Tr2RenderContextEnum::PixelFormat GetAlPixelFormat( VkFormat format );

	// The sRGB sibling of a format, or VK_FORMAT_UNDEFINED where there is none.
	//
	// Deliberately keyed on VkFormat rather than routing through the AL's
	// Tr2RenderContextEnum::MakeSrgb, because a view format has to be *compatible* with the
	// image format -- same texel block size -- not merely different, and MakeSrgb does not
	// promise that. PIXEL_FORMAT_B8G8R8X8_TYPELESS is the counterexample: it maps to
	// VK_FORMAT_B8G8R8_UNORM at 3 bytes a texel, while MakeSrgb of it maps to
	// VK_FORMAT_B8G8R8A8_SRGB at 4. Every pair below is same-class by construction.
	VkFormat GetSrgbCounterpartVulkan( VkFormat format );

	ALResult AllocateMemory( VkDeviceMemory& memory, const VkMemoryRequirements& memoryRequirements, VkMemoryPropertyFlagBits memoryProperty, Tr2PrimaryRenderContextAL& renderContext );
	ALResult CreateBuffer( VkBuffer& buffer, VkDeviceMemory& memory, size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlagBits memoryProperty, Tr2PrimaryRenderContextAL& renderContext );
	ALResult CreateImage( VkImage& image, VkDeviceMemory& memory, const Tr2BitmapDimensions& desc, const Tr2MsaaDesc& msaa, VkImageUsageFlags usage, VkImageCreateFlags flags, VkMemoryPropertyFlagBits memoryProperty, Tr2PrimaryRenderContextAL& renderContext );

	// The pipeline stage and access mask that go with an image layout. Every barrier in
	// this backend used to spell both out by hand at the call site, which is how the
	// backend ended up transitioning some images and simply not others: there was no one
	// place that knew what a layout implies. Callers now name only the layout they want.
	//
	// These are the conservative pairings, not the tightest ones. A tighter srcStage for
	// a known producer would be faster; getting it wrong is a data race that validation
	// cannot always see, so the wide mask is the right default until something measures.
	void GetLayoutStageAccessVulkan( VkImageLayout layout, VkPipelineStageFlags2& stage, VkAccessFlags2& access );

	// Colour formats get the colour aspect, depth formats get depth (plus stencil where
	// the format has one). Barriers and image views both need this and both used to
	// hardcode VK_IMAGE_ASPECT_COLOR_BIT, which is why nothing depth-shaped worked.
	VkImageAspectFlags GetAspectMaskVulkan( VkFormat format );

	// The texel extent of one mip, for `VkBufferImageCopy::imageExtent`.
	//
	// NOT BitmapDimensions::GetMipWidth/GetMipHeight, which is the trap this exists to close.
	// Those round a compressed mip UP to a whole block, because they answer the *storage*
	// question -- a 2x2 BC2 mip really does occupy one whole 4x4 block, and DX12 wants exactly
	// that number in D3D12_SUBRESOURCE_FOOTPRINT. Vulkan wants the other number: imageExtent is
	// counted in texels of the destination subresource and must fit inside it
	// (VUID-vkCmdCopyBufferToImage-imageSubresource-07971/07972). A copy region is allowed to
	// end on a partial block precisely when it ends at the mip's edge, which is the case here.
	//
	// Passing the block-rounded value cost 168 validation errors in a single shippreview frame
	// -- every one of them on the 2x2 and 1x1 tail mips of the scene's BC2 cube maps, which is
	// why nothing smaller than a cube map with a full mip chain ever showed it.
	//
	// Depth comes straight from GetMipDepth: it is never block-aligned (no 3D block format is
	// mapped here) and is 1 for everything that is not TEX_TYPE_3D.
	//
	// Returns a zero extent for a level past the chain, keeping GetMipWidth's own convention.
	VkExtent3D GetCopyTexelExtentVulkan( const Tr2BitmapDimensions& desc, uint32_t level );

}