#include "mods/lp2.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>

#include <stdint.h>

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
	bool started = false;
	for (int tick = 0; tick < 30000 && (mapsPending || pluginsPending || !started); tick++) {
		started = TitleStarted();
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
	}
	return 0;
}

}

void Start()
{
	HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	}
}

}
