////////////////////////////////////////////////////////////////////////////////
// RitoTex - PhotoshopRead
//
// Host-side load/parse path. Photoshop drives this through the read selectors
// (Prepare/Start/Continue/Finish/LayerStart). Start sniffs the file: a "TEX"
// magic routes to the .tex path (header parse + BlockDecompress BC1/BC3 + RGBA
// repack); otherwise it is parsed as DDS via DirectXTex (LoadFromDDSMemory,
// Decompress/Convert, cube-face and mip-as-layer handling). Continue streams
// each image/face/mip into Photoshop's buffer. FillFromCompositedLayers feeds
// merged document layers back for re-encode.
//
// The per-layer channel reads (ReadLayerData) live in LayerOps.cpp; the .tex
// header struct + format codes in TexFormat.h; the load dialog in the UI layer.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"
#include "IntelPluginUIWin.h"		// ShowLoadDialog, errorMessage
#include "s3tc.h"					// BlockDecompressImageDXT1 / DXT5

#include <memory>

using namespace DirectX;

void IntelPlugin::DoFilterFile (void)
{
	// File can be tested for validity here
	//SetResult(formatCannotRead);
	SetResult(noErr);
}


// TEX format definitions moved to IntelPlugin.h

struct
{
	std::unique_ptr<TEX_HEADER> header;
	std::unique_ptr<uint8[]> pixelData;
	uint8 currentLayer;
	tex_format tex_format;
	bool isLoadingTex;
} texLoadInfo;


void IntelPlugin::DoReadPrepare()
{
	ps.formatRecord->maxData = 0;
	loadInfo.isCubeMap = false;
	loadInfo.readImagePtr = NULL;
	loadInfo.loadMipMapIndex = 0;
	loadInfo.hasMips = false;
	loadInfo.hasAlpha = false;

	texLoadInfo.header = NULL;
	texLoadInfo.pixelData = NULL;
	texLoadInfo.currentLayer = 0;
	texLoadInfo.isLoadingTex = false;
}

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define LINE_STRING "Line:" TOSTRING(__LINE__)

