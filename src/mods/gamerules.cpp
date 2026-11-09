#include "mods/gamerules.h"
#include "mods/package.h"

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "core/utils.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

// Rule files: the game keeps a match's rules as 0x38 bytes, which are full. These files hold them by name, so new rules
// and custom game types can be added without touching that layout:
//   ModCache\gamerules\current.json          what the setup screen last held
//   ModCache\gamerules\profiles\*.json       saved profiles: {"name", "type", "rules": {name: value}}
// A file lists only what differs from its type's defaults, so it stays readable and later changes to a default reach it.
// Map, max players, private slots and players desired aren't part of a type or profile; only current.json has them.

namespace xls::mods {

namespace {

const int kMain = 0x16, kSub = 0x17, kSurvival = 0x32, kMap = 0x1b;
const std::vector<GameType> kTypes = {
	{"elimination-gauge", 0, 0, false},
	{"elimination-kills", 0, 1, false},
	{"elimination-survival", 0, 1, true},
	{"elimination-points", 0, 2, false},
	{"elimination-random", 0, 3, false},
	{"team-elimination-gauge", 1, 0, false},
	{"team-elimination-kills", 1, 1, false},
	{"team-elimination-survival", 1, 1, true},
	{"team-elimination-points", 1, 2, false},
	{"team-elimination-vs", 1, 3, false},
	{"team-elimination-random", 1, 4, false},
	{"data-post-struggle", 2, 0, false},
	{"data-post-counter", 2, 1, false},
	{"data-post-random", 2, 2, false},
	{"akrid-egg", 3, 0, false},
	{"fox-hunt", 4, 0, false},
	{"numbered-battle", 5, 0, false},
	{"random", 6, 0, false},
};

struct RuleName {
	const char* name;
	int offset;
	int max; // a file's value above this is ignored
};
const RuleName kRuleNames[] = {
	{"time", 0x01, 5}, {"teamChange", 0x02, 2}, {"weapon", 0x03, 6}, {"grenade", 0x04, 7}, {"setWeapon", 0x05, 4},
	{"setGrenade", 0x06, 4}, {"setVs", 0x07, 4}, {"setType", 0x08, 5}, {"kills", 0x09, 5}, {"points", 0x0a, 5},
	{"hunt", 0x0b, 3}, {"dataPosts", 0x0c, 3}, {"eggOrVs", 0x0d, 3}, {"gauge", 0x0e, 4}, {"friendlyFire", 0x0f, 2},
	{"penalty", 0x10, 4}, {"kick", 0x11, 5}, {"numberedBattle", 0x1e, 6}, {"weather", 0x20, 2}, {"halftime", 0x26, 2},
	{"customize", 0x27, 2}, {"ability", 0x28, 2}, {"referee", 0x29, 2}, {"respawn", 0x2a, 2}, {"secondary", 0x2e, 20},
	{"infinite", 0x2f, 3}, {"supply", 0x31, 20}, {"lives", 0x33, 20}, {"harmonizer", 0x34, 1}, {"grappling", 0x35, 5},
	{"stagger", 0x36, 20},
};
const RuleName kCarried[] = {{"map", kMap, 0x14}, {"maxPlayers", 0x12, 16}, {"private", 0x13, 16}, {"playersDesired", 0x15, 16}};

bool ReadText(const std::wstring& path, std::string* out)
{
	Bytes bytes;
	if (!ReadWholeFile(path, &bytes)) {
		return false;
	}
	out->assign(bytes.begin(), bytes.end());
	return true;
}

const RuleName kExtraRules[] = {{"bots", 0, 1}, {"botDifficulty", 1, 3}, {"botBrain", 1, 1}, {"botPersona", 0, 5}, {"botTaunts", 2, 3}, {"radar", 1, 2}, {"radarPlayers", 0, 3}, {"radarVs", 0, 3}, {"anchorRange", 3, 12}, {"anchorSpeed", 2, 11}, {"fallStagger", 0, 3}, {"grenadeCount", 0, 11}, {"grenadePickup", 0, 10}, {"quickResupply", 0, 1}, {"rounds", 0, 13}, {"switchSides", 1, 10}, {"radarTeam", 1, 1}, {"explosionStagger", 0, 6}, {"anchorStagger", 0, 6}, {"meleeStagger", 0, 6}};
std::map<std::string, int> g_extras;

void ExtrasDefault()
{
	for (const RuleName& r : kExtraRules) {
		g_extras[r.name] = r.offset; // here: the default
	}
}

void ExtrasApply(const std::map<std::string, int>& values)
{
	for (const RuleName& r : kExtraRules) {
		auto it = values.find(r.name);
		if (it != values.end() && it->second >= 0 && it->second <= r.max) {
			g_extras[r.name] = it->second;
		}
	}
}

std::wstring Dir()
{
	return Cfg().modCacheDir + L"gamerules\\";
}

bool WriteText(const std::wstring& path, const std::string& text)
{
	CreateDirectoryW(Cfg().modCacheDir.c_str(), nullptr);
	CreateDirectoryW(Dir().c_str(), nullptr);
	CreateDirectoryW((Dir() + L"profiles\\").c_str(), nullptr);
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		XLS_LOG_WARN("mods: game rules: can't write a rule file (error %u).", (unsigned)GetLastError());
		return false;
	}
	DWORD wrote = 0;
	bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
	CloseHandle(f);
	return ok;
}

// The rules that differ from the type's defaults, as JSON members.
std::string RulesText(const uint8_t* rules, int type)
{
	uint8_t defaults[0x38];
	memcpy(defaults, rules, sizeof(defaults));
	std::map<std::string, int> extras = GameRulesExtras();
	GameRulesDefaults(type, defaults);
	GameRulesExtras() = extras; // the defaults reset them: RulesText only wants the bytes' defaults
	std::string out;
	for (const RuleName& r : kRuleNames) {
		if (rules[r.offset] != defaults[r.offset]) {
			out += (out.empty() ? "\n    " : ",\n    ") + Json::Quote(r.name) + ": " + std::to_string(rules[r.offset]);
		}
	}
	for (const RuleName& r : kExtraRules) {
		if (GameRulesExtras()[r.name] != r.offset) {
			out += (out.empty() ? "\n    " : ",\n    ") + Json::Quote(r.name) + ": " + std::to_string(GameRulesExtras()[r.name]);
		}
	}
	return out;
}

void ReadValues(const Json& members, std::map<std::string, int>* out)
{
	for (const auto& item : members.Items()) {
		if (item.second.IsNumber()) {
			(*out)[item.first] = (int)item.second.GetInt(0);
		}
	}
}

void WriteValues(const std::map<std::string, int>& values, const RuleName* names, size_t count, uint8_t* rules)
{
	for (size_t i = 0; i < count; i++) {
		auto it = values.find(names[i].name);
		if (it != values.end() && it->second >= 0 && it->second <= names[i].max) {
			rules[names[i].offset] = (uint8_t)it->second;
		}
	}
}

int TypeById(const std::string& id)
{
	for (size_t i = 0; i < kTypes.size(); i++) {
		if (id == kTypes[i].id) {
			return (int)i;
		}
	}
	return -1;
}

}

