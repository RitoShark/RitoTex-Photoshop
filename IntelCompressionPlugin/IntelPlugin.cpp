////////////////////////////////////////////////////////////////////////////////
// Copyright 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License.  You may obtain a copy
// of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
// License for the specific language governing permissions and limitations
// under the License.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"
#include "IntelPluginUIWin.h"
#include "CustomSaveDialog.h"
#include "UpdateChecker.h"
#include <memory>
#include <algorithm>
#include <string>

using namespace DirectX;

	// this global pointer is necessary to use code from PIUSuites.cpp
SPBasicSuite * sSPBasic = NULL;


IntelPlugin& IntelPlugin::GetInstance()
{
	static IntelPlugin singleton;
	return singleton;
}

IntelPlugin::IntelPlugin(void)
{
	memset(&ps, 0, sizeof(ps));
}


IntelPlugin::~IntelPlugin(void)
{
}


// ===========================================================================
bool IntelPlugin::IsCombinationValid(TextureTypeEnum textype, CompressionTypeEnum comptype)
{
	if (textype < TextureTypeEnum::TEXTURE_TYPE_COUNT && comptype < CompressionTypeEnum::COMPRESSION_TYPE_COUNT)
	{
		//Matrix of which compression options make sense depending on the selected texture type
		//Rows are the TextureTypeEnum, and Columns the Compression types
		bool CompressionVsTextureTypeMatrix[][CompressionTypeEnum::COMPRESSION_TYPE_COUNT] =
		{
		//   BC1,    BC3,    NONE
			{true,   false,  true}, //COLOR
			{false,  true,   true}, //COLOR+A
			{true,   true,   true}, //CUBEMAP+LAYER
			{true,   true,   true}, //CUBEMAP+CROSS
			{true,   false,  true}, //NORMAL MAP
		};

		return CompressionVsTextureTypeMatrix[textype][comptype];
	}

	return false;
}

/*****************************************************************************/
/*****************************************************************************/
//Cursor WAIT->ARROW convenince functions
void IntelPlugin::showLoadingCursor()
{
	::SetCursor( LoadCursor( 0, IDC_WAIT ) );
}

void IntelPlugin::showNormalCursor()
{
	//Forces a WM_SETCURSOR mesï¿½sage.
    POINT pt; // Screen coordinates!
    ::GetCursorPos(&pt);
    ::SetCursorPos( pt.x, pt.y );
}

/*****************************************************************************/
/*****************************************************************************/
//Copy data from photoshop buffer into scrUncompressedImageScratch_
bool IntelPlugin::CopyDataForEncoding(ScratchImage *scrUncompressedImageScratch_, bool hasAlpha_, bool DoMipMaps_, bool gammaCorrect)
{
	//Do advanceState to get image data, fill the ps.formatRecord->data buffer
	FetchImageData();

	int planesToGet_ = ps.formatRecord->hiPlane - ps.formatRecord->loPlane + 1;

	//Allocate space for one rgba 8bit image
	scrUncompressedImageScratch_->Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, ps.formatRecord->imageSize.h, ps.formatRecord->imageSize.v, 1, 1, DDS_FLAGS_NONE);

	//Get pointer to first (and only) image
	unsigned8 *rowBigDataPtr = scrUncompressedImageScratch_->GetImages()->pixels;

	//Copy data from photoshop buffer into scrUncompressedImageScratch
	//Convert pixels to 8bit for BC encoding
	if (ps.formatRecord->depth == 8)
	{
		ConvertToBCFrom8Bit(rowBigDataPtr, planesToGet_, hasAlpha_);
	}
	else if (ps.formatRecord->depth == 16)
	{
		ConvertToBCFrom16Bit(rowBigDataPtr, planesToGet_, hasAlpha_);
	}
	else if (ps.formatRecord->depth == 32)
	{
		ConvertToBCFrom32Bit(rowBigDataPtr, planesToGet_, hasAlpha_, gammaCorrect);
	}
	else
	{
		return false;
	}
	
	return true;
}


