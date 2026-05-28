////////////////////////////////////////////////////////////////////////////////
// RitoTex — Preview
//
// Everything the Save dialog needs to render its live preview and stats:
// reported dimensions/byte sizes, the RGB blit into the dialog's pixel buffer
// (FetchPreviewRGB), and the two scratch-image builders that produce a
// preview-ready image. GetCompressedImageForPreview runs the full encode then
// decodes it back so the user sees real compression artifacts;
// GetUncompressedImageForPreview shows the source after cube/mip/normal-map
// processing without compression.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"

using namespace DirectX;

POINT IntelPlugin::GetPreviewDimensions()
{
	POINT ret = {ps.formatRecord->imageSize.h, ps.formatRecord->imageSize.v};
	if (ps.data->TextureTypeIndex==TextureTypeEnum::CUBEMAP_LAYERS)
	{
		ret.x *= 4;
		ret.y *= 3;
	}
	return ret;
}

int IntelPlugin::GetCompressedByteSize()
{
	return preview.compressedSize;
}

int IntelPlugin::GetOriginalBitsPerPixel()
{
	int planes = ps.formatRecord->planes;
	if (planes > 3 && ps.data->TextureTypeIndex!=TextureTypeEnum::COLOR_ALPHA)	// we have enough planes for alpha but are not exporting it right now
		planes--;

	return ps.formatRecord->depth * planes;
}

int IntelPlugin::GetOriginalByteSize()
{
	int byteSize =  ps.formatRecord->imageSize.h * ps.formatRecord->imageSize.v * ((GetOriginalBitsPerPixel() + 7) >> 3);

	switch (ps.data->TextureTypeIndex)
	{
	case TextureTypeEnum::CUBEMAP_LAYERS:
		byteSize *= 6;
		break;
	case TextureTypeEnum::CUBEMAP_CROSSED:	// a cross view only uses half the image resolution
		byteSize = byteSize >> 1;
		break;
	}

	return byteSize;
}

int IntelPlugin::GetLayerCount()
{
	int layerCount = 0;
	for (auto layer = ps.formatRecord->documentInfo->layersDescriptor; layer; layer = layer->next)
		layerCount++;

	return layerCount;
}

