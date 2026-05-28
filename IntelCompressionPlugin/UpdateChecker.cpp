////////////////////////////////////////////////////////////////////////////////
// RitoTex — Update Checker implementation
//
// See UpdateChecker.h and docs/UpdateChecking_Design.md.
////////////////////////////////////////////////////////////////////////////////

#include "UpdateChecker.h"
#include "IntelPluginName.h"   // RITOTEX_VERSION_STR, RITOTEX_GITHUB_OWNER/REPO

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <string>
#include <atomic>
#include <thread>

// Trace to DebugView (Sysinternals). Every line is prefixed so you can filter
// on "RitoTex/Update". Active in all builds — it is cheap and silent unless a
// debugger/DebugView is attached.
#define RT_UPDLOG(msg) OutputDebugStringA("[RitoTex/Update] " msg "\n")

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
	// --- Configuration ---------------------------------------------------------
	constexpr wchar_t kRegKey[]        = L"Software\\RitoTex";
	constexpr wchar_t kValLastCheck[]  = L"LastCheckUnix";   // REG_QWORD
	constexpr wchar_t kValLastSeen[]   = L"LastSeenTag";     // REG_SZ
	constexpr wchar_t kValMutedTag[]   = L"MutedTag";        // REG_SZ
	constexpr long long kThrottleSecs  = 24 * 60 * 60;       // once per 24h
	constexpr DWORD kMaxBodyBytes      = 512 * 1024;         // bound untrusted input

	std::atomic<bool> g_checkStarted{ false };

	// --- Time ------------------------------------------------------------------
	long long NowUnix()
	{
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		ULARGE_INTEGER u;
		u.LowPart  = ft.dwLowDateTime;
		u.HighPart = ft.dwHighDateTime;
		// FILETIME is 100-ns intervals since 1601-01-01; convert to Unix seconds.
		constexpr unsigned long long kEpochDiff = 116444736000000000ULL;
		return static_cast<long long>((u.QuadPart - kEpochDiff) / 10000000ULL);
	}

	// --- Registry (per-user, HKCU; no admin needed) ----------------------------
	long long RegReadLastCheck()
	{
		HKEY hKey = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
			return 0;
		long long value = 0;
		DWORD size = sizeof(value), type = 0;
		RegQueryValueExW(hKey, kValLastCheck, nullptr, &type,
		                 reinterpret_cast<LPBYTE>(&value), &size);
		RegCloseKey(hKey);
		return (type == REG_QWORD) ? value : 0;
	}

	void RegWriteLastCheck(long long unix)
	{
		HKEY hKey = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0,
		                    KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
			return;
		RegSetValueExW(hKey, kValLastCheck, 0, REG_QWORD,
		               reinterpret_cast<const BYTE*>(&unix), sizeof(unix));
		RegCloseKey(hKey);
	}

	std::wstring RegReadString(const wchar_t* name)
	{
		HKEY hKey = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
			return L"";
		wchar_t buf[128] = {};
		DWORD size = sizeof(buf), type = 0;
		LONG r = RegQueryValueExW(hKey, name, nullptr, &type,
		                          reinterpret_cast<LPBYTE>(buf), &size);
		RegCloseKey(hKey);
		if (r != ERROR_SUCCESS || type != REG_SZ)
			return L"";
		return std::wstring(buf);
	}

	void RegWriteString(const wchar_t* name, const std::wstring& value)
	{
		HKEY hKey = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0,
		                    KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
			return;
		RegSetValueExW(hKey, name, 0, REG_SZ,
		               reinterpret_cast<const BYTE*>(value.c_str()),
		               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
		RegCloseKey(hKey);
	}

	// --- Semver compare --------------------------------------------------------
	void ParseSemver(const std::string& in, int out[3])
	{
		out[0] = out[1] = out[2] = 0;
		size_t i = 0;
		if (i < in.size() && (in[i] == 'v' || in[i] == 'V')) ++i;
		std::string core = in.substr(i);
		size_t cut = core.find_first_of("-+");          // strip prerelease/build
		if (cut != std::string::npos) core = core.substr(0, cut);
		int idx = 0;
		size_t pos = 0;
		while (idx < 3 && pos <= core.size())
		{
			size_t dot = core.find('.', pos);
			std::string part = core.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
			try { out[idx] = std::stoi(part); } catch (...) { out[idx] = 0; }
			++idx;
			if (dot == std::string::npos) break;
			pos = dot + 1;
		}
	}

	bool IsNewer(const std::string& remoteTag, const std::string& localVersion)
	{
		int r[3], l[3];
		ParseSemver(remoteTag, r);
		ParseSemver(localVersion, l);
		for (int i = 0; i < 3; ++i)
			if (r[i] != l[i]) return r[i] > l[i];
		return false;
	}

	// --- Minimal JSON string-field extractor -----------------------------------
	// Extracts the value of "key":"value", handling \" and \\ escapes. Good
	// enough for the flat, trusted-shape GitHub release object; we still bound
	// the input size and fail closed.
	bool ExtractJsonString(const std::string& body, const char* key, std::string& out)
	{
		std::string needle = std::string("\"") + key + "\"";
		size_t k = body.find(needle);
		if (k == std::string::npos) return false;
		size_t colon = body.find(':', k + needle.size());
		if (colon == std::string::npos) return false;
		size_t q1 = body.find('"', colon);
		if (q1 == std::string::npos) return false;
		std::string result;
		for (size_t i = q1 + 1; i < body.size(); ++i)
		{
			char c = body[i];
			if (c == '\\' && i + 1 < body.size())
			{
				char n = body[++i];
				switch (n)
				{
				case '"':  result += '"';  break;
				case '\\': result += '\\'; break;
				case '/':  result += '/';  break;
				case 'n':  result += '\n'; break;
				case 't':  result += '\t'; break;
				default:   result += n;    break;
				}
			}
			else if (c == '"')
			{
				out = result;
				return true;
			}
			else
			{
				result += c;
			}
		}
		return false;
	}

	std::wstring Widen(const std::string& s)
	{
		if (s.empty()) return L"";
		int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
		std::wstring w(n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
		return w;
	}

	// --- WinHTTP GET over TLS --------------------------------------------------
	bool HttpsGet(const wchar_t* host, const wchar_t* path, std::string& outBody)
	{
		outBody.clear();
		bool ok = false;

		HINTERNET hSession = WinHttpOpen(L"RitoTex-Plugin/" L"" RITOTEX_VERSION_STR,
		                                 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!hSession) return false;

		WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 8000);

		DWORD proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof(proto));

		HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
		if (hConnect)
		{
			HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
			                                        WINHTTP_NO_REFERER,
			                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
			                                        WINHTTP_FLAG_SECURE);
			if (hRequest)
			{
				const wchar_t* headers =
					L"User-Agent: RitoTex-Plugin/" L"" RITOTEX_VERSION_STR L"\r\n"
					L"Accept: application/vnd.github+json\r\n"
					L"X-GitHub-Api-Version: 2022-11-28\r\n";
				WinHttpAddRequestHeaders(hRequest, headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

				if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
				    WinHttpReceiveResponse(hRequest, nullptr))
				{
					DWORD code = 0, codeSize = sizeof(code);
					WinHttpQueryHeaders(hRequest,
					                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize,
					                    WINHTTP_NO_HEADER_INDEX);

					if (code == 200)
					{
						DWORD avail = 0;
						do
						{
							avail = 0;
							if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
							if (avail == 0) break;
							std::string chunk(avail, '\0');
							DWORD read = 0;
							if (!WinHttpReadData(hRequest, &chunk[0], avail, &read)) break;
							chunk.resize(read);
							outBody += chunk;
							if (outBody.size() > kMaxBodyBytes) break;   // bound input
						} while (avail > 0);
						ok = !outBody.empty();
					}
				}
				WinHttpCloseHandle(hRequest);
			}
			WinHttpCloseHandle(hConnect);
		}
		WinHttpCloseHandle(hSession);
		return ok;
	}

	// --- Notification ----------------------------------------------------------
	void NotifyUpdateAvailable(const std::string& tag, const std::string& htmlUrl)
	{
		// Only ever launch a github.com https URL.
		std::wstring wUrl = Widen(htmlUrl);
		bool urlSafe = wUrl.rfind(L"https://github.com/", 0) == 0;

		std::wstring msg = L"A new version of RitoTex is available.\n\n";
		msg += L"Installed: v" RITOTEX_VERSION_STR L"\n";
		msg += L"Latest: " + Widen(tag) + L"\n\n";
		msg += urlSafe ? L"Open the release page in your browser?"
		               : L"Visit the RitoTex GitHub releases page to update.";

		UINT flags = urlSafe ? (MB_YESNO | MB_ICONINFORMATION) : (MB_OK | MB_ICONINFORMATION);
		int r = MessageBoxW(nullptr, msg.c_str(), L"RitoTex — Update Available", flags);

		if (urlSafe && r == IDYES)
			ShellExecuteW(nullptr, L"open", wUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	// --- Worker ----------------------------------------------------------------
	void WorkerThread()
	{
		// Build "/repos/<owner>/<repo>/releases/latest".
		std::wstring path = L"/repos/"
			L"" RITOTEX_GITHUB_OWNER L"/"
			L"" RITOTEX_GITHUB_REPO  L"/releases/latest";

		std::string body;
		// Always stamp LastCheck (success or benign failure) so we back off and
		// stay well under the 60/hr unauthenticated rate limit.
		RegWriteLastCheck(NowUnix());

		RT_UPDLOG("worker: querying api.github.com for latest release...");
		if (!HttpsGet(L"api.github.com", path.c_str(), body))
		{
			RT_UPDLOG("worker: HTTPS GET failed (offline / 403 / non-200) - silent");
			return;   // offline / 403 / non-200 — silent
		}

		std::string tag, htmlUrl;
		if (!ExtractJsonString(body, "tag_name", tag) || tag.empty())
		{
			RT_UPDLOG("worker: no tag_name in response - bailing");
			return;
		}
		ExtractJsonString(body, "html_url", htmlUrl);

		OutputDebugStringA(("[RitoTex/Update] worker: latest tag = " + tag +
		                    ", installed = " RITOTEX_VERSION_STR "\n").c_str());

		if (!IsNewer(tag, RITOTEX_VERSION_STR))
		{
			RT_UPDLOG("worker: up to date - no notification");
			return;   // up to date
		}

		// Don't nag for a version the user already saw or muted.
		std::wstring wTag    = Widen(tag);
		std::wstring lastSeen = RegReadString(kValLastSeen);
		std::wstring muted    = RegReadString(kValMutedTag);
		if (wTag == lastSeen || wTag == muted)
		{
			RT_UPDLOG("worker: newer version found but already seen/muted - suppressed");
			return;
		}

		RT_UPDLOG("worker: UPDATE AVAILABLE - showing notification");
		NotifyUpdateAvailable(tag, htmlUrl);
		RegWriteString(kValLastSeen, wTag);
	}
}

namespace RitoTex
{
	void MaybeCheckForUpdatesAsync()
	{
		// Once per process session.
		bool expected = false;
		if (!g_checkStarted.compare_exchange_strong(expected, true))
		{
			RT_UPDLOG("MaybeCheck: already started this session - skipping");
			return;
		}

		// Throttle: skip the network entirely if we checked within 24h.
		// Debug builds bypass the throttle so every launch hits GitHub while
		// testing. (Define RITOTEX_FORCE_UPDATE_CHECK to force it in any build.)
#if defined(_DEBUG) || defined(RITOTEX_FORCE_UPDATE_CHECK)
		RT_UPDLOG("MaybeCheck: debug build - bypassing 24h throttle");
#else
		long long last = RegReadLastCheck();
		if (last > 0 && (NowUnix() - last) < kThrottleSecs)
		{
			RT_UPDLOG("MaybeCheck: checked within 24h - throttled, skipping");
			return;
		}
#endif

		RT_UPDLOG("MaybeCheck: spawning background update worker");
		try
		{
			std::thread(WorkerThread).detach();
		}
		catch (...)
		{
			RT_UPDLOG("MaybeCheck: failed to spawn worker thread - skipping");
			// If we can't spawn a thread, just skip — never disturb the host.
		}
	}
}
