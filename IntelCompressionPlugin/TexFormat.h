////////////////////////////////////////////////////////////////////////////////
// RitoTex — TexFormat
//
// The Riot `.tex` container format: the on-disk format codes, the 12-byte
// header struct, block-size math, and the mapping between DirectXTex DXGI
// formats and `.tex` format codes.
//
// This module is pure and stateless — it knows nothing about Photoshop or the
// plugin. It is the single source of truth for "what a .tex file looks like".
// Format codes match the Paint.NET / LtMAO reference backend.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <dxgiformat.h>

// On-disk `.tex` pixel-format codes.
//   0x0A DXT1/BC1 · 0x0C DXT5/BC3 · 0x0D BC7 · 0x0E BC5 · 0x14 BGRA8
enum tex_format : uint8_t {
	tex_format_etc1     = 0x1,
	tex_format_etc2_eac = 0x2,
	tex_format_etc2     = 0x3,
	tex_format_dxt1     = 0xA,   // BC1
	tex_format_dxt5     = 0xC,   // BC3
	tex_format_bc7      = 0xD,   // BC7
	tex_format_bc5      = 0xE,   // BC5 (two-channel / normal maps)
	tex_format_bgra8    = 0x14
};

#define tex_magic "TEX"

// 12-byte `.tex` file header (little-endian on disk).
typedef struct {
	uint8_t    magic[4];      // "TEX\0"
	uint16_t   image_width;
	uint16_t   image_height;
	uint8_t    unk1;          // reserved (Riot/Paint.NET write 1)
	tex_format tex_format;
	uint8_t    unk2;          // reserved (written 0)
	bool       has_mipmaps;
} TEX_HEADER;

// Bytes per 4x4 block for a block-compressed tex_format (0 if not block-compressed).
inline uint32_t TexBlockBytes(tex_format fmt)
{
	switch (fmt)
	{
	case tex_format_dxt1: return 8;   // BC1
	case tex_format_dxt5: return 16;  // BC3
	case tex_format_bc7:  return 16;  // BC7
	case tex_format_bc5:  return 16;  // BC5
	default:              return 0;   // BGRA8 / ETC* not block-compressed here
	}
}

inline bool TexIsBlockCompressed(tex_format fmt)
{
	return TexBlockBytes(fmt) != 0;
}

// Map a DirectXTex DXGI format (the user's chosen encoding) to a `.tex` code.
// Unsupported formats fall back to DXT5.
tex_format GetTEXFormatFromDXGI(DXGI_FORMAT dxgiFormat);

// Build a 12-byte `.tex` header (magic + dimensions + format + mip flag).
TEX_HEADER BuildTEXHeader(uint16_t width, uint16_t height, tex_format format, bool hasMipmaps);

// Number of mip levels for a full chain down to 1x1, given base dimensions.
int get_num_mipmaps(int width, int height);
