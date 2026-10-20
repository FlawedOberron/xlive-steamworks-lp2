// Custom part registry (parts.cpp): every part set in the Parts folder, however many there are.
//
// Plan (not wired to the game yet): part ids are bytes and 180-249 are the DLC slots, so custom
// parts never get ids of their own. Each category gets one "carrier" id from the unused range;
// a menu hook lets L/R on the carrier step through the registry (filtered by faction), the
// chosen part per player and category lives in a side table (saved next to the game's save),
// and the part path builder sends a carrier to a path unique to that part (so two players in
// different custom parts never share a cached resource), served from ModCache by the overlay.
// The DLC slots stay as they are.
#pragma once

#include <stdint.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace xls::mods {

enum PartSlot { kPartHead, kPartBody, kPartBack, kPartLegs, kPartSlotCount };

// One material value for one colour's model copy (experimental, lpc.json part
// "material_overrides": {"<letter>": {"<material index or *>": {"<word>": value}}}): the float
// at word (4-byte index) of the 160-byte material record. Words 18/19 look like specular power /
// strength.
struct MaterialOverride {
	int colour = 0;    // 0-3
	int material = -1; // -1 = every material
	int word = 0;      // 0-39
	float value = 0;
};

struct PartSet {
	std::string id;           // LPC id, unique ("scav01")
	std::string name;         // menu name (UTF-8, up to 43 bytes)
	std::string faction;      // RND FJ SPE NEV FF ALL
	int factionIndex = -1;    // the game's faction byte, -1 = every faction
	std::wstring source;      // the zip or folder it came from
	std::string hash;         // content hash of its bundle (16 hex; the same on every PC), see MakeBundle
	std::wstring models[kPartSlotCount]; // unpacked .rModel per slot, empty = set has no such part
	std::wstring chainCols[kPartSlotCount]; // the part's own .rChainCol (what the chain collides with), empty = the template's
	std::wstring chains[kPartSlotCount]; // the part's own .rChain (bone physics), empty = the template's
	std::string templates[kPartSlotCount]; // original part each was built on ("bd2900")
	bool noEffects[kPartSlotCount] = {}; // lpc.json part "effects": "none": drop the template's effects (lamps)
	int colour = 0;           // default colour (0-3 = A-D): the first of colours
	std::vector<int> colours; // colours the menu offers (lpc.json "colours"; one = locked swatch)
	// Custom textures per slot ("BM_A".."BM_D", "NM", "MM" -> .rTexture file), replacing the template's.
	std::vector<std::pair<std::string, std::wstring>> textures[kPartSlotCount];
	std::vector<MaterialOverride> overrides[kPartSlotCount];
};

// A generated DLC content package: one custom part in DLC slot `item` (game part id 180+item).
struct PartPackage {
	std::string setId;
	int slot = 0;             // PartSlot
	int item = 0;             // DLC slot (69: the carrier)
	std::wstring root;        // the package folder (holds Content\nativePC\...)
	std::wstring displayName;
	uint8_t contentId[20] = {};
	std::string lpcPath;      // carrier route: model path (no extension) in the slot archives, model\hm\lpc\...
	std::vector<int> colours; // the set's colours (0-3); the first is worn when the code's isn't one
	uint8_t modelVariants = 0; // bit c: colour c has its own model copy (<lpcPath>_<letter>)
	std::string name;         // the set's menu name
	std::string carrierToken; // carrier builds: the set's 6-character stand-in for the slot's part names
	int faction = -1;         // the set's faction byte, -1 = every faction
	bool carrier = false;     // the carrier slot's registered package (the worn set's files are swapped in)
	std::string setHash;      // carrier builds: the set's content hash
	bool remote = false;      // a set downloaded from another player: worn by them only, never in our menu
};

// Builds (once; cached in ModCache\parts\pkg\) a DLC package per custom part, cloning the
// template part's archives with the exported model in place, and returns them for the content
// scan (api/xcontent.cpp).
const std::vector<PartPackage>& PartPackages();

// The generated colour archives B-D (<pfx>0e/2e<nn>01..03.arc) at the game's own
// nativePC\arcXmlTmpPC\CustomParts\ for the overlay index: outside the parts menu the game opens
// a DLC part's colour archive there instead of in its package (colour A is found either way).
void PartsIndex(std::unordered_map<std::wstring, std::wstring>* index);

// The carrier slot (DLC item 69, one per category) wears any custom set: opens of the registered
// carrier package's part archives go to the worn set's carrier build instead. path is a full path
// (as opened, or as the overlay index maps it); true with *out set when it is swapped.
bool PartsCarrierRedirect(const std::wstring& path, std::wstring* out);

// Hooks the part path builder (DX11 FUN_00f664b0, DX9 FUN_00f56650) so generated parts load their
// lpc resources from the slot archives; returns true once hooked.
bool PartsTryInstallPathHook(unsigned char* base);

// Keeps the generated parts unlocked in the player's profile (a fresh save has DLC ids locked).
void PartsStartUnlocker();

// Scans Parts\ (once; thread-safe): <name>.zip or folders, each holding lpc.json and the models
// the Blender exporter wrote (body.rModel, head.rModel, back.rModel, legs.rModel).
void PartsLoad();
const std::vector<PartSet>& PartSets();

}
