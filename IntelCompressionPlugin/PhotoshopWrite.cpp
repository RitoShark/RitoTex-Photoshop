////////////////////////////////////////////////////////////////////////////////
// RitoTex — PhotoshopWrite
//
// Host-side orchestration of a save. Photoshop drives this through the format
// selectors (Prepare/Start/Continue/Finish). Start sets up parameters + the
// modal Save dialog, then dispatches to the right container writer based on
// the output extension (.tex vs .dds, sniffed from finalSpec). The actual
// pixel encode lives in Codec.cpp and the file framing in ContainerWrite.cpp.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"
#include "IntelPluginUIWin.h"		// InitWin32Threads / DestroyThreads
#include "CustomSaveDialog.h"		// CustomSaveDialog::DoModal

using namespace DirectX;

void IntelPlugin::DoWritePrepare()
{
	ps.formatRecord->maxData = 0;   //this signifies that we will provide our own buffer
	ps.formatRecord->layerData = 0; //we dont want the layers handed in separately, we read them ourselfes
}

/*****************************************************************************/
// Detect if we're running as the TEX format plugin entry.
// Uses formatRecord->finalSpec (New in PS 11.0 / CS4) which contains the
// final output file path with the correct extension (.dds or .tex).
// Photoshop writes to a .tmp file first, so dataFork/fileSpec point to .tmp,
// but finalSpec always has the real destination path.
static bool IsSavingToTEXFormat(FormatRecordPtr formatRecord)
{
	if (!formatRecord)
	{
		OutputDebugStringA("IsSavingToTEXFormat: formatRecord is NULL, defaulting to DDS");
		return false;
	}

	// finalSpec is a UTF-16 string with the final output file path
	if (formatRecord->finalSpec)
	{
		const uint16* spec = formatRecord->finalSpec;
		int len = 0;
		while (spec[len] != 0) len++;

		// Search backwards for '.'
		int dotPos = -1;
		for (int i = len - 1; i >= 0; i--)
		{
			if (spec[i] == L'.')
			{
				dotPos = i;
				break;
			}
		}

		if (dotPos >= 0 && (len - dotPos - 1) == 3)
		{
			// Case-insensitive compare of the 3-char extension
			uint16 e0 = spec[dotPos + 1] | 0x20; // tolower for ASCII
			uint16 e1 = spec[dotPos + 2] | 0x20;
			uint16 e2 = spec[dotPos + 3] | 0x20;
			bool isTEX = (e0 == 't' && e1 == 'e' && e2 == 'x');

			char dbg[512];
			char narrowTail[64] = {0};
			int start = (len > 60) ? len - 60 : 0;
			for (int i = start; i < len && (i - start) < 63; i++)
				narrowTail[i - start] = (char)(spec[i] & 0x7F);
			sprintf(dbg, "IsSavingToTEXFormat: finalSpec='...%s', isTEX=%d", narrowTail, isTEX ? 1 : 0);
			OutputDebugStringA(dbg);

			return isTEX;
		}
	}

	OutputDebugStringA("IsSavingToTEXFormat: finalSpec unavailable, defaulting to DDS");
	return false;
}

void IntelPlugin::DoWriteStart()
{
	InitData();

	/* check with the scripting system whether to pop our dialog */

	ps.data->queryForParameters = ReadScriptParamsForWrite();

	//If Multithreading init win32 threads and events
	if (ps.data->gMultithreaded)
	{
		InitWin32Threads();
	}

	//Show main dialog and get parameters
	if (CustomSaveDialog::DoModal(this) != IDOK)
		SetResult(userCanceledErr);

	if (GetResult() == noErr)
	{
		// Detect format from formatRecord->fileType (set from PiPL FmtFileType).
		// DDS PiPL uses 'DDS ', TEX PiPL uses 'TEX '.
		bool isTEX = IsSavingToTEXFormat(ps.formatRecord);

		char debugMsg[256];
		sprintf(debugMsg, "DoWriteStart: saving as: %s", isTEX ? "TEX" : "DDS");
		OutputDebugStringA(debugMsg);

		// Save in the detected format
		if (isTEX)
			DoWriteTEX();
		else
			DoWriteDDS();
	}

	DisposeImageData();

	DestroyThreads();
}


void IntelPlugin::DoWriteContinue()
{
	// should not be called as data has already been disposed
}

void IntelPlugin::DoWriteFinish()
{
	// only called if not cancelled
	WriteScriptParamsForWrite(); // should be different for read/write
}
