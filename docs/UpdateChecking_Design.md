# RitoTex — Update Checking via GitHub Releases (Research)

**Scope:** One narrow capability — an update checker for the *rebuilt* RitoTex plugin: a native C++ **Adobe Photoshop File Format plugin** (`.8bi` DLL on Windows, built with the Photoshop C++ Plug-in SDK) that opens/saves Riot Games `.tex` textures. Decision is **locked**: the check queries **GitHub Releases**. This document covers the API, the native Windows HTTPS call, JSON parsing, trigger/UX strategy, version embedding, and security.

> Note on the existing `update_check.py`: a Python reference can call `requests.get(".../releases/latest")` trivially, but that logic does **not** transfer into a `.8bi`. A format plugin is a DLL loaded into Photoshop's process; it must do the HTTP(S) call with a native Windows stack (WinHTTP) on a background thread, and it has essentially no persistent UI. The Python script's *logic* (endpoint, semver compare, throttle) is reusable; its *mechanism* (Python `requests`) is not.
>
> _Project survey note:_ no `update_check.py` was found in the local `RitoTex` tree at research time (sibling repos `Gimp-Tex-Plugin`, `Paint.NET-Tex-Plugin`, `RitoTex-Photoshop` contain no update-check source; `RitoTex - Reborn` is empty apart from this file). If a `update_check.py` exists elsewhere in the project ecosystem, port its endpoint/semver/throttle constants into the C++ design below rather than re-deriving them. The Photoshop C++ Plug-in SDK is present at `e:\RitoShark\RitoTex\PhotoshopSDK\pluginsdk` (see format samples under `samplecode\format\simpleformat` and PiPL resources under `photoshopapi\resources\PIPL.r`).

---

## 1. GitHub Releases API

### Endpoint — "get the latest release"

```
GET https://api.github.com/repos/{owner}/{repo}/releases/latest
```

`/releases/latest` returns the most recent release that is **not a draft and not a prerelease**, sorted by `created_at` (the commit date of the release, not the publish date). If you also want to surface prereleases/betas, instead fetch `GET /repos/{owner}/{repo}/releases` (a JSON array, newest first) and pick element `[0]`, or filter on the `prerelease` flag yourself.

