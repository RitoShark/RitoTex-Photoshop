////////////////////////////////////////////////////////////////////////////////
// RitoTex — Codec
//
// The shared block-compression core. Turns an uncompressed RGBA8 ScratchImage
// (single image, mip chain, or 6-face cube) into a compressed ScratchImage in
// the chosen DXGI block format. This is the single encode path used by BOTH
// the .dds and .tex writers — the only thing that differs downstream is the
// container/header, not the pixel encoding.
//
// BC1/BC3 with dithering off go through the Intel ISPC kernels (fast path);
// everything else (BC5/BC7, or BC1/BC3 with dithering on) goes through
// DirectXTex Compress(). Uncompressed (RGBA8) output just swaps pointers.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"
#include "s3tc.h"		// ISPC entry points: CompressImageMT/ST, CompressImageBC1/BC3, CompressionFunc

#include <cstdio>
#include <d3d11.h>			// GPU device for DirectXTex's DirectCompute BC7 encoder
#include <wrl/client.h>		// Microsoft::WRL::ComPtr
#include <mutex>

#pragma comment(lib, "d3d11.lib")

using namespace DirectX;

//-------------------------------------------------------------------------------
// GPU compression device (DirectXTex DirectCompute path, used for BC7).
//
// Created lazily on the first BC7 encode and cached for the rest of the session.
// Hardware first, WARP (software) fallback so headless / no-GPU / RDP boxes still
// get a working device; if even WARP fails the caller falls back to CPU Compress().
//-------------------------------------------------------------------------------
namespace
{
	Microsoft::WRL::ComPtr<ID3D11Device> g_compressDevice;
	bool       g_deviceTried = false;   // only attempt creation once per session
	std::mutex g_deviceMutex;           // compression may run off the main thread

	void CreateCompressDevice(Microsoft::WRL::ComPtr<ID3D11Device>& device)
	{
		// Feature level 11_0 minimum — the BC7 encoder uses cs_5_0 compute shaders.
		static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

		// Hardware first.
		HRESULT hr = D3D11CreateDevice(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
			levels, _countof(levels), D3D11_SDK_VERSION,
			device.ReleaseAndGetAddressOf(), nullptr, nullptr);

		if (FAILED(hr))
		{
			// WARP software rasterizer fallback (headless / no GPU / remote desktop).
			hr = D3D11CreateDevice(
				nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
				levels, _countof(levels), D3D11_SDK_VERSION,
				device.ReleaseAndGetAddressOf(), nullptr, nullptr);
		}

		if (FAILED(hr))
		{
			device.Reset();   // leave null → caller uses CPU Compress()
			OutputDebugStringA("[RitoTex/Codec] GPU device creation failed (HW+WARP) — using CPU\n");
		}
		else
		{
			OutputDebugStringA("[RitoTex/Codec] GPU compress device ready\n");
		}
	}

	// Returns the cached device (creating it on first call), or nullptr if unavailable.
	ID3D11Device* GetCompressDevice()
	{
		std::lock_guard<std::mutex> lock(g_deviceMutex);
		if (!g_deviceTried)
		{
			g_deviceTried = true;
			CreateCompressDevice(g_compressDevice);
		}
		return g_compressDevice.Get();
	}
}

