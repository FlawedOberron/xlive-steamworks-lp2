// Mod packages: map .zip files in the Maps folder (packages.cpp). Parts come later.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace xls::mods {

struct MapPackage {
	std::wstring package; // file name without .zip
	std::string name;     // shown in the map picker
	int base = 0;         // online map whose preload list (vehicles, weapons, objects) it uses
	int number = 0;       // map number the package's files were built for
	int size = -1;        // 0 small, 1 medium, 2 large, -1 = the base map's
	int slot = 0;         // online map id it plays as (13 or 14)
};

// Scans the package folders and unpacks what changed (once; thread-safe).
void PackagesLoad();
const std::vector<MapPackage>& PackageMaps();
// Adds the unpacked files to the overlay index (lower game-relative path -> file).
void PackagesIndex(std::unordered_map<std::wstring, std::wstring>* index);
// A generated replacement for the shared game file rel (lower game-relative path), built from
// source (the file the game would otherwise open), or nullptr when nothing needs changing.
const std::wstring* PackagesGenerated(const std::wstring& rel, const std::wstring& source);

}
