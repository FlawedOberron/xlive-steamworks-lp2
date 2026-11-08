#include <ctime>
#include "mods/lp2.h"

#include "core/config.h"
#include "core/log.h"
#include "mods/parts.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>
#include <wchar.h>

// The mods worker: loads the plugin mods, then polls while SteamStub decrypts the game's code in
// place (which can finish after DllMain returns) to apply the mod-support patches (map slots) and
// let plugins find theirs.

namespace xls::mods {

namespace {

DWORD WINAPI Worker(LPVOID)
{
	ConfigEnsureLoaded();
	auto* base = (uint8_t*)GetModuleHandleW(nullptr);
	bool mapsPending = MapsWanted();
	PluginsLoad();
	bool pluginsPending = true;
	bool pathPending = true; // also without sets of our own: other players' sets are loaded through it
	bool started = false;
	bool versionsPending = true;
	for (int tick = 0; tick < 30000 && (mapsPending || pluginsPending || pathPending || !started); tick++) {
		started = TitleStarted();
		// not tied to a known build (the disc exes have no map slots), and needed before the first search
		if (started && versionsPending) {
			GameBuild();
			VersionsInstall(base);
			versionsPending = false;
		}
		if (pathPending && PartsTryInstallPathHook(base)) {
			pathPending = false;
		}
		if (pluginsPending) {
			pluginsPending = PluginsPoll();
		}
		if (mapsPending && MapsTryInstall(base)) {
			mapsPending = false;
		}
		OverlayVerify();
		Sleep(1);
	}
	if (pluginsPending) {
		PluginsGiveUp();
	}
	if (mapsPending) {
		XLS_LOG_WARN("mods: maps: unknown game build, mod map slots not enabled.");
	}
	else if (!started) {
		XLS_LOG_WARN("mods: maps: the game did not start within 30 s, map pickers not patched.");
	}
	else {
		// The game has called XLiveInitialize, so all of its code is decrypted.
		MapsPatchPickers(base);
		RulesInstall(base);
		UiInstall(GameBuild());
		GravityInstall(base);
		PostInstall(base);
		WaterInstall(base);
		DamageInstall(base);
		OnewayInstall(base);
		ForceInstall(base);
		BotsInstall(base);
		SpawnInstall(base);
		ScriptsInit();
		MapPicsInstall(base);
		NetMenuInstall(base);
		BotMatchInstall(base);
	}
	if (started) {
		PartsStartUnlocker();
	}
	return 0;
}

// The retail exes (not encrypted, so patchable from DllMain) refuse to start ("InitRpl Failed",
// 80070002) unless Launcher.exe is their parent and serves a pipe; the handle is only stored and
// closed, so skip the check by turning its jge into a jmp (the handle is already 0 on failure).
void SkipLauncherCheck()
{
	static const uint8_t kCheck[] = {0x33, 0xdb, 0x83, 0xc4, 0x04, 0x3b, 0xc3, 0x7d, 0x3e, 0x50, 0x68};
	auto* base = (uint8_t*)GetModuleHandleW(nullptr);
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto* sec = IMAGE_FIRST_SECTION(nt);
	for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
		if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
			continue;
		}
		uint8_t* p = base + sec->VirtualAddress;
		uint8_t* end = p + sec->Misc.VirtualSize - sizeof(kCheck);
		for (; p < end; p++) {
			if (memcmp(p, kCheck, sizeof(kCheck)) == 0) {
				DWORD old;
				VirtualProtect(p + 7, 1, PAGE_EXECUTE_READWRITE, &old);
				p[7] = 0xeb;
				VirtualProtect(p + 7, 1, old, &old);
				XLS_LOG_INFO("mods: launcher check skipped (retail build).");
				return;
			}
		}
	}
}

}

const char* BuildName(int build)
{
	static const char* const kNames[kBuildCount] = {"DX11", "DX9", "DX11 1.1", "DX9 1.1"};
	return build >= 0 && build < kBuildCount ? kNames[build] : "unknown";
}

int GameBuild()
{
	// found once the exe is readable (SteamStub decrypts the Steam exes after DllMain); unknown until then
	static int build = -1;
	if (build >= 0) {
		return build;
	}
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	const wchar_t* name = wcsrchr(path, L'\\');
	name = name ? name + 1 : path;
	int dx = _wcsnicmp(name, L"LP2DX11", 7) == 0 ? 0 : _wcsnicmp(name, L"LP2DX9", 6) == 0 ? 1 : -1;
	auto* base = (uint8_t*)GetModuleHandleW(nullptr);
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto* sec = IMAGE_FIRST_SECTION(nt);
	int version = -1;
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && version < 0; i++, sec++) {
		if (strncmp((const char*)sec->Name, ".rdata", 8) != 0) {
			continue;
		}
		uint8_t* p = base + sec->VirtualAddress;
		uint8_t* end = p + sec->Misc.VirtualSize - 8;
		for (p++; p < end; p++) {
			if (p[-1] == 0 && p[6] == 0 && memcmp(p, " 1.000", 6) == 0) {
				version = 0;
				break;
			}
			if (p[-1] == 0 && p[6] == 0 && memcmp(p, " 1.100", 6) == 0) {
				version = 1;
				break;
			}
		}
	}
	char linked[32] = "?";
	time_t stamp = (time_t)nt->FileHeader.TimeDateStamp;
	if (tm* t = gmtime(&stamp)) {
		strftime(linked, sizeof linked, "%Y-%m-%d", t);
	}
	if (dx >= 0 && version >= 0) {
		build = version * 2 + dx;
		XLS_LOG_INFO("game: %ls version %s (%s build, exe linked %s) -> %s.", name, version ? "1.100" : "1.000",
			version ? "retail/disc" : "Steam", linked, BuildName(build));
	}
	else if (TitleStarted()) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			XLS_LOG_WARN("game: unrecognised build: %ls, version string %s (exe linked %s); build-specific mods are off.", name,
				version < 0 ? "not found" : "found", linked);
		}
	}
	return build;
}

void Start()
{
	CrashLogInstall();
	SkipLauncherCheck();
	HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	}
}

}