/*****************************************************************************/
/*****************************************************************************/
//Take an Uncompressed ScratchImage scrUncompressedImageScratch_ and Compresses it into scrImageScratch_






// get RGB buffer





/*****************************************************************************/
/*****************************************************************************/
//Compress rgba_surface into tgtPixels, uisng Intel ISPC encoders

/*****************************************************************************/
/*****************************************************************************/
//Pad the surface size to boundaries of 4
//Calculates new width,height,stride and new buffer
//It does not deallocate the old buffer
//It does return the new surface 
//It is the responsibility of the user to deallocate the new/old buffer when not needed anymore

/*****************************************************************************/
/*****************************************************************************/
//AdvanceState () has to be called before entering this function so that the ps.formatRecord->data buffer is full;

/*****************************************************************************/
/*****************************************************************************/
//AdvanceState () has to be called before entering this function so that the ps.formatRecord->data buffer is full;

/*****************************************************************************/
/*****************************************************************************/
//Return true if texture type cube maps and the setMipLevel is checked

/*****************************************************************************/
/*****************************************************************************/
//Returns true if mip maps are defined by layers
//Mip map form layers is not applicable to cube maps.


/*****************************************************************************/
/*****************************************************************************/
//Convert scrUncompressedImageScratch_ from a  crossed layout image to a cube map ScratchImage

/*****************************************************************************/
/*****************************************************************************/
//Convert scrUncompressedImageScratch_ from document layers to a cube map DirectX::ScratchImage

/*****************************************************************************/
/*****************************************************************************/
//Convert scrUncompressedImageScratch_ from a cube map to a horizontal crossed layout image 

/*****************************************************************************/
/*****************************************************************************/
//Flip the X(Red) channel and or the Y(Green) channel of the normal map

/*****************************************************************************/
/*****************************************************************************/
//Normalize all values of this Nomral map in main image and all mip maps

/*****************************************************************************/
/*****************************************************************************/
/* Read in the channel data from the original document. */


/*****************************************************************************/
/*****************************************************************************/
// Copy one component from layer channel to Image 

/*****************************************************************************/
/*****************************************************************************/
//Copy document Layers into mipmap Images of ScratchImage
//startMipIndex: specify from which layer to start copy. 
//endMipIndex: until which layer to copy. You can leave this blank in which case it will copy al remaining layers.
//Each layer copies into the corresponding mip map level accorsing to its order from the base layer.


/*****************************************************************************/
/*****************************************************************************/
//Saves a special mip version of acube map out as a normal cube map. Special function to save out low res cubemaps

void IntelPlugin::InitData()
{
	memset(ps.data, 0, sizeof(*ps.data));

	ps.data->gMultithreaded = GetProcessorCount() > 1;
	ps.data->encoding_g     = DXGI_FORMAT_BC3_UNORM;
	ps.data->queryForParameters = true;

	ps.data->TextureTypeIndex = TextureTypeEnum::COLOR;   //Col,Col+alpha,CubeFromLayer,CubefromCross,NM
	ps.data->MipMapTypeIndex = MipmapEnum::NONE;      //None,Autogen,FromLayers
	ps.data->MipLevel=0;			                   // only valid if SetMipLevel == true
	ps.data->SetMipLevel   = false;
	ps.data->Normalize     = false;
	ps.data->FlipX         = false;
	ps.data->FlipY         = false;
	ps.data->exposure	   = 1;
	ps.data->mipmapBatchAllowed = false;
	ps.data->alphaBatchSeperate = false;
	ps.data->useDithering = true;       // Default: dithering enabled
	ps.data->useUniformMetric = false;  // Default: perceptual metric
} 