Source: GitHub REST API docs — *REST API endpoints for releases* (https://docs.github.com/en/rest/releases/releases).

### Required / recommended headers

| Header | Value | Why |
|---|---|---|
| `User-Agent` | e.g. `RitoTex-Plugin/1.2.3` | **MANDATORY.** GitHub rejects requests with no `User-Agent`. They ask you to use your app or GitHub username so they can contact you about problems. |
| `Accept` | `application/vnd.github+json` | Recommended media type for the v3 REST API. |
| `X-GitHub-Api-Version` | `2022-11-28` | Pins the API version. If omitted, GitHub uses a default version, which can change under you. Pin it. (A newer version `2026-03-10` exists as of 2026, but `2022-11-28` is still supported and remains the default when the header is omitted — pinning it is the conservative choice; the latest-release endpoint's shape is unchanged across these versions.) |
| `Authorization` | `Bearer <token>` | **Optional** — only needed for higher rate limits or private repos. **Do NOT ship a token in the plugin** (see Security). For a public repo, unauthenticated is correct. |

Sources: GitHub REST API docs — *Getting started with the REST API* and *About the REST API* (https://docs.github.com/en/rest/using-the-rest-api/getting-started-with-the-rest-api, https://docs.github.com/en/rest/about-the-rest-api/about-the-rest-api). The `User-Agent` requirement is also stated in *Resources in the REST API* / *Best practices* (https://docs.github.com/en/rest/using-the-rest-api/best-practices-for-using-the-rest-api).

### JSON response — fields that matter

```jsonc
{
  "tag_name": "v1.2.3",                         // the git tag — compare this to embedded version
  "name": "RitoTex 1.2.3",                      // human-readable release title
  "html_url": "https://github.com/OWNER/REPO/releases/tag/v1.2.3",  // <-- link the user here
  "published_at": "2026-05-20T14:03:11Z",       // ISO-8601 UTC timestamp
  "prerelease": false,
  "draft": false,
  "body": "## Changelog ...",                    // markdown release notes (optional to display)
  "assets": [
    {
      "name": "RitoTex-1.2.3-win64.8bi",
      "browser_download_url": "https://github.com/OWNER/REPO/releases/download/v1.2.3/RitoTex-1.2.3-win64.8bi",
      "size": 245760,
      "content_type": "application/octet-stream"
    }
  ]
}
```

- **`tag_name`** — the value you parse and compare against the plugin's embedded version.
- **`html_url`** — the release page. **This is the only URL you should hand the user** (we are not auto-downloading; see Security).
- **`assets[].browser_download_url`** — direct download URLs, useful only if you later add an opt-in download; not needed for the link-only UX.
- **`published_at`** — display "released N days ago" and/or to decide if a release is genuinely newer than what the user has.

### Rate limit (the part that matters for a desktop plugin)

- **Unauthenticated requests: 60 per hour, keyed to the originating IP address** (not per user). Source: GitHub REST API docs — *Rate limits for the REST API* (https://docs.github.com/en/rest/using-the-rest-api/rate-limits-for-the-rest-api).
- Response headers report budget: `x-ratelimit-limit`, `x-ratelimit-remaining`, `x-ratelimit-reset` (UTC epoch seconds). Honor `x-ratelimit-reset` (and `Retry-After` on a 403/429) before retrying.

**Implication for RitoTex:** 60/hr/IP is *plenty* for a single user, but on a shared/corporate NAT many machines share one public IP. A plugin that checks on *every* file open could, across a studio behind one IP, exhaust 60/hr collectively and start getting `403`s. **Throttle hard** (once per 24h per machine — see §4). With a 24h throttle you use ~1 request/day/machine, which is negligible even behind shared NAT. Authenticating would raise the limit to 5,000/hr, but that requires shipping/managing a token and is **not worth it** for a public-repo version check — just throttle.

### Comparing a semver `tag_name`

`tag_name` is typically `vMAJOR.MINOR.PATCH` (e.g. `v1.2.3`), sometimes with a prerelease suffix (`v1.2.3-rc.1`). To compare:

1. Strip a leading `v`/`V`.
2. Split on `.` into integers (major, minor, patch). Parse defensively (missing parts = 0).
3. Compare numerically component-by-component (major, then minor, then patch) — **not** lexicographically (string compare wrongly orders `v1.10.0 < v1.9.0`).
4. Treat a prerelease suffix (`-rc.1`, `-beta`) as *lower precedence* than the same version without it, per semver, if you handle prereleases at all. Simplest robust rule: ignore prerelease metadata for the "is there a newer stable build?" question and rely on `/releases/latest` (which already excludes prereleases).

Minimal integer-triple comparison in C++:

```cpp
#include <string>
#include <sstream>
#include <array>

// Parse "v1.2.3" / "1.2.3" -> {1,2,3}. Extra/prerelease junk after patch is ignored.
static std::array<int,3> ParseSemver(const std::string& in) {
    std::array<int,3> v{0,0,0};
    size_t i = 0;
    if (i < in.size() && (in[i] == 'v' || in[i] == 'V')) ++i;
    std::string core = in.substr(i);
    // Cut anything from a '-' (prerelease) or '+' (build metadata) onward.
    size_t cut = core.find_first_of("-+");
    if (cut != std::string::npos) core = core.substr(0, cut);
    std::stringstream ss(core);
    std::string part;
    for (int idx = 0; idx < 3 && std::getline(ss, part, '.'); ++idx) {
        try { v[idx] = std::stoi(part); } catch (...) { v[idx] = 0; }
    }
    return v;
}

// returns true if 'remote' is strictly newer than 'local'
static bool IsNewer(const std::string& remoteTag, const std::string& localVersion) {
    auto r = ParseSemver(remoteTag);
    auto l = ParseSemver(localVersion);
    for (int i = 0; i < 3; ++i) {
        if (r[i] != l[i]) return r[i] > l[i];
    }
    return false;
}
```

---

## 2. Making the HTTPS call from a native Windows C++ Photoshop plugin

### Why the host won't do it for you

Photoshop format plugins (`.8bi`) are DLLs loaded into Photoshop's process and called only for specific selectors (open/read, write/save, etc.). The Photoshop Plug-in SDK exposes **no general-purpose networking suite** for format modules — the callback suites are about file I/O, progress, descriptors, color, channel ports, etc. So a network call must be made with a **native Windows HTTP stack from inside the DLL**.

### WinHTTP vs WinINet vs helper process

| Option | Verdict for RitoTex |
|---|---|
| **WinHTTP** (`winhttp.dll`, link `winhttp.lib`) | **RECOMMENDED.** Microsoft's HTTP client designed for **services and non-interactive/background code** (exactly our case — running inside another app, off the UI thread). No user-profile/IE dependency, clean session model, proxy support, robust TLS. |
| **WinINet** (`wininet.dll`) | Microsoft explicitly says WinINet is **not supported for use in a service or non-interactive server context** and is tied to the interactive user's settings (IE cache/cookies/UI prompts). Wrong tool for a background check inside a host app. Avoid. |
| **Spawn a helper process** (e.g. `curl.exe`, PowerShell, or your own `.exe`) | Works and isolates the network code from Photoshop's process, but adds deployment complexity (ship/locate the helper), AV/SmartScreen friction, and IPC. Only consider if you later need something WinHTTP can't do. **Not recommended** for a simple GET. |

Sources: Microsoft Learn — *About WinHTTP* / *Porting WinINet applications to WinHTTP* note WinHTTP's suitability for service/non-interactive contexts and WinINet's unsuitability there (https://learn.microsoft.com/en-us/windows/win32/winhttp/about-winhttp, https://learn.microsoft.com/en-us/windows/win32/winhttp/porting-winhttp-and-wininet). WinHTTP session/request flow: *WinHTTP Sessions Overview* (https://learn.microsoft.com/en-us/windows/win32/winhttp/winhttp-sessions-overview).

### TLS / SNI for api.github.com

- Use the **`WINHTTP_FLAG_SECURE`** flag on `WinHttpOpenRequest` to do HTTPS. WinHTTP performs the TLS handshake, **sends SNI automatically** (the SNI hostname comes from the `WinHttpConnect` server name — pass `L"api.github.com"`, not an IP), and **validates the server certificate chain against the Windows trust store** by default. Do **not** disable certificate checks.
- **TLS version:** GitHub's API requires TLS 1.2+. On Windows 8.1/10/11 and Server 2012 R2+, modern WinHTTP negotiates TLS 1.2/1.3 by default. On older OS or to be explicit/safe, set the secure protocols option after `WinHttpOpen`:

```cpp
DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
                      | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3; // TLS1_3 flag exists on Win11/Server2022+
WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS,
                 &secureProtocols, sizeof(secureProtocols));
```

(If a target SDK header lacks the TLS1_3 constant, just use `...TLS1_2`. Never include the SSL3/TLS1.0/1.1 flags — GitHub will refuse them anyway.)

### Minimal WinHTTP snippet — GET latest-release JSON over TLS

```cpp
// Link: winhttp.lib
#include <windows.h>
#include <winhttp.h>
#include <string>
#pragma comment(lib, "winhttp.lib")

// Returns true on success; fills 'outBody' with the response body (UTF-8 JSON)
// and 'outStatus' with the HTTP status code. Safe to call on a worker thread.
bool HttpsGetGitHub(const wchar_t* host,        // L"api.github.com"
                    const wchar_t* path,        // L"/repos/OWNER/REPO/releases/latest"
                    std::string& outBody,
                    DWORD& outStatus)
{
    outBody.clear();
    outStatus = 0;
    bool ok = false;

    // 1) Session. WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY uses the system proxy config.
    HINTERNET hSession = WinHttpOpen(
        L"RitoTex-Plugin/1.0",                          // User-Agent (also set as header below)
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    // Timeouts (ms): resolve, connect, send, receive — keep short so we never linger.
    WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 8000);

    // Be explicit about TLS 1.2 (covers older Windows).
    DWORD proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof(proto));

    // 2) Connect (HTTPS default port 443). 'host' is used for SNI + cert validation.
    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        // 3) Request handle; WINHTTP_FLAG_SECURE => TLS.
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", path,
            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (hRequest) {
            // 4) Required/recommended GitHub headers.
            const wchar_t* headers =
                L"User-Agent: RitoTex-Plugin/1.0\r\n"
                L"Accept: application/vnd.github+json\r\n"
                L"X-GitHub-Api-Version: 2022-11-28\r\n";
            WinHttpAddRequestHeaders(hRequest, headers, (DWORD)-1,
                                     WINHTTP_ADDREQ_FLAG_ADD);

            // 5) Send + receive.
            if (WinHttpSendRequest(hRequest,
                    WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, NULL))
            {
                // Status code.
                DWORD code = 0, codeSize = sizeof(code);
                WinHttpQueryHeaders(hRequest,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize,
                    WINHTTP_NO_HEADER_INDEX);
                outStatus = code;

                // 6) Read body in chunks.
                DWORD avail = 0;
                do {
                    avail = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
                    if (avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, &chunk[0], avail, &read)) break;
                    chunk.resize(read);
                    outBody += chunk;
                } while (avail > 0);

                ok = (outStatus == 200);
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}
```

WinHTTP call sequence reference: *WinHTTP Sessions Overview* (https://learn.microsoft.com/en-us/windows/win32/winhttp/winhttp-sessions-overview) and the function pages for `WinHttpOpen`, `WinHttpConnect`, `WinHttpOpenRequest`, `WinHttpSendRequest`, `WinHttpReceiveResponse`, `WinHttpQueryDataAvailable`, `WinHttpReadData` (https://learn.microsoft.com/en-us/windows/win32/api/winhttp/).

**Proxy note:** `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` is available on Windows 8.1+ and uses the machine/system proxy config, which is what you want inside a corporate environment. On older Windows fall back to `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`. If you need full WPAD/PAC resolution you'd use `WinHttpGetIEProxyConfigForCurrentUser` + `WinHttpGetProxyForUrl`, but that's overkill for a once-a-day check — the automatic mode is fine.

---

## 3. Parsing JSON in C++ without heavy dependencies

You only need a handful of string fields, so weigh two options:

**Option A — nlohmann/json (RECOMMENDED).** Single header `json.hpp`, MIT-licensed, no build system changes — just drop it in and `#include`. Modern (actively maintained through 2025/2026), header-only, exception-based, trivial to read fields. The whole "extract tag_name" is three lines and it's robust against field ordering, whitespace, and escaped strings. Cost is a larger header and longer compile time for that one translation unit — negligible for a plugin.

```cpp
#include "json.hpp"          // nlohmann/json single header
using nlohmann::json;

bool ExtractTagName(const std::string& body, std::string& tag, std::string& htmlUrl) {
    try {
        json j = json::parse(body);
        tag     = j.value("tag_name", "");
        htmlUrl = j.value("html_url", "");
        // optional: bool pre = j.value("prerelease", false);
        return !tag.empty();
    } catch (const std::exception&) {
        return false;   // malformed / unexpected response — fail closed, never crash the host
    }
}
```

Source: nlohmann/json (https://github.com/nlohmann/json), single-include `single_include/nlohmann/json.hpp`.

**Option B — tiny hand parse for just `tag_name`.** Zero dependency, but you must correctly handle JSON string escaping, and a naive `find("\"tag_name\"")` can match the wrong key if the substring appears elsewhere (it won't for this exact response, but it's fragile). Acceptable only if you refuse to add any header. Sketch:

```cpp
// Extremely small, brittle extractor. Prefer Option A.
bool CrudeFindString(const std::string& body, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = body.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    size_t q1 = body.find('"', colon);
    if (q1 == std::string::npos) return false;
    size_t q2 = body.find('"', q1 + 1);          // does NOT handle \" escapes
    if (q2 == std::string::npos) return false;
    out = body.substr(q1 + 1, q2 - q1 - 1);
    return true;
}
```

**Recommendation: use nlohmann/json (Option A).** The cost is one header file; the benefit is correctness and the ability to read `html_url`, `prerelease`, `assets[]` later without rewriting a parser. (If even one header is unacceptable, `picojson` is a smaller single-header alternative; RapidJSON is faster but multi-file. Hand parsing is the last resort.)

---

## 4. When/where to trigger the check (and how to surface results)

### The constraint

A File Format plugin has **no persistent panel and no message loop of its own** — it's only entered when Photoshop calls it for a selector (read/open or write/save of a `.tex`). So the check must hang off one of those entry points, and it must **never block or slow the file operation**.

### Recommended trigger design

1. **Trigger point:** On the format plugin's **read selector (open)** — the most common entry — kick off the check. (Optionally also on write/save; gate so it runs at most once per Photoshop session via a static/once flag.) Doing it on open means a user who just opens a `.tex` will, at most once a day, learn there's a newer plugin.

2. **Throttle with a persisted timestamp** so you don't hit GitHub on every open and don't annoy the user. Store "last check" (and optionally "last seen latest tag" + a "muted version") in **per-user state**:
   - **Registry:** `HKEY_CURRENT_USER\Software\RitoTex\UpdateCheck` with values `LastCheckUnix` (REG_QWORD), `LastSeenTag` (REG_SZ), `MutedTag` (REG_SZ). HKCU is per-user and needs no admin rights.
   - **or AppData:** a small JSON/INI at `%LOCALAPPDATA%\RitoTex\update.json`.
   - On entry: read `LastCheckUnix`; if `now - last < 24h`, **skip** the network entirely. With a 24h interval you make ~1 request/day/machine — well under the 60/hr limit even behind shared NAT (§1).

3. **Run on a detached background thread.** Spawn a worker (`std::thread` detached, or `CreateThread`) that does the WinHTTP GET + parse + compare, then **return from the format selector immediately** so Photoshop's open/save is never delayed by network latency or a stalled connection. The worker must:
   - Capture only owned copies of data (no pointers into Photoshop's parameter blocks that may be freed).
   - **Never touch Photoshop suites/callbacks from the worker thread** — those are not thread-safe and are only valid during the selector call. The worker's job is purely: HTTP -> parse -> compare -> persist -> (maybe) notify via an OS-level mechanism.
   - Swallow all errors (offline, DNS fail, 403 rate-limit, malformed JSON). A failed update check must be completely silent and must never crash or hang the host. Update `LastCheckUnix` even on benign failure so you back off (or keep the old timestamp on transient failure if you prefer to retry sooner — pick one; backing off is safer for rate limits).

4. **Surface "update available" without blocking** — pick by how intrusive you want to be:
   - **Least intrusive (RECOMMENDED default): one-time, non-blocking toast/dialog.** When `IsNewer(tag, embeddedVersion)` and `tag != MutedTag`, pop a simple `MessageBox(NULL, ..., MB_OK | MB_ICONINFORMATION)` **from the worker thread** (a top-level MessageBox with `NULL` owner runs on its own and does not block Photoshop's UI thread or the file op). Offer "Open release page" (launch `html_url` via `ShellExecuteW(NULL, L"open", htmlUrl, ...)`) and remember the shown tag in `LastSeenTag` so you don't nag again for the same version. Optionally a "Skip this version" that writes `MutedTag`.
   - **Even quieter:** write a line to a log file (`%LOCALAPPDATA%\RitoTex\ritotex.log`: `Update available: v1.2.3 -> https://.../releases/tag/v1.2.3`) and/or set a flag the *next* time the plugin shows any dialog (e.g. append "(update available)" to an options dialog title). No popup at all.
   - **Avoid:** any modal dialog *inline* in the read/write selector (blocks file I/O), and any repeated nagging (always gate on `LastSeenTag`/`MutedTag`).

This gives: at most one network request per day per machine, zero added latency to file operations, and a single, dismissible, per-version notification that links to the release page.

---

## 5. Embedding the current version in the plugin

You need the plugin's own version available at runtime to compare against `tag_name`. Use **all three of these consistently driven from one source of truth** so they never drift:

1. **A single `#define` (source of truth).** In a shared header, e.g.:
   ```cpp
   #define RITOTEX_VERSION_MAJOR 1
   #define RITOTEX_VERSION_MINOR 2
   #define RITOTEX_VERSION_PATCH 3
   #define RITOTEX_VERSION_STR   "1.2.3"   // used by IsNewer()
   ```
   Use `RITOTEX_VERSION_STR` directly in the update-check comparison — simplest and zero runtime lookup.

2. **PiPL `Version` property.** Photoshop plugins carry a PiPL resource describing the plugin to the host; it includes a version field (plug-in version, distinct from the PiPL format version). Set it so Photoshop's Plug-ins info reports the right version. Drive it from the same numbers.

3. **Win32 `VERSIONINFO` resource (`.rc`).** Standard Windows file/product version on the `.8bi` DLL, visible in Explorer -> Properties -> Details and queryable at runtime with `GetFileVersionInfo`/`VerQueryValue`. Useful as a robust runtime fallback: read your own module's version with `GetModuleFileNameW(hInstance,...)` then `GetFileVersionInfoW`. Drive `FILEVERSION`/`PRODUCTVERSION` from the same numbers.

**Recommendation:** Keep the `#define` as the single source of truth, reference `RITOTEX_VERSION_STR` directly in the comparison (no resource lookup needed at check time), and mirror those numbers into the PiPL version and the `VERSIONINFO` resource for tooling/host display. Sources: Adobe Photoshop C++ Plug-in SDK — *Plug-in Property List (PiPL)* documentation; Microsoft Learn — *VERSIONINFO resource* and *GetFileVersionInfo/VerQueryValue* (https://learn.microsoft.com/en-us/windows/win32/menurc/versioninfo-resource).

---

## 6. Security

- **TLS is verified by default — keep it that way.** WinHTTP validates the server certificate chain against the Windows trust store and checks the hostname automatically when you use `WINHTTP_FLAG_SECURE` and pass the real hostname to `WinHttpConnect`. **Do not** set `WINHTTP_OPTION_SECURITY_FLAGS` to ignore cert errors (`SECURITY_FLAG_IGNORE_*`). If the handshake fails, abort the check silently — do not fall back to HTTP.
- **Talk only to `https://api.github.com`** (and follow GitHub's own redirects to `*.github.com` only). Pin to TLS 1.2+.
- **Do NOT auto-download or auto-execute anything.** The plugin's only action on "update available" is to **link the user to `html_url`** (the GitHub release page) via `ShellExecuteW`. The user reviews and installs manually. This eliminates the entire class of "the updater downloaded and ran a malicious/corrupted binary" risks and avoids needing code-signing/verification of downloaded payloads inside the plugin.
- **Do not ship a GitHub token** in the binary. A public repo's releases are readable unauthenticated; an embedded token is both unnecessary and a leak risk (it would sit in plaintext in the `.8bi`). Live within the 60/hr unauthenticated limit via throttling (§1, §4).
- **Fail closed and silent.** Any network/parse error, non-200 status, or rate-limit `403` must result in *no UI and no crash* — the update check is strictly best-effort and must never interfere with opening/saving a `.tex`.
- **Treat the response as untrusted text.** Parse defensively (exceptions caught), bound the body size you accept, and only ever *display* `tag_name`/`name`/`html_url` and *launch* `html_url` (validate it begins with `https://github.com/`). Never feed response content to a shell or interpret it as code.

---

## Recommended design (summary diagram)

```
[Photoshop calls RitoTex read/write selector]
        |
        v
  once-per-session guard  --(already checked this session)--> return
        |
        v
  read LastCheckUnix from HKCU\Software\RitoTex
        |
   (now - last < 24h)? --yes--> return  (no network)
        | no
        v
  spawn DETACHED worker thread  --> return from selector IMMEDIATELY
        |
        v (on worker, never touches PS suites)
  WinHTTP GET https://api.github.com/repos/OWNER/REPO/releases/latest
     headers: User-Agent, Accept: application/vnd.github+json, X-GitHub-Api-Version
        |
   200? --no--> write LastCheckUnix, exit silently
        | yes
        v
  nlohmann::json parse -> tag_name, html_url
        |
  IsNewer(tag, RITOTEX_VERSION_STR) && tag != MutedTag ?
        | yes
        v
  non-blocking MessageBox(NULL,...) "Update v1.2.3 available"
     [Open release page -> ShellExecute(html_url)] [Skip this version -> MutedTag]
        |
        v
  write LastSeenTag + LastCheckUnix(now)  -> done
```

---

## Key citations

- GitHub — *REST API endpoints for releases* (get-latest-release endpoint, response fields): https://docs.github.com/en/rest/releases/releases
- GitHub — *Getting started with the REST API* (Accept + X-GitHub-Api-Version: 2022-11-28): https://docs.github.com/en/rest/using-the-rest-api/getting-started-with-the-rest-api
- GitHub — *About the REST API*: https://docs.github.com/en/rest/about-the-rest-api/about-the-rest-api
- GitHub — *Rate limits for the REST API* (60/hr unauthenticated, per IP; x-ratelimit-* headers): https://docs.github.com/en/rest/using-the-rest-api/rate-limits-for-the-rest-api
- GitHub — *Best practices for using the REST API* / *Resources in the REST API* (User-Agent mandatory): https://docs.github.com/en/rest/using-the-rest-api/best-practices-for-using-the-rest-api , https://docs.github.com/en/rest/using-the-rest-api/resources-in-the-rest-api
- Microsoft Learn — *WinHTTP Sessions Overview* (call sequence): https://learn.microsoft.com/en-us/windows/win32/winhttp/winhttp-sessions-overview
- Microsoft Learn — *About WinHTTP* / *Porting WinINet applications to WinHTTP* (WinHTTP for services/non-interactive; WinINet not supported there): https://learn.microsoft.com/en-us/windows/win32/winhttp/about-winhttp , https://learn.microsoft.com/en-us/windows/win32/winhttp/porting-winhttp-and-wininet
- Microsoft Learn — WinHTTP API reference (WinHttpOpen/Connect/OpenRequest/SendRequest/ReceiveResponse/QueryDataAvailable/ReadData, WINHTTP_OPTION_SECURE_PROTOCOLS): https://learn.microsoft.com/en-us/windows/win32/api/winhttp/
- Microsoft Learn — *VERSIONINFO resource* / file-version APIs: https://learn.microsoft.com/en-us/windows/win32/menurc/versioninfo-resource
- nlohmann/json (single-header JSON parser): https://github.com/nlohmann/json
- Adobe Photoshop C++ Plug-in SDK — Plug-in Resource (PiPL) and Format module documentation (Adobe Developer; SDK download/guide).
```
