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
	for (const char* context : {"foot", "vs"}) {
		for (const auto& remap : root[context].Items()) {
			std::string source = remap.second.GetString(std::string());
			if (!source.empty()) {
				g_settings.remaps.push_back({context, remap.first, source});
			}
		}
	}
}

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
	g_zoomPending = g_settings.holdToAim;
	g_controlsPending = ControlsLoadRules() || g_settings.log;
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
	if ((g_zoomPending || g_controlsPending) && g_host->title_started()) {
		if (g_zoomPending) {
			LOG_WARN("hold to aim: zoom check not found in this game build.");
		}
		if (g_controlsPending) {
			LOG_WARN("pad update call not found in this game build, split controls off.");
		}
		return 1;
	}
	return !g_zoomPending && !g_controlsPending;
}