/*****************************************************************************/
// Detect if we're running as the TEX format plugin entry.
// Uses formatRecord->finalSpec (New in PS 11.0 / CS4) which contains the
// final output file path with the correct extension (.dds or .tex).
// Photoshop writes to a .tmp file first, so dataFork/fileSpec point to .tmp,
// but finalSpec always has the real destination path.






//Report error to Photoshop, plugin aborts by itself depicting this error message automatically
void IntelPlugin::UserError(const char *usrerror)
{
	unsigned char *pErrorString = reinterpret_cast<unsigned char*>(ps.formatRecord->errorString);
	
	if (pErrorString != NULL && (strlen(usrerror) < 256))
	{
		*ps.resultPtr = errReportString;

		*pErrorString = unsigned char(strlen(usrerror));

		memcpy(pErrorString+1, usrerror, unsigned char(*pErrorString));
	}
}

void IntelPlugin::FetchImageData()
{
	if (!ps.formatRecord->data)
	{
		int planesToGet = ps.formatRecord->planes;
		if (planesToGet > 4)
			planesToGet = 4;

		ps.formatRecord->theRect.left = 0;
		ps.formatRecord->theRect.right = ps.formatRecord->imageSize.h;
		ps.formatRecord->theRect.top = 0;
		ps.formatRecord->theRect.bottom = ps.formatRecord->imageSize.v;
		ps.formatRecord->loPlane = 0;
		ps.formatRecord->hiPlane = planesToGet - 1;
		//The offset in bytes between planes of data in the buffers, for 8 bit interleved data this is 1. Doing this operation computes correctly for depth 16,32 etc 
		ps.formatRecord->planeBytes = (ps.formatRecord->depth + 7) >> 3; 
		//The offset in bytes between columns of data in the buffer. usually 1 for non-interleaved data, or hiPlane-loPlane+1 for interleaved data. 
		ps.formatRecord->colBytes = (ps.formatRecord->hiPlane - ps.formatRecord->loPlane + 1) * ps.formatRecord->planeBytes;
		//The offset in bytes between rows of data in the buffer.
		ps.formatRecord->rowBytes = ps.formatRecord->colBytes * (ps.formatRecord->theRect.right - ps.formatRecord->theRect.left);

	    // seems we have to allocate for ourselves here because we set maxData to 0
		uint32 bufferSize = 
			(ps.formatRecord->theRect.bottom - ps.formatRecord->theRect.top) * 
			ps.formatRecord->rowBytes;

		if (ps.formatRecord->data = sPSBuffer->New( &bufferSize, bufferSize))
		{
			if (ps.formatRecord->documentInfo->layerCount > 1)
			{
				//Because Layermode is enabled for dds mip loading we can not just use the alpha as ususal when there are layers.
		        //RGBA should come from the merged layer data, all this is done in FillFromCompositedLayers().
		        FillFromCompositedLayers();
			}
			else
			{
			    SetResult(ps.formatRecord->advanceState());
			}
		}
		else
			SetResult(memFullErr);

	}
}

void IntelPlugin::DisposeImageData()
{
	if (ps.formatRecord->data)
	{
		sPSBuffer->Dispose(reinterpret_cast<char**>(&ps.formatRecord->data));
		ps.formatRecord->data = NULL;
	}

	if (preview.compressedImage)
	{
		delete preview.compressedImage;
		preview.compressedImage = NULL;
	}
	if (preview.uncompressedImage)
	{
		delete preview.uncompressedImage;
		preview.uncompressedImage = NULL;
	}
}



bool IntelPlugin::SetProgress(int part, int total)
{
	if (ps.formatRecord)
	{
		if (ps.data->previewing)	// don't do progress if we have UI! Allow cancel though
		{
			return !ps.formatRecord->abortProc();
		}
		else
		{
			if (ps.formatRecord->progressProc)
				ps.formatRecord->progressProc(part, total);
			if (ps.formatRecord->abortProc)
			{
				if (ps.formatRecord->abortProc())	// TRUE if user aborted
				{
					SetResult(userCanceledErr);
					return false;
				}
			}
		}
	}

	return true;	// continue!
}


