////////////////////////////////////////////////////////////////////////////////
// RitoTex — CubeMap
//
// Cube-map assembly and disassembly. Converts between the three on-canvas
// representations (horizontal/vertical cross, six named layers) and a proper
// 6-face DirectXTex cube ScratchImage, plus the helper that saves a single
// cube mip level back out as a standalone cube DDS.
//
// Face order throughout is +X,-X,+Y,-Y,+Z,-Z (the CubemapGen convention).
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"

#include <map>
#include <string>

using namespace DirectX;

//Return true if texture type cube maps and the setMipLevel is checked
bool IntelPlugin::IsCubeMapWithSetMipLevelOverride()
{
	if (ps.data->SetMipLevel &&
	   (ps.data->TextureTypeIndex == TextureTypeEnum::CUBEMAP_CROSSED || ps.data->TextureTypeIndex == TextureTypeEnum::CUBEMAP_LAYERS))
	   return true;

	return false;
}

//Convert scrUncompressedImageScratch_ from a  crossed layout image to a cube map ScratchImage
void IntelPlugin::ConvertToCubeMapFromCross(ScratchImage **scrUncompressedImageScratch_)
{
		//Open a empty uncompressed scratch image
	    ScratchImage *cubemapUncompressedImageScratch = new ScratchImage();

		//Hold the rectangles which define the siz cube face in the large image
		DirectX::Rect coords[6];

		//These coords are for the following cubemap order +X,-X,+Y,-Y,+Z,-Z . This is the order CubemapGen likes
		//They define a absolute width/height multiplactor into the image to get the coords for horizontal.vertical crossed layout cubemap .
		//CubeMaps have all their 6 faces equal.
		int crossedCoords[6][2] =  {{2,1}, {0,1}, {1,0}, {1,2}, {1,1}, {3,1}};

		//Setup width/height with Horziontal(4x3) cross layout as default
		int width = ps.formatRecord->imageSize.h/4;
		int height = ps.formatRecord->imageSize.v/3;

		//Determine if Vertical(3x4) cubemap layout
		if (ps.formatRecord->imageSize.h < ps.formatRecord->imageSize.v)
		{
			//Vertical Cross layout
			width = ps.formatRecord->imageSize.h/3;
			height = ps.formatRecord->imageSize.v/4;

			//flip the last coords if in vertical layout
			crossedCoords[5][0] = 1;
			crossedCoords[5][1] = 3;
		}

		//Compute final coordinates
		for (int i=0; i<6; i++)
		{
			coords[i] = DirectX::Rect(crossedCoords[i][0]*width, crossedCoords[i][1]*height, width, height);
		}


		//Allocate a cubemap scratchImage
		cubemapUncompressedImageScratch->InitializeCube(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);


		//Copy from crossed cubemap from big scrUncompressedImageScratch_ to images in cubemapUncompressedImageScratch
		const Image *crossedImage = (*scrUncompressedImageScratch_)->GetImages();
		const Image *cubeImage = cubemapUncompressedImageScratch->GetImages();

		for (int i=0; i<6; i++)
		{
			DirectX::CopyRectangle(*crossedImage, coords[i], cubeImage[i], TEX_FILTER_DEFAULT, 0, 0);
		}


		//Free previous space
		delete *scrUncompressedImageScratch_;

		//Copy over pointer from ScratchImage which has CubeMap,
		//now scrUncompressedImageScratch holds the crossed cubemap disected
		*scrUncompressedImageScratch_ = cubemapUncompressedImageScratch;
}

