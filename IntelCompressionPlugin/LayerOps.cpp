////////////////////////////////////////////////////////////////////////////////
// RitoTex — LayerOps
//
// Reading Photoshop document layers and feeding them into a ScratchImage.
// Covers raw channel reads, white-fill fallback for missing alpha, per-channel
// copy into an RGBA image, and the "mipmaps from layers" path that maps each
// document layer onto a successive mip level.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"

#include <climits>

using namespace DirectX;

//Returns true if mip maps are defined by layers
//Mip map form layers is not applicable to cube maps.
bool IntelPlugin::IsMipMapsDefinedByLayer()
{
	if (ps.data->MipMapTypeIndex == MipmapEnum::FROM_LAYERS &&
		ps.data->TextureTypeIndex != TextureTypeEnum::CUBEMAP_CROSSED &&
	    ps.data->TextureTypeIndex != TextureTypeEnum::CUBEMAP_LAYERS &&
		ps.formatRecord->documentInfo->layerCount > 1)
		return true;

	return false;
}

/* Read in the channel data from the original document. */
void IntelPlugin::ReadLayerData(ReadChannelDesc *pChannel, char *pLayerData, int width, int height)
{
	// Make sure there is something for me to read from
	if (pChannel == NULL || pChannel->port == NULL || pLayerData == NULL)
		return;

	Boolean canRead;
	if (sPSChannelProcs->CanRead(pChannel->port, &canRead))
	{
		// this function should not error, tell the host accordingly
		SetResult(errPlugInHostInsufficient);
		return;
	}

	if (!canRead)
		return;

	// some local variables to play with
	VRect read_rect;
	PixelMemoryDesc destination;

	// What area of the document do we want to read from
	read_rect.top = 0;
	read_rect.left = 0;
	read_rect.bottom = height;
	read_rect.right = width;

	// set up the PixelMemoryDesc
	destination.data = pLayerData;
	destination.depth = pChannel->depth;
	destination.rowBits = width*pChannel->depth;
	destination.colBits = pChannel->depth;
	destination.bitOffset = 0;

	// Read this data into our buffer, you could check the read_rect to see if you got everything you desired
	if (sPSChannelProcs->ReadPixelsFromLevel(
		pChannel->port,
		0,
		&read_rect,
		&destination))
	{
		SetResult(errPlugInHostInsufficient);
		return;
	}
}

void IntelPlugin::FillLayerDataToWhite(char *pLayerData, int width, int height)
{
	for (int x=0; x<width; x++)
	{
		for (int y=0; y<height; y++)
		{
			int indexToLayerChannel = y*width + x;

			if (ps.formatRecord->depth == 8)
			{
				unsigned8 *rowData = reinterpret_cast<unsigned8 *>(pLayerData);
				rowData[indexToLayerChannel] = 255;
			}
			else if (ps.formatRecord->depth == 16)
			{
				unsigned16 *rowData16bit = reinterpret_cast<unsigned16 *>(pLayerData);
				rowData16bit[indexToLayerChannel] = ConvertTo16Bit(static_cast<unsigned8>(255));
			}
			else if (ps.formatRecord->depth == 32)
			{
				float *rowData32bit = reinterpret_cast<float *>(pLayerData);
				rowData32bit[indexToLayerChannel] = 1.0;
			}
		}
	}
}

// Copy one component from layer channel to Image
void IntelPlugin::CopyFromLayerChannelIntoImage(char *pLayerData, const Image *image, int indexToImage, int indexToLayerChannel)
{
	//Get pointer to first (and only) image
	unsigned8 *rowBigDataPtr = image->pixels;

	//Convert pixels to 8bit for BC encoding
	if (ps.formatRecord->depth == 8)
	{
		unsigned8 *rowData = reinterpret_cast<unsigned8 *>(pLayerData);
		rowBigDataPtr[indexToImage] = ConvertTo8Bit(rowData[indexToLayerChannel]);
	}
	else if (ps.formatRecord->depth == 16)
	{
		unsigned16 *rowData16bit = reinterpret_cast<unsigned16 *>(pLayerData);
		rowBigDataPtr[indexToImage] = ConvertTo8Bit(rowData16bit[indexToLayerChannel]);
	}
	else if (ps.formatRecord->depth == 32)
	{
		float *rowData32bit = reinterpret_cast<float *>(pLayerData);
		rowBigDataPtr[indexToImage] = ConvertTo8Bit(rowData32bit[indexToLayerChannel]);
	}
}