void IntelPlugin::DoReadStart()
{
	LARGE_INTEGER fileSize;
	
	showLoadingCursor();

	//Read any descriptor values for scripting support and return if we are in batch mode.
	ps.data->queryForParameters = ReadScriptParamsForRead();

	//Rewind to start of file
	OSErr err= PSSDKSetFPos (ps.formatRecord->dataFork, fsFromStart, 0);
	if (err != noErr)
	{
		errorMessage("Failed to seek to start of TEX file", "TEX Load Error");
		SetResult(err);
		showNormalCursor();
		return;
	}

	int32 texHeaderSize = sizeof(TEX_HEADER);
	auto header = std::make_unique<TEX_HEADER>();

	err = PSSDKRead(ps.formatRecord->dataFork, &texHeaderSize, header.get());
	if (err != noErr)
	{
		errorMessage("Failed to read TEX header (12 bytes)", "TEX Load Error");
		SetResult(err);
		showNormalCursor();
		return;
	}

	bool isTex = memcmp(header->magic, "TEX", 3) == 0;
	if (isTex) {
		texLoadInfo.isLoadingTex = true;

		UINT width = header->image_width;
		UINT height = header->image_height;
		UINT stride = width * 4;
		UINT imageSize = stride * height;

		uint32 blockSize = (header->tex_format == tex_format_dxt5) ? 16 : 8;
		uint32 blockWidth = (header->image_width + 3) / 4;
		uint32 blockHeight = (header->image_height + 3) / 4;
		int32 readCount = blockWidth * blockHeight * blockSize;

		auto pixelData = std::make_unique<uint8[]>(readCount);

		err = PSSDKSetFPos(ps.formatRecord->dataFork, fsFromStart, sizeof(TEX_HEADER));
		if (err != noErr) {
			errorMessage("Failed to set file position", "Err");
			SetResult(err);
			showNormalCursor();
			return;
		}

		if (header->has_mipmaps) {
			unsigned int mipMapCount = get_num_mipmaps(header->image_width, header->image_height);

			UINT skip = 0;
			// block_size = 4 for dxt5 and dxt1
			// Note (+ block_size - 1) simplified to +3 bcs 4 - 1
			for (auto x = mipMapCount; x > 0; x--) {
				auto curr_width = max(header->image_width / (1 << x), 1);
				auto curr_height = max(header->image_height / (1 << x), 1);

				auto blockWidth = (curr_width + 3) / 4;
				auto blockHeight = (curr_height + 3) / 4;
				skip += blockSize * blockWidth * blockHeight;
			}

			err = PSSDKSetFPos(ps.formatRecord->dataFork, fsFromStart, sizeof(TEX_HEADER) + skip);
			if (err != noErr) {
				errorMessage("Failed to set file position", "Err");
				SetResult(err);
				showNormalCursor();
				return;
			}
		}

		//ReadIn PixelData
		err = PSSDKRead(ps.formatRecord->dataFork, &readCount, pixelData.get());
		if (err != noErr)
		{
			errorMessage("Failed to read TEX compressed pixel data", "TEX Load Error");
			SetResult(err);
			showNormalCursor();
			return;
		}

		//Setup formatRecord for loading

		bool compressedImageHasAlpha = (header->tex_format == tex_format_dxt5 || header->tex_format == tex_format_bgra8);
		bool compressedImageHasMipMaps = header->has_mipmaps;
		bool loadDDSMipMaps = ps.data->mipmapBatchAllowed;
		
		/* 
		Slightly modified dialog from the original, does not show the mipmap option
		Its for the alpha be an channel instead of part of the layer itself
		*/
		if (compressedImageHasAlpha)
		{
			if (ps.data->queryForParameters)
			{
				unsigned8 loadDialogResult = ShowLoadDialog(compressedImageHasAlpha, false, GetActiveWindow());
				bool separateAlphaChannel = (loadDialogResult & LoadInfoEnum::USE_SEPARATEALPHA) ? true : false;
				ps.data->alphaBatchSeperate = separateAlphaChannel;
			}
		}
		
		ps.formatRecord->imageRsrcData = NULL;
		ps.formatRecord->imageRsrcSize = 0;
		ps.formatRecord->imageMode = plugInModeRGBColor;
		ps.data->mipmapBatchAllowed = false;

		if (ps.formatRecord->HostSupports32BitCoordinates &&
			ps.formatRecord->PluginUsing32BitCoordinates)
		{
			ps.formatRecord->imageSize32.v = static_cast<int16>(header->image_height);
			ps.formatRecord->imageSize32.h = static_cast<int16>(header->image_width);
		}
		else
		{
			ps.formatRecord->imageSize.v = static_cast<int16>(header->image_height);
			ps.formatRecord->imageSize.h = static_cast<int16>(header->image_width);
		}

		ps.formatRecord->depth = int(BitsPerColor(DXGI_FORMAT_R8G8B8A8_UNORM));
		
		ps.formatRecord->planes = 4; // 4 channels for alpha
		ps.formatRecord->transparencyPlane = 3;
		ps.formatRecord->transparencyMatting = 0;
		ps.formatRecord->layerData = ps.data->alphaBatchSeperate ? 0 : 1; // 0 = separate alpha, 1 = use layer transparency
		texLoadInfo.pixelData = std::move(pixelData);
		texLoadInfo.header = std::move(header);
	}
	else {
		//Rewind to start of file after reading texHeader
		OSErr err = PSSDKSetFPos(ps.formatRecord->dataFork, fsFromStart, 0);
		if (err != noErr)
		{
			SetResult(err);
			showNormalCursor();
			return;
		}

		//Get filesize
		GetFileSizeEx(reinterpret_cast<HANDLE>(ps.formatRecord->dataFork), &fileSize);

		//Allocate buffer equal filesize
		uint8* wholeFileBuffer = new uint8[size_t(fileSize.QuadPart)];
		int32 readCount = int32(fileSize.QuadPart);

		//ReadIn whole file
		err = PSSDKRead(ps.formatRecord->dataFork, &readCount, wholeFileBuffer);
		if (err != noErr)
		{
			SetResult(err);
			showNormalCursor();
			return;
		}

		//If not enough bytes read, error
		if (err == noErr && readCount != fileSize.QuadPart)
		{
			SetResult(eofErr);
			showNormalCursor();
			return;
		}

		//Load DDS from memory
		ScratchImage* ddsCompressedImage = new ScratchImage;
		TexMetadata  readImageInfo;

		HRESULT hr = LoadFromDDSMemory(wholeFileBuffer, readCount, DDS_FLAGS_NONE, &readImageInfo, *ddsCompressedImage);

		//Check if the image suports alpha. Note For DX, BC1 supports alpha for us not
		bool alphaIsWhite = ddsCompressedImage->IsAlphaAllOpaque();
		bool compressedImageHasAlpha = (DirectX::HasAlpha(readImageInfo.format) && !alphaIsWhite &&
			readImageInfo.format != DXGI_FORMAT_BC1_UNORM);
		loadInfo.hasAlpha = compressedImageHasAlpha;

		//Check if there are mip maps
		bool compressedImageHasMipMaps = (readImageInfo.mipLevels > 1) ? true : false;

		bool separateAlphaChannel = ps.data->alphaBatchSeperate; //get predefined values for batching from descriptors in ReadScriptParamsForRead()
		bool loadDDSMipMaps = ps.data->mipmapBatchAllowed; //get predefined values for batching from descriptors in ReadScriptParamsForRead
		bool compressedImageIsCubemap = (readImageInfo.arraySize == 6) ? true : false;

		//For cube maps disable alpha and mip maps for now
		if (compressedImageIsCubemap)
		{
			compressedImageHasAlpha = compressedImageHasMipMaps = false;
			separateAlphaChannel = false;
		}

		//Show load dialog
		//Do we need the user to make a selection regarding alpha or mip maps?
		if (compressedImageHasAlpha || compressedImageHasMipMaps)
		{
			//ask user if he wants them, and not in batch
			if (ps.data->queryForParameters)
			{
				unsigned8 loadDialogResult = ShowLoadDialog(compressedImageHasAlpha, compressedImageHasMipMaps, GetActiveWindow());

				//decode result 
				separateAlphaChannel = (loadDialogResult & LoadInfoEnum::USE_SEPARATEALPHA) ? true : false;
				loadDDSMipMaps = (loadDialogResult & LoadInfoEnum::USE_MIPMAPS) ? true : false;

				//store descriptor values for scripting. Actual write happens in DoReadFinish()
				ps.data->alphaBatchSeperate = separateAlphaChannel;
				ps.data->mipmapBatchAllowed = loadDDSMipMaps;
			}
		}

		//init to no layers
		ps.formatRecord->layerData = 0;

		//Do we have mip maps, is this a cube map, or a simple image
		if (compressedImageIsCubemap)
		{
			//Cube map
			loadInfo.isCubeMap = true;
			ps.formatRecord->layerData = 6; //specify the creatoin of six layers
		}
		else if (compressedImageHasMipMaps)
		{
			//Image has mip maps
			if (loadDDSMipMaps)
			{
				loadInfo.hasMips = true;
				ps.formatRecord->layerData = uint32(readImageInfo.mipLevels); //specify creation of layers
			}
		}

		//By default we use layer transparency when the image has alpha, but when the separatealpha flag 
		//is set we use the background image with a dedicated alpha channel
		if (!separateAlphaChannel && compressedImageHasAlpha && ps.formatRecord->layerData == 0)
		{
			ps.formatRecord->layerData = 1;
		}

		// pick a target format
		auto targetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

		if (BitsPerColor(readImageInfo.format) > 16)
			targetFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
		else if (BitsPerColor(readImageInfo.format) > 8)
			targetFormat = DXGI_FORMAT_R16G16B16A16_UNORM;


		// Is it already in the correct format?
		if (readImageInfo.format == targetFormat)
		{
			// No conversion required, just swap pointers
			loadInfo.readImagePtr = ddsCompressedImage;
			ddsCompressedImage = NULL;
		}
		else
		{
			loadInfo.readImagePtr = new ScratchImage();
			if (IsCompressed(readImageInfo.format))	// is it compressed?
				hr = Decompress(ddsCompressedImage->GetImages(), ddsCompressedImage->GetImageCount(), ddsCompressedImage->GetMetadata(), targetFormat, *loadInfo.readImagePtr);
			else
				hr = Convert(ddsCompressedImage->GetImages(), ddsCompressedImage->GetImageCount(), ddsCompressedImage->GetMetadata(), targetFormat, 0, 0, *loadInfo.readImagePtr);
		}

		if (FAILED(hr))
		{
			UserError("Can not load image.");
			delete loadInfo.readImagePtr;
			delete ddsCompressedImage;
			delete[] wholeFileBuffer;
			showNormalCursor();
			return;
		}

		// update the metadata
		readImageInfo = loadInfo.readImagePtr->GetMetadata();

		//Setup formatRecord for loading
		ps.formatRecord->imageRsrcData = NULL;
		ps.formatRecord->imageRsrcSize = 0;
		ps.formatRecord->imageMode = plugInModeRGBColor;
		if (ps.formatRecord->HostSupports32BitCoordinates &&
			ps.formatRecord->PluginUsing32BitCoordinates)
		{
			ps.formatRecord->imageSize32.v = static_cast<int16>(readImageInfo.height);
			ps.formatRecord->imageSize32.h = static_cast<int16>(readImageInfo.width);
		}
		else
		{
			ps.formatRecord->imageSize.v = static_cast<int16>(readImageInfo.height);
			ps.formatRecord->imageSize.h = static_cast<int16>(readImageInfo.width);
		}
		ps.formatRecord->depth = int(BitsPerColor(targetFormat));

		if (separateAlphaChannel)
		{
			ps.formatRecord->planes = compressedImageHasAlpha ? 4 : 3;	// 4 channels for alpha
			ps.formatRecord->transparencyPlane = 3;
		}
		else
		{
			ps.formatRecord->planes = (ps.formatRecord->layerData > 0) ? 4 : 3;	// layers have alpha, background not so much
			ps.formatRecord->transparencyPlane = 3;
		}

		ps.formatRecord->transparencyMatting = 0;


		//Cleanup
		delete[] wholeFileBuffer;

		if (ddsCompressedImage)
			delete ddsCompressedImage;
	}
}

