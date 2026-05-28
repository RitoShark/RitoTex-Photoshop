////////////////////////////////////////////////////////////////////////////////
// RitoTex — Scripting
//
// Photoshop Actions / scripting descriptor I/O. Reads and writes the plugin's
// persistent parameters (preset name for write, mipmap + separate-alpha flags
// for read) through the host descriptor suite so the plugin participates in
// batch automation and recorded actions. The Read*ParamsFor* functions also
// return whether the modal dialog should be shown for this invocation.
////////////////////////////////////////////////////////////////////////////////
#include "IntelPlugin.h"

using namespace DirectX;

bool IntelPlugin::ReadScriptParamsForWrite()
{
	// Populate this array if we're expecting any keys,
	// must be NULLID terminated:
	DescriptorKeyIDArray array = { NULLID };


	// Assume we want to pop our dialog unless explicitly told not to:
	Boolean				returnValue = true;

	auto descParams = ps.formatRecord->descriptorParameters;

	if (HostDescriptorAvailable(descParams, NULL))
	{ // descriptor suite is available, go ahead and open descriptor:

		auto reader = descParams->readDescriptorProcs;

		// PIUtilities routine to open descriptor handed to us by host:
		if (PIReadDescriptor token = reader->openReadDescriptorProc(descParams->descriptor, array))
		{ // token was valid, so read keys from it:

			DescriptorKeyID		key = NULLID;	// the next key
			DescriptorTypeID	type = NULLID;	// the type of the key we read
			int32				flags = 0;		// any flags for the key
			while (reader->getKeyProc(token, &key, &type, &flags))
			{ // got a valid key.  Figure out where to put it:

				switch (key)
				{ // match a key to its expected type:case keyAmount:

					case keyPreset:
						reader->getStringProc(token, &(ps.data->presetBatchName));
						break;

					// ignore all other cases and classes
					// See PIActions.h and PIUtilities.h for
					// routines and macros for scripting functions.

				} // key

			} // PIGetKey

			// PIUtilities routine that automatically deallocates,
			// closes, and sets token to NULL:

			if (OSErr err = HostCloseReader(descParams, ps.formatRecord->handleProcs, &token))
			{ // an error did occur while we were reading keys:

				if (err == errMissingParameter) // missedParamErr == -1715
				{ // missing parameter somewhere.  Walk IDarray to find which one.
				}
				else
				{ // serious error.  Return it as a global result:
					SetResult(err);
				}

			} // stickyError

		} // didn't have a valid token

		// Whether we had a valid token or not, we were given information
		// as to whether to pop our dialog or not.  PIUtilities has a routine
		// to check that and return TRUE if we should pop it, FALSE if not:
		returnValue = HostPlayDialog(descParams);

	} // descriptor suite unavailable

	return !!returnValue;
}


OSErr IntelPlugin::WriteScriptParamsForWrite (void)
{
	OSErr						gotErr = writErr;

	if (auto descParams = ps.formatRecord->descriptorParameters)
	{
		if (auto writeProcs = descParams->writeDescriptorProcs)
		{
			if (auto token = writeProcs->openWriteDescriptorProc())
			{
				// now write our data
				writeProcs->putStringProc(token, keyPreset, ps.data->presetBatchName);

				sPSHandle->Dispose(descParams->descriptor);
				PIDescriptorHandle	h;
				writeProcs->closeWriteDescriptorProc(token, &h);
				descParams->descriptor = h;

				gotErr = noErr;	// we're ok!
			}
		}
	}

	return gotErr;
}

bool IntelPlugin::ReadScriptParamsForRead()
{
	// Populate this array if we're expecting any keys,
	// must be NULLID terminated:
	DescriptorKeyIDArray array = { NULLID };

	// Assume we want to pop our dialog unless explicitly told not to:
	Boolean				returnValue = true;

	auto descParams = ps.formatRecord->descriptorParameters;

	if (HostDescriptorAvailable(descParams, NULL))
	{ // descriptor suite is available, go ahead and open descriptor:

		auto reader = descParams->readDescriptorProcs;

		// PIUtilities routine to open descriptor handed to us by host:
		if (PIReadDescriptor token = reader->openReadDescriptorProc(descParams->descriptor, array))
		{ // token was valid, so read keys from it:

			DescriptorKeyID		key = NULLID;	// the next key
			DescriptorTypeID	type = NULLID;	// the type of the key we read
			int32				flags = 0;		// any flags for the key
			Boolean             mipFlag = false;
			Boolean             alphaFlag = false;
			while (reader->getKeyProc(token, &key, &type, &flags))
			{ // got a valid key.  Figure out where to put it:

				switch (key)
				{ // match a key to its expected type:case keyAmount:

					case keyMipMap:
						reader->getBooleanProc(token, &mipFlag);
						ps.data->mipmapBatchAllowed = !!mipFlag; //conve form Boolean to bool
						break;
					case keyAlphaS:
						reader->getBooleanProc(token, &alphaFlag);
						ps.data->alphaBatchSeperate = !!alphaFlag; //conve form Boolean to bool
						break;

					// ignore all other cases and classes
					// See PIActions.h and PIUtilities.h for
					// routines and macros for scripting functions.

				} // key

			} // PIGetKey

			// PIUtilities routine that automatically deallocates,
			// closes, and sets token to NULL:

			if (OSErr err = HostCloseReader(descParams, ps.formatRecord->handleProcs, &token))
			{ // an error did occur while we were reading keys:

				if (err == errMissingParameter) // missedParamErr == -1715
				{ // missing parameter somewhere.  Walk IDarray to find which one.
				}
				else
				{ // serious error.  Return it as a global result:
					SetResult(err);
				}

			} // stickyError

		} // didn't have a valid token

		// Whether we had a valid token or not, we were given information
		// as to whether to pop our dialog or not.  PIUtilities has a routine
		// to check that and return TRUE if we should pop it, FALSE if not:
		returnValue = HostPlayDialog(descParams);

	} // descriptor suite unavailable

	return !!returnValue;

	//auto descParams = ps.formatRecord->descriptorParameters;

	//return descParams->playInfo == plugInDialogDisplay;
}

OSErr IntelPlugin::WriteScriptParamsForRead ()
{
	OSErr						gotErr = writErr;

	if (auto descParams = ps.formatRecord->descriptorParameters)
	{
		if (auto writeProcs = descParams->writeDescriptorProcs)
		{
			if (auto token = writeProcs->openWriteDescriptorProc())
			{
				// now write our data
				Boolean temp = ps.data->mipmapBatchAllowed;
				writeProcs->putBooleanProc(token, keyMipMap, temp);
				temp = ps.data->alphaBatchSeperate;
				writeProcs->putBooleanProc(token, keyAlphaS, temp);

				sPSHandle->Dispose(descParams->descriptor);
				PIDescriptorHandle	h;
				writeProcs->closeWriteDescriptorProc(token, &h);
				descParams->descriptor = h;

				gotErr = noErr;	// we're ok!
			}
		}
	}

	return gotErr;
}
