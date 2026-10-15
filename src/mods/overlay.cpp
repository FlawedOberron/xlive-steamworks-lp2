#include "mods/lp2.h"

#include "core/config.h"
#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

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

void BuildIndex()
{
	ConfigEnsureLoaded();
	g_gameDirLower = Lower(ExeDirectory());
	for (const std::wstring& mod : Cfg().modsEnabled) {
		std::wstring root = Cfg().modDir + mod + L"\\";
		size_t count = 0;
		IndexFolder(root, L"", &count);
		XLS_LOG_INFO("mods: overlay %s: %u files from %s.", WideToUtf8(mod.c_str()).c_str(), (unsigned)count, WideToUtf8(root.c_str()).c_str());
	}
}

// The mod file that replaces path, or nullptr.
const std::wstring* Redirect(const std::wstring& path)
{
	std::call_once(g_indexOnce, BuildIndex);
	if (Cfg().modTraceOpens > 0 && InterlockedIncrement(&g_traced) <= Cfg().modTraceOpens) {
		XLS_LOG_INFO("mods: open %s", WideToUtf8(path.c_str()).c_str());
	}
	if (g_index.empty()) {
		return nullptr;
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
	auto it = g_index.find(lower.substr(g_gameDirLower.size()));
	if (it == g_index.end()) {
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(g_loggedMutex);
	if (g_logged.insert(it->first).second) {
		XLS_LOG_INFO("mods: %s -> %s", WideToUtf8(it->first.c_str()).c_str(), WideToUtf8(it->second.c_str()).c_str());
	}
	return &it->second;
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
