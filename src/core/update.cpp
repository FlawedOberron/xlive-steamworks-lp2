#include "core/update.h"

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/utils.h"
#include "mods/package.h"
#include "xlive/xfuncs.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <string>
#include <vector>

#ifndef XLS_RELEASE_TAG
#define XLS_RELEASE_TAG ""
#endif

// the tag as one string in the binary (the compiler builds `ours` below from immediates), for release/make.sh to check
__attribute__((used)) static const char kReleaseTagMarker[] = "xls-release-tag:" XLS_RELEASE_TAG;

namespace xls {
namespace {

const wchar_t kHost[] = L"api.github.com";
const wchar_t kLatest[] = L"/repos/FlawedOberron/xlive-steamworks-lp2/releases/latest";
const char kAgent[] = "xlive-steamworks-lp2-updater";
const uint64_t kMaxZip = 64ull << 20;
std::atomic<bool> g_started{false};
const DWORD kWaitMs = 10000;   // start-up waits this long for the check (and a download) before going on
std::string g_installed;       // the release tag put in place this run (read after g_done)
HANDLE g_done = nullptr;

// GET https://host/path (redirects followed) -> body; false on any failure or a status other than 200.
bool HttpsGet(const std::wstring& host, const std::wstring& path, std::vector<uint8_t>* out, int depth = 0)
{
	HINTERNET session = WinHttpOpen(Utf8ToWide(kAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		return false;
	}
	WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
	bool ok = false;
	HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	const wchar_t headers[] = L"Accept: application/vnd.github+json\r\n";
	if (request && WinHttpSendRequest(request, headers, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
		DWORD status = 0, size = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		if (status == 200) {
			out->clear();
			ok = true;
			for (;;) {
				DWORD avail = 0;
				if (!WinHttpQueryDataAvailable(request, &avail)) {
					ok = false;
					break;
				}
				if (!avail) {
					break;
				}
				size_t at = out->size();
				out->resize(at + avail);
				DWORD got = 0;
				if (!WinHttpReadData(request, out->data() + at, avail, &got) || out->size() > kMaxZip) {
					ok = false;
					break;
				}
				out->resize(at + got);
			}
		}
		else {
			XLS_LOG_WARN("update: %ls%ls answered HTTP %lu.", host.c_str(), path.c_str(), status);
		}
	}
	if (request) {
		WinHttpCloseHandle(request);
	}
	if (connect) {
		WinHttpCloseHandle(connect);
	}
	WinHttpCloseHandle(session);
	return ok;
}

// https://host/path?query -> host, path?query
bool SplitUrl(const std::string& url, std::wstring* host, std::wstring* path)
{
	const std::string scheme = "https://";
	if (url.compare(0, scheme.size(), scheme) != 0) {
		return false;
	}
	size_t slash = url.find('/', scheme.size());
	if (slash == std::string::npos) {
		return false;
	}
	*host = Utf8ToWide(url.substr(scheme.size(), slash - scheme.size()).c_str());
	*path = Utf8ToWide(url.substr(slash).c_str());
	return true;
}

std::wstring OwnPath()
{
	HMODULE self = nullptr;
	wchar_t buf[MAX_PATH] = {};
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&OwnPath, &self)
	    || !GetModuleFileNameW(self, buf, MAX_PATH)) {
		return std::wstring();
	}
	return buf;
}

// Release tags sort by date then letter ("test-2026-10-21" < "test-2026-10-21b" < "test-2026-10-22").
bool Newer(const std::string& latest, const std::string& ours)
{
	const std::string prefix = "test-";
	if (latest.compare(0, prefix.size(), prefix) != 0 || ours.compare(0, prefix.size(), prefix) != 0) {
		return false; // another naming scheme: never guess
	}
	return latest > ours;
}

void Check()
{
	std::wstring self = OwnPath();
	if (!self.empty()) {
		DeleteFileW((self + L".old").c_str()); // the one an earlier update replaced
	}
	std::string ours = XLS_RELEASE_TAG;
	if (!ours.empty()) {
		// downloaded zips of this release or older ones: installed already, not needed again
		std::wstring dir = Cfg().modCacheDir + L"update\\";
		WIN32_FIND_DATAW found;
		HANDLE find = FindFirstFileW((dir + L"*.zip").c_str(), &found);
		if (find != INVALID_HANDLE_VALUE) {
			do {
				std::wstring name = found.cFileName;
				std::string tag = WideToUtf8(name.substr(0, name.size() - 4).c_str(), -1);
				if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !Newer(tag, ours) && DeleteFileW((dir + name).c_str())) {
					XLS_LOG_INFO("update: removed the old download %ls.", name.c_str());
				}
			} while (FindNextFileW(find, &found));
			FindClose(find);
		}
	}
	std::vector<uint8_t> body;
	if (!HttpsGet(kHost, kLatest, &body)) {
		XLS_LOG_INFO("update: couldn't ask GitHub for the latest release.");
		return;
	}
	Json root;
	std::string error;
	if (!Json::Parse(std::string(body.begin(), body.end()), root, &error)) {
		XLS_LOG_WARN("update: GitHub's answer isn't JSON (%s).", error.c_str());
		return;
	}
	std::string latest = root["tag_name"].GetString(std::string());
	if (ours.empty()) {
		XLS_LOG_INFO("update: development build; the latest release is %s (not installed).", latest.c_str());
		return;
	}
	if (!Newer(latest, ours)) {
		XLS_LOG_INFO("update: %s is up to date (latest release %s).", ours.c_str(), latest.c_str());
		return;
	}
	std::string url;
	for (const Json& asset : root["assets"].Elements()) {
		std::string name = asset["name"].GetString(std::string());
		if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".zip") == 0) {
			url = asset["browser_download_url"].GetString(std::string());
			break;
		}
	}
	std::wstring host, path;
	if (url.empty() || !SplitUrl(url, &host, &path)) {
		XLS_LOG_WARN("update: release %s has no zip to download.", latest.c_str());
		return;
	}
	XLS_LOG_INFO("update: %s -> %s, downloading %s.", ours.c_str(), latest.c_str(), url.c_str());
	std::vector<uint8_t> zipBytes;
	if (!HttpsGet(host, path, &zipBytes)) {
		XLS_LOG_WARN("update: the download failed; trying again next start.");
		return;
	}
	std::wstring dir = Cfg().modCacheDir + L"update\\";
	std::wstring zipPath = dir + Utf8ToWide(latest.c_str()) + L".zip";
	mods::Zip zip;
	mods::Bytes dll;
	const mods::ZipEntry* entry = nullptr;
	if (!mods::WriteWholeFile(zipPath, zipBytes) || !zip.Open(zipPath) || !(entry = zip.Find("xlive.dll")) || !zip.Read(*entry, &dll)
	    || dll.size() < 0x1000 || dll[0] != 'M' || dll[1] != 'Z') {
		XLS_LOG_WARN("update: %s's zip has no usable xlive.dll.", latest.c_str());
		return;
	}
	if (self.empty()) {
		XLS_LOG_WARN("update: can't find our own file to replace.");
		return;
	}
	// a loaded DLL can be renamed (not overwritten): the new one takes its name, the game loads it next start
	std::wstring staged = self + L".new", old = self + L".old";
	DeleteFileW(old.c_str());
	if (!WriteFileBytes(staged, dll.data(), dll.size())) {
		XLS_LOG_WARN("update: couldn't write %ls.", staged.c_str());
		return;
	}
	if (!MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		XLS_LOG_WARN("update: couldn't move the running xlive.dll aside (error %lu).", GetLastError());
		DeleteFileW(staged.c_str());
		return;
	}
	if (!MoveFileExW(staged.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		XLS_LOG_WARN("update: couldn't put the new xlive.dll in place (error %lu); restoring.", GetLastError());
		MoveFileExW(old.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING);
		return;
	}
	XLS_LOG_INFO("update: installed %s (%zu bytes); it runs from the next start.", latest.c_str(), dll.size());
	g_installed = latest;
}

DWORD WINAPI Worker(LPVOID)
{
	Check();
	SetEvent(g_done);
	return 0;
}

// The game waits on the dialog (the system UI notification pauses it), so this is seen before the save loads.
void Announce(const std::string& tag)
{
	std::wstring text = L"Update downloaded (" + Utf8ToWide(tag.c_str()) + L").\nRestart the game to install it.";
	LPCWSTR buttons[] = {L"OK"};
	MESSAGEBOX_RESULT result = {};
	XShowMessageBoxUI(XUSER_INDEX_ANY, L"Lost Planet 2 mod update", text.c_str(), 1, buttons, 0, XMB_ALERTICON, &result, nullptr);
}

}

void UpdateStart()
{
	if (!Cfg().updateAuto || g_started.exchange(true)) {
		return;
	}
	g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
	if (!thread) {
		return;
	}
	CloseHandle(thread);
	// before the title screen / save load: a slow GitHub only delays start-up by kWaitMs (the update then still
	// installs, unannounced, for the next start)
	if (WaitForSingleObject(g_done, kWaitMs) == WAIT_OBJECT_0 && !g_installed.empty()) {
		Announce(g_installed);
	}
}

}