//Take an Uncompressed ScratchImage scrUncompressedImageScratch_ and Compresses it into scrImageScratch_
bool IntelPlugin::CompressToScratchImage(ScratchImage **scrImageScratch_, ScratchImage **scrUncompressedImageScratch_, bool hasAlpha_)
{
	//============================================================================================
	//============================================================================================
	//Compress image section
	//ISPC is used for BC1,BC3 (when dithering is off). DirectXTex Compress() used when dithering is on.
	//Uncompressed handled separately
	if (ps.data->encoding_g != DXGI_FORMAT_R8G8B8A8_UNORM)
	{
		// ISPC only encodes BC1/BC3. Everything else (BC5/BC7), and BC1/BC3 when
		// dithering is on, goes through DirectXTex Compress().
		const bool ispcCanEncode = (ps.data->encoding_g == DXGI_FORMAT_BC1_UNORM ||
		                            ps.data->encoding_g == DXGI_FORMAT_BC3_UNORM);
		const bool useDirectXTex = !ispcCanEncode || ps.data->useDithering;

		if (useDirectXTex)
		{
			DWORD flags = 0;
			// Dither + error metric only apply to the BC1/BC3 (low-precision) path.
			if (ps.data->useDithering && ispcCanEncode)
			{
				flags |= TEX_COMPRESS_DITHER;
				if (ps.data->useUniformMetric)
					flags |= TEX_COMPRESS_UNIFORM;
			}
			// Note: TEX_COMPRESS_PARALLEL not used - this DirectXTex is built without OpenMP

			// Release the output scratch image - Compress() initializes it internally
			(*scrImageScratch_)->Release();

			// BC7 is the only format DirectXTex's GPU (DirectCompute) encoder supports
			// that we emit, and it is by far the slowest on CPU — route it to the GPU.
			// BC5 and dithered BC1/BC3 stay on the CPU path below.
			const bool gpuEligible =
				(ps.data->encoding_g == DXGI_FORMAT_BC7_UNORM ||
				 ps.data->encoding_g == DXGI_FORMAT_BC7_UNORM_SRGB);

			ID3D11Device* dev = gpuEligible ? GetCompressDevice() : nullptr;

			HRESULT hr = E_FAIL;

			if (dev)
			{
				// GPU encode. alphaWeight = 1.0f is the BC7 weight for this overload.
				// Whole mip chain + cube faces are handled internally by DirectXTex.
				// Serialize on the device mutex: the immediate context is not thread-safe.
				std::lock_guard<std::mutex> lock(g_deviceMutex);
				hr = Compress(
					dev,
					(*scrUncompressedImageScratch_)->GetImages(),
					(*scrUncompressedImageScratch_)->GetImageCount(),
					(*scrUncompressedImageScratch_)->GetMetadata(),
					ps.data->encoding_g, flags, 1.0f,
					**scrImageScratch_);

				if (SUCCEEDED(hr))
					OutputDebugStringA("[RitoTex/Codec] BC7 encoded on GPU\n");
				else
					OutputDebugStringA("[RitoTex/Codec] GPU BC7 encode failed — falling back to CPU\n");
			}

			// CPU path: the fallback for BC7 (no device / GPU failure) and the normal
			// path for BC5 and dithered BC1/BC3. alphaRef = 0.5f (BC1 alpha cutoff).
			if (!dev || FAILED(hr))
			{
				hr = Compress(
					(*scrUncompressedImageScratch_)->GetImages(),
					(*scrUncompressedImageScratch_)->GetImageCount(),
					(*scrUncompressedImageScratch_)->GetMetadata(),
					ps.data->encoding_g, flags, 0.5f,
					**scrImageScratch_);
			}

			if (FAILED(hr))
			{
				char errMsg[256];
				sprintf(errMsg, "DirectXTex Compress failed (hr=0x%08X, fmt=%d, imgs=%zu, w=%zu, h=%zu)",
					hr, (int)ps.data->encoding_g,
					(*scrUncompressedImageScratch_)->GetImageCount(),
					(*scrUncompressedImageScratch_)->GetMetadata().width,
					(*scrUncompressedImageScratch_)->GetMetadata().height);
				OutputDebugStringA(errMsg);
				UserError(errMsg);
				return false;
			}
			return true;
		}
		//How many mip levels are generated? If no mip map override this is 1 so that only one image is encoded
		size_t mipLevels = (*scrUncompressedImageScratch_)->GetMetadata().mipLevels;

		//Allocate space for Image + mip chain, according to what the uncompressed image size/miplevel
		//Check if cubemap or not (a cubemap has an arraysize of six)
		if ((*scrUncompressedImageScratch_)->GetMetadata().arraySize == 6)
		{
			//Image is a CubeMap
			(*scrImageScratch_)->InitializeCube(ps.data->encoding_g, (*scrUncompressedImageScratch_)->GetMetadata().width,
											   (*scrUncompressedImageScratch_)->GetMetadata().height, 1, mipLevels);
		}
		else
		{
			//Image is a 2D texture
			(*scrImageScratch_)->Initialize2D(ps.data->encoding_g, (*scrUncompressedImageScratch_)->GetMetadata().width,
											 (*scrUncompressedImageScratch_)->GetMetadata().height, 1, mipLevels, DDS_FLAGS_NONE);
		}

		//Get pointer to image chains for compressed and uncompressed scratchImage
		const Image* imgCompressed = (*scrImageScratch_)->GetImages();
		const Image* imgUnCompressedMipMap = (*scrUncompressedImageScratch_)->GetImages();

		//Number of total images (Image1+MipChain), (Image2+MipChain), ..... (Multiple Images image array only in case of Cube Maps)
		size_t nimgCompressed = (*scrImageScratch_)->GetImageCount();
		if (nimgCompressed == 0)
		{
			UserError("Can not allocate compressed image");
			return false;
		}

		//StopWatch stopWatch;
		//stopWatch.Reset();
		//stopWatch.Start();
		//ISPC is used on for BC1,3,6,7.
		//Iterate over the whole image chain (Image+MipChain)
		for (size_t allImagesIndex = 0; allImagesIndex < nimgCompressed; allImagesIndex++)
		{
			//Fill in the struct needed by Intel ispc code with the uncompressed  image
			rgba_surface input;

			//Get pointer to this image in the chain
			const Image* rgbaimg = &imgUnCompressedMipMap[allImagesIndex];

			input.height = int32_t(rgbaimg->height);
			input.width = int32_t(rgbaimg->width);
			input.ptr =  rgbaimg->pixels; //buffer in unsigned8* format
			input.stride = int32_t(rgbaimg->rowPitch); //number of bytes of a row

			//Determine if the image size is not multiples of 4.
			//In that case it needs padding when encoding
			bool DoPadding = ((input.height | input.width) & 0x3) != 0;

			//If it needs padding, process it
			if (DoPadding)
				input = DoPaddingToMultiplesOf4(input);

			//Call in ISPC compression functions, output compressed image into imgCompressed[allImagesIndex]
			ISPC_compression(input, imgCompressed[allImagesIndex], hasAlpha_);

			//If padding free buffer that got allocated
			if (DoPadding)
				delete [] input.ptr;
		}
		//stopWatch.Stop();
		//std::stringstream ss;
		//ss << stopWatch.TimeInMilliseconds();
		//errorMessage(ss.str(),"Time");
	}
	else
	{
		//Uncompressed do nothing, just swap pointers and free space
		delete *scrImageScratch_;
		*scrImageScratch_ = *scrUncompressedImageScratch_;
	    *scrUncompressedImageScratch_ = NULL;
	}

	return true;
}

