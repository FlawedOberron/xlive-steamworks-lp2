#include "mods/lp2.h"
#include "mods/parts.h"
#include "mods/package.h"
#include "mods/packages.h"

#include "core/config.h"
#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace xls::mods {

namespace {

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetFileAttributesAFn = DWORD(WINAPI*)(LPCSTR);

CreateFileAFn g_createFileA = nullptr;
CreateFileWFn g_createFileW = nullptr;
GetFileAttributesAFn g_getFileAttributesA = nullptr;

// Lower-cased path relative to the game folder -> the mod file that replaces it.
std::unordered_map<std::wstring, std::wstring> g_index;
std::wstring g_gameDirLower;
std::once_flag g_indexOnce;
std::mutex g_loggedMutex;
std::set<std::wstring> g_logged;
LONG g_traced = 0;

std::wstring Lower(std::wstring s)
{
	for (wchar_t& c : s) {
		c = c == L'/' ? L'\\' : (wchar_t)towlower(c);
	}
	return s;
}

void IndexFolder(const std::wstring& root, const std::wstring& rel, size_t* count)
{
	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW((root + rel + L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		std::wstring name = data.cFileName;
		if (name == L"." || name == L"..") {
			continue;
		}
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			IndexFolder(root, rel + name + L"\\", count);
		}
		else {
			g_index[Lower(rel + name)] = root + rel + name;
			(*count)++;
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
}

// A zip mod is unpacked once into ModCache\mods\<name>\ (again when the zip changes).
std::wstring UnpackZipMod(const std::wstring& zipPath, const std::wstring& name)
{
	std::wstring dir = Cfg().modCacheDir + L"mods\\" + name + L"\\";
	WIN32_FILE_ATTRIBUTE_DATA a{};
	GetFileAttributesExW(zipPath.c_str(), GetFileExInfoStandard, &a);
	char buf[128];
	snprintf(buf, sizeof(buf), "lp2-mods 1 size %lu:%lu time %lu:%lu", a.nFileSizeHigh, a.nFileSizeLow, a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
	std::string stamp = buf;
	Bytes old;
	if (ReadWholeFile(dir + L"stamp.txt", &old) && std::string(old.begin(), old.end()) == stamp) {
		return dir;
	}
	Zip zip;
	if (!zip.Open(zipPath)) {
		XLS_LOG_WARN("mods: %ls is not a readable zip.", zipPath.c_str());
		return L"";
	}
	for (const ZipEntry& e : zip.Entries()) {
		Bytes data;
		if (e.name.find("..") != std::string::npos || !zip.Read(e, &data)) {
			continue;
		}
		std::wstring out = dir + Utf8ToWide(e.name.c_str());
		for (wchar_t& c : out) {
			c = c == L'/' ? L'\\' : c;
		}
		WriteWholeFile(out, data);
	}
	WriteWholeFile(dir + L"stamp.txt", Bytes(stamp.begin(), stamp.end()));
	return dir;
}

// Overlay mods in load order: the "enabled" list if given, else everything in the Mods folder
// (folders and .zip files) alphabetically, minus "disabled". Later mods win.
std::vector<std::wstring> ModRoots()
{
	std::vector<std::wstring> names = Cfg().modsEnabled;
	if (names.empty()) {
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW((Cfg().modDir + L"*").c_str(), &data);
		if (find != INVALID_HANDLE_VALUE) {
			do {
				std::wstring n = data.cFileName;
				bool dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				if (n == L"." || n == L".." || (!dir && (n.size() < 4 || _wcsicmp(n.c_str() + n.size() - 4, L".zip") != 0))) {
					continue;
				}
				names.push_back(n);
			} while (FindNextFileW(find, &data));
			FindClose(find);
		}
		std::sort(names.begin(), names.end(), [](const std::wstring& x, const std::wstring& y) { return _wcsicmp(x.c_str(), y.c_str()) < 0; });
	}
	std::vector<std::wstring> roots;
	for (const std::wstring& n : names) {
		std::wstring stem = n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".zip") == 0 ? n.substr(0, n.size() - 4) : n;
		bool disabled = false;
		for (const std::wstring& d : Cfg().modsDisabled) {
			disabled |= _wcsicmp(d.c_str(), n.c_str()) == 0 || _wcsicmp(d.c_str(), stem.c_str()) == 0;
		}
		if (disabled) {
			continue;
		}
		std::wstring path = Cfg().modDir + n;
		if (stem != n) {
			path = UnpackZipMod(path, stem);
			if (path.empty()) {
				continue;
			}
		}
		else {
			path += L"\\";
		}
		roots.push_back(path);
	}
	return roots;
}

void BuildIndex()
{
	ConfigEnsureLoaded();
	g_gameDirLower = Lower(ExeDirectory());
	for (const std::wstring& root : ModRootList()) {
		size_t count = 0;
		IndexFolder(root, L"", &count);
		XLS_LOG_INFO("mods: overlay %s: %u files.", WideToUtf8(root.c_str()).c_str(), (unsigned)count);
	}
	size_t before = g_index.size();
	PackagesIndex(&g_index);
	PartsIndex(&g_index);
	if (g_index.size() != before) {
		XLS_LOG_INFO("mods: map packages: %u files.", (unsigned)(g_index.size() - before));
	}
}

// The mod file that replaces path, or nullptr.
const std::wstring* Redirect(const std::wstring& path)
{
	std::call_once(g_indexOnce, BuildIndex);
	if (Cfg().modTraceOpens > 0 && InterlockedIncrement(&g_traced) <= Cfg().modTraceOpens) {
		XLS_LOG_INFO("mods: open %s", WideToUtf8(path.c_str()).c_str());
	}
	wchar_t full[MAX_PATH * 2];
	DWORD length = GetFullPathNameW(path.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
	if (length == 0 || length >= sizeof(full) / sizeof(full[0])) {
		return nullptr;
	}
	std::wstring lower = Lower(std::wstring(full, length));
	if (lower.compare(0, g_gameDirLower.size(), g_gameDirLower) != 0) {
		return nullptr;
	}
	std::wstring rel = lower.substr(g_gameDirLower.size());
	auto it = g_index.find(rel);
	const std::wstring* found = it == g_index.end() ? nullptr : &it->second;
	// Shared game files that map packages add to (names), built from whatever would be opened.
	if (const std::wstring* generated = PackagesGenerated(rel, found ? *found : std::wstring(full, length))) {
		found = generated;
	}
	if (!found) {
		// custom part loose files (model\hm\lpc\): log misses too, they show the name the game wants
		if (rel.find(L"\\lpc\\") != std::wstring::npos) {
			std::lock_guard<std::mutex> lock(g_loggedMutex);
			if (g_logged.insert(rel).second) {
				XLS_LOG_WARN("mods: part file not found: %s", WideToUtf8(rel.c_str()).c_str());
			}
		}
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(g_loggedMutex);
	if (g_logged.insert(rel).second) {
		XLS_LOG_INFO("mods: %s -> %s", WideToUtf8(rel.c_str()).c_str(), WideToUtf8(found->c_str()).c_str());
	}
	return found;
}

std::wstring Widen(LPCSTR path)
{
	int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
	std::wstring out(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1) {
		MultiByteToWideChar(CP_ACP, 0, path, -1, &out[0], n);
	}
	return out;
}

bool ReadOnlyOpen(DWORD access, DWORD disposition)
{
	return !(access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) && disposition == OPEN_EXISTING;
}

HANDLE WINAPI HookCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
{
	if (path && ReadOnlyOpen(access, disposition)) {
		if (const std::wstring* mod = Redirect(Widen(path))) {
			return g_createFileW(mod->c_str(), access, share, security, disposition, flags, templateFile);
		}
	}
	return g_createFileA(path, access, share, security, disposition, flags, templateFile);
}

HANDLE WINAPI HookCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
{
	if (path && ReadOnlyOpen(access, disposition)) {
		if (const std::wstring* mod = Redirect(path)) {
			return g_createFileW(mod->c_str(), access, share, security, disposition, flags, templateFile);
		}
	}
	return g_createFileW(path, access, share, security, disposition, flags, templateFile);
}

DWORD WINAPI HookGetFileAttributesA(LPCSTR path)
{
	if (path) {
		if (const std::wstring* mod = Redirect(Widen(path))) {
			return GetFileAttributesW(mod->c_str());
		}
	}
	return g_getFileAttributesA(path);
}

struct Hooked {
	const char* name;
	void** slot;
	void* hook;
};
Hooked g_hooked[3];
int g_hookedCount = 0;

// Points the game module's KERNEL32 import of name at hook; returns the original.
void* HookImport(HMODULE module, const char* name, void* hook)
{
	auto* base = (uint8_t*)module;
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) {
		return nullptr;
	}
	for (auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); desc->Name; desc++) {
		if (_stricmp((const char*)(base + desc->Name), "kernel32.dll") != 0 || !desc->OriginalFirstThunk) {
			continue;
		}
		auto* names = (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk);
		auto* slots = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
		for (; names->u1.AddressOfData; names++, slots++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
				continue;
			}
			auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
			if (strcmp((const char*)byName->Name, name) != 0) {
				continue;
			}
			void* original = (void*)slots->u1.Function;
			DWORD old = 0;
			if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
				return nullptr;
			}
			slots->u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
			if (g_hookedCount < 3) {
				g_hooked[g_hookedCount++] = {name, (void**)&slots->u1.Function, hook};
			}
			return original;
		}
	}
	return nullptr;
}

}

