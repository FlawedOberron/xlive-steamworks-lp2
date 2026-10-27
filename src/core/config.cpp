#include "core/config.h"

#include "core/json.h"
#include "core/spa.h"
#include "core/utils.h"

#include <ctype.h>
#include <stdio.h>

#include <mutex>

namespace xls {

namespace {

// Old "mods" keys whose features moved into plugin mods, reported once the log is open.
std::vector<std::string> g_movedSettings;

Config g_config;
std::wstring g_configPath;

LogLevel ParseLogLevel(const std::string& text, LogLevel fallback)
{
	if (text == "off") return LogLevel::Off;
	if (text == "error") return LogLevel::Error;
	if (text == "warn" || text == "warning") return LogLevel::Warn;
	if (text == "info") return LogLevel::Info;
	if (text == "debug") return LogLevel::Debug;
	if (text == "trace") return LogLevel::Trace;
	return fallback;
}

uint32_t KeyToId(const std::string& key)
{
	return (uint32_t)strtoul(key.c_str(), nullptr, 0);
}

// --- Text edits of the configuration file (comments and layout kept). Positions are byte offsets.
size_t SkipBlank(const std::string& t, size_t at)
{
	while (at < t.size()) {
		if (isspace((unsigned char)t[at])) {
			at++;
		}
		else if (t.compare(at, 2, "//") == 0) {
			at = t.find('\n', at);
			at = at == std::string::npos ? t.size() : at + 1;
		}
		else if (t.compare(at, 2, "/*") == 0) {
			at = t.find("*/", at + 2);
			at = at == std::string::npos ? t.size() : at + 2;
		}
		else {
			break;
		}
	}
	return at;
}

size_t SkipString(const std::string& t, size_t at) // at = the opening quote
{
	for (at++; at < t.size(); at++) {
		if (t[at] == '\\') {
			at++;
		}
		else if (t[at] == '"') {
			return at + 1;
		}
	}
	return t.size();
}

size_t SkipValue(const std::string& t, size_t at)
{
	if (at >= t.size()) {
		return at;
	}
	if (t[at] == '"') {
		return SkipString(t, at);
	}
	if (t[at] == '{' || t[at] == '[') {
		int depth = 0;
		while (at < t.size()) {
			at = SkipBlank(t, at);
			if (at >= t.size()) {
				break;
			}
			char c = t[at];
			if (c == '"') {
				at = SkipString(t, at);
				continue;
			}
			at++;
			if (c == '{' || c == '[') {
				depth++;
			}
			else if ((c == '}' || c == ']') && --depth == 0) {
				return at;
			}
		}
		return t.size();
	}
	while (at < t.size() && t[at] != ',' && t[at] != '}' && t[at] != ']' && !isspace((unsigned char)t[at]) && t.compare(at, 2, "//") != 0) {
		at++;
	}
	return at;
}

// The object whose '{' is at open: where key's value starts and ends (npos when missing), where its
// last member's value ends (npos when empty) and where its '}' is.
struct ObjectSpot {
	size_t valueBegin = std::string::npos, valueEnd = std::string::npos, lastEnd = std::string::npos, close = std::string::npos;
	std::string indent = "\t";
};

ObjectSpot FindMember(const std::string& t, size_t open, const std::string& key)
{
	ObjectSpot spot;
	size_t at = open + 1;
	bool first = true;
	for (;;) {
		at = SkipBlank(t, at);
		if (at >= t.size()) {
			return spot;
		}
		if (t[at] == '}') {
			spot.close = at;
			return spot;
		}
		if (t[at] != '"') {
			return spot;
		}
		if (first) {
			size_t line = t.rfind('\n', at);
			line = line == std::string::npos ? 0 : line + 1;
			if (line > open && t.find_first_not_of(" \t", line) == at) {
				spot.indent = t.substr(line, at - line);
			}
			first = false;
		}
		size_t keyEnd = SkipString(t, at);
		std::string name = t.substr(at + 1, keyEnd - at - 2);
		at = SkipBlank(t, keyEnd);
		if (at >= t.size() || t[at] != ':') {
			return spot;
		}
		size_t begin = SkipBlank(t, at + 1), end = SkipValue(t, begin);
		if (name == key && spot.valueBegin == std::string::npos) {
			spot.valueBegin = begin;
			spot.valueEnd = end;
		}
		spot.lastEnd = end;
		at = SkipBlank(t, end);
		if (at < t.size() && t[at] == ',') {
			at++;
		}
	}
}

// Sets section.key to the JSON text value in the file text (adding the key / section as needed).
bool SetSetting(std::string* text, const std::string& section, const std::string& key, const std::string& value)
{
	std::string& t = *text;
	size_t root = SkipBlank(t, 0);
	if (root >= t.size() || t[root] != '{') {
		return false;
	}
	ObjectSpot top = FindMember(t, root, section);
	if (top.close == std::string::npos && top.valueBegin == std::string::npos) {
		return false;
	}
	if (top.valueBegin == std::string::npos) {
		std::string add = "\"" + section + "\": {\n" + top.indent + top.indent + "\"" + key + "\": " + value + "\n" + top.indent + "}";
		if (top.lastEnd == std::string::npos) {
			t.insert(top.close, "\n" + top.indent + add + "\n");
		}
		else {
			t.insert(top.lastEnd, ",\n" + top.indent + add);
		}
		return true;
	}
	if (t[top.valueBegin] != '{') {
		t.replace(top.valueBegin, top.valueEnd - top.valueBegin, "{\"" + key + "\": " + value + "}");
		return true;
	}
	ObjectSpot sec = FindMember(t, top.valueBegin, key);
	if (sec.valueBegin != std::string::npos) {
		t.replace(sec.valueBegin, sec.valueEnd - sec.valueBegin, value);
	}
	else if (sec.lastEnd != std::string::npos) {
		t.insert(sec.lastEnd, ",\n" + (sec.indent == "\t" ? top.indent + top.indent : sec.indent) + "\"" + key + "\": " + value);
	}
	else if (sec.close != std::string::npos) {
		t.insert(sec.close, "\"" + key + "\": " + value);
	}
	else {
		return false;
	}
	return true;
}

std::wstring PathSetting(const Json& value, const std::wstring& fallback)
{
	std::string text = value.GetString(std::string());
	if (text.empty()) {
		return fallback;
	}
	std::wstring wide = Utf8ToWide(text.c_str());
	// A relative path is relative to the exe.
	if (wide.size() < 2 || (wide[1] != L':' && wide[0] != L'\\')) {
		wide = ExeDirectory() + wide;
	}
	if (!wide.empty() && wide.back() != L'\\' && wide.back() != L'/') {
		wide.push_back(L'\\');
	}
	return wide;
}

void LoadLeaderboards(const Json& node, Config& config)
{
	for (const auto& item : node.Items()) {
		LeaderboardMapping mapping;
		mapping.viewId = KeyToId(item.first);
		const Json& entry = item.second;
		if (entry.IsString()) {
			mapping.name = entry.GetString(std::string());
		}
		else {
			mapping.name = entry["name"].GetString(std::string());
			mapping.ratingColumn = (int32_t)entry["rating_column"].GetInt(-1);
			for (const Json& column : entry["detail_columns"].Elements()) {
				mapping.detailColumns.push_back((uint16_t)column.GetInt(0));
			}
			mapping.keepBest = entry["keep_best"].GetBool(true);
			mapping.createIfMissing = entry["create_if_missing"].GetBool(config.leaderboardsCreateIfMissing);
			mapping.ascending = entry["ascending"].GetBool(false);
			mapping.displayType = (int)entry["display_type"].GetInt(1);
		}
		if (mapping.name.empty()) {
			mapping.name = FormatA(config.leaderboardNameFormat.c_str(), mapping.viewId);
		}
		config.leaderboards[mapping.viewId] = mapping;
	}
}

void LoadDlc(const Json& node, Config& config)
{
	for (const Json& entry : node.Elements()) {
		DlcMapping dlc;
		dlc.appId = entry["app_id"].GetUint32(0);
		std::string contentId = entry["content_id"].GetString(std::string());
		if (!contentId.empty() && !HexDecode(contentId, dlc.contentId, sizeof(dlc.contentId))) {
			XLS_LOG_WARN("config: dlc app %u has a content_id that is not 40 hex digits, a hash of the app id is used instead.", dlc.appId);
			contentId.clear();
		}
		if (contentId.empty()) {
			uint64_t hash = Fnv1a64(&dlc.appId, sizeof(dlc.appId));
			memcpy(dlc.contentId, &hash, sizeof(hash));
			uint32_t hash32 = Fnv1a32(&hash, sizeof(hash));
			memcpy(dlc.contentId + 8, &hash32, sizeof(hash32));
			memcpy(dlc.contentId + 12, &dlc.appId, sizeof(dlc.appId));
		}
		dlc.contentType = entry["content_type"].GetUint32(2);
		dlc.offerId = (uint64_t)entry["offer_id"].GetInt((int64_t)dlc.appId);
		dlc.displayName = Utf8ToWide(entry["display_name"].GetString(std::string()).c_str());
		dlc.path = PathSetting(entry["path"], std::wstring());
		dlc.licensePath = PathSetting(entry["license_path"], std::wstring());
		dlc.licenseMask = entry["license_mask"].GetUint32(0xFFFFFFFF);
		config.dlc.push_back(dlc);
	}
}

void LoadTitleServers(const Json& node, Config& config)
{
	for (const Json& entry : node.Elements()) {
		TitleServerConfig server;
		server.name = entry["name"].GetString(std::string());
		server.ip = entry["ip"].GetString(std::string());
		server.port = (uint16_t)entry["port"].GetInt(0);
		server.serviceId = entry["service_id"].GetUint32(0);
		server.info = entry["info"].GetString(server.name);
		if (!server.ip.empty()) {
			config.titleServers.push_back(server);
		}
	}
}

}

void ConfigEnsureLoaded()
{
	static std::once_flag once;
	std::call_once(once, [] { ConfigLoad(); });
}

bool ConfigLoad()
{
	Config config;
	std::wstring moduleDir = ModuleDirectory();
	std::wstring exeDir = ExeDirectory();
	config.logPath = moduleDir + L"xlive_steamworks.log";
	config.titleStorageDir = exeDir + L"xlive-title-storage\\";
	config.localStorageDir = exeDir + L"xlive-storage\\";
	config.dlcRootDir = exeDir + L"DLC\\";
	config.modDir = exeDir + L"Mods\\";
	config.mapsDir = exeDir + L"Maps\\";
	config.partsDir = exeDir + L"Parts\\";
	config.modCacheDir = exeDir + L"ModCache\\";

	g_configPath = moduleDir + L"xlive_steamworks.json";
	if (!FileExists(g_configPath)) {
		g_configPath = exeDir + L"xlive_steamworks.json";
	}

	std::vector<uint8_t> bytes;
	bool loaded = false;
	bool legacyLeaderboards = false;
	if (ReadFileBytes(g_configPath, bytes)) {
		std::string text(bytes.begin(), bytes.end());
		Json root;
		std::string error;
		if (Json::Parse(text, root, &error)) {
			loaded = true;

			const Json& log = root["log"];
			config.logLevel = ParseLogLevel(log["level"].GetString("warn"), LogLevel::Warn);
			config.logPath = PathSetting(log["path"], config.logPath);
			if (log["path"].IsString()) {
				// PathSetting appends a separator for directories, the log is a file.
				if (!config.logPath.empty() && config.logPath.back() == L'\\') {
					config.logPath.pop_back();
				}
			}
			config.logToDebugger = log["debugger"].GetBool(false);

			const Json& steam = root["steam"];
			config.appId = steam["app_id"].GetUint32(0);
			config.restartAppIfNecessary = steam["restart_app_if_necessary"].GetBool(false);
			config.requireSteam = steam["required"].GetBool(true);
			config.steamShutdownApi = steam["shutdown_api"].GetBool(true);
			config.asyncTimeoutMs = steam["async_timeout_ms"].GetUint32(15000);

			const Json& title = root["title"];
			config.titleId = title["title_id"].GetUint32(0);
			config.titleVersion = title["title_version"].GetUint32(0);
			config.language = (uint8_t)title["language"].GetInt(0);

			const Json& users = root["users"];
			config.asciiGamertags = users["ascii_gamertags"].GetBool(true);
			config.localUsersAllSignedIn = users["extra_local_users_signed_in"].GetBool(false);

			const Json& achievements = root["achievements"];
			config.achievementNameFormat = achievements["name_format"].GetString("ACH_%u");
			config.achievementsFromSteamWhenNoSpa = achievements["from_steam_when_no_spa"].GetBool(true);
			config.achievementsLocalFallback = achievements["local_fallback"].GetBool(true);
			for (const auto& item : achievements["map"].Items()) {
				config.achievements[KeyToId(item.first)] = item.second.GetString(std::string());
			}

			const Json& leaderboards = root["leaderboards"];
			config.leaderboardNameFormat = leaderboards["name_format"].GetString("LB_%u");
			config.leaderboardsCreateIfMissing = leaderboards["create_if_missing"].GetBool(true);
			LoadLeaderboards(leaderboards["map"], config);
			// the json shipped up to test-2026-10-23 mapped every view to LB_<id> ranked by column 28 (a hidden
			// column, not the Score) with create_if_missing off: self-update keeps that json, so set it aside here
			auto legacy = [&config](uint32_t id) {
				auto it = config.leaderboards.find(id);
				return it != config.leaderboards.end() && it->second.name == FormatA("LB_%u", id) && it->second.ratingColumn == 28;
			};
			bool allLegacy = !config.leaderboards.empty();
			for (const auto& item : config.leaderboards) {
				allLegacy = allLegacy && legacy(item.first);
			}
			if (allLegacy) {
				config.leaderboards.clear();
				config.leaderboardsCreateIfMissing = true;
				legacyLeaderboards = true;
			}

			for (const auto& item : root["stats"].Items()) {
				config.stats[item.first] = item.second.GetString(std::string());
			}

			const Json& sessions = root["sessions"];
			config.lobbyKeyPrefix = sessions["lobby_key_prefix"].GetString("xl_");
			config.lobbyDistanceFilter = (int)sessions["lobby_distance_filter"].GetInt(1);
			config.sessionsAcceptAnyPeer = sessions["accept_any_peer"].GetBool(true);
			config.startWaitSeconds = (uint32_t)sessions["start_wait_seconds"].GetInt(180);
			config.relayOnly = root["network"]["relay_only"].GetBool(false);
			config.sendRateKBytes = (uint32_t)root["network"]["send_rate_kbytes"].GetInt(0);

			const Json& presence = root["presence"];
			config.richPresenceKey = presence["key"].GetString("status");
			config.richPresenceSteamDisplay = presence["steam_display"].GetBool(true);
			config.updateAuto = root["update"]["auto"].GetBool(true);
			config.discordEnabled = root["discord"]["enabled"].GetBool(true);
			config.discordAppId = root["discord"]["app_id"].GetString(config.discordAppId);

			const Json& storage = root["storage"];
			config.cloudEnabled = storage["cloud"].GetBool(true);
			config.titleStorageDir = PathSetting(storage["title_dir"], config.titleStorageDir);
			config.localStorageDir = PathSetting(storage["local_dir"], config.localStorageDir);

			const Json& content = root["content"];
			config.dlcAutoDiscover = content["auto_discover"].GetBool(true);
			config.dlcRootDir = PathSetting(content["dlc_dir"], config.dlcRootDir);
			LoadDlc(content["dlc"], config);

			LoadTitleServers(root["title_servers"], config);

			config.voiceEnabled = root["voice"]["enabled"].GetBool(true);
			config.nativeDialogs = root["ui"]["native_dialogs"].GetBool(true);
			config.homeKeyOpensOverlay = root["ui"]["home_key_opens_overlay"].GetBool(true);
			for (const char* moved : {"skip_splash", "hold_zoom", "controls", "solo_start", "game_log"}) {
				if (root["mods"][moved].IsObject() || root["mods"][moved].GetBool(false)) {
					g_movedSettings.push_back(moved);
				}
			}
			config.modDir = PathSetting(root["mods"]["dir"], config.modDir);
			config.mapsDir = PathSetting(root["mods"]["maps_dir"], config.mapsDir);
			config.partsDir = PathSetting(root["mods"]["parts_dir"], config.partsDir);
			config.partsAnyFaction = root["mods"]["parts_any_faction"].GetBool(false);
			config.partsShare = root["mods"]["share_parts"].GetBool(true);
			config.partsReceive = root["mods"]["receive_parts"].GetBool(true);
			config.rulesUnlimitedAmmo = root["mods"]["rules"]["unlimited_ammo"].GetBool(false);
			config.rulesUnlimitedGrenades = root["mods"]["rules"]["unlimited_grenades"].GetBool(false);
			config.rulesSecondary = root["mods"]["rules"]["secondary"].GetString(std::string());
			config.modCacheDir = PathSetting(root["mods"]["cache_dir"], config.modCacheDir);
			for (const Json& name : root["mods"]["disabled"].Elements()) {
				if (name.IsString() && !name.GetString(std::string()).empty()) {
					config.modsDisabled.push_back(Utf8ToWide(name.GetString(std::string()).c_str()));
				}
			}
			config.modTraceOpens = (int)root["mods"]["trace_opens"].GetInt(0);
			for (const Json& name : root["mods"]["enabled"].Elements()) {
				if (name.IsString() && !name.GetString(std::string()).empty()) {
					config.modsEnabled.push_back(Utf8ToWide(name.GetString(std::string()).c_str()));
				}
			}
		}
		else {
			g_config = config;
			LogInit(config.logPath, LogLevel::Warn, false);
			XLS_LOG_ERROR("config: %ls failed to parse: %s. Defaults are in use.", g_configPath.c_str(), error.c_str());
			return false;
		}
	}

	g_config = config;
	LogInit(config.logPath, config.logLevel, config.logToDebugger);
	for (const std::string& moved : g_movedSettings) {
		XLS_LOG_WARN("config: mods.%s is now a plugin mod (Mods\\SkipSplash, Controls or DevTools); the setting is ignored.", moved.c_str());
	}
	if (loaded) {
		XLS_LOG_INFO("config: loaded %ls.", g_configPath.c_str());
	}
	if (legacyLeaderboards) {
		XLS_LOG_INFO("config: the old leaderboards map (LB_<view>) is ignored; boards use the game's view names and Score.");
	}
	else {
		XLS_LOG_INFO("config: no xlive_steamworks.json found, defaults are in use.");
	}
	return true;
}

const Config& Cfg()
{
	return g_config;
}

const std::wstring& ConfigPath()
{
	return g_configPath;
}

std::string AchievementApiName(uint32_t achievementId)
{
	std::string override;
	if (ExtAchievementName(achievementId, &override)) {
		return override;
	}
	auto it = g_config.achievements.find(achievementId);
	if (it != g_config.achievements.end() && !it->second.empty()) {
		return it->second;
	}
	return FormatA(g_config.achievementNameFormat.c_str(), achievementId);
}

const LeaderboardMapping* LeaderboardFor(uint32_t viewId)
{
	auto it = g_config.leaderboards.find(viewId);
	if (it != g_config.leaderboards.end()) {
		return &it->second;
	}
	return nullptr;
}

std::string LeaderboardName(uint32_t viewId)
{
	std::string override;
	if (ExtLeaderboardName(viewId, &override)) {
		return override;
	}
	const LeaderboardMapping* mapping = LeaderboardFor(viewId);
	if (mapping && !mapping->name.empty()) {
		return mapping->name;
	}
	// default format: the SPA view's own name ("RANK_TRIAL_A_00_00") where it has one
	if (g_config.leaderboardNameFormat == "LB_%u") {
		const spa::StatView* view = spa::FindStatView(viewId);
		if (view && !view->name.empty() && view->name.find(L' ') == std::wstring::npos) {
			return WideToUtf8(view->name.c_str());
		}
	}
	return FormatA(g_config.leaderboardNameFormat.c_str(), viewId);
}

const std::string* StatNameFor(uint32_t viewId, uint16_t columnId)
{
	std::string key = FormatA("%u:%u", viewId, columnId);
	auto it = g_config.stats.find(key);
	if (it != g_config.stats.end() && !it->second.empty()) {
		return &it->second;
	}
	return nullptr;
}

void ConfigSetNetwork(bool share, bool receive, uint32_t sendRateKBytes, bool updateAuto)
{
	g_config.partsShare = share;
	g_config.partsReceive = receive;
	g_config.sendRateKBytes = sendRateKBytes;
	g_config.updateAuto = updateAuto;
	std::vector<uint8_t> bytes;
	std::string text = "{\n}\n";
	if (ReadFileBytes(g_configPath, bytes)) {
		text.assign(bytes.begin(), bytes.end());
	}
	if (!SetSetting(&text, "mods", "share_parts", share ? "true" : "false") || !SetSetting(&text, "mods", "receive_parts", receive ? "true" : "false") ||
	    !SetSetting(&text, "network", "send_rate_kbytes", std::to_string(sendRateKBytes)) ||
	    !SetSetting(&text, "update", "auto", updateAuto ? "true" : "false")) {
		XLS_LOG_WARN("config: couldn't place the network settings in %ls; not saved.", g_configPath.c_str());
		return;
	}
	Json check;
	std::string error;
	if (!Json::Parse(text, check, &error)) {
		XLS_LOG_WARN("config: the network settings edit broke %ls (%s); not saved.", g_configPath.c_str(), error.c_str());
		return;
	}
	if (!WriteFileBytes(g_configPath, text.data(), text.size())) {
		XLS_LOG_WARN("config: couldn't write %ls.", g_configPath.c_str());
		return;
	}
	XLS_LOG_INFO("config: network settings saved (share parts %d, receive parts %d, send rate %u KB/s (0 = auto), auto update %d).", share, receive,
	             sendRateKBytes, updateAuto);
}

}