//Compress rgba_surface into tgtPixels, uisng Intel ISPC encoders
bool IntelPlugin::ISPC_compression(rgba_surface &source, const Image& target, bool hasAlpha_rgba)
{
	CompressionFunc* cmpFunc = NULL;

	switch (ps.data->encoding_g)
	{
		case DXGI_FORMAT_BC1_UNORM:
			cmpFunc = CompressImageBC1;
			break;

		case DXGI_FORMAT_BC3_UNORM:
			cmpFunc = CompressImageBC3;
			break;

		default:
			break;
	}

	if (cmpFunc)
	{
		int slices = (source.width * source.height) / 0x40000;	// 256K pixels per chunk
		if (slices < 1)
			slices = 1;

		for (int i = 0; i < slices; i++)
		{
			if (i > 0 && !SetProgress(i, slices))	// allow an early out
				return false;

			int ylo = (i * source.height / slices) & ~0x3;	// 4 pixels per block row
			int yhi = ((i+1) * source.height / slices) & ~0x3;	// 4 pixels per block row

			if (yhi > source.height)
				yhi = source.height;

			if (yhi > ylo)
			{
				auto input = source;
				input.ptr += input.stride * ylo;
				input.height = yhi - ylo;

				auto dst = target.pixels + target.rowPitch * (ylo >> 2);	// rowPitch is actually blockRowPitch here

				if (ps.data->gMultithreaded)
					CompressImageMT(&input, dst, cmpFunc, ps.data->encoding_g);
				else
					CompressImageST(&input, dst, cmpFunc, ps.data->encoding_g);
			}
		}
		return true;
	}

	return false;
}

//Pad the surface size to boundaries of 4
//Calculates new width,height,stride and new buffer
//It does not deallocate the old buffer
//It does return the new surface
//It is the responsibility of the user to deallocate the new/old buffer when not needed anymore
rgba_surface IntelPlugin::DoPaddingToMultiplesOf4(const rgba_surface &input)
{
	rgba_surface out;

	// now we're going to copy to a new buffer to do padding to boundaries of 4
	// boundaries of 4 are needed for the compression algorithms to work
	// since they rely on 4x4 blocks
	int pixelSize = 4;
	out.width = (input.width+3) & ~3;       //pad witdh to boundaries of 4
	out.height = (input.height+3) & ~3;     //pad height to boundaries of 4
	out.stride = out.width * pixelSize;                  //size of a row
	out.ptr = new uint8_t[out.height * out.stride];    //new buffer

	//Copy row by row into new buffer
	for (int y = 0; y < input.height; y++)
	{
		auto rowSrc = input.ptr + y * input.stride;
		auto rowDst = out.ptr + y * out.stride;

		//Copy existing rows
		memcpy(rowDst, rowSrc, input.width * pixelSize);

		//Pad to new width, copy the last pixel of the old row into the remaining pixels of new row
		for (int x=input.width; x < out.width; x++)	// trailing pixels
			memcpy(rowDst + x * pixelSize, rowSrc + (input.width-1) * pixelSize, pixelSize);
	}

	//Pad to new height, copy last row multiple times to fill new rows
	for (int y = input.height; y < out.height; y++)	// extra rows
	{
		auto rowDst = out.ptr + y * out.stride;
		memcpy(rowDst, rowDst - out.stride, out.stride);
	}

	return out;
}