// get RGB buffer
void IntelPlugin::FetchPreviewRGB(unsigned8 *dst, int width, int height, int xo, int yo, double zoom, int previewOptions, int matteColor)
{
	FetchImageData();

	if (!(previewOptions & PREVIEW_CHANNEL_MASK))
		previewOptions |= PREVIEW_CHANNEL_RGB;

	// default use original source
	int planes = ps.formatRecord->hiPlane - ps.formatRecord->loPlane + 1;
	int srcWidth = ps.formatRecord->theRect.right - ps.formatRecord->theRect.left;
	int srcHeight = ps.formatRecord->theRect.bottom - ps.formatRecord->theRect.top;
	int depthOrFormat = ps.formatRecord->depth;
	int rowPitch = ps.formatRecord->rowBytes;
	const uint8 * pixelData = static_cast<const uint8 *>(ps.formatRecord->data);
	auto exposure = ps.data->exposure;

	int mipLevel = (ps.data->SetMipLevel ? ps.data->MipLevel : 0);
	zoom = zoom / (1 << mipLevel);

	//Note
	//The Get(Un)CompressedImageForPreview() functions and the preview scratchimage are only used if the image to be fetched is compressed
	//or its a cube map or has mip layers. Otherwise the above pixelData is used as is.

	// flush invalid previews based on parameter changes...
	if (preview.textureType != ps.data->TextureTypeIndex || preview.mipMap != ps.data->MipMapTypeIndex || preview.mipLevel != mipLevel ||
		preview.flipRChannel != ps.data->FlipX || preview.flipGChannel != ps.data->FlipY)
	{
		if (preview.uncompressedImage)
		{
			delete preview.uncompressedImage;
			preview.uncompressedImage = NULL;
		}

		if (preview.compressedImage)
		{
			delete preview.compressedImage;
			preview.compressedImage = NULL;
			preview.compressedSize = 0;
		}

		preview.textureType = ps.data->TextureTypeIndex;
		preview.mipMap = ps.data->MipMapTypeIndex;
		preview.mipLevel = mipLevel;
		preview.flipRChannel = ps.data->FlipX;
		preview.flipGChannel = ps.data->FlipY;
	}

	if (preview.encoding != ps.data->encoding_g)
	{
		if (preview.compressedImage)
		{
			delete preview.compressedImage;
			preview.compressedImage = NULL;
		}

		preview.encoding = ps.data->encoding_g;
	}

	if ((previewOptions & PREVIEW_SOURCE_MASK) == PREVIEW_SOURCE_COMPRESSED)
	{
		if (!preview.compressedImage)
			preview.compressedImage = GetCompressedImageForPreview(planes, preview.compressedSize);

		if (!preview.compressedImage)	// Compressed can return NULL, ie no compression, so set it to uncompressed version
			preview.compressedImage = GetUncompressedImageForPreview(planes);

		if (preview.compressedImage)	// shouldn't be null
		{
			auto image = preview.compressedImage->GetImages();
			planes = 4;
			srcWidth = int(image->width);
			srcHeight = int(image->height);
			depthOrFormat = -1;	// signal this is 8 bit
			rowPitch = int(image->rowPitch);
			pixelData = image->pixels;
		}
	}
	else if (ps.data->TextureTypeIndex==TextureTypeEnum::CUBEMAP_CROSSED ||
			 ps.data->TextureTypeIndex==TextureTypeEnum::CUBEMAP_LAYERS ||
			 (ps.data->MipMapTypeIndex == MipmapEnum::FROM_LAYERS && IsMipMapsDefinedByLayer()) ||
			 mipLevel > 0)
	{
		if (!preview.uncompressedImage)
			preview.uncompressedImage = GetUncompressedImageForPreview(planes);

		if (preview.uncompressedImage)
		{
			auto image = preview.uncompressedImage->GetImages();
			planes = 4;
			srcWidth = int(image->width);
			srcHeight = int(image->height);
			depthOrFormat = -1;	// signal this is 8 bit
			rowPitch = int(image->rowPitch);
			pixelData = image->pixels;
		}
	}

	//Iterate over preview area, the preview is a window of width/height onto the original image
	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			int sx = int((x + xo) * zoom);
			int sy = int((y + yo) * zoom);

			unsigned8 *dstPixel = dst + (y * width + x) * 3;

			if (sx < 0 || sx >= srcWidth || sy < 0 || sy >= srcHeight)
			{
				// Out of source bounds - fill with checkerboard or black
				dstPixel[0] = dstPixel[1] = dstPixel[2] = ((sx>>4) ^ (sy>>4)) & 1 ? 0xCC : 0x99;
				continue;
			}

			const uint8 *srcPixel = pixelData + sy * rowPitch + sx * (depthOrFormat > 0 ? (planes * (depthOrFormat>>3)) : 4);

			if (depthOrFormat == -1)	// 8 bit BGRA from scratch image
			{
				dstPixel[0] = srcPixel[0];
				dstPixel[1] = srcPixel[1];
				dstPixel[2] = srcPixel[2];
			}
			else if (depthOrFormat == 8)
			{
				for (int p = 0; p < 3; p++)
					dstPixel[p] = (planes > p) ? srcPixel[(planes > p ? p : 0)] : srcPixel[0];
			}
			else if (depthOrFormat == 16)
			{
				const uint16 *srcPixel16 = reinterpret_cast<const uint16 *>(srcPixel);
				for (int p = 0; p < 3; p++)
					dstPixel[p] = ConvertTo8Bit(srcPixel16[planes > p ? p : 0]);
			}
			else if (depthOrFormat == 32)
			{
				const float *srcPixel32 = reinterpret_cast<const float *>(srcPixel);
				for (int p = 0; p < 3; p++)
					dstPixel[p] = ConvertTo8Bit(double(srcPixel32[planes > p ? p : 0]) * exposure, true);
			}
		}
	}
}

