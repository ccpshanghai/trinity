// Copyright © 2023 CCP ehf.

#pragma once
#ifndef BcDecompress_H
#define BcDecompress_H

#include "Tr2RenderContextEnum.h"
#include "Tr2HalHelperStructures.h"

// The format BcDecompress writes for a block format: B8G8R8A8_UNORM for BC1-3,
// R16G16B16A16_FLOAT for BC6H (alpha 1), PIXEL_FORMAT_UNKNOWN for one it has no decoder for.
Tr2RenderContextEnum::PixelFormat BcDecompressedFormat( Tr2RenderContextEnum::PixelFormat format );

// Decodes one level of a BC1-3 or BC6H image into tightly packed texels of
// BcDecompressedFormat( format ). Returns false for a format with no decoder. Allocates
// `decompressed` only when it is empty, sized for this call, so a caller reusing one buffer across
// a texture's levels passes the largest level first.
bool BcDecompress(
	uint32_t width,
	uint32_t height,
	uint32_t depth,
	Tr2RenderContextEnum::PixelFormat format,
	const Tr2SubresourceData& src,
	std::unique_ptr<uint8_t[]>& decompressed );

#endif