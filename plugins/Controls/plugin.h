// Controls plugin: hold to aim and split on-foot / VS controls (see controls.json).
#pragma once

#include "../../sdk/lp2mod.h"

#include <string>
#include <vector>

namespace controls {

extern const LP2ModHost* g_host;

#define LOG_ERROR(...) ::controls::g_host->log(LP2MOD_LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...) ::controls::g_host->log(LP2MOD_LOG_WARN, __VA_ARGS__)
#define LOG_INFO(...) ::controls::g_host->log(LP2MOD_LOG_INFO, __VA_ARGS__)

struct Remap {
	std::string context;
	std::string dest;
	std::string source;
};

struct Settings {
	bool holdToAim = true;      // zoom while the zoom button is held, instead of toggling
	bool log = false;           // log on-foot / VS context changes and the units seen
	std::vector<Remap> remaps;  // per context: action dest takes the input of action source
	std::vector<int> flyTypes = {0xa8, 0xda}; // VS types (unit id byte 2) that fly
};
extern Settings g_settings;

// True when [base+rva, base+rva+size) lies inside the game image (so a known address can be read).
inline bool InImage(const unsigned char* base, unsigned rva, unsigned size)
{
	const unsigned char* nt = base + *(const int*)(base + 0x3c);
	unsigned sizeOfImage = *(const unsigned*)(nt + 0x50);
	return rva + size <= sizeOfImage;
}

// hold to aim (zoom.cpp): true once the zoom check is hooked.
bool HoldZoomTryInstall(unsigned char* base);
// Split controls (controls.cpp): true if any rule applies; true once the pad update is hooked.
bool ControlsLoadRules();
bool ControlsTryInstall(unsigned char* base);
// Prompts follow the rules (controls.cpp): true once the prompt refresh is hooked.
bool PromptsTryInstall(unsigned char* base);

}