std::map<std::string, int>& GameRulesExtras()
{
	if (g_extras.empty()) {
		ExtrasDefault();
	}
	return g_extras;
}

const std::vector<GameType>& GameTypes()
{
	return kTypes;
}

int GameTypeOf(const uint8_t* rules)
{
	for (size_t i = 0; i < kTypes.size(); i++) {
		const GameType& t = kTypes[i];
		if (t.main == rules[kMain] && (t.main >= 3 || t.sub == rules[kSub]) && t.survival == (rules[kSurvival] == 1)) {
			return (int)i;
		}
	}
	return 1;
}

// The game's own defaults (DX11 FUN_008219b0, run when the game type changes), then ours off.
void GameRulesDefaults(int type, uint8_t* r)
{
	ExtrasDefault();
	const GameType& t = kTypes[type];
	r[kMain] = (uint8_t)t.main;
	r[kSub] = (uint8_t)t.sub;
	r[0xe] = 2; r[9] = 3; r[10] = 3; r[0xd] = 0; r[0x22] = 1; r[0xc] = 1; r[0xb] = 2; r[0x1e] = 0;
	r[0x1c] = 3; r[0x1d] = 0x13; r[3] = 0; r[0x23] = 0; r[1] = 0; r[8] = 4; r[0x25] = 1; r[0xf] = 2;
	r[0x27] = 1; r[0x28] = 0; r[4] = 6; r[0x24] = 1; r[5] = 0; r[6] = 0; r[7] = 0; r[0x2a] = 1;
	r[0x19] = 0; r[0x20] = 1; r[2] = 0; r[0x26] = 1; r[0x10] = 2; r[0x11] = 2; r[0x1a] = 7; r[0x18] = 0;
	r[0x2b] = 0; r[0x2c] = 0; r[0x2d] = 0;
	switch (t.main) {
	case 0:
		r[0xe] = 1; r[9] = 1; r[10] = 1; r[0xf] = 1; r[0x26] = 0;
		break;
	case 2:
		if (t.sub == 1) {
			r[0x2a] = 0;
		}
		break;
	case 5:
		r[0x1e] = 3;
		r[0x23] = 1; r[3] = 5;
		break;
	case 6:
		r[0x23] = 1; r[3] = 5;
		break;
	}
	memset(r + 0x2e, 0, 0x38 - 0x2e);
	r[kSurvival] = t.survival ? 1 : 0;
	if (t.main == 1 && t.sub == 3 && (r[kMap] == 2 || r[kMap] == 4 || r[kMap] == 9)) {
		r[kMap] = 0x13; // the game's own rule: VS Annihilator can't use these maps
	}
}

