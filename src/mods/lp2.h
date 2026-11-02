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
// The game builds the per-build address tables list, in this order: the Steam exes (1.000) LP2DX11, LP2DX9, then
// the disc exes (1.100) LP2DX11, LP2DX9 (even = DX11). BuildName(i) for log lines.
const int kBuildCount = 4;
const char* BuildName(int build);
// Which of those this game is (-1: unknown): DX11 / DX9 from the exe's name, Steam / disc from its version string
// (" 1.000" / " 1.100" in .rdata). Hooks use their table's row for it.
int GameBuild();
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
void GravityInstall(unsigned char* base); // map packages' gravity (gravity.cpp)
// Map packages' post-process volumes (post.cpp): PostSetMap at area load (null: not ours); the map's own colour
// filter grades them.
struct MapPhysics;
void PostInstall(unsigned char* base);
void PostSetMap(const MapPhysics* physics);
// Map packages' water volumes (water.cpp): the game's water lookups redirected; WaterSetMap at area load (null: not ours).
void WaterInstall(unsigned char* base);
// Live create-match pictures (mappics.cpp): rTexture file data for slot index k (0: 13, 1: 14) into its loaded picture.
void MapPicsInstall(unsigned char* base);
void MapPicsUpdate(int k, const std::vector<uint8_t>& rtexture);
// The create-match Flash (id221) given pictures for the mod slots' frames (thumbflash.cpp): 1 patched, 0 already, -1 not the stock layout.
int ThumbsPatchMenuFlash(std::vector<uint8_t>* gfx);
void WaterSetMap(const MapPhysics* physics);
// Map packages' damage volumes (damage.cpp): the game's slip damage with our values; DamageSetMap at area load (null:
// not ours), DamageCharacter from the per-character update (gravity.cpp).
void DamageInstall(unsigned char* base);
void DamageSetMap(const MapPhysics* physics);
void DamageCharacter(unsigned char* character);
// A character's health and T-Eng (damage.cpp); false when not known yet.
struct CharacterVitals {
	float life, lifeMax, energy, energyMax;
};
bool CharacterStats(const unsigned char* character, CharacterVitals* out);
void CharacterDamage(unsigned char* character, float amount);    // the game's damage (deaths, kill feed, network)
void CharacterAddEnergy(unsigned char* character, float amount); // T-Eng, kept within 0..its maximum
void CharacterTeleport(unsigned char* character, const float pos[3], float yaw); // cm, game axes; yaw degrees as
                                                                                 // the markers' (NaN: keep facing)
// Map packages' one-way volumes (oneway.cpp): characters', VS's and shells' updates wrapped; OnewaySetMap at area load.
void OnewayInstall(unsigned char* base);

// Game versions that may play together (sessions.compatible_versions; versions.cpp).
void VersionsInstall(unsigned char* base);

// Bot brains per game type and the "other weapon" FilterWeapon mode (bots.cpp).
void BotsInstall(unsigned char* base);
// The overlay mod file that replaces a game file (overlay.cpp), or nullptr.
const std::wstring* OverlayRedirect(const std::wstring& path);
void OnewaySetMap(const MapPhysics* physics);
// Scripts' spawns (spawn.cpp): a weapon pickup (kSpawnWeapon: grenades, VS weapons too) or an empty VS (kSpawnVs) of
// item / VS type at pos (cm), turned yaw degrees; on the game thread. False when the game refused (VS setting).
enum { kSpawnVs = 2, kSpawnWeapon = 4 };
void SpawnInstall(unsigned char* base);
bool SpawnObject(int kind, int item, const float pos[3], float yaw);
// Map packages' visual scripts (scripts.cpp): ScriptSetMap at area load (null: not ours), ScriptCharacter from the
// per-character update (gravity.cpp); ScriptVolumeOn(sid): whether a script volume's effects are on (-1: not a script's).
void ScriptsInit();
void ScriptSetMap(const MapPhysics* physics);
void ScriptCharacter(unsigned char* character);
bool ScriptVolumeOn(int sid);
// A script volume's setting (ScriptSetting) as a script has set it (fading to it), else base.
float ScriptValue(int sid, int setting, float base);
// The create-match rows of our rules (offsets 0x2e.. of the rules bytes): steps one (true when it
// was ours), answers our MsgOnline texts, adds the rows to the menu's Flash (id221 GFx).
int RulesStep(int rule, int key, uint8_t* value, const std::function<int()>& original); // -1 not ours, 0 unchanged, 1 changed, 2 changed by original (sound made)
const char* RulesMessage(uint32_t id);
int RulesSlotOf(const uint8_t* character); // the match slot whose body it is, -1 none (AI, akrid)
int RulesLocalSlot();                      // the local player's match slot, -1 not known
unsigned char* RulesBody(int slot);        // the slot's character (on foot or its VS), null none
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