/*****************************************************************************/
//Main compression function which calls all other functions and saves dds

/*****************************************************************************/
// Helper function to map DirectX format to TEX format enum

/*****************************************************************************/
// Helper function to build TEX header

/*****************************************************************************/
// Helper function to write TEX mipmaps in reverse order (smallest to largest)

/*****************************************************************************/
// Helper function to write TEX file with header and image data

/*****************************************************************************/
// Main TEX compression and save function






// Read/load path (DoFilterFile, DoRead*, FillFromCompositedLayers, texLoadInfo) moved to PhotoshopRead.cpp

//-------------------------------------------------------------------------------
//
// CreateDataHandle
//
// Create a handle to our Data structure. Photoshop will take ownership of this
// handle and delete it when necessary.
//-------------------------------------------------------------------------------
void IntelPlugin::CreateDataHandle(void)
{
	Handle h = sPSHandle->New(sizeof(Globals));
	if (h != NULL)
		*ps.dataPtr = reinterpret_cast<intptr_t>(h);
	else
		*ps.resultPtr = memFullErr;
}

//-------------------------------------------------------------------------------
//
// LockHandles
//
// Lock the handles and get the pointers for data
// Set the global error, *ps.resultPtr, if there is trouble
//
//-------------------------------------------------------------------------------
void IntelPlugin::LockHandles(void)
{
	if ( ! (*ps.dataPtr) )
	{
		*ps.resultPtr = formatBadParameters;
		return;
	}
	
	Boolean oldLock = FALSE;
	sPSHandle->SetLock(reinterpret_cast<Handle>(*ps.dataPtr), true, 
		               reinterpret_cast<Ptr *>(&ps.data), &oldLock);
	
	if (ps.data == NULL)
	{
		*ps.resultPtr = memFullErr;
		return;
	}
}

//-------------------------------------------------------------------------------
//
// UnlockHandles
//
// Unlock the handles used by the data and params pointers
//
//-------------------------------------------------------------------------------
void IntelPlugin::UnlockHandles(void)
{
	Boolean oldLock = FALSE;
	if (*ps.dataPtr)
		sPSHandle->SetLock(reinterpret_cast<Handle>(*ps.dataPtr), false, 
		                   reinterpret_cast<Ptr *>(&ps.data), &oldLock);
}



