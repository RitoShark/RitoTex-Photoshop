////////////////////////////////////////////////////////////////////////////////
// RitoTex — ContainerWrite
//
// The two write paths that turn a compressed ScratchImage into an on-disk
// file: DDS (via DirectXTex SaveToDDSMemory) and Riot .tex (a 12-byte header
// followed by mip data written smallest-to-largest, then the base image).
//
// Both share the same pre-encode pipeline (fetch -> cube/layer assembly ->
// normal-map flip -> mip generation -> normalize -> Codec compress); only the
// container/header differs. The shared encode lives in Codec.cpp; this module
// owns the container framing.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"
#include "IntelPluginUIWin.h"		// errorMessage()

#include <string>

using namespace DirectX;

/*****************************************************************************/
//Main compression function which calls all other functions and saves dds
void IntelPlugin::DoWriteDDS()
{
	if (GetResult() != noErr)
		return;

	bool DoMipMaps = ps.data->MipMapTypeIndex == MipmapEnum::AUTOGEN || ps.data->MipMapTypeIndex == MipmapEnum::FROM_LAYERS;

	//============================================================================================
	//============================================================================================
	//Setup Photoshop callback structs and what data to get, section

	bool hasAlpha = HasAlpha();

	ScratchImage* scrUncompressedImageScratch = new ScratchImage();

	if (!CopyDataForEncoding(scrUncompressedImageScratch, hasAlpha, DoMipMaps, true))
	{
		UserError("Unsupported bit depth");
		return;
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
			return;
		}
	}

	//============================================================================================
	//============================================================================================
	//MipMap from Layers
	//Here we overwrite only the initial image MipLevel 0 from Layer 0, in order to generate automatic MipMaps
	//the second step of MipMap from layers is furter down
	if (IsMipMapsDefinedByLayer())
	{
		//We use this to avoid having to disable the visibility of all layers when creating mip level 0.
		//By default all images are composited onto mip level 0
		CopyLayersIntoMipMaps(scrUncompressedImageScratch, hasAlpha, 0, 1);
	}

	//============================================================================================
	//============================================================================================
	//If FlipX/Y true, invert R/G channels scrUncompressedImageScratch only if Normal Map, section
	if ((ps.data->FlipX || ps.data->FlipY) && (ps.data->TextureTypeIndex==TextureTypeEnum::NORMALMAP))
	{
		FlipXYChannelNormalMap(scrUncompressedImageScratch);
	}

	//============================================================================================
	//============================================================================================
	//If MipMaps generate using DirectXTexLib, section
	//Also force mipmaps if setMipLevels on cube maps are specified
	if (DoMipMaps || IsCubeMapWithSetMipLevelOverride())
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
			return;
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


	// Compress scrUncompressedImageScratch into scrImageScratch, section
	ScratchImage* scrImageScratch = new ScratchImage();

	ps.data->previewing = false;
	if (!CompressToScratchImage(&scrImageScratch, &scrUncompressedImageScratch, hasAlpha))
		return;

	// Save compressed DirectXTex image structure to file, section
	Blob blob;
	HRESULT hr;

	// Save compressed image

	if (IsCubeMapWithSetMipLevelOverride()) 	//If setMipLevel on cubemap specified
		hr = SaveCubeMipLevelToDDSFile(scrImageScratch, blob);
	else
		hr = SaveToDDSMemory(scrImageScratch->GetImages(), scrImageScratch->GetImageCount(), scrImageScratch->GetMetadata(), DDS_FLAGS_NONE, blob);

	if (hr == S_OK)
	{
		if (int size = int(blob.GetBufferSize()))
		{
			if (!WriteFile(reinterpret_cast<HANDLE>(ps.formatRecord->dataFork), blob.GetBufferPointer(), size, reinterpret_cast<LPDWORD>(&size), NULL))
				SetResult(writErr);
			else if (size < int(blob.GetBufferSize()))
				SetResult(dskFulErr);
		}
	}
	else
		UserError("Failed to save");


	// Cleanup

	if (scrImageScratch != NULL)
	    delete scrImageScratch;

	if (scrUncompressedImageScratch != NULL)
	    delete scrUncompressedImageScratch;
}