void IntelPlugin::DoReadContinue()
{
	if (texLoadInfo.isLoadingTex && texLoadInfo.currentLayer >= 1)
	{
		texLoadInfo.header = NULL;
		texLoadInfo.pixelData = NULL;
		texLoadInfo.currentLayer = 0;
		texLoadInfo.isLoadingTex = false;
		SetResult(noErr); // Done
		return;
	}

	//Prepare formatRecord to get whole image
	ps.formatRecord->loPlane = 0;
	ps.formatRecord->hiPlane = ps.formatRecord->planes - 1;

	ps.formatRecord->theRect.left = 0;
	ps.formatRecord->theRect.right = ps.formatRecord->imageSize.h;
	ps.formatRecord->theRect.top = 0;
	ps.formatRecord->theRect.bottom = ps.formatRecord->imageSize.v;

	//The offset in BYTES between planes of data in the buffers, for 8 bit interleved data this is 1. Doing this operation computes correctly for depth 16 bit, 32 bit etc 
	ps.formatRecord->planeBytes = (ps.formatRecord->depth + 7) >> 3;
	//The offset in bytes between columns of data in the buffer. usually 1 for non-interleaved data, or hiPlane-loPlane+1 for interleaved data. 
	ps.formatRecord->colBytes = (ps.formatRecord->hiPlane - ps.formatRecord->loPlane + 1) * ps.formatRecord->planeBytes;
	//The offset in bytes between rows of data in the buffer.
	ps.formatRecord->rowBytes = ps.formatRecord->colBytes * (ps.formatRecord->theRect.right - ps.formatRecord->theRect.left);

	if (texLoadInfo.isLoadingTex) {
		texLoadInfo.currentLayer++;

		const UINT blockSize = texLoadInfo.header->tex_format == tex_format_dxt5 ? 16 : 8;
		const UINT blockWidth = (texLoadInfo.header->image_width + 3) / 4;
		const UINT blockHeight = (texLoadInfo.header->image_height + 3) / 4;
		const UINT dataSize = blockWidth * blockHeight * blockSize;
		const UINT stride = texLoadInfo.header->image_width * 4;

		auto imageData = std::make_unique<unsigned long[]>(stride * texLoadInfo.header->image_height);
		if (!imageData)
		{
			errorMessage("Failed to allocate decompression buffer", "TEX Load Error");
			SetResult(memFullErr);
			return;
		}

		if (!texLoadInfo.pixelData) {
			errorMessage("Invalid pixel data pointer", "TEX Load Error");
			SetResult(memFullErr);
			return;
		}


		if (texLoadInfo.header->tex_format == tex_format_dxt5) {
			BlockDecompressImageDXT5(texLoadInfo.header->image_width, texLoadInfo.header->image_height, texLoadInfo.pixelData.get(), imageData.get());
		}
		else if (texLoadInfo.header->tex_format == tex_format_dxt1) {
			BlockDecompressImageDXT1(texLoadInfo.header->image_width, texLoadInfo.header->image_height, texLoadInfo.pixelData.get(), imageData.get());
		} // No need for decompress rgba8

		auto finalImage = std::make_unique<unsigned long[]>(stride * texLoadInfo.header->image_height);
		if (!finalImage)
		{
			errorMessage("Failed to allocate final image buffer", "TEX Load Error");
			SetResult(memFullErr);
			return;
		}

		// RGBA -> ARGB
		for (size_t i = 0; i < texLoadInfo.header->image_width * texLoadInfo.header->image_height; ++i)
		{
			unsigned long rgba = imageData[i];
			unsigned char r = (rgba >> 24) & 0xFF;
			unsigned char g = (rgba >> 16) & 0xFF;
			unsigned char b = (rgba >> 8) & 0xFF;
			unsigned char a = rgba & 0xFF;

			finalImage[i] = (
				(unsigned long)a << 24) |
				((unsigned long)b << 16) |
				((unsigned long)g << 8) |
				((unsigned long)r);
		}

		imageData = std::move(finalImage);
		unsigned int bufferSize = stride * texLoadInfo.header->image_height;
		Ptr pixelData = sPSBuffer->New(&bufferSize, bufferSize);
		if (pixelData == NULL)
		{
			errorMessage("Failed to allocate Photoshop buffer for image data", "TEX Load Error");
			SetResult(memFullErr);
			return;
		}

		// Copy decompressed data to Photoshop buffer
		memcpy(pixelData, imageData.get(), bufferSize);

		// Assign buffer to Photoshop
		ps.formatRecord->data = pixelData;

		// Notify Photoshop we have data ready
		SetResult(ps.formatRecord->advanceState());

		// Cleanup
		ps.formatRecord->data = NULL;
		sPSBuffer->Dispose(&pixelData);
	}
	else {
		//Allocate buffer for ->data field
		//seems we have to allocate for ourselves here because we set maxData to 0
		uint32 bufferSize = (ps.formatRecord->theRect.bottom - ps.formatRecord->theRect.top) * ps.formatRecord->rowBytes;
		Ptr pixelData = sPSBuffer->New(&bufferSize, bufferSize);

		if (pixelData == NULL)
		{
			SetResult(memFullErr);
			return;
		}
		else
		{
			memset(pixelData, 0, bufferSize); //init buffer to 0
			//Assign buffer to photoshop buffer
			ps.formatRecord->data = pixelData;
		}


		//Get pointer to  image
		const Image* img;

		//Get the right image for this iteration (this function is called multiple times in case of mips or cube map)
		if (loadInfo.isCubeMap)
		{
			//Cube map load six images as layers
			img = loadInfo.readImagePtr->GetImage(0, loadInfo.loadMipMapIndex, 0);
			loadInfo.loadMipMapIndex++;
		}
		else if (loadInfo.hasMips)
		{
			//Load image maps as layers
			img = loadInfo.readImagePtr->GetImage(loadInfo.loadMipMapIndex, 0, 0);
			loadInfo.loadMipMapIndex++;
		}
		else
		{
			//Normal single mip image
			img = loadInfo.readImagePtr->GetImages();
		}


		//Now copy image into photoshop buffer
		unsigned8* rowSrcDataPtr = img->pixels;
		unsigned8* rowTgtDataPtr = reinterpret_cast<unsigned8*>(pixelData);

		for (size_t y = 0; y < img->height; y++)
		{
			for (size_t x = 0; x < img->width; x++)
			{

				switch (img->format)
				{
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				{
					unsigned8* src = rowSrcDataPtr + y * img->rowPitch + x * 4;
					unsigned8* dst = rowTgtDataPtr + y * ps.formatRecord->rowBytes + x * ps.formatRecord->colBytes;
					for (int p = 0; p < ps.formatRecord->planes; p++)
					{
						dst[p] = src[p];

						//If we are loading a cube map face then force alpha to white
						if (loadInfo.isCubeMap && loadInfo.hasAlpha)
							dst[ps.formatRecord->planes - 1] = 255;
					}
				} break;

				case DXGI_FORMAT_R16G16B16A16_UNORM:
				{
					const uint16* src = reinterpret_cast<const uint16*>(rowSrcDataPtr + y * img->rowPitch + x * 8);
					uint16* dst = reinterpret_cast<uint16*>(rowTgtDataPtr + y * ps.formatRecord->rowBytes + x * ps.formatRecord->colBytes);
					for (int p = 0; p < ps.formatRecord->planes; p++)
					{
						dst[p] = src[p];

						//If we are loading a cube map face then force alpha to white
						if (loadInfo.isCubeMap && loadInfo.hasAlpha)
							dst[ps.formatRecord->planes - 1] = ConvertTo16Bit(static_cast<unsigned8>(255));
					}
				} break;

				case DXGI_FORMAT_R32G32B32A32_FLOAT:	// any hdr should be decompressed to this format if possible
				{
					const float* src = reinterpret_cast<const float*>(rowSrcDataPtr + y * img->rowPitch + x * 16);
					float* dst = reinterpret_cast<float*>(rowTgtDataPtr + y * ps.formatRecord->rowBytes + x * ps.formatRecord->colBytes);
					for (int p = 0; p < ps.formatRecord->planes; p++)
					{
						dst[p] = src[p];

						//If we are loading a cube map face then force alpha to white
						if (loadInfo.isCubeMap && loadInfo.hasAlpha)
							dst[ps.formatRecord->planes - 1] = 1.0f;
					}
				} break;

				default:
					// not recognized
					break;
				}
			}
		}

		SetResult(ps.formatRecord->advanceState());


		//Cleanup
		ps.formatRecord->data = NULL;
		sPSBuffer->Dispose(&pixelData);
	}
}