std::vector<GameProfile> GameProfiles()
{
	std::vector<GameProfile> out;
	WIN32_FIND_DATAW find;
	std::wstring dir = Dir() + L"profiles\\";
	HANDLE h = FindFirstFileW((dir + L"*.json").c_str(), &find);
	if (h == INVALID_HANDLE_VALUE) {
		return out;
	}
	do {
		std::string text, error;
		Json json;
		if (!ReadText(dir + find.cFileName, &text) || !Json::Parse(text, json, &error)) {
			XLS_LOG_WARN("mods: game rules: a profile can't be read: %s", error.c_str());
			continue;
		}
		GameProfile p;
		p.file = dir + find.cFileName;
		p.type = TypeById(json["type"].GetString(""));
		p.name = json["name"].GetString("");
		if (p.type < 0 || p.name.empty()) {
			XLS_LOG_WARN("mods: game rules: a profile has no known type or no name: %s", json["name"].GetString("?").c_str());
			continue;
		}
		for (char& c : p.name) {
			c = c == '|' ? '/' : c; // the row list's separator
		}
		ReadValues(json["rules"], &p.values);
		out.push_back(p);
	} while (FindNextFileW(h, &find));
	FindClose(h);
	return out;
}

void GameProfileApply(const GameProfile& profile, uint8_t* rules)
{
	GameRulesDefaults(profile.type, rules);
	WriteValues(profile.values, kRuleNames, sizeof(kRuleNames) / sizeof(kRuleNames[0]), rules);
	ExtrasApply(profile.values);
}

bool GameProfileSave(const uint8_t* rules, const std::string& name)
{
	int type = GameTypeOf(rules);
	std::wstring path;
	for (const GameProfile& p : GameProfiles()) {
		if (p.type == type && p.name == name) {
			path = p.file;
		}
	}
	if (path.empty()) {
		std::wstring slug;
		for (const char* c = kTypes[type].id; *c; c++) {
			slug += (wchar_t)*c;
		}
		slug += L"-";
		for (char c : name) {
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
				slug += (wchar_t)c;
			}
		}
		for (int n = 1; n < 1000 && path.empty(); n++) {
			std::wstring candidate = Dir() + L"profiles\\" + slug + (n > 1 ? L"-" + std::to_wstring(n) : L"") + L".json";
			if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
				path = candidate;
			}
		}
	}
	std::string body = RulesText(rules, type);
	return !path.empty() && WriteText(path, "{\n  \"name\": " + Json::Quote(name) + ",\n  \"type\": " + Json::Quote(kTypes[type].id) + ",\n  \"rules\": {" + body + (body.empty() ? "}" : "\n  }") + "\n}\n");
}

bool GameRulesLoadCurrent(uint8_t* rules, std::string* profile, bool* edited)
{
	std::string text, error;
	Json json;
	if (!ReadText(Dir() + L"current.json", &text)) {
		return false;
	}
	if (!Json::Parse(text, json, &error)) {
		XLS_LOG_WARN("mods: game rules: current.json can't be read: %s", error.c_str());
		return false;
	}
	int type = TypeById(json["type"].GetString(""));
	if (type < 0) {
		XLS_LOG_WARN("mods: game rules: current.json has no known type.");
		return false;
	}
	std::map<std::string, int> values, carried;
	ReadValues(json["rules"], &values);
	ReadValues(json, &carried);
	WriteValues(carried, kCarried, sizeof(kCarried) / sizeof(kCarried[0]), rules);
	GameRulesDefaults(type, rules);
	WriteValues(values, kRuleNames, sizeof(kRuleNames) / sizeof(kRuleNames[0]), rules);
	ExtrasApply(values);
	*profile = json["profile"].GetString("");
	*edited = json["edited"].GetBool(false);
	return true;
}

void GameRulesSaveCurrent(const uint8_t* rules, const std::string& profile, bool edited)
{
	int type = GameTypeOf(rules);
	std::string body = RulesText(rules, type);
	std::string text = "{\n  \"type\": " + Json::Quote(kTypes[type].id) + ",\n  \"profile\": " + Json::Quote(profile) + ",\n  \"edited\": " + (edited ? "true" : "false");
	for (const RuleName& r : kCarried) {
		text += ",\n  " + Json::Quote(r.name) + ": " + std::to_string(rules[r.offset]);
	}
	text += ",\n  \"rules\": {" + body + (body.empty() ? "}" : "\n  }") + "\n}\n";
	WriteText(Dir() + L"current.json", text);
}

}
