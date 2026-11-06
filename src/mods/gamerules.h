// Game types, their built-in defaults and the rule files (ModCache\gamerules). See gamerules.cpp.
#pragma once

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace xls::mods {

struct GameType {
	const char* id;
	int main;
	int sub;
	bool survival;
};
const std::vector<GameType>& GameTypes();
int GameTypeOf(const uint8_t* rules);
// The type's built-in rules, for everything but what carries between types (map, max players, private, players desired).
void GameRulesDefaults(int type, uint8_t* rules);

// Rules with no place in the 0x38 bytes, by name; reset to their defaults with a type's defaults and saved with its files.
std::map<std::string, int>& GameRulesExtras();

struct GameProfile {
	std::string name;
	std::wstring file;
	int type = 0;
	std::map<std::string, int> values;
};
std::vector<GameProfile> GameProfiles();
void GameProfileApply(const GameProfile& profile, uint8_t* rules);
bool GameProfileSave(const uint8_t* rules, const std::string& name); // over a profile of this name and type, if there is one

// What the setup screen last held. False when there's no file yet: the rules stay as they are.
bool GameRulesLoadCurrent(uint8_t* rules, std::string* profile, bool* edited);
void GameRulesSaveCurrent(const uint8_t* rules, const std::string& profile, bool edited);

}