const std::vector<std::wstring>& ModRootList()
{
	static std::once_flag once;
	static std::vector<std::wstring> roots;
	std::call_once(once, [] {
		ConfigEnsureLoaded();
		roots = ModRoots();
	});
	return roots;
}

void OverlayInstall()
{
	// The import table sits outside the SteamStub-encrypted .text and the loader fills it before
	// DllMain runs, so it can be patched now. Whether any mod is enabled is decided lazily on the
	// first open, because DllMain must not read the configuration.
	HMODULE game = GetModuleHandleW(nullptr);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	g_createFileA = (CreateFileAFn)GetProcAddress(kernel, "CreateFileA");
	g_createFileW = (CreateFileWFn)GetProcAddress(kernel, "CreateFileW");
	g_getFileAttributesA = (GetFileAttributesAFn)GetProcAddress(kernel, "GetFileAttributesA");
	if (!g_createFileA || !g_createFileW || !g_getFileAttributesA) {
		return;
	}
	HookImport(game, "CreateFileA", (void*)HookCreateFileA);
	HookImport(game, "CreateFileW", (void*)HookCreateFileW);
	HookImport(game, "GetFileAttributesA", (void*)HookGetFileAttributesA);
}

void OverlayVerify()
{
	for (int i = 0; i < g_hookedCount; i++) {
		Hooked& h = g_hooked[i];
		if (*h.slot == h.hook) {
			continue;
		}
		XLS_LOG_WARN("mods: import %s was rewritten (now 0x%p), hooking it again.", h.name, *h.slot);
		DWORD old = 0;
		if (VirtualProtect(h.slot, sizeof(void*), PAGE_READWRITE, &old)) {
			*h.slot = h.hook;
			VirtualProtect(h.slot, sizeof(void*), old, &old);
		}
	}
}

}