void IntelPlugin::DoReadFinish()
{
	if (!texLoadInfo.isLoadingTex) {
		delete loadInfo.readImagePtr;
		loadInfo.readImagePtr = NULL;
	}
	WriteScriptParamsForRead();
	showNormalCursor();
}

void IntelPlugin::DoReadLayerStart()
{
	static uint16  gLayerNameUtf16[10];
	char           gLayerNameAscii[10];
	int i=0;

	if (texLoadInfo.isLoadingTex) {
		//Create Tex Layer
		sprintf(gLayerNameAscii, "Layer 0");

		do
		{
			gLayerNameUtf16[i] = gLayerNameAscii[i];
			i++;
		} while (gLayerNameAscii[i] != '\0');
		gLayerNameUtf16[i] = '\0';

		ps.formatRecord->layerName = gLayerNameUtf16;
		return;
	}
	//Assign layer names
	if (loadInfo.hasMips)
	{
		//Create mip layer name
		sprintf(gLayerNameAscii,"Mip%d", loadInfo.loadMipMapIndex);
		
		do
		{
			gLayerNameUtf16[i] = gLayerNameAscii[i];
			i++;
		} while (gLayerNameAscii[i] != '\0');
		gLayerNameUtf16[i] = '\0';
							
		ps.formatRecord->layerName = gLayerNameUtf16;
	}
	else if (loadInfo.isCubeMap)
	{
		char* facesmap[] = {"+X","-X","+Y","-Y","+Z","-Z"};

		sprintf(gLayerNameAscii,"%s", facesmap[loadInfo.loadMipMapIndex]);
		
		do
		{
			gLayerNameUtf16[i] = gLayerNameAscii[i];
			i++;
		} while (gLayerNameAscii[i] != '\0');
		gLayerNameUtf16[i] = '\0';
							
		ps.formatRecord->layerName = gLayerNameUtf16;
	}
	else
	{
		//ps.formatRecord->layerName = L"Layer 0";
	}
}