//Copy document Layers into mipmap Images of ScratchImage
//startMipIndex: specify from which layer to start copy.
//endMipIndex: until which layer to copy. You can leave this blank in which case it will copy al remaining layers.
//Each layer copies into the corresponding mip map level accorsing to its order from the base layer.
void IntelPlugin::CopyLayersIntoMipMaps(ScratchImage *scrUncompressedImageScratch_, bool hasAlpha_, int startMipIndex, int endMipIndex)
{
	int mipMapIndex = startMipIndex;  // mip level=0 is original image
	ReadChannelDesc *pChannel;
	ReadLayerDesc *layerDesc;
	char *pLayerData;

	// Get a buffer to hold each channel as we process, formatRecord->planeBytes are computed the first time fetchImage() is called
	pLayerData = sPSBuffer->New(NULL, ps.formatRecord->imageSize.h * ps.formatRecord->imageSize.v * ps.formatRecord->planeBytes);

	if (pLayerData == NULL)
	{
		SetResult(memFullErr);
		return;
	}

	//Get the first layer (usually Backround) in this layer list
	layerDesc = ps.formatRecord->documentInfo->layersDescriptor;

	//Skip initial layers to startLayerIndex
	int startLayerIndex = startMipIndex;
	while (startLayerIndex != 0)
	{
		layerDesc = layerDesc->next;
		startLayerIndex--;
	}

	//Cycle through remaining layers, Assign each layer to a different mip map in ScratchImage
	while ((layerDesc != NULL) && (mipMapIndex <= endMipIndex))
	{
		//There are not enough mip maps in this Image, there are too much layers
		if (int(scrUncompressedImageScratch_->GetImageCount()) <= mipMapIndex)
			break;

		//Get mip map image
		const Image *image = scrUncompressedImageScratch_->GetImages() + mipMapIndex;
		if (!image)
			break;

		//Get the first channel in this channel list
		pChannel = layerDesc->compositeChannelsList;
		int planeNumber = 0;

		//Get the RGB channels
		while (pChannel != NULL)
		{
			//Get pixel data from channel
			ReadLayerData(pChannel, pLayerData, ps.formatRecord->imageSize.h, ps.formatRecord->imageSize.v);

			for (size_t y = 0; y < image->height; y++)
			{
				for (size_t x = 0; x < image->width; x++)
				{
					int indexToImage = int(image->width*y*4 + x*4);  //the ScratchImage is always RGBA therefore pitch of 4
					int indexToLayerChannel = int(ps.formatRecord->imageSize.h*y + x);  //here the pitch is only 1

					CopyFromLayerChannelIntoImage(pLayerData, image, indexToImage+planeNumber, indexToLayerChannel);
				}
			}

			pChannel = pChannel->next;
			planeNumber++;
		}


		//Get first layermask and set as alpha
		//If no layermask then use transparency channel
		if (!(pChannel = layerDesc->layerMask))
		{
			//Get first Transparency channel and set as alpha
			pChannel = layerDesc->transparency;
		}

		//Get alpha
		if (hasAlpha_ && (pChannel != NULL))
		{
			//Get pixel data from channel
			ReadLayerData(pChannel, pLayerData, ps.formatRecord->imageSize.h, ps.formatRecord->imageSize.v);
		}
		else
		{
			FillLayerDataToWhite(pLayerData, ps.formatRecord->imageSize.h, ps.formatRecord->imageSize.v);
		}

		for (size_t y = 0; y < image->height; y++)
		{
			for (size_t x = 0; x < image->width; x++)
			{
				int indexToImage = int(image->width*y*4 + x*4);  //the ScratchImage is always RGBA therefore pitch of 4
				int indexToLayerChannel = int(ps.formatRecord->imageSize.h*y + x);  //here the pitch is only 1

				//Here the planeumber is 3 for alpha
				CopyFromLayerChannelIntoImage(pLayerData, image, indexToImage+3, indexToLayerChannel);
			}
		}

		layerDesc = layerDesc->next; //Get next layer
		mipMapIndex++; //Next mip map
	}

	sPSBuffer->Dispose(static_cast<char**>(&(pLayerData)));
}