/*****************************************************************************/
/*****************************************************************************/
//AdvanceState () has to be called before entering this function so that the ps.formatRecord->data buffer is full;
ScratchImage* IntelPlugin::GetCompressedImageForPreview(int planesToGet_, int &compressedSize)
{
	//Returns null if image to preview is uncompressed
	if (ps.data->encoding_g == DXGI_FORMAT_R8G8B8A8_UNORM)
		return NULL;

	bool hasAlpha = HasAlpha();

	ScratchImage* scrUncompressedImageScratch = new ScratchImage();
	if (!CopyDataForEncoding(scrUncompressedImageScratch, hasAlpha, false, false))
	{
		UserError("Unsupported bit depth");
		return NULL;
	}

	//============================================================================================
	//============================================================================================
	//If this is a cube map, change scrUncompressedImageScratch into a CubeMap type image, section
	if (ps.data->TextureTypeIndex==TextureTypeEnum::CUBEMAP_CROSSED)
	{
		ConvertToCubeMapFromCross(&scrUncompressedImageScratch);
	}
	else if (ps.data->TextureTypeIndex==TextureTypeEnum::CUBEMAP_LAYERS)
	{
		if (!ConvertToCubeMapFromLayers(&scrUncompressedImageScratch, hasAlpha))
		{
			UserError("Cubemap has not enough layers available. Consult the documentation (question mark next to the TextureType drop down) on how to create cubemaps");
			return NULL;
		}
	}
	//If autogen mipmaps and from layers and not preview the specific mip level
	else if (IsMipMapsDefinedByLayer() && !(ps.data->SetMipLevel))
	{
		CopyLayersIntoMipMaps(scrUncompressedImageScratch, hasAlpha, 0, 1);
	}


	//If FlipX/Y true, invert R/G channels scrUncompressedImageScratch only if Normal Map, section
	if ((ps.data->FlipX || ps.data->FlipY) && (ps.data->TextureTypeIndex==TextureTypeEnum::NORMALMAP))
	{
		FlipXYChannelNormalMap(scrUncompressedImageScratch);
	}

	//============================================================================================
	//============================================================================================
	//If autogen mip maps and not preview specific mip level
	if (ps.data->MipMapTypeIndex == MipmapEnum::AUTOGEN ||
		(IsCubeMapWithSetMipLevelOverride()))
	{
		//Open temporary imageScratch to save mipmaps
		ScratchImage *scrImageMipMapScratch = new ScratchImage();

		DWORD mimapFilter = TEX_FILTER_DEFAULT|TEX_FILTER_SEPARATE_ALPHA;

		//Generate MipMaps from scrUncompressedImageScratch and save to scrImageMipMapScratch
		HRESULT hr = GenerateMipMaps(scrUncompressedImageScratch->GetImages(), scrUncompressedImageScratch->GetImageCount(),
					                 scrUncompressedImageScratch->GetMetadata(), mimapFilter, 0, *scrImageMipMapScratch );
		if( hr != S_OK )
		{
			UserError("Could not create MipMaps");
			return NULL;
		}

		//free previous space
		delete scrUncompressedImageScratch;

		//Copy over pointer from ScratchImage which has MipMap, now scrUncompressedImageScratch holds the initial image + mip chain
		scrUncompressedImageScratch = scrImageMipMapScratch;

		//MipMap from Layers
		//If we should get the mip maps from layers then override the existing mip maps with the layer images
		if (IsMipMapsDefinedByLayer())
		{
			//Copy all Layers starting 1. We omit 0 becasue we already copied it above.
			CopyLayersIntoMipMaps(scrUncompressedImageScratch, hasAlpha, 1);
		}
	}

	// If Normalization Postprocess option has been specified and this is a Normal Type texture type
	// Normalize all mip chain
	if (ps.data->Normalize && ps.data->TextureTypeIndex == TextureTypeEnum::NORMALMAP)
		NormalizeNormalMapChain(scrUncompressedImageScratch);

	//Compress scrUncompressedImageScratch into scrImageScratch, section
	ScratchImage* scrImageScratch = new ScratchImage();

	ps.data->previewing = true;
	if (!CompressToScratchImage(&scrImageScratch, &scrUncompressedImageScratch, hasAlpha))
	{
		delete scrImageScratch;
		delete scrUncompressedImageScratch;
		return NULL;
	}

	//If we have a compressed image decompress it for preview, section
	ScratchImage* scrDecompressedImageScratch = new ScratchImage();

	HRESULT hr = Decompress(scrImageScratch->GetImages(), scrImageScratch->GetImageCount(),
							scrImageScratch->GetMetadata(), DXGI_FORMAT_R8G8B8A8_UNORM, *scrDecompressedImageScratch);

	if (hr != S_OK)
	{
		delete scrImageScratch;
		delete scrUncompressedImageScratch;
		delete scrDecompressedImageScratch;
		return NULL;
	}

	// store the compressed size for stats
	compressedSize = 0;
	for (size_t i = 0; i < scrImageScratch->GetImageCount(); i++)
		compressedSize += int(scrImageScratch->GetImage(i, 0, 0)->slicePitch);

	delete scrImageScratch;
	delete scrUncompressedImageScratch;

	return scrDecompressedImageScratch;
}

