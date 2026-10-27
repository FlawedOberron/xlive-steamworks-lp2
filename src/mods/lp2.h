// Lost Planet 2 specific mods, enabled from the "mods" section of the configuration.
#pragma once

#include <stdint.h>

#include <string>
#include <functional>
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
int RulesStep(int rule, int key, uint8_t* value, const std::function<int()>& original); // -1 not ours, 0 unchanged, 1 changed, 2 changed by original (sound made)
const char* RulesMessage(uint32_t id);
uint32_t RulesMessageAlias(uint32_t id); // our ids that reuse a game message -> that message
bool RulesPatchMenuFlash(std::vector<uint8_t>* gfx);
bool RulesPatchTrainingFlash(std::vector<uint8_t>* gfx); // our AI Enemy Battle rows (flash\training\id300)
// Adds a DoAction with script before a GFx movie's first ShowFrame (after the root's own script).
bool FlashAddRootScript(std::vector<uint8_t>* gfx, const std::vector<uint8_t>& script, const char* what);
// Options > Network (netmenu.cpp): the entry's script for the Options Flash (flash\option\id500_PC),
// and the hook that answers it.
bool NetMenuPatchFlash(std::vector<uint8_t>* gfx);
// Adds the Network entry's texts to message\msgPC\MsgOption, every language (netmenu.cpp).
bool NetMenuPatchMessages(std::vector<uint8_t>* file);
void NetMenuInstall(unsigned char* base);
// A map package was bound to online slot 13 / 14 (packages.cpp): its base map's preload list and its size class.
void MapsRebind(int slot, int base, int size, const std::vector<std::string>& preload);

// Enabled mod folders in load order (zip mods unpacked into ModCache), computed once.
const std::vector<std::wstring>& ModRootList();

// Code plugins (plugins.cpp, contract in sdk/lp2mod.h): top-level .dll files of enabled mods.
void PluginsLoad();
bool PluginsPoll(); // true while some plugin still wants polling
void PluginsGiveUp();
// Called on the game's first XLiveInitialize: its code is decrypted by then.
void NotifyTitleStarted();
bool TitleStarted();

// First run under a new gamertag: copies the newest other gamertag's save in (GFWL → Steam switch);
// then converts our save if another game version wrote it (the original kept as <save>.original).
void SavesImport(const std::string& gamertag);
// A save file from any game version (a GFWL-era file has GFWL's protected header in front): fills `out`
// with the save in the current format and returns true; false = already current, or not an LP2 save.
bool SavesConvert(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

// Re-applies the import hooks if something (SteamStub) restored the import table.
void OverlayVerify();

}
