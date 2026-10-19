// Lost Planet 2 specific mods, enabled from the "mods" section of the configuration.
#pragma once

#include <string>
#include <vector>

namespace xls::mods {

// Called from DllMain. Starts a worker that loads the plugin mods and waits for SteamStub to
// decrypt the game's code to apply the mod-support patches.
void Start();
void CrashLogInstall(); // logs faults (module + offset, registers, call trail) to the xlive log

// Hooks the game's file opens so files in enabled mod folders replace the game's own
// (mods/<name>/nativePC/... overrides <game>/nativePC/...). Called from DllMain.
void OverlayInstall();
// Extra online map slots declared by mod.json files (maps.cpp). MapsWanted loads them;
// MapsTryInstall patches the game once its code is decrypted and returns true when done.
bool MapsWanted();
bool MapsTryInstall(unsigned char* base);
void MapsPatchPickers(unsigned char* base);

// Enabled mod folders in load order (zip mods unpacked into ModCache), computed once.
const std::vector<std::wstring>& ModRootList();

// Code plugins (plugins.cpp, contract in sdk/lp2mod.h): top-level .dll files of enabled mods.
void PluginsLoad();
bool PluginsPoll(); // true while some plugin still wants polling
void PluginsGiveUp();
// Called on the game's first XLiveInitialize: its code is decrypted by then.
void NotifyTitleStarted();
bool TitleStarted();

// Re-applies the import hooks if something (SteamStub) restored the import table.
void OverlayVerify();

}