/*****************************************************************************/
/*****************************************************************************/
ScratchImage* IntelPlugin::GetUncompressedImageForPreview(int planesToGet_)
{
	bool hasAlpha = HasAlpha();

	ScratchImage* scrUncompressedImageScratch = new ScratchImage();
	if (!CopyDataForEncoding(scrUncompressedImageScratch, hasAlpha, false, false))
	{
		UserError("Unsupported bit depth");
		return NULL;
	}

	//If this is a cube map, change scrUncompressedImageScratch into a CubeMap type image
	if (ps.data->TextureTypeIndex==TextureTypeEnum::CUBEMAP_CROSSED)
	{
		ConvertToCubeMapFromCross(&scrUncompressedImageScratch);
		ConvertToHorizontalCrossFromCubeMap(&scrUncompressedImageScratch);
	}
	else if (ps.data->TextureTypeIndex==TextureTypeEnum::CUBEMAP_LAYERS)
	{
		if (!ConvertToCubeMapFromLayers(&scrUncompressedImageScratch, hasAlpha))
		{
			UserError("Cubemap has not enough layers available. Consult the documentation (question mark next to the TextureType drop down) on how to create cubemaps");
			return NULL;
		}
		ConvertToHorizontalCrossFromCubeMap(&scrUncompressedImageScratch);
	}
	else if (IsMipMapsDefinedByLayer() && !(ps.data->SetMipLevel))
	{
		CopyLayersIntoMipMaps(scrUncompressedImageScratch, hasAlpha, 0, 1);
	}

	//If FlipX/Y true, invert R/G channels scrUncompressedImageScratch only if Normal Map
	if ((ps.data->FlipX || ps.data->FlipY) && (ps.data->TextureTypeIndex==TextureTypeEnum::NORMALMAP))
	{
		FlipXYChannelNormalMap(scrUncompressedImageScratch);
	}

	//If autogen mip maps, or cube map with mip level override
	if ((ps.data->MipMapTypeIndex == MipmapEnum::AUTOGEN) || IsCubeMapWithSetMipLevelOverride())
	{
		ScratchImage *scrImageMipMapScratch = new ScratchImage();

		DWORD mimapFilter = TEX_FILTER_DEFAULT|TEX_FILTER_SEPARATE_ALPHA;

		HRESULT hr = GenerateMipMaps(scrUncompressedImageScratch->GetImages(), scrUncompressedImageScratch->GetImageCount(),
					                 scrUncompressedImageScratch->GetMetadata(), mimapFilter, 0, *scrImageMipMapScratch );
		if( hr != S_OK )
		{
			UserError("Could not create MipMaps");
			return NULL;
		}

		delete scrUncompressedImageScratch;
		scrUncompressedImageScratch = scrImageMipMapScratch;

		if (IsMipMapsDefinedByLayer())
		{
			CopyLayersIntoMipMaps(scrUncompressedImageScratch, hasAlpha, 1);
		}
	}

	// If Normalization Postprocess option specified and Normal texture type, normalize mip chain
	if (ps.data->Normalize && ps.data->TextureTypeIndex == TextureTypeEnum::NORMALMAP)
		NormalizeNormalMapChain(scrUncompressedImageScratch);

	return scrUncompressedImageScratch;
}
