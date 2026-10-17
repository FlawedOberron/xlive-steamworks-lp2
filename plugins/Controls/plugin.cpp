#include "plugin.h"

#include "core/json.h"

#include <windows.h>

#include <stdio.h>

#include <string>

// Settings: controls.json next to Controls.dll. Defaults when the file is missing:
// hold to aim on, no remaps.

namespace controls {

const LP2ModHost* g_host = nullptr;
Settings g_settings;

namespace {

bool g_zoomPending = false;
bool g_controlsPending = false;
bool g_promptsPending = false;
bool g_menuPending = false;
bool g_hadScoped = false; // controls.json has a "scoped" section (else the wheel zooms by default)

void LoadSettings()
{
	std::wstring path = std::wstring(g_host->mod_dir) + L"controls.json";
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		LOG_INFO("no controls.json, using defaults (hold to aim on).");
		return;
	}
	std::string text;
	char buffer[4096];
	DWORD read = 0;
	while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
		text.append(buffer, read);
	}
	CloseHandle(file);
	xls::Json root;
	std::string error;
	if (!xls::Json::Parse(text, root, &error)) {
		LOG_ERROR("controls.json failed to parse: %s. Using defaults.", error.c_str());
		return;
	}
	g_settings.holdToAim = root["hold_to_aim"].GetBool(true);
	g_settings.log = root["log"].GetBool(false);
	if (root["fly_types"].Elements().size()) {
		g_settings.flyTypes.clear();
		for (const xls::Json& t : root["fly_types"].Elements()) {
			g_settings.flyTypes.push_back((int)t.GetInt(0));
		}
	}
	g_hadScoped = root["scoped"].IsObject();
	for (const char* context : {"foot", "vs", "fly", "scoped"}) {
		for (const auto& remap : root[context].Items()) {
			std::string source = remap.second.GetString(std::string());
			if (!source.empty()) {
				g_settings.remaps.push_back({context, remap.first, source});
			}
		}
	}
}

}


void SaveSettings()
{
	std::string text =
		"{\n"
		"\t// Written by the Controls plugin (the key config menu saves here too). Hand edits are kept\n"
		"\t// as values, comments are rewritten.\n"
		"\t//\n"
		"\t// hold_to_aim: zoom while the zoom button is held; release to unzoom (on foot).\n"
		"\t// foot / vs / fly: an action takes the button of another action, or a key:\n"
		"\t//   \"key:F\", \"key:5\", \"key:F13\", \"key:NUM5\", \"key:MOUSE_X1\", \"key:MOUSE_R\", \"key:0x52\".\n"
		"\t//   A key takes over that key in its context (what the game puts on it there is dropped).\n"
		"\t// Foot actions: jump, grapple, next_weapon, crouch, fire, grenade, reload, action,\n"
		"\t//   quick_turn, zoom, melee, gesture, teng, dodge\n"
		"\t// VS actions: fire, secondary, zoom, get_out, brake, reload, quick_turn, dash, boost\n"
		"\t// Flying VS (inherits vs unless set here): the VS actions plus ascend, descend.\n"
		"\t// Scoped (on foot while zoomed, inherits foot): the foot actions plus zoom_in, zoom_out\n"
		"\t//   (\"key:MOUSE_WUP\" / \"key:MOUSE_WDOWN\" = the mouse wheel).\n"
		"\t// fly_types: VS types (unit id byte 2) that fly. log: log on-foot / VS changes.\n";
	text += std::string("\t\"hold_to_aim\": ") + (g_settings.holdToAim ? "true" : "false") + ",\n";
	for (const char* context : {"foot", "vs", "fly", "scoped"}) {
		text += std::string("\t\"") + context + "\": {";
		bool first = true;
		for (const Remap& r : g_settings.remaps) {
			if (r.context == context) {
				text += std::string(first ? "\n" : ",\n") + "\t\t\"" + r.dest + "\": \"" + r.source + "\"";
				first = false;
			}
		}
		text += first ? "},\n" : "\n\t},\n";
	}
	text += "\t\"fly_types\": [";
	for (size_t i = 0; i < g_settings.flyTypes.size(); i++) {
		char n[16];
		snprintf(n, sizeof(n), "%s%d", i ? ", " : "", g_settings.flyTypes[i]);
		text += n;
	}
	text += std::string("],\n\t\"log\": ") + (g_settings.log ? "true" : "false") + "\n}\n";
	std::wstring path = std::wstring(g_host->mod_dir) + L"controls.json";
	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		LOG_WARN("could not save controls.json (%lu).", GetLastError());
		return;
	}
	DWORD written = 0;
	WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
	CloseHandle(file);
}

}

using namespace controls;

extern "C" __declspec(dllexport) int lp2mod_load(const LP2ModHost* host)
{
	if (!host || host->version < 1 || host->size < sizeof(LP2ModHost)) {
		return 1;
	}
	g_host = host;
	LoadSettings();
	if (!g_hadScoped) {
		g_settings.remaps.push_back({"scoped", "zoom_in", "key:MOUSE_WUP"});
		g_settings.remaps.push_back({"scoped", "zoom_out", "key:MOUSE_WDOWN"});
	}
	g_zoomPending = true; // installed even when off, so the menu can switch it on
	g_menuPending = true;
	bool rules = ControlsLoadRules();
	g_controlsPending = rules || g_settings.log;
	g_promptsPending = rules;
	LOG_INFO("hold to aim %s, %u remap(s).", g_settings.holdToAim ? "on" : "off", (unsigned)g_settings.remaps.size());
	return 0;
}

extern "C" __declspec(dllexport) int lp2mod_poll(void)
{
	if (g_zoomPending && HoldZoomTryInstall(g_host->game_base)) {
		g_zoomPending = false;
	}
	if (g_controlsPending && ControlsTryInstall(g_host->game_base)) {
		g_controlsPending = false;
	}
	if (g_menuPending && MenuTryInstall(g_host->game_base)) {
		g_menuPending = false;
	}
	if (g_promptsPending && PromptsTryInstall(g_host->game_base)) {
		g_promptsPending = false;
	}
	if ((g_zoomPending || g_controlsPending || g_promptsPending || g_menuPending) && g_host->title_started()) {
		if (g_zoomPending) {
			LOG_WARN("hold to aim: zoom check not found in this game build.");
		}
		if (g_controlsPending) {
			LOG_WARN("pad update call not found in this game build, split controls off.");
		}
		if (g_promptsPending) {
			LOG_WARN("prompt refresh not found in this game build, prompts keep the game's keys.");
		}
		if (g_menuPending) {
			LOG_WARN("menu: key config code is not as expected, Controls page left as it is.");
		}
		return 1;
	}
	return !g_zoomPending && !g_controlsPending && !g_promptsPending && !g_menuPending;
}
