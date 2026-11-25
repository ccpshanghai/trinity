// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include "BcDecompress.h"

#include <cstring>

using namespace Tr2RenderContextEnum;

namespace
{
inline unsigned ConvertBGR565A8ToBGRA8( uint32_t color, uint32_t alpha )
{
	return uint32_t( color & 0x1f ) * 255 / 31 |
		( uint32_t( ( color >> 5 ) & 0x3f ) * 255 / 63 ) << 8 |
		( uint32_t( ( color >> 11 ) & 0x1f ) * 255 / 31 ) << 16 |
		( alpha << 24 );
}

inline uint32_t InterpolatedColor( uint32_t color0, uint32_t weight0, uint32_t color1, uint32_t weight1, uint32_t offset, uint32_t divisor )
{
	uint32_t r0 = ( color0 >> 16 ) & 0xff;
	uint32_t g0 = ( color0 >> 8 ) & 0xff;
	uint32_t b0 = ( color0 >> 0 ) & 0xff;
	uint32_t r1 = ( color1 >> 16 ) & 0xff;
	uint32_t g1 = ( color1 >> 8 ) & 0xff;
	uint32_t b1 = ( color1 >> 0 ) & 0xff;

	uint32_t r = ( r0 * weight0 + r1 * weight1 + offset ) / divisor & 0xff;
	uint32_t g = ( g0 * weight0 + g1 * weight1 + offset ) / divisor & 0xff;
	uint32_t b = ( b0 * weight0 + b1 * weight1 + offset ) / divisor & 0xff;
	return ( r << 16 ) | ( g << 8 ) | b;
}

void DecompressBc1( uint32_t width, uint32_t height, uint32_t depth, const Tr2SubresourceData& src, uint8_t* decompressed )
{
	uint32_t pitch = width * 4;
	uint32_t slice = pitch * height;
	for( uint32_t k = 0; k < depth; ++k )
	{
		const uint8_t* source = static_cast<const uint8_t*>( src.m_sysMem ) + src.m_sysMemSlicePitch * k;
		for( uint32_t j = 0; j < height; j += 4 )
		{
			for( uint32_t i = 0; i < width; i += 4 )
			{
				uint32_t color0 = *reinterpret_cast<const uint16_t*>( source );
				uint32_t color1 = *reinterpret_cast<const uint16_t*>( source + 2 );
				uint32_t bits = *reinterpret_cast<const uint32_t*>( source + 4 );
				if( color0 > color1 )
				{
					color0 = ConvertBGR565A8ToBGRA8( color0, 0 );
					color1 = ConvertBGR565A8ToBGRA8( color1, 0 );
					uint32_t color2 = InterpolatedColor( color0, 2, color1, 1, 1, 3 );
					uint32_t color3 = InterpolatedColor( color0, 1, color1, 2, 1, 3 );
					for( uint32_t y = 0; y < 4; ++y )
					{
						for( uint32_t x = 0; x < 4; ++x )
						{
							uint32_t destY = j + y;
							uint32_t* destPixel = reinterpret_cast<uint32_t*>( decompressed + k * slice + destY * pitch + ( x + i ) * sizeof( uint32_t ) );
							switch( ( bits >> 2 * ( 4 * y + x ) ) & 3 )
							{
							case 0:
								*destPixel = color0 | 0xff000000;
								break;
							case 1:
								*destPixel = color1 | 0xff000000;
								break;
							case 2:
								*destPixel = color2 | 0xff000000;
								break;
							case 3:
								*destPixel = color3 | 0xff000000;
								break;
							}
						}
					}
				}
				else
				{
					color0 = ConvertBGR565A8ToBGRA8( color0, 0 );
					color1 = ConvertBGR565A8ToBGRA8( color1, 0 );
					uint32_t color2 = InterpolatedColor( color0, 1, color1, 1, 0, 2 );
					for( unsigned y = 0; y < 4; ++y )
					{
						for( unsigned x = 0; x < 4; ++x )
						{
							uint32_t destY = j + y;
							uint32_t* destPixel = reinterpret_cast<uint32_t*>( decompressed + k * slice + destY * pitch + ( x + i ) * sizeof( uint32_t ) );
							switch( ( bits >> 2 * ( 4 * y + x ) ) & 3 )
							{
							case 0:
								*destPixel = color0 | 0xff000000;
								break;
							case 1:
								*destPixel = color1 | 0xff000000;
								break;
							case 2:
								*destPixel = color2 | 0xff000000;
								break;
							case 3:
								*destPixel = color2;
								break;
							}
						}
					}
				}
				source += 8;
			}
		}
	}
}

void DecompressBc2( uint32_t width, uint32_t height, uint32_t depth, const Tr2SubresourceData& src, uint8_t* decompressed )
{
	uint32_t pitch = width * 4;
	uint32_t slice = pitch * height;
	for( uint32_t k = 0; k < depth; ++k )
	{
		const uint8_t* source = static_cast<const uint8_t*>( src.m_sysMem ) + src.m_sysMemSlicePitch * k;
		for( uint32_t j = 0; j < height; j += 4 )
		{
			for( uint32_t i = 0; i < width; i += 4 )
			{
				uint32_t color0 = ConvertBGR565A8ToBGRA8( *reinterpret_cast<const uint16_t*>( source + 8 ), 0 );
				uint32_t color1 = ConvertBGR565A8ToBGRA8( *reinterpret_cast<const uint16_t*>( source + 8 + 2 ), 0 );
				uint32_t color2 = InterpolatedColor( color0, 2, color1, 1, 1, 3 );
				uint32_t color3 = InterpolatedColor( color0, 1, color1, 2, 1, 3 );
				uint32_t bits = *reinterpret_cast<const uint32_t*>( source + 8 + 4 );
				for( uint32_t y = 0; y < 4; ++y )
				{
					for( uint32_t x = 0; x < 4; ++x )
					{
						uint32_t destY = j + y;
						uint32_t* destPixel = reinterpret_cast<uint32_t*>( decompressed + k * slice + destY * pitch + ( x + i ) * sizeof( uint32_t ) );
						unsigned alphaValue = ( *( reinterpret_cast<const uint16_t*>( source ) + y ) >> x * 4 ) & 15;
						alphaValue = alphaValue * 255 / 15;
						alphaValue <<= 24;
						switch( ( bits >> 2 * ( 4 * y + x ) ) & 3 )
						{
						case 0:
							*destPixel = color0 | alphaValue;
							break;
						case 1:
							*destPixel = color1 | alphaValue;
							break;
						case 2:
							*destPixel = color2 | alphaValue;
							break;
						case 3:
							*destPixel = color3 | alphaValue;
							break;
						}
					}
				}
				source += 16;
			}
		}
	}
}

void DecompressBc3( uint32_t width, uint32_t height, uint32_t depth, const Tr2SubresourceData& src, uint8_t* decompressed )
{
	uint32_t pitch = width * 4;
	uint32_t slice = pitch * height;
	for( uint32_t k = 0; k < depth; ++k )
	{
		const uint8_t* source = static_cast<const uint8_t*>( src.m_sysMem ) + src.m_sysMemSlicePitch * k;
		for( uint32_t j = 0; j < height; j += 4 )
		{
			for( uint32_t i = 0; i < width; i += 4 )
			{
				uint32_t alpha[8];
				alpha[0] = *reinterpret_cast<const uint8_t*>( source );
				alpha[1] = *reinterpret_cast<const uint8_t*>( source + 1 );
				auto alphaMask = source + 2;
				uint32_t alphaMask0 = ( alphaMask[0] ) | ( alphaMask[1] << 8 ) | ( alphaMask[2] << 16 );
				uint32_t alphaMask1 = ( alphaMask[3] ) | ( alphaMask[4] << 8 ) | ( alphaMask[5] << 16 );
				if( alpha[0] > alpha[1] )
				{
					alpha[2] = ( 6 * alpha[0] + 1 * alpha[1] + 3 ) / 7;
					alpha[3] = ( 5 * alpha[0] + 2 * alpha[1] + 3 ) / 7;
					alpha[4] = ( 4 * alpha[0] + 3 * alpha[1] + 3 ) / 7;
					alpha[5] = ( 3 * alpha[0] + 4 * alpha[1] + 3 ) / 7;
					alpha[6] = ( 2 * alpha[0] + 5 * alpha[1] + 3 ) / 7;
					alpha[7] = ( 1 * alpha[0] + 6 * alpha[1] + 3 ) / 7;
				}
				else
				{
					alpha[2] = ( 4 * alpha[0] + 1 * alpha[1] + 2 ) / 5;
					alpha[3] = ( 3 * alpha[0] + 2 * alpha[1] + 2 ) / 5;
					alpha[4] = ( 2 * alpha[0] + 3 * alpha[1] + 2 ) / 5;
					alpha[5] = ( 1 * alpha[0] + 4 * alpha[1] + 2 ) / 5;
					alpha[6] = 0;
					alpha[7] = 255;
				}

				uint32_t color0 = ConvertBGR565A8ToBGRA8( *reinterpret_cast<const uint16_t*>( source + 8 ), 0 );
				uint32_t color1 = ConvertBGR565A8ToBGRA8( *reinterpret_cast<const uint16_t*>( source + 8 + 2 ), 0 );
				uint32_t color2 = InterpolatedColor( color0, 2, color1, 1, 1, 3 );
				uint32_t color3 = InterpolatedColor( color0, 1, color1, 2, 1, 3 );
				uint32_t bits = *reinterpret_cast<const uint32_t*>( source + 8 + 4 );
				for( uint32_t y = 0; y < 4; ++y )
				{
					for( uint32_t x = 0; x < 4; ++x )
					{
						uint32_t alphaValue;
						if( y < 2 )
						{
							alphaValue = alpha[( alphaMask0 >> ( x + y * 4 ) * 3 ) & 0x7];
						}
						else
						{
							alphaValue = alpha[( alphaMask1 >> ( x + ( y - 2 ) * 4 ) * 3 ) & 0x7];
						}
						alphaValue <<= 24;
						uint32_t* destPixel = reinterpret_cast<uint32_t*>( decompressed + k * slice + ( y + j ) * pitch + ( x + i ) * sizeof( uint32_t ) );
						switch( ( bits >> 2 * ( 4 * y + x ) ) & 3 )
						{
						case 0:
							*destPixel = color0 | alphaValue;
							break;
						case 1:
							*destPixel = color1 | alphaValue;
							break;
						case 2:
							*destPixel = color2 | alphaValue;
							break;
						case 3:
							*destPixel = color3 | alphaValue;
							break;
						}
					}
				}
				source += 16;
			}
		}
	}
}

// BC6H, decoded as the Direct3D 11 specification describes it
// (https://learn.microsoft.com/windows/win32/direct3d11/bc6h-format): bit for bit what a GPU with
// BC6H returns, signed zeros included. carbon-bind's `cbdev.bc6h` decodes from the same layout
// table.
namespace Bc6h
{

// Endpoint fields: endpoint 0/1 are region 0's pair, 2/3 region 1's.
enum Field : uint8_t
{
	R0, G0, B0, R1, G1, B1, R2, G2, B2, R3, G3, B3
};

// A run of header bits in stream order: the first stream bit lands in endpoint bit `first`, the
// last in `last`. first > last runs downwards, as modes 13 and 14 store their top bits.
struct Segment
{
	uint8_t field;
	uint8_t first;
	uint8_t last;
};

const Segment s_mode1[] = { { G2, 4, 4 }, { B2, 4, 4 }, { B3, 4, 4 }, { R0, 0, 9 }, { G0, 0, 9 }, { B0, 0, 9 }, { R1, 0, 4 }, { G3, 4, 4 }, { G2, 0, 3 }, { G1, 0, 4 }, { B3, 0, 0 }, { G3, 0, 3 }, { B1, 0, 4 }, { B3, 1, 1 }, { B2, 0, 3 }, { R2, 0, 4 }, { B3, 2, 2 }, { R3, 0, 4 }, { B3, 3, 3 } };
const Segment s_mode2[] = { { G2, 5, 5 }, { G3, 4, 4 }, { G3, 5, 5 }, { R0, 0, 6 }, { B3, 0, 0 }, { B3, 1, 1 }, { B2, 4, 4 }, { G0, 0, 6 }, { B2, 5, 5 }, { B3, 2, 2 }, { G2, 4, 4 }, { B0, 0, 6 }, { B3, 3, 3 }, { B3, 5, 5 }, { B3, 4, 4 }, { R1, 0, 5 }, { G2, 0, 3 }, { G1, 0, 5 }, { G3, 0, 3 }, { B1, 0, 5 }, { B2, 0, 3 }, { R2, 0, 5 }, { R3, 0, 5 } };
const Segment s_mode3[] = { { R0, 0, 9 }, { G0, 0, 9 }, { B0, 0, 9 }, { R1, 0, 4 }, { R0, 10, 10 }, { G2, 0, 3 }, { G1, 0, 3 }, { G0, 10, 10 }, { B3, 0, 0 }, { G3, 0, 3 }, { B1, 0, 3 }, { B0, 10, 10 }, { B3, 1, 1 }, { B2, 0, 3 }, { R2, 0, 4 }, { B3, 2, 2 }, { R3, 0, 4 }, { B3, 3, 3 } };
const Segment s_mode4[] = { { R0, 0, 9 }, { G0, 0, 9 }, { B0, 0, 9 }, { R1, 0, 3 }, { R0, 10, 10 }, { G3, 4, 4 }, { G2, 0, 3 }, { G1, 0, 4 }, { G0, 10, 10 }, { G3, 0, 3 }, { B1, 0, 3 }, { B0, 10, 10 }, { B3, 1, 1 }, { B2, 0, 3 }, { R2, 0, 3 }, { B3, 0, 0 }, { B3, 2, 2 }, { R3, 0, 3 }, { G2, 4, 4 }, { B3, 3, 3 } };
const Segment s_mode5[] = { { R0, 0, 9 }, { G0, 0, 9 }, { B0, 0, 9 }, { R1, 0, 3 }, { R0, 10, 10 }, { B2, 4, 4 }, { G2, 0, 3 }, { G1, 0, 3 }, { G0, 10, 10 }, { B3, 0, 0 }, { G3, 0, 3 }, { B1, 0, 4 }, { B0, 10, 10 }, { B2, 0, 3 }, { R2, 0, 3 }, { B3, 1, 1 }, { B3, 2, 2 }, { R3, 0, 3 }, { B3, 4, 4 }, { B3, 3, 3 } };
const Segment s_mode6[] = { { R0, 0, 8 }, { B2, 4, 4 }, { G0, 0, 8 }, { G2, 4, 4 }, { B0, 0, 8 }, { B3, 4, 4 }, { R1, 0, 4 }, { G3, 4, 4 }, { G2, 0, 3 }, { G1, 0, 4 }, { B3, 0, 0 }, { G3, 0, 3 }, { B1, 0, 4 }, { B3, 1, 1 }, { B2, 0, 3 }, { R2, 0, 4 }, { B3, 2, 2 }, { R3, 0, 4 }, { B3, 3, 3 } };
const Segment s_mode7[] = { { R0, 0, 7 }, { G3, 4, 4 }, { B2, 4, 4 }, { G0, 0, 7 }, { B3, 2, 2 }, { G2, 4, 4 }, { B0, 0, 7 }, { B3, 3, 3 }, { B3, 4, 4 }, { R1, 0, 5 }, { G2, 0, 3 }, { G1, 0, 4 }, { B3, 0, 0 }, { G3, 0, 3 }, { B1, 0, 4 }, { B3, 1, 1 }, { B2, 0, 3 }, { R2, 0, 5 }, { R3, 0, 5 } };
const Segment s_mode8[] = { { R0, 0, 7 }, { B3, 0, 0 }, { B2, 4, 4 }, { G0, 0, 7 }, { G2, 5, 5 }, { G2, 4, 4 }, { B0, 0, 7 }, { G3, 5, 5 }, { B3, 4, 4 }, { R1, 0, 4 }, { G3, 4, 4 }, { G2, 0, 3 }, { G1, 0, 5 }, { G3, 0, 3 }, { B1, 0, 4 }, { B3, 1, 1 }, { B2, 0, 3 }, { R2, 0, 4 }, { B3, 2, 2 }, { R3, 0, 4 }, { B3, 3, 3 } };
const Segment s_mode9[] = { { R0, 0, 7 }, { B3, 1, 1 }, { B2, 4, 4 }, { G0, 0, 7 }, { B2, 5, 5 }, { G2, 4, 4 }, { B0, 0, 7 }, { B3, 5, 5 }, { B3, 4, 4 }, { R1, 0, 4 }, { G3, 4, 4 }, { G2, 0, 3 }, { G1, 0, 4 }, { B3, 0, 0 }, { G3, 0, 3 }, { B1, 0, 5 }, { B2, 0, 3 }, { R2, 0, 4 }, { B3, 2, 2 }, { R3, 0, 4 }, { B3, 3, 3 } };
const Segment s_mode10[] = { { R0, 0, 5 }, { G3, 4, 4 }, { B3, 0, 0 }, { B3, 1, 1 }, { B2, 4, 4 }, { G0, 0, 5 }, { G2, 5, 5 }, { B2, 5, 5 }, { B3, 2, 2 }, { G2, 4, 4 }, { B0, 0, 5 }, { G3, 5, 5 }, { B3, 3, 3 }, { B3, 5, 5 }, { B3, 4, 4 }, { R1, 0, 5 }, { G2, 0, 3 }, { G1, 0, 5 }, { G3, 0, 3 }, { B1, 0, 5 }, { B2, 0, 3 }, { R2, 0, 5 }, { R3, 0, 5 } };
const Segment s_mode11[] = { { R0, 0, 9 }, { G0, 0, 9 }, { B0, 0, 9 }, { R1, 0, 9 }, { G1, 0, 9 }, { B1, 0, 9 } };
const Segment s_mode12[] = { { R0, 0, 9 }, { G0, 0, 9 }, { B0, 0, 9 }, { R1, 0, 8 }, { R0, 10, 10 }, { G1, 0, 8 }, { G0, 10, 10 }, { B1, 0, 8 }, { B0, 10, 10 } };
const Segment s_mode13[] = { { R0, 0, 9 }, { G0, 0, 9 }, { B0, 0, 9 }, { R1, 0, 7 }, { R0, 11, 10 }, { G1, 0, 7 }, { G0, 11, 10 }, { B1, 0, 7 }, { B0, 11, 10 } };
const Segment s_mode14[] = { { R0, 0, 9 }, { G0, 0, 9 }, { B0, 0, 9 }, { R1, 0, 3 }, { R0, 15, 10 }, { G1, 0, 3 }, { G0, 15, 10 }, { B1, 0, 3 }, { B0, 15, 10 } };

struct Mode
{
	uint8_t modeBits;
	uint8_t endpointBits;
	uint8_t deltaBits[3];
	bool transformed;
	const Segment* segments;
	size_t segmentCount;
};

#define BC6H_SEGMENTS( table ) table, sizeof( table ) / sizeof( table[0] )
const Mode s_modes[] = {
	{ 2, 10, { 5, 5, 5 }, true, BC6H_SEGMENTS( s_mode1 ) },
	{ 2, 7, { 6, 6, 6 }, true, BC6H_SEGMENTS( s_mode2 ) },
	{ 5, 11, { 5, 4, 4 }, true, BC6H_SEGMENTS( s_mode3 ) },
	{ 5, 11, { 4, 5, 4 }, true, BC6H_SEGMENTS( s_mode4 ) },
	{ 5, 11, { 4, 4, 5 }, true, BC6H_SEGMENTS( s_mode5 ) },
	{ 5, 9, { 5, 5, 5 }, true, BC6H_SEGMENTS( s_mode6 ) },
	{ 5, 8, { 6, 5, 5 }, true, BC6H_SEGMENTS( s_mode7 ) },
	{ 5, 8, { 5, 6, 5 }, true, BC6H_SEGMENTS( s_mode8 ) },
	{ 5, 8, { 5, 5, 6 }, true, BC6H_SEGMENTS( s_mode9 ) },
	{ 5, 6, { 6, 6, 6 }, false, BC6H_SEGMENTS( s_mode10 ) },
	{ 5, 10, { 10, 10, 10 }, false, BC6H_SEGMENTS( s_mode11 ) },
	{ 5, 11, { 9, 9, 9 }, true, BC6H_SEGMENTS( s_mode12 ) },
	{ 5, 12, { 8, 8, 8 }, true, BC6H_SEGMENTS( s_mode13 ) },
	{ 5, 16, { 4, 4, 4 }, true, BC6H_SEGMENTS( s_mode14 ) },
};
#undef BC6H_SEGMENTS

// Index into s_modes by the low five bits, for the modes with five mode bits; -1 is reserved
// and decodes to zero. Values whose low two bits are 00 or 01 are modes 1 and 2.
const int8_t s_fiveBitModes[32] = {
	-1, -1, 2, 10, -1, -1, 3, 11, -1, -1, 4, 12, -1, -1, 5, 13,
	-1, -1, 6, -1, -1, -1, 7, -1, -1, -1, 8, -1, -1, -1, 9, -1
};

// BC7's two-subset partitions 0..31: bit t is the region of texel t.
const uint16_t s_partitions[32] = {
	0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80,
	0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
	0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE,
	0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C
};

// Region 1's anchor texel per partition (region 0's is texel 0). An anchor's index omits its
// top bit, which is always zero.
const uint8_t s_anchors[32] = {
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2
};

const int32_t s_weights3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
const int32_t s_weights4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

const uint16_t s_halfOne = 0x3C00;

class BlockBits
{
public:
	explicit BlockBits( const uint8_t* block )
	{
		std::memcpy( &m_low, block, 8 );
		std::memcpy( &m_high, block + 8, 8 );
	}

