#include "plugin.h"

#include "core/json.h"

#include <windows.h>

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
	for (const char* context : {"foot", "vs", "fly"}) {
		for (const auto& remap : root[context].Items()) {
			std::string source = remap.second.GetString(std::string());
			if (!source.empty()) {
				g_settings.remaps.push_back({context, remap.first, source});
			}
		}
	}
}

}


void SaveHoldToAim()
{
	std::wstring path = std::wstring(g_host->mod_dir) + L"controls.json";
	std::string text;
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file != INVALID_HANDLE_VALUE) {
		char buffer[4096];
		DWORD read = 0;
		while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
			text.append(buffer, read);
		}
		CloseHandle(file);
	}
	const char* value = g_settings.holdToAim ? "true" : "false";
	size_t key = text.find("\"hold_to_aim\"");
	if (key != std::string::npos) {
		size_t colon = text.find(':', key);
		size_t start = text.find_first_not_of(" \t", colon + 1);
		size_t end = text.find_first_of(",\r\n}", start);
		if (colon == std::string::npos || start == std::string::npos || end == std::string::npos) {
			return;
		}
		text.replace(start, end - start, value);
	}
	else {
		size_t brace = text.find('{');
		if (brace == std::string::npos) {
			text = "{\n}\n";
			brace = 0;
		}
		text.insert(brace + 1, std::string("\n\t\"hold_to_aim\": ") + value + (text.find(':', brace) != std::string::npos ? "," : ""));
	}
	file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		LOG_WARN("menu: could not save controls.json (%lu).", GetLastError());
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
