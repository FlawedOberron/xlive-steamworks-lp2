// Lost Planet 2 specific mods, enabled from the "mods" section of the configuration.
#pragma once

#include <stdint.h>

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
// Extra match rules (rules.cpp): unlimited ammo / grenades. The host's apply in its lobby.
void RulesInstall(unsigned char* base);
// The create-match rows of our rules (offsets 0x2e.. of the rules bytes): steps one (true when it
// was ours), answers our MsgOnline texts, adds the rows to the menu's Flash (id221 GFx).
bool RulesStep(int rule, int key, uint8_t* value);
const char* RulesMessage(uint32_t id);
bool RulesPatchMenuFlash(std::vector<uint8_t>* gfx);
// A map package was bound to online slot 13 / 14 (packages.cpp): its base map's preload list and its size class.
void MapsRebind(int slot, int base, int size);

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
