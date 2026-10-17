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
#include <vector>

namespace xls::mods {

enum PartSlot { kPartHead, kPartBody, kPartBack, kPartLegs, kPartSlotCount };

struct PartSet {
	std::string id;           // LPC id, unique ("scav01")
	std::string name;         // menu name (UTF-8, up to 43 bytes)
	std::string faction;      // RND FJ SPE NEV FF ALL
	int factionIndex = -1;    // the game's faction byte, -1 = every faction
	std::wstring source;      // the zip or folder it came from
	std::wstring models[kPartSlotCount]; // unpacked .rModel per slot, empty = set has no such part
	std::string templates[kPartSlotCount]; // original part each was built on ("bd2900")
	int colour = 0;           // which of the template's colour textures (A-D) the part wears
	// Custom textures per slot ("BM_A".."BM_D", "NM", "MM" -> .rTexture file), replacing the template's.
	std::vector<std::pair<std::string, std::wstring>> textures[kPartSlotCount];
};

// A generated DLC content package: one custom part in DLC slot `item` (game part id 180+item).
struct PartPackage {
	std::string setId;
	int slot = 0;             // PartSlot
	int item = 0;             // DLC slot 20-69 (listed one by one in the parts menus)
	std::wstring root;        // the package folder (holds Content\nativePC\...)
	std::wstring displayName;
	uint8_t contentId[20] = {};
};

// Builds (once; cached in ModCache\parts\pkg\) a DLC package per custom part, cloning the
// template part's archives with the exported model in place, and returns them for the content
// scan (api/xcontent.cpp).
const std::vector<PartPackage>& PartPackages();

// Keeps the generated parts unlocked in the player's profile (a fresh save has DLC ids locked).
void PartsStartUnlocker();

// Scans Parts\ (once; thread-safe): <name>.zip or folders, each holding lpc.json and the models
// the Blender exporter wrote (body.rModel, head.rModel, back.rModel, legs.rModel).
void PartsLoad();
const std::vector<PartSet>& PartSets();

}