	uint32_t Get( uint32_t position ) const
	{
		return uint32_t( ( position < 64 ? m_low >> position : m_high >> ( position - 64 ) ) & 1 );
	}

	uint32_t Get( uint32_t position, uint32_t count ) const
	{
		uint32_t value = 0;
		for( uint32_t i = 0; i < count; ++i )
		{
			value |= Get( position + i ) << i;
		}
		return value;
	}

private:
	uint64_t m_low;
	uint64_t m_high;
};

int32_t SignExtend( int32_t value, uint32_t bits )
{
	const uint32_t shift = 32 - bits;
	return int32_t( uint32_t( value ) << shift ) >> shift;
}

int32_t Unquantize( int32_t value, uint32_t bits, bool isSigned )
{
	if( !isSigned )
	{
		if( bits >= 15 || value == 0 )
		{
			return value;
		}
		if( value == ( 1 << bits ) - 1 )
		{
			return 0xFFFF;
		}
		return ( ( value << 16 ) + 0x8000 ) >> bits;
	}
	if( bits >= 16 )
	{
		return value;
	}
	const int32_t magnitude = value < 0 ? -value : value;
	int32_t result;
	if( magnitude == 0 )
	{
		result = 0;
	}
	else if( magnitude >= ( 1 << ( bits - 1 ) ) - 1 )
	{
		result = 0x7FFF;
	}
	else
	{
		result = ( ( magnitude << 15 ) + 0x4000 ) >> ( bits - 1 );
	}
	return value < 0 ? -result : result;
}

// The last step to a half float's bits: scale by 31/64 (unsigned) or 31/32 (signed magnitude).
uint16_t FinishUnquantize( int32_t value, bool isSigned )
{
	if( !isSigned )
	{
		return uint16_t( ( value * 31 ) >> 6 );
	}
	const int32_t magnitude = ( ( value < 0 ? -value : value ) * 31 ) >> 5;
	// A negative value that scales to zero is +0, as the hardware returns it.
	return uint16_t( value < 0 && magnitude != 0 ? magnitude | 0x8000 : magnitude );
}

// Decodes one block into 16 texels of RGB half-float bits, row-major.
void DecodeBlock( const uint8_t* block, bool isSigned, uint16_t texels[16][3] )
{
	const BlockBits bits( block );
	const uint32_t lowTwo = bits.Get( 0, 2 );
	const int modeIndex = lowTwo < 2 ? int( lowTwo ) : s_fiveBitModes[bits.Get( 0, 5 )];
	if( modeIndex < 0 )
	{
		std::memset( texels, 0, sizeof( uint16_t ) * 16 * 3 );
		return;
	}
	const Mode& mode = s_modes[modeIndex];
	const bool twoRegions = modeIndex < 10;
	const uint32_t endpointCount = twoRegions ? 4 : 2;

	int32_t endpoints[12] = {};
	uint32_t position = mode.modeBits;
	for( size_t s = 0; s < mode.segmentCount; ++s )
	{
		const Segment& segment = mode.segments[s];
		const int step = segment.last >= segment.first ? 1 : -1;
		for( int bit = segment.first;; bit += step )
		{
			endpoints[segment.field] |= int32_t( bits.Get( position++ ) << bit );
			if( bit == segment.last )
			{
				break;
			}
		}
	}

	// Endpoint 0 holds full precision; the others are deltas from it in a transformed mode.
	// Deltas are signed even in the unsigned format.
	if( isSigned )
	{
		for( uint32_t c = 0; c < 3; ++c )
		{
			endpoints[c] = SignExtend( endpoints[c], mode.endpointBits );
		}
	}
	if( mode.transformed || isSigned )
	{
		for( uint32_t e = 1; e < endpointCount; ++e )
		{
			for( uint32_t c = 0; c < 3; ++c )
			{
				endpoints[e * 3 + c] = SignExtend( endpoints[e * 3 + c], mode.deltaBits[c] );
			}
		}
	}
	if( mode.transformed )
	{
		const int32_t mask = ( 1 << mode.endpointBits ) - 1;
		for( uint32_t e = 1; e < endpointCount; ++e )
		{
			for( uint32_t c = 0; c < 3; ++c )
			{
				const int32_t value = ( endpoints[e * 3 + c] + endpoints[c] ) & mask;
				endpoints[e * 3 + c] = isSigned ? SignExtend( value, mode.endpointBits ) : value;
			}
		}
	}
	for( uint32_t i = 0; i < endpointCount * 3; ++i )
	{
		endpoints[i] = Unquantize( endpoints[i], mode.endpointBits, isSigned );
	}

	uint32_t partitionMask = 0;
	uint32_t anchor = 0;
	uint32_t indexBits = 4;
	const int32_t* weights = s_weights4;
	if( twoRegions )
	{
		const uint32_t partition = bits.Get( 77, 5 );
		partitionMask = s_partitions[partition];
		anchor = s_anchors[partition];
		indexBits = 3;
		weights = s_weights3;
		position = 82;
	}
	for( uint32_t t = 0; t < 16; ++t )
	{
		const bool isAnchor = t == 0 || ( twoRegions && t == anchor );
		const uint32_t width = isAnchor ? indexBits - 1 : indexBits;
		const int32_t weight = weights[bits.Get( position, width )];
		position += width;
		const uint32_t region = ( partitionMask >> t ) & 1;
		for( uint32_t c = 0; c < 3; ++c )
		{
			const int32_t a = endpoints[region * 6 + c];
			const int32_t b = endpoints[region * 6 + 3 + c];
			texels[t][c] = FinishUnquantize( ( a * ( 64 - weight ) + b * weight + 32 ) >> 6, isSigned );
		}
	}
}

// Writes RGBA16Float texels (alpha 1), tightly packed: a row is width * 8 bytes. Blocks are read in
// order, one row of ceil(width / 4) blocks after another, each slice at its slice pitch. A level
// smaller than a block (2x2, 1x1) keeps only the texels inside it.
void Decompress( uint32_t width, uint32_t height, uint32_t depth, bool isSigned, const Tr2SubresourceData& src, uint8_t* decompressed )
{
	const uint32_t blocksWide = ( width + 3 ) / 4;
	const uint32_t blocksHigh = ( height + 3 ) / 4;
	uint16_t* out = reinterpret_cast<uint16_t*>( decompressed );
	uint16_t texels[16][3];
	for( uint32_t k = 0; k < depth; ++k )
	{
		const uint8_t* source = static_cast<const uint8_t*>( src.m_sysMem ) + size_t( src.m_sysMemSlicePitch ) * k;
		uint16_t* slice = out + size_t( k ) * width * height * 4;
		for( uint32_t by = 0; by < blocksHigh; ++by )
		{
			for( uint32_t bx = 0; bx < blocksWide; ++bx )
			{
				DecodeBlock( source, isSigned, texels );
				source += 16;
				for( uint32_t t = 0; t < 16; ++t )
				{
					const uint32_t x = bx * 4 + ( t & 3 );
					const uint32_t y = by * 4 + ( t >> 2 );
					if( x < width && y < height )
					{
						uint16_t* texel = slice + ( size_t( y ) * width + x ) * 4;
						texel[0] = texels[t][0];
						texel[1] = texels[t][1];
						texel[2] = texels[t][2];
						texel[3] = s_halfOne;
					}
				}
			}
		}
	}
}

}

}