//Fill ps.formatRecord->data with the mergedLayers.
void IntelPlugin::FillFromCompositedLayers()
{
	ReadChannelDesc *pChannel;
	char *pLayerData;
	
	int planesToGet_ = ps.formatRecord->hiPlane - ps.formatRecord->loPlane + 1;

	// Get a buffer to hold each channel as we process, formatRecord->planeBytes are computed the first time fetchImage() is called
	pLayerData = sPSBuffer->New(NULL, ps.formatRecord->imageSize.h * ps.formatRecord->imageSize.v * ps.formatRecord->planeBytes);

	if (pLayerData == NULL)
	{
		SetResult(memFullErr);
		return;
	}
		
	//Get the composite channel in this channel list 
    pChannel = ps.formatRecord->documentInfo->mergedCompositeChannels;

	int planeNumber = 0;

	//Copy the RGB channels
	while (pChannel != NULL)
	{
		//Get pixel data from channel
		ReadLayerData(pChannel, pLayerData, ps.formatRecord->imageSize.h, ps.formatRecord->imageSize.v);

		///Copy int data rgb
		for (size_t y = 0; y < ps.formatRecord->imageSize.v; y++)
		{
			for (size_t x = 0; x < ps.formatRecord->imageSize.h; x++)
			{
				int indexToImage = static_cast<int>(ps.formatRecord->imageSize.h*y*planesToGet_ + x*planesToGet_);  //the ScratchImage is always RGBA therefore pitch of 4
				int indexToLayerChannel = static_cast<int>(ps.formatRecord->imageSize.h*y + x);  //here the pitch is only 1
				
				if (ps.formatRecord->depth == 8)
				{
					unsigned8 *rowBigDataPtr = (unsigned8 *)ps.formatRecord->data;
					unsigned8 *rowData = reinterpret_cast<unsigned8 *>(pLayerData); //Get image pointer
					rowBigDataPtr[indexToImage+planeNumber] = rowData[indexToLayerChannel];
				}
				else if (ps.formatRecord->depth == 16)
				{
					unsigned16 *rowBigDataPtr = (unsigned16 *)ps.formatRecord->data;
					unsigned16 *rowData = reinterpret_cast<unsigned16 *>(pLayerData); //Get image pointer
					rowBigDataPtr[indexToImage+planeNumber] = rowData[indexToLayerChannel];
				}
				else if (ps.formatRecord->depth == 32)
				{
					
					float *rowBigDataPtr = (float *)ps.formatRecord->data;
					float *rowData = reinterpret_cast<float *>(pLayerData); //Get image pointer
					rowBigDataPtr[indexToImage+planeNumber] = rowData[indexToLayerChannel];
				}
			}
		}

		pChannel = pChannel->next;
		planeNumber++;
	}

	
	//Get first alpha channel and set as alpha, of not get transparency planes
	if (!(pChannel = ps.formatRecord->documentInfo->alphaChannels))
	{
		pChannel = ps.formatRecord->documentInfo->mergedTransparency;
	}
	
	//Copy alpha
	if ((ps.formatRecord->planes > 3) && (pChannel != NULL))
	{
		//Get pixel data from channel
		ReadLayerData(pChannel, pLayerData, ps.formatRecord->imageSize.h, ps.formatRecord->imageSize.v);

		//Copy into data alpha
		for (size_t y = 0; y < ps.formatRecord->imageSize.v; y++)
		{
			for (size_t x = 0; x < ps.formatRecord->imageSize.h; x++)
			{
				int indexToImage = static_cast<int>(ps.formatRecord->imageSize.h*y*planesToGet_ + x*planesToGet_);  //the ScratchImage is always RGBA therefore pitch of 4
				int indexToLayerChannel = static_cast<int>(ps.formatRecord->imageSize.h*y + x);  //here the pitch is only 1
				
				//Here the planeumber is 3 for alpha
				if (ps.formatRecord->depth == 8)
				{
					unsigned8 *rowBigDataPtr = (unsigned8 *)ps.formatRecord->data;
					unsigned8 *rowData = reinterpret_cast<unsigned8 *>(pLayerData); //Get image pointer
					rowBigDataPtr[indexToImage+3] = rowData[indexToLayerChannel];
				}
				else if (ps.formatRecord->depth == 16)
				{
					unsigned16 *rowBigDataPtr = (unsigned16 *)ps.formatRecord->data;
					unsigned16 *rowData = reinterpret_cast<unsigned16 *>(pLayerData); //Get image pointer
					rowBigDataPtr[indexToImage+3] = rowData[indexToLayerChannel];
				}
				else if (ps.formatRecord->depth == 32)
				{
					
					float *rowBigDataPtr = (float *)ps.formatRecord->data;
					float *rowData = reinterpret_cast<float *>(pLayerData); //Get image pointer
					rowBigDataPtr[indexToImage+3] = rowData[indexToLayerChannel];
				}
			}
		}
	}
		
			
	sPSBuffer->Dispose(static_cast<char**>(&pLayerData));	
}