//Convert scrUncompressedImageScratch_ from document layers to a cube map DirectX::ScratchImage
bool IntelPlugin::ConvertToCubeMapFromLayers(ScratchImage **scrUncompressedImageScratch_, bool hasAlpha_)
{
	ReadChannelDesc *pChannel;
	ReadLayerDesc *layerDesc;
	char *pLayerData;

	if (ps.formatRecord->documentInfo->layerCount < 6)
	{
	    return false;
	}

	// Get a buffer to hold each channel as we process, formatRecord->planeBytes are computed the first time fetchImage() is called
	pLayerData = sPSBuffer->New(NULL, ps.formatRecord->imageSize.h * ps.formatRecord->imageSize.v * ps.formatRecord->planeBytes);

	if (pLayerData == NULL)
	{
		SetResult(memFullErr);
		return false;
	}

	//Open a empty uncompressed scratch image
    ScratchImage *cubemapUncompressedImageScratch = new ScratchImage();

	//All layers must have these names and feature the correspanding image +X,-X,+Y,-Y,+Z,-Z.
	//Any layers with different names will be ignored.
	//There must be at least 6 layers present including the background layer, otherwise some cube faces will be blank.
	//The final Cubemap dds will have the images in this order, +X,-X,+Y,-Y,+Z,-Z. This is the order CubemapGen likes and sort of a standard.

	//Which face goes into what index inside the cubemap
	std::map<std::string, int> facesmap;
	facesmap["+X"]=0;
	facesmap["-X"]=1;
	facesmap["+Y"]=2;
	facesmap["-Y"]=3;
	facesmap["+Z"]=4;
	facesmap["-Z"]=5;

	//Setup width/height
	int width =  static_cast<int>((*scrUncompressedImageScratch_)->GetMetadata().width);
	int height = static_cast<int>((*scrUncompressedImageScratch_)->GetMetadata().height);

	//Allocate a cubemap scratchImage
	cubemapUncompressedImageScratch->InitializeCube(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);

	//*****************************************************
	//Copy from Layers into cubemapUncompressedImageScratch
	const Image *cubeImage = cubemapUncompressedImageScratch->GetImages();


	//Get the first layer in this layer list
	layerDesc = ps.formatRecord->documentInfo->layersDescriptor;

	//Cycle through remaining layers, Assign each layer to a different mip map in ScratchImage
	while (layerDesc != NULL)
	{
		std::map<std::string,int>::iterator it;

		//Check layername for validity only these are supported "+X","-X","+Y","-Y","+Z","-Z"
		it = facesmap.find(layerDesc->name);
		if ( it != facesmap.end())
		{
			//Get index into SratchImage array for this face name
			int i = it->second;

			//Get the first channel in this channel list
			pChannel = layerDesc->compositeChannelsList;
			int planeNumber = 0;

			//Get the RGB channels
			while (pChannel != NULL)
			{
				//Get pixel data from channel
				ReadLayerData(pChannel, pLayerData, ps.formatRecord->imageSize.h, ps.formatRecord->imageSize.v);

				for (size_t y = 0; y < cubeImage[i].height; y++)
				{
					for (size_t x = 0; x < cubeImage[i].width; x++)
					{
						int indexToImage = int(cubeImage[i].width*y*4 + x*4);  //the ScratchImage is always RGBA therefore pitch of 4
						int indexToLayerChannel = int(ps.formatRecord->imageSize.h*y + x);  //here the pitch is only 1

						CopyFromLayerChannelIntoImage(pLayerData, &cubeImage[i], indexToImage+planeNumber, indexToLayerChannel);
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

			for (size_t y = 0; y < cubeImage[i].height; y++)
			{
				for (size_t x = 0; x < cubeImage[i].width; x++)
				{
					int indexToImage = int(cubeImage[i].width*y*4 + x*4);  //the ScratchImage is always RGBA therefore pitch of 4
					int indexToLayerChannel = int(ps.formatRecord->imageSize.h*y + x);  //here the pitch is only 1

					//Here the plane number is 3 for alpha
					CopyFromLayerChannelIntoImage(pLayerData, &cubeImage[i], indexToImage+3, indexToLayerChannel);
				}
			}

		}

		layerDesc = layerDesc->next; //Get next layer
	}

	sPSBuffer->Dispose(static_cast<char**>(&pLayerData));


	//Free previous space
	delete *scrUncompressedImageScratch_;

	//Copy over pointer from ScratchImage which has CubeMap,
	//now scrUncompressedImageScratch holds the crossed cubemap disected
	*scrUncompressedImageScratch_ = cubemapUncompressedImageScratch;

	return true;
}

//Convert scrUncompressedImageScratch_ from a cube map to a horizontal crossed layout image
void IntelPlugin::ConvertToHorizontalCrossFromCubeMap(ScratchImage **scrUncompressedImageScratch_)
{
		//Open a empty uncompressed scratch image
	    ScratchImage *crossedUncompressedImageScratch = new ScratchImage();

		//Hold the rectangles which define the siz cube face in the large image
		DirectX::Rect coord;

		//Which mipLevel to get for crossed image cubemap preview. Normaly 0 but can change when setMipLevels is specified
		int mipLevel = 0;

		//These coords are for the following cubemap order +X,-X,+Y,-Y,+Z,-Z . This is the order CubemapGen likes
		//They define a absolute width/height multiplactor into the image to get the coords for horizontal.vertical crossed layout cubemap .
		//CubeMaps have all their 6 faces equal.
		int crossedCoords[6][2] =  {{2,1}, {0,1}, {1,0}, {1,2}, {1,1}, {3,1}};

		//Setup width/height with Horziontal(4x3) cross layout as default
		int width = static_cast<int>((*scrUncompressedImageScratch_)->GetMetadata().width);
		int height = static_cast<int>((*scrUncompressedImageScratch_)->GetMetadata().height);

		//============================================================================================
	    //============================================================================================
	    //Force mipmaps if setMipLevels on cube maps are specified
    	if (IsCubeMapWithSetMipLevelOverride())
		{
			//Open temporary imageScratch to save mipmaps
			ScratchImage *scrImageMipMapScratch = new ScratchImage();

			//Generate MipMaps from scrUncompressedImageScratch and save to scrImageMipMapScratch
			HRESULT hr = GenerateMipMaps((*scrUncompressedImageScratch_)->GetImages(), (*scrUncompressedImageScratch_)->GetImageCount(),
										 (*scrUncompressedImageScratch_)->GetMetadata(), TEX_FILTER_DEFAULT|TEX_FILTER_SEPARATE_ALPHA, 0, *scrImageMipMapScratch );
			if( hr != S_OK )
			{
				UserError("Could not create MipMaps");
				return;
			}

		    //Override with mip level size
		    width = int(scrImageMipMapScratch->GetImage(ps.data->MipLevel, 0, 0)->width);
		    height = int(scrImageMipMapScratch->GetImage(ps.data->MipLevel, 0, 0)->height);

			//Set which mip level to geto for creating crossed image
			mipLevel = ps.data->MipLevel;

			//free previous space
			delete *scrUncompressedImageScratch_;

			//Copy over pointer from ScratchImage which has MipMap, now scrUncompressedImageScratch holds the initial image + mip chain
			(*scrUncompressedImageScratch_) = scrImageMipMapScratch;
		}

		coord = DirectX::Rect(0, 0, width, height);

		//Allocate a scratchImage
		crossedUncompressedImageScratch->Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, width*4, height*3, 1, 1);

		//Copy from cubemap scrUncompressedImageScratch_ to crossed horizontal layout in crossedUncompressedImageScratch
		const Image *crossedImage = crossedUncompressedImageScratch->GetImages();

		for (int i=0; i<6; i++)
		{
		    const Image *cubeImage = (*scrUncompressedImageScratch_)->GetImage(mipLevel, i, 0);
			DirectX::CopyRectangle(*cubeImage, coord, *crossedImage, TEX_FILTER_DEFAULT, crossedCoords[i][0]*width, crossedCoords[i][1]*height);
		}

		//Free previous space
		delete *scrUncompressedImageScratch_;

		//Copy over pointer from ScratchImage which has CubeMap,
		//now scrUncompressedImageScratch holds the crossed cubemap disected
		*scrUncompressedImageScratch_ = crossedUncompressedImageScratch;
}

//Saves a special mip version of acube map out as a normal cube map. Special function to save out low res cubemaps
HRESULT IntelPlugin::SaveCubeMipLevelToDDSFile(ScratchImage *scrImageScratch_, Blob& blob)
{
		//Open a empty uncompressed scratch image
	    ScratchImage customMipLevelScratch;
		Image customMipLevel[6];

		//Get specific mip images
		for (int i=0; i<6; i++)
			customMipLevel[i] = *scrImageScratch_->GetImage(ps.data->MipLevel, i, 0);

		//Init new cube map from these images
		customMipLevelScratch.InitializeCubeFromImages(customMipLevel, 6);

		//Save to file
		return SaveToDDSMemory(customMipLevelScratch.GetImages(), customMipLevelScratch.GetImageCount(), customMipLevelScratch.GetMetadata(), DDS_FLAGS_NONE, blob);
}
