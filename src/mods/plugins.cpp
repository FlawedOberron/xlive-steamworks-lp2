#include "mods/lp2.h"

#include "core/config.h"
#include "core/log.h"
#include "core/utils.h"

#include "../../sdk/lp2mod.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

// Code plugins: top-level .dll files of enabled mods, loaded from the mods worker thread
// (see sdk/lp2mod.h for the contract).

namespace xls::mods {

namespace {

struct Plugin {
	std::wstring path;
	std::string name;
	std::wstring dir;
	HMODULE module = nullptr;
	LP2ModHost host = {};
	LP2ModPollFn poll = nullptr;
	bool done = false;
};

std::vector<Plugin*> g_plugins;
volatile LONG g_titleStarted = 0;
thread_local const Plugin* t_current = nullptr;

// The plugin whose code contains address, for logs from game threads.
const Plugin* PluginAt(void* address)
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module)) {
		return nullptr;
	}
	for (const Plugin* p : g_plugins) {
		if (p->module == module) {
			return p;
		}
	}
	return nullptr;
}

__attribute__((noinline)) void HostLog(int level, const char* format, ...)
{
	char text[1024];
	va_list args;
	va_start(args, format);
	_vsnprintf(text, sizeof(text) - 1, format, args);
	va_end(args);
	text[sizeof(text) - 1] = 0;
	const Plugin* from = t_current ? t_current : PluginAt(__builtin_return_address(0));
	const char* name = from ? from->name.c_str() : "?";
	LogLevel l = level <= LP2MOD_LOG_ERROR ? LogLevel::Error : level == LP2MOD_LOG_WARN ? LogLevel::Warn : level == LP2MOD_LOG_INFO ? LogLevel::Info : LogLevel::Debug;
	LogWrite(l, "plugin %s: %s", name, text);
}

int HostWriteCode(void* at, const void* bytes, size_t size)
{
	DWORD old = 0;
	if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) {
		return 0;
	}
	memcpy(at, bytes, size);
	VirtualProtect(at, size, old, &old);
	FlushInstructionCache(GetCurrentProcess(), at, size);
	return 1;
}

int HostTitleStarted()
{
	return g_titleStarted != 0;
}

int GameExe()
{
	char exe[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	if (strstr(exe, "LP2DX11")) {
		return LP2MOD_EXE_DX11;
	}
	if (strstr(exe, "LP2DX9")) {
		return LP2MOD_EXE_DX9;
	}
	return LP2MOD_EXE_UNKNOWN;
}

}

void PluginsLoad()
{
	int exe = GameExe();
	for (const std::wstring& root : ModRootList()) {
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW((root + L"*.dll").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE) {
			continue;
		}
		do {
			auto* p = new Plugin();
			p->path = root + data.cFileName;
			p->dir = root;
			std::wstring stem = data.cFileName;
			p->name = WideToUtf8(stem.substr(0, stem.size() - 4).c_str());
			p->module = LoadLibraryW(p->path.c_str());
			if (!p->module) {
				XLS_LOG_ERROR("mods: plugin %s: load failed (%lu).", WideToUtf8(p->path.c_str()).c_str(), GetLastError());
				delete p;
				continue;
			}
			auto load = (LP2ModLoadFn)(void*)GetProcAddress(p->module, "lp2mod_load");
			if (!load) {
				XLS_LOG_ERROR("mods: plugin %s: no lp2mod_load export, unloaded.", p->name.c_str());
				FreeLibrary(p->module);
				delete p;
				continue;
			}
			p->poll = (LP2ModPollFn)(void*)GetProcAddress(p->module, "lp2mod_poll");
			p->host.size = sizeof(LP2ModHost);
			p->host.version = LP2MOD_HOST_VERSION;
			p->host.game_base = (unsigned char*)GetModuleHandleW(nullptr);
			p->host.game_exe = exe;
			p->host.mod_dir = p->dir.c_str();
			p->host.mod_name = p->name.c_str();
			p->host.log = HostLog;
			p->host.write_code = HostWriteCode;
			p->host.title_started = HostTitleStarted;
			t_current = p;
			int result = load(&p->host);
			t_current = nullptr;
			if (result != 0) {
				XLS_LOG_WARN("mods: plugin %s: lp2mod_load returned %d, unloaded.", p->name.c_str(), result);
				FreeLibrary(p->module);
				delete p;
				continue;
			}
			p->done = !p->poll;
			XLS_LOG_INFO("mods: plugin %s loaded from %s.", p->name.c_str(), WideToUtf8(p->path.c_str()).c_str());
			g_plugins.push_back(p);
		} while (FindNextFileW(find, &data));
		FindClose(find);
	}
}

bool PluginsPoll()
{
	bool pending = false;
	for (Plugin* p : g_plugins) {
		if (p->done) {
			continue;
		}
		t_current = p;
		p->done = p->poll() != 0;
		t_current = nullptr;
		pending |= !p->done;
	}
	return pending;
}

void PluginsGiveUp()
{
	for (Plugin* p : g_plugins) {
		if (!p->done) {
			XLS_LOG_WARN("mods: plugin %s: still polling after 30 s, stopped.", p->name.c_str());
			p->done = true;
		}
	}
}

void NotifyTitleStarted()
{
	InterlockedExchange(&g_titleStarted, 1);
}

bool TitleStarted()
{
	return g_titleStarted != 0;
}

}