/*****************************************************************************/
// Helper function to write TEX mipmaps in reverse order (smallest to largest)
OSErr IntelPlugin::WriteTEXMipmaps(DirectX::ScratchImage* compressedImage)
{
	size_t mipCount = compressedImage->GetMetadata().mipLevels;

	if (mipCount <= 1) {
		return noErr; // No mipmaps to write
	}

	// Write mipmaps in REVERSE order (from smallest to largest)
	// Skip mip 0 (main image) - write from mipCount-1 down to 1
	for (int mipLevel = static_cast<int>(mipCount) - 1; mipLevel >= 1; mipLevel--) {
		const DirectX::Image* mipImage = compressedImage->GetImage(mipLevel, 0, 0);
		if (!mipImage) {
			errorMessage("Failed to get mipmap data at level " + std::to_string(mipLevel), "TEX Save Error");
			return writErr;
		}

		int32 mipSize = static_cast<int32>(mipImage->slicePitch);
		int32 bytesWritten = mipSize;

		if (!WriteFile(reinterpret_cast<HANDLE>(ps.formatRecord->dataFork),
		               mipImage->pixels, mipSize, reinterpret_cast<LPDWORD>(&bytesWritten), NULL)) {
			errorMessage("Failed to write mipmap data at level " + std::to_string(mipLevel), "TEX Save Error");
			return writErr;
		}

		if (bytesWritten < mipSize) {
			errorMessage("Incomplete mipmap write at level " + std::to_string(mipLevel), "TEX Save Error");
			return dskFulErr;
		}
	}

	return noErr;
}

/*****************************************************************************/
// Helper function to write TEX file with header and image data
OSErr IntelPlugin::WriteTEXFile(const TEX_HEADER& header, DirectX::ScratchImage* compressedImage)
{
	// 1. Write TEX header (12 bytes)
	int32 bytesWritten = sizeof(TEX_HEADER);
	if (!WriteFile(reinterpret_cast<HANDLE>(ps.formatRecord->dataFork),
	               &header, sizeof(TEX_HEADER), reinterpret_cast<LPDWORD>(&bytesWritten), NULL)) {
		errorMessage("Failed to write TEX header", "TEX Save Error");
		return writErr;
	}

	if (bytesWritten < sizeof(TEX_HEADER)) {
		errorMessage("Incomplete TEX header write", "TEX Save Error");
		return dskFulErr;
	}

	// 2. Write mipmaps in REVERSE order (smallest to largest) if present
	if (header.has_mipmaps) {
		OSErr err = WriteTEXMipmaps(compressedImage);
		if (err != noErr) return err;
	}

	// 3. Write main image (mip level 0)
	const DirectX::Image* mainImage = compressedImage->GetImage(0, 0, 0);
	if (!mainImage) {
		errorMessage("Failed to get main image data", "TEX Save Error");
		return writErr;
	}

	int32 dataSize = static_cast<int32>(mainImage->slicePitch);
	bytesWritten = dataSize;
	if (!WriteFile(reinterpret_cast<HANDLE>(ps.formatRecord->dataFork),
	               mainImage->pixels, dataSize, reinterpret_cast<LPDWORD>(&bytesWritten), NULL)) {
		errorMessage("Failed to write TEX image data", "TEX Save Error");
		return writErr;
	}

	if (bytesWritten < dataSize) {
		errorMessage("Incomplete TEX image data write", "TEX Save Error");
		return dskFulErr;
	}

	return noErr;
}

