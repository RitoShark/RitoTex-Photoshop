////////////////////////////////////////////////////////////////////////////////
// RitoTex — Update Checker
//
// Queries the GitHub Releases API for the latest release and, if it is newer
// than the embedded RITOTEX_VERSION_STR, shows a single non-blocking
// notification linking to the release page.
//
// Design (see docs/UpdateChecking_Design.md):
//   - Runs on a DETACHED background thread; never blocks Photoshop file I/O.
//   - Throttled to once per 24h per machine via HKCU\Software\RitoTex.
//   - Once per process session.
//   - Never touches Photoshop suites/callbacks from the worker thread.
//   - Fails closed and silent on any error (offline, 403, malformed JSON).
//   - Link-only: never downloads or executes anything.
////////////////////////////////////////////////////////////////////////////////

#pragma once

namespace RitoTex
{
	// Kick off an asynchronous, throttled update check. Safe to call from a
	// format selector (e.g. ReadStart). Returns immediately. Repeated calls
	// within the same process are no-ops after the first.
	void MaybeCheckForUpdatesAsync();
}