PixelFormat BcDecompressedFormat( PixelFormat format )
{
	switch( format )
	{
	case PIXEL_FORMAT_BC1_TYPELESS:
	case PIXEL_FORMAT_BC1_UNORM:
	case PIXEL_FORMAT_BC1_UNORM_SRGB:
	case PIXEL_FORMAT_BC2_TYPELESS:
	case PIXEL_FORMAT_BC2_UNORM:
	case PIXEL_FORMAT_BC2_UNORM_SRGB:
	case PIXEL_FORMAT_BC3_TYPELESS:
	case PIXEL_FORMAT_BC3_UNORM:
	case PIXEL_FORMAT_BC3_UNORM_SRGB:
		return PIXEL_FORMAT_B8G8R8A8_UNORM;

	case PIXEL_FORMAT_BC6H_TYPELESS:
	case PIXEL_FORMAT_BC6H_UF16:
	case PIXEL_FORMAT_BC6H_SF16:
		return PIXEL_FORMAT_R16G16B16A16_FLOAT;

	default:
		return PIXEL_FORMAT_UNKNOWN;
	}
}

bool BcDecompress( uint32_t width, uint32_t height, uint32_t depth, PixelFormat format, const Tr2SubresourceData& src, std::unique_ptr<uint8_t[]>& decompressed )
{
	const uint32_t bytesPerTexel = BcDecompressedFormat( format ) == PIXEL_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
	if( !decompressed )
	{
		decompressed.reset( new uint8_t[size_t( width ) * height * depth * bytesPerTexel] );
	}
	switch( format )
	{
	case PIXEL_FORMAT_BC1_TYPELESS:
	case PIXEL_FORMAT_BC1_UNORM:
	case PIXEL_FORMAT_BC1_UNORM_SRGB:
		DecompressBc1( width, height, depth, src, decompressed.get() );
		return true;

	case PIXEL_FORMAT_BC2_TYPELESS:
	case PIXEL_FORMAT_BC2_UNORM:
	case PIXEL_FORMAT_BC2_UNORM_SRGB:
		DecompressBc2( width, height, depth, src, decompressed.get() );
		return true;

	case PIXEL_FORMAT_BC3_TYPELESS:
	case PIXEL_FORMAT_BC3_UNORM:
	case PIXEL_FORMAT_BC3_UNORM_SRGB:
		DecompressBc3( width, height, depth, src, decompressed.get() );
		return true;

	case PIXEL_FORMAT_BC6H_TYPELESS:
	case PIXEL_FORMAT_BC6H_UF16:
		Bc6h::Decompress( width, height, depth, false, src, decompressed.get() );
		return true;

	case PIXEL_FORMAT_BC6H_SF16:
		Bc6h::Decompress( width, height, depth, true, src, decompressed.get() );
		return true;

	default:
		return false;
	}
}
