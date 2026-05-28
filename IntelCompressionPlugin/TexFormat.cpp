////////////////////////////////////////////////////////////////////////////////
// RitoTex — TexFormat implementation
//
// See TexFormat.h. Pure, stateless `.tex` format helpers.
////////////////////////////////////////////////////////////////////////////////

#include "TexFormat.h"
#include <cstring>

tex_format GetTEXFormatFromDXGI(DXGI_FORMAT dxgiFormat)
{
	switch (dxgiFormat) {
		case DXGI_FORMAT_BC1_UNORM:
			return tex_format_dxt1;

		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
			return tex_format_dxt5;

		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
			return tex_format_bc5;

		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return tex_format_bc7;

		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			return tex_format_bgra8;

		default:
			// Default to DXT5 for unsupported formats
			return tex_format_dxt5;
	}
}

TEX_HEADER BuildTEXHeader(uint16_t width, uint16_t height, tex_format format, bool hasMipmaps)
{
	TEX_HEADER header;
	memcpy(header.magic, "TEX\0", 4);
	header.image_width = width;
	header.image_height = height;
	header.unk1 = 0;  // Unknown field - using 0 as default
	header.tex_format = format;
	header.unk2 = 0;  // Unknown field - using 0 as default
	header.has_mipmaps = hasMipmaps;
	return header;
}

int get_num_mipmaps(int width, int height) {
	int num = 0;
	while (width > 1 || height > 1) {
		if (width > 1) width >>= 1;
		if (height > 1) height >>= 1;
		++num;
	}
	return num;
}
