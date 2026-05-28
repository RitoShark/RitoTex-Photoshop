////////////////////////////////////////////////////////////////////////////////
// RitoTex — NormalMap
//
// Normal-map post-processing on the uncompressed scratch image: optional X/Y
// channel inversion (flip green/red) and re-normalization of the whole mip
// chain so every texel is a unit-length vector. Handles both 8-bit UNORM and
// 16-bit float layouts.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"

using namespace DirectX;

//Flip the X(Red) channel and or the Y(Green) channel of the normal map
void IntelPlugin::FlipXYChannelNormalMap(ScratchImage *scrUncompressedImageScratch_)
{
	//Get first on only image
	const Image *image = scrUncompressedImageScratch_->GetImages();

	//Traverse all pixels
	for (size_t i = 0; i < image->height*image->width; i++)
	{
		//scrUncompressedImageScratch is by default RGBA data so we need a pitch of 4
		int index = (int)i*4;

		if (image->format == DXGI_FORMAT_R8G8B8A8_UNORM)
		{
			//Get pointer to first (and only) image
			unsigned8 *rowBigDataPtr = image->pixels;

			//Inverse values of R/G channels only
			if (ps.data->FlipX)
				rowBigDataPtr[index] = 255 - rowBigDataPtr[index];

			if (ps.data->FlipY)
				rowBigDataPtr[index + 1] = 255 - rowBigDataPtr[index + 1];
		}
		else if (image->format == DXGI_FORMAT_R16G16B16A16_FLOAT)
		{
			//Get pointer to first (and only) image, cast to 16bit for BC6
		    unsigned16 *rowBigDataPtr = reinterpret_cast<unsigned16 *>(image->pixels);

			//Inverse values of R/G channels only
			float r = F16toF32(rowBigDataPtr[index]);
			float g = F16toF32(rowBigDataPtr[index+1]);

			if (ps.data->FlipX)
				rowBigDataPtr[index] = F32toF16(1.f - r);

			if (ps.data->FlipY)
				rowBigDataPtr[index+1] = F32toF16(1.f - g);
		}
	}
}

//Normalize all values of this Nomral map in main image and all mip maps
void IntelPlugin::NormalizeNormalMapChain(ScratchImage *scrUncompressedImageScratch_)
{
	//Open temporary imageScratch to save mipmaps
	for (size_t i = 0; i < scrUncompressedImageScratch_->GetImageCount(); i++)
	{
		if (auto image = scrUncompressedImageScratch_->GetImages() + i)
		{
			for (size_t y = 0; y < image->height; y++)
			{
				auto ptr = image->pixels + image->rowPitch * y;
				if (image->format == DXGI_FORMAT_R8G8B8A8_UNORM)
				{
					for (size_t x = 0; x < image->width; x++)
					{
						auto p = ptr + x * 4;
						float r = static_cast<float>(p[0] - 128);
						float g = static_cast<float>(p[1] - 128);
						float b = static_cast<float>(p[2] - 128);
						float m = sqrt(r*r + g*g + b*b);
						if (m > 0)
						{
							m = 127/m;
							p[0] = static_cast<uint8_t>(r*m + 128);
							p[1] = static_cast<uint8_t>(g*m + 128);
							p[2] = static_cast<uint8_t>(b*m + 128);
						}
						else
						{
							//This is the default normal vector (0,0,1)
							p[0] = 128;
							p[1] = 128;
							p[2] = 255;
						}
					}
				}
				else if (image->format == DXGI_FORMAT_R16G16B16A16_FLOAT)
				{
					for (size_t x = 0; x < image->width; x++)
					{
						auto p = reinterpret_cast<uint16_t*>(ptr) + x * 4;
						float r = F16toF32(p[0]);
						float g = F16toF32(p[1]);
						float b = F16toF32(p[2]);
						float m = sqrt(r*r + g*g + b*b);
						if (m > 0)
						{
							m = 1.0f/m;
							p[0] = F32toF16(r * m);
							p[1] = F32toF16(g * m);
							p[2] = F32toF16(b * m);
						}
						else
						{
							//This is the default normal vector (0,0,1)
							p[0] = F32toF16(0);
							p[1] = F32toF16(0);
							p[2] = F32toF16(1);
						}
					}
				}
			}
		}
	}
}