/*****************************************************************************/
// Main TEX compression and save function
void IntelPlugin::DoWriteTEX()
{
	if (GetResult() != noErr)
		return;

	bool DoMipMaps = ps.data->MipMapTypeIndex == MipmapEnum::AUTOGEN || ps.data->MipMapTypeIndex == MipmapEnum::FROM_LAYERS;

	//============================================================================================
	// Setup Photoshop callback structs and get image data
	bool hasAlpha = HasAlpha();
	ScratchImage* scrUncompressedImageScratch = new ScratchImage();

	if (!CopyDataForEncoding(scrUncompressedImageScratch, hasAlpha, DoMipMaps, true))
	{
		UserError("Unsupported bit depth");
		return;
	}

	//============================================================================================
	// Handle cube maps (TEX format doesn't typically support cube maps, but we'll skip for now)
	if (ps.data->TextureTypeIndex==TextureTypeEnum::CUBEMAP_CROSSED ||
	    ps.data->TextureTypeIndex==TextureTypeEnum::CUBEMAP_LAYERS)
	{
		UserError("TEX format does not support cube maps. Please use DDS format instead.");
		return;
	}

	//============================================================================================
	// MipMap from Layers
	if (IsMipMapsDefinedByLayer())
	{
		CopyLayersIntoMipMaps(scrUncompressedImageScratch, hasAlpha, 0, 1);
	}

	//============================================================================================
	// Handle normal map channel flipping
	if ((ps.data->FlipX || ps.data->FlipY) && (ps.data->TextureTypeIndex==TextureTypeEnum::NORMALMAP))
	{
		FlipXYChannelNormalMap(scrUncompressedImageScratch);
	}

	//============================================================================================
	// Generate mipmaps if requested
	if (DoMipMaps)
	{
		ScratchImage *scrImageMipMapScratch = new ScratchImage();

		DWORD mimapFilter = TEX_FILTER_DEFAULT|TEX_FILTER_SEPARATE_ALPHA;

		HRESULT hr = GenerateMipMaps(scrUncompressedImageScratch->GetImages(), scrUncompressedImageScratch->GetImageCount(),
				                     scrUncompressedImageScratch->GetMetadata(), mimapFilter, 0, *scrImageMipMapScratch );
		if( hr != S_OK )
		{
			UserError("Could not create MipMaps");
			return;
		}

		delete scrUncompressedImageScratch;
		scrUncompressedImageScratch = scrImageMipMapScratch;

		// Copy layers into mipmaps if needed
		if (IsMipMapsDefinedByLayer())
		{
			CopyLayersIntoMipMaps(scrUncompressedImageScratch, hasAlpha, 1);
		}
	}

	//============================================================================================
	// Normalize normal maps if specified
	if (ps.data->Normalize && ps.data->TextureTypeIndex == TextureTypeEnum::NORMALMAP)
		NormalizeNormalMapChain(scrUncompressedImageScratch);

	//============================================================================================
	// Compress the image
	ScratchImage* scrImageScratch = new ScratchImage();

	ps.data->previewing = false;
	if (!CompressToScratchImage(&scrImageScratch, &scrUncompressedImageScratch, hasAlpha))
		return;

	//============================================================================================
	// Build TEX header
	uint16_t width = static_cast<uint16_t>(ps.formatRecord->imageSize.h);
	uint16_t height = static_cast<uint16_t>(ps.formatRecord->imageSize.v);
	tex_format texFormat = GetTEXFormatFromDXGI(ps.data->encoding_g);
	bool hasMipmaps = (scrImageScratch->GetMetadata().mipLevels > 1);

	TEX_HEADER header = BuildTEXHeader(width, height, texFormat, hasMipmaps);

	//============================================================================================
	// Write TEX file
	OSErr err = WriteTEXFile(header, scrImageScratch);
	SetResult(err);

	if (err != noErr)
		UserError("Failed to save TEX file");

	//============================================================================================
	// Cleanup
	if (scrImageScratch != NULL)
	    delete scrImageScratch;

	if (scrUncompressedImageScratch != NULL)
	    delete scrUncompressedImageScratch;
}
