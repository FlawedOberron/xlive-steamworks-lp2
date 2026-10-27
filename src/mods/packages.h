#pragma once

// Mod packages: map .zip files in the Maps folder (packages.cpp). Parts: parts.cpp.

#include <string>
#include <unordered_map>
#include <vector>

namespace xls::mods {

struct MapPackage {
	std::wstring package; // file name without .zip (downloaded ones: "remote-<hash>")
	std::string name;     // shown in the map picker
	int base = 0;         // online map whose preload list (vehicles, weapons, objects) it uses
	int number = 0;       // map number the package's files were built for
	int size = -1;        // 0 small, 1 medium, 2 large, -1 = the base map's
	int slot = 0;         // online map id it plays as now (13 or 14), 0 = not bound
	std::wstring thumbnail; // unpacked thumbnail.rTexture of its binding (the create-match picture), or empty
	std::string hash;     // FNV-1a 64 of the zip (16 hex): the package's identity between players
	std::wstring zip;     // the zip it was loaded from
	bool remote = false;  // downloaded from another player (ModCache\maps\remote\), never bound by default
	std::vector<std::string> preload; // extra autoArc archives ("uVs50", "uWp63") added to the base map's preload list
};

// Scans the package folders and unpacks what changed (once; thread-safe).
void PackagesLoad();
// The packages bound to the map slots right now (copies; slot set).
std::vector<MapPackage> PackageMaps();
// Whether a package is bound to online map slot (13 / 14) right now.
bool PackagesSlotBound(int slot);
// The create-match map picker: slot 13 holds one of our own packages at a time. Steps the choice
// by step (+1 / -1), or with enter picks the first (step > 0) or last one; false when there is no
// package that way (the picker then moves on to the stock maps).
bool PackagesPick(int step, bool enter);
// The name of the package bound to slot 13 / 14 (stays valid), or nullptr.
const char* PackagesSlotName(int slot);
// The name for slot in a "13=<name>\n14=<name>" value (another player's published map names), or "".
std::string PackagesNameIn(const std::string& value, int slot);
// The bound package's file for rel (lower game-relative path, "nativepc\..."), if it has one.
bool PackagesMapFile(const std::wstring& rel, std::wstring* out);
// A generated replacement for the shared game file rel (lower game-relative path), built from
// source (the file the game would otherwise open), or nullptr when nothing needs changing.
const std::wstring* PackagesGenerated(const std::wstring& rel, const std::wstring& source);

}
