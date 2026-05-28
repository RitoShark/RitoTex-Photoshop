////////////////////////////////////////////////////////////////////////////////
// RitoTex — PixelConvert
//
// Conversion of Photoshop's source pixel buffer (8 / 16 / 32 bit) into the
// packed RGBA8 layout the block-compression encoders expect. Missing channels
// are zero-filled; missing alpha is forced opaque.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"

using namespace DirectX;

bool IntelPlugin::ConvertToBCFrom32Bit(unsigned8 *tgtDataPtr, int planesToGet, bool hasAlphaChannel, bool gammaCorrect)
{
	if (ps.formatRecord->depth != 32)
		return false;

	float *rowData32bit = static_cast<float *>(ps.formatRecord->data);

	for (int height=0; height< ps.formatRecord->imageSize.v; height++)
	for (int width=0; width< ps.formatRecord->imageSize.h; width++)
	{
		int index = height*ps.formatRecord->imageSize.h*planesToGet + width*planesToGet;//height*ps.formatRecord->imageSize.h*ps.formatRecord->planes + width*ps.formatRecord->planes;

		//We assign black to other channels if photoshop does not provide enough channels as the compressed text must be RGBA
		tgtDataPtr[0] = ConvertTo8Bit(rowData32bit[index], gammaCorrect);
		tgtDataPtr[1] = (planesToGet > 1)? ConvertTo8Bit(rowData32bit[index+1], gammaCorrect) : 0;
		tgtDataPtr[2] = (planesToGet > 2)? ConvertTo8Bit(rowData32bit[index+2], gammaCorrect) : 0;
		tgtDataPtr[3] = hasAlphaChannel? ConvertTo8Bit(rowData32bit[index+3], gammaCorrect) : 255;
		tgtDataPtr +=4;
	}

	return true;
}

bool IntelPlugin::ConvertToBCFrom16Bit(unsigned8 *tgtDataPtr, int planesToGet, bool hasAlphaChannel)
{
	if (ps.formatRecord->depth != 16)
		return false;

	unsigned16 *rowData16bit = static_cast<unsigned16 *>(ps.formatRecord->data);

	for (int height=0; height< ps.formatRecord->imageSize.v; height++)
	for (int width=0; width< ps.formatRecord->imageSize.h; width++)
	{
		int index = height*ps.formatRecord->imageSize.h*planesToGet + width*planesToGet;//height*ps.formatRecord->imageSize.h*ps.formatRecord->planes + width*ps.formatRecord->planes;

		//We assign black to other channels if photoshop does not provide enough channels as the compressed text must be RGBA
		tgtDataPtr[0] = ConvertTo8Bit(rowData16bit[index]);
		tgtDataPtr[1] = (planesToGet > 1)? ConvertTo8Bit(rowData16bit[index+1]) : 0;
		tgtDataPtr[2] = (planesToGet > 2)? ConvertTo8Bit(rowData16bit[index+2]) : 0;
		tgtDataPtr[3] = hasAlphaChannel? ConvertTo8Bit(rowData16bit[index+3]) : 255;
		tgtDataPtr +=4;
	}

	return true;
}

bool IntelPlugin::ConvertToBCFrom8Bit(unsigned8 *tgtDataPtr, int planesToGet, bool hasAlphaChannel)
{
	if (ps.formatRecord->depth != 8)
		return false;

	//Get image pointer
	unsigned8 *rowData = static_cast<unsigned8 *>(ps.formatRecord->data);

	//Copy data from photoshop buffer into local buffer
	for (int height=0; height< ps.formatRecord->imageSize.v; height++)
	for (int width=0; width< ps.formatRecord->imageSize.h; width++)
	{
		int index = height*ps.formatRecord->imageSize.h*planesToGet + width*planesToGet;//height*ps.formatRecord->imageSize.h*ps.formatRecord->planes + width*ps.formatRecord->planes;

		//We assign black to other channels is photoshop does not provide enought channels as the compressed text must be RGBA
		tgtDataPtr[0] = ConvertTo8Bit(rowData[index]);
		tgtDataPtr[1] = (planesToGet > 1)? ConvertTo8Bit(rowData[index+1]) : 0;
		tgtDataPtr[2] = (planesToGet > 2)? ConvertTo8Bit(rowData[index+2]) : 0;
		tgtDataPtr[3] = hasAlphaChannel? ConvertTo8Bit(rowData[index+3]) : 255;
		tgtDataPtr +=4;
	}

	return true;
}