void IntelPlugin::PluginMain(const int16 selector,
						             FormatRecordPtr formatRecord,
						             intptr_t * data,
						             int16 * result)
{
	//---------------------------------------------------------------------------
	//	Store persistent data
	//---------------------------------------------------------------------------

	ps.formatRecord = formatRecord;
	ps.pluginRef = reinterpret_cast<SPPluginRef>(formatRecord->plugInRef);
	ps.resultPtr = result;
	ps.dataPtr = data;

	//---------------------------------------------------------------------------
	//	(2) Check for about box request.
	//
	// 	The about box is a special request; the parameter block is not filled
	// 	out, none of the callbacks or standard data is available.  Instead,
	// 	the parameter block points to an AboutRecord, which is used
	// 	on Windows.
	//---------------------------------------------------------------------------
	if (selector == formatSelectorAbout)
	{
		AboutRecordPtr aboutRecord = reinterpret_cast<AboutRecordPtr>(formatRecord);
		sSPBasic = aboutRecord->sSPBasic;
		ShowAboutIntel(aboutRecord);
	}
	else
	{ // do the rest of the process as normal:

		sSPBasic = ps.formatRecord->sSPBasic;

		if (formatRecord->advanceState == NULL)
		{
			*ps.resultPtr = errPlugInHostInsufficient;
			return;
		}

		// new for Photoshop 8, big documents, rows and columns are now > 30000 pixels
		//if (formatRecord->HostSupports32BitCoordinates)
		//	formatRecord->PluginUsing32BitCoordinates = true;


		//-----------------------------------------------------------------------
		//	(3) Allocate and initalize ps.data.
		//
		//-----------------------------------------------------------------------

 		if ( ! (*ps.dataPtr) )
		{
			CreateDataHandle();
			if (*ps.resultPtr != noErr) return;
			LockHandles();
			if (*ps.resultPtr != noErr) return;
			InitData();
		}

		if (*ps.resultPtr == noErr)
		{
			LockHandles();
			if (*ps.resultPtr != noErr) return;
		}


		//-----------------------------------------------------------------------
		//	(4) Dispatch selector.
		//-----------------------------------------------------------------------
		switch (selector)
		{
			case formatSelectorReadPrepare:
				DoReadPrepare();
				break;
			case formatSelectorReadStart:
				RitoTex::MaybeCheckForUpdatesAsync();
				DoReadStart();
				break;
			case formatSelectorReadContinue:
				DoReadContinue(); //This is run when no layers are set
				break;
			case formatSelectorReadFinish:
				DoReadFinish();
				break;

			case formatSelectorOptionsPrepare:
				ps.formatRecord->maxData = 0;
				break;
			case formatSelectorOptionsStart:
				ps.formatRecord->data = NULL;
				break;
			case formatSelectorOptionsContinue:
				break;
			case formatSelectorOptionsFinish:
				break;

				// estimations
			case formatSelectorEstimatePrepare:
				ps.formatRecord->maxData = 0;
				break;
			case formatSelectorEstimateStart:
				{
					int dataBytes = ps.formatRecord->imageSize32.h * ps.formatRecord->imageSize32.v;
					formatRecord->minDataBytes = dataBytes >> 1;
					formatRecord->maxDataBytes = dataBytes * 4;
					formatRecord->data = NULL;
				}
				break;
			case formatSelectorEstimateContinue:
				break;
			case formatSelectorEstimateFinish:
				break;

			case formatSelectorWritePrepare:
				DoWritePrepare();
				break;
			case formatSelectorWriteStart:
				RitoTex::MaybeCheckForUpdatesAsync();
				DoWriteStart();
				break;
			case formatSelectorWriteContinue:
				DoWriteContinue();
				break;
			case formatSelectorWriteFinish:
				DoWriteFinish();
				break;

			case formatSelectorFilterFile:
				DoFilterFile();
				break;

			case formatSelectorReadLayerStart:
				DoReadLayerStart();
			break;
			case formatSelectorReadLayerContinue:
				DoReadContinue(); //this is run when layers are set
			break;
            case formatSelectorReadLayerFinish:
			break;

			case formatSelectorWriteLayerStart:
			break;
            case formatSelectorWriteLayerContinue:
			break;
            case formatSelectorWriteLayerFinish:
			break;
		}
			
		//-----------------------------------------------------------------------
		//	(5) Unlock data, and exit resource.
		//
		//	Result is automatically returned in *result, which is
		//	pointed to by ps.resultPtr.
		//-----------------------------------------------------------------------	
		
		UnlockHandles();
	
	} // about selector special		

	// release any suites that we may have acquired
	if (selector == formatSelectorAbout ||
		selector == formatSelectorWriteFinish ||
		selector == formatSelectorReadFinish ||
		selector == formatSelectorOptionsFinish ||
		selector == formatSelectorEstimateFinish ||
		selector == formatSelectorFilterFile ||
		*ps.resultPtr != noErr)
	{
		PIUSuitesRelease();
	}

}


DLLExport MACPASCAL void PluginMain (const int16 selector,
						             FormatRecordPtr formatRecord,
						             intptr_t * data,
						             int16 * result)
{
	try 
	{ 
		IntelPlugin::GetInstance().PluginMain(selector, formatRecord, data, result);
	}
	catch(...)
	{
		if (NULL != result)
			*result = -1;
	}

} // end PluginMain


