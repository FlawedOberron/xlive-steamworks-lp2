#include "mods/lp2.h"
#include "mods/flash/botmatch.h"
#include "mods/parts.h"

#include "core/log.h"

#include <windows.h>

#include <stdint.h>
#include <iterator>
#include <vector>

// Bot Match: a fifth entry on the Multiplayer screen (flash\online\id200, uIdBattle under aBattle) that
// opens AI Enemy Battle (aTrainingTop) with all its bot rows, as the main menu's Training entry does.
//
// The screen's input function (DX11 FUN_00e80320) wraps its index at 0..3 and maps it through a four-entry
// table when an entry is chosen, behind a network check. So: index 4 is ours. From 4 the game's own Up and
// Down already land on 3 and 0; we turn its 3 -> 0 and 0 -> 3 wraps into 4, and choose entry 4 ourselves
// with the value kChoice, which aBattle::vf7 would send to the main menu and we send to aTrainingTop.
// The mouse goes through flash/botmatch.as (events 992, 993; FlashCallback in netmenu.cpp).

namespace xls::mods {

namespace {

struct BotMatchVas {
	uint32_t input;     // uIdBattle's input, thiscall(screen)
	uint32_t scene;     // aBattle::vf7, thiscall(scene)
	uint32_t pad;       // the pad global (+0x320 trigger bits, +0x330 repeat bits, +0x78 sound arg)
	uint32_t soundObj;  // the sound object global
	uint32_t sound;     // its "play menu sound", thiscall(obj, pad[+0x78], id, screen, 0)
	uint32_t leave;     // what aBattle's way back to the main menu runs first (FUN_00ff8c10)
	uint32_t net;       // the global whose +0x5a6 the main menu sets before Training
	uint32_t sceneMgr;  // the scene manager global (vtable +0x24: switch scene to a DTI)
	uint32_t training;  // aTrainingTop::DTI
	uint32_t flashCall; // a menu's Flash call: thiscall(menu, 0, name, args, count)
};
const BotMatchVas kBotMatchVas[kBuildCount] = {
	{0xe80320, 0x899c20, 0x18e0860, 0x185289c, 0x832f00, 0xff8c10, 0x1852168, 0x18d0cd4, 0x18552b4, 0xf4a390}, // DX11
	{0xe70360, 0x88a5c0, 0x18ce774, 0x18427fc, 0x824110, 0xfe8870, 0x18420c8, 0x18c0c34, 0x1845214, 0xf3a660}, // DX9
	{0xe80750, 0x899ff0, 0x18e1860, 0x185389c, 0x833130, 0xff8f40, 0x1853168, 0x18d1cd4, 0x18562b4, 0xf4a7f0}, // DX11 1.1
	{0xe706b0, 0x88a7e0, 0x18ce774, 0x18427fc, 0x8241c0, 0xfe8b50, 0x18420c8, 0x18c0c34, 0x1845214, 0xf3a730}, // DX9 1.1
};
// push ebx; push esi; mov esi,ecx; xor ebx,ebx
const uint8_t kInputPrologue[] = {0x53, 0x56, 0x8b, 0xf1, 0x33, 0xdb};
// push esi; mov esi,ecx; mov eax,[esi+4]
const uint8_t kScenePrologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x04};

const int kEntry = 4;   // our index on the screen
const int kChoice = 6;  // the value we choose, past the game's 2-5
const int kFirstMouseId = 500; // the game's four entries' mouse ids
const int kMouseDecide = 2;    // the on-screen Decide button's mouse id

// uIdBattle
const int kIndex = 0x1a0, kInputOn = 0x1b1, kSubState = 0x1a4, kState = 0x18, kMouseId = 0x154, kMouseBits = 0x15c;
const int kHelpMsg = 0x1ac, kResult = 0x19c, kResultKind = 0x194;

struct NumberArg {
	uint8_t flag;
	uint32_t type; // 3 number
	float value;
};

typedef void(__attribute__((thiscall)) * ScreenFn)(uint8_t*);
typedef void(__attribute__((thiscall)) * SoundFn)(void*, uint32_t, int, void*, int);
typedef bool(__attribute__((thiscall)) * FlashCallFn)(uint8_t*, int, const char*, const void*, int);
typedef void(__attribute__((thiscall)) * SwitchFn)(void*, void*);
typedef void (*LeaveFn)();

const BotMatchVas* g_vas = nullptr;
uint8_t* g_base = nullptr;
ScreenFn g_input = nullptr;
ScreenFn g_scene = nullptr;
bool g_hasEntry = false; // the Flash script added the row (never index 4 without it)

uint8_t* At(uint32_t va)
{
	return g_base + (va - 0x400000);
}

int32_t& Int(uint8_t* p, int offset)
{
	return *(int32_t*)(p + offset);
}

uint8_t* Pad()
{
	return *(uint8_t**)At(g_vas->pad);
}

void Sound(uint8_t* screen, int id)
{
	((SoundFn)At(g_vas->sound))(*(void**)At(g_vas->soundObj), *(uint32_t*)(Pad() + 0x78), id, screen, 0);
}

void Select(uint8_t* screen, int index)
{
	NumberArg arg = {0, 3, (float)index};
	((FlashCallFn)At(g_vas->flashCall))(screen, 0, "_root.selectItem", &arg, 1);
}

bool Live(uint8_t* screen)
{
	return screen[kInputOn] && Int(screen, kSubState) == 0 && screen[kState] != 2;
}

void Choose(uint8_t* screen)
{
	XLS_LOG_INFO("mods: bot match: chosen on the Multiplayer screen.");
	Sound(screen, 8);
	screen[kState] = 2;
	screen[kState + 1] = 0;
	screen[kState + 2] = 0;
	screen[kState + 3] = 0;
	Int(screen, kResult) = kChoice;
	Int(screen, kResultKind) = 1;
}

void __attribute__((thiscall)) Input(uint8_t* screen)
{
	const int before = Int(screen, kIndex);
	const bool live = g_hasEntry && Live(screen);
	if (live && before == kEntry) {
		if (*(uint32_t*)(Pad() + 0x320) & 1) {
			Choose(screen);
			return;
		}
		if (screen[kMouseBits] & 1) {
			// a click with the mouse over something else: never let the game read its table at our index
			const int id = Int(screen, kMouseId);
			if (id == kMouseDecide) {
				Choose(screen);
				return;
			}
			if (id >= kFirstMouseId && id < kFirstMouseId + 4) {
				Int(screen, kIndex) = id - kFirstMouseId;
				Select(screen, id - kFirstMouseId);
			}
		}
	}
	g_input(screen);
	if (!live || screen[kState] == 2) {
		return;
	}
	const uint32_t repeat = *(uint32_t*)(Pad() + 0x330);
	int& index = Int(screen, kIndex);
	if ((before == 3 && index == 0 && (repeat & 0x80)) || (before == 0 && index == 3 && (repeat & 0x40))) {
		index = kEntry;
		Select(screen, kEntry);
	}
	if (index == kEntry) {
		Int(screen, kHelpMsg) = -1; // the game's help text is by index; the script shows ours
	}
}

void __attribute__((thiscall)) Scene(uint8_t* scene)
{
	if ((*(uint32_t*)(scene + 4) & 0xff) == 5 && *(int32_t*)(scene + 0x2c) == kChoice) {
		XLS_LOG_INFO("mods: bot match: opening AI Enemy Battle.");
		((LeaveFn)At(g_vas->leave))();
		uint8_t* mgr = *(uint8_t**)At(g_vas->sceneMgr);
		*(int32_t*)(mgr + 0x3868) = 2; // as the main menu: its Training entry
		(*(uint8_t**)At(g_vas->net))[0x5a6] = 1;
		((SwitchFn)(*(void***)mgr)[0x24 / 4])(mgr, At(g_vas->training));
		return;
	}
	g_scene(scene);
}

}

// Log only, for bots in player matches (notes/bot-match-plan.md, section E): the player slot records as this machine
// sees them, once per match at the first spawn. Compare a host and a client.
void BotMatchLogSlots()
{
	static DWORD last = 0;
	const DWORD now = GetTickCount();
	if (!g_vas || (last && now - last < 60000)) {
		return;
	}
	last = now;
	static const uint32_t kSlots[kBuildCount] = {0x1852850, 0x18427b0, 0x1853850, 0x18427b0};
	static const uint32_t kGame[kBuildCount] = {0x18520c0, 0x1842020, 0x18530c0, 0x1842020};
	uint8_t* slots = *(uint8_t**)At(kSlots[g_vas - kBotMatchVas]);
	uint8_t* game = *(uint8_t**)At(kGame[g_vas - kBotMatchVas]);
	if (!slots || !game) {
		return;
	}
	XLS_LOG_INFO("mods: slots: game players %u, others %d, versus %u, category %u.", game[0x254], *(int32_t*)(game + 0x50188), game[0x256], game[599]);
	for (int i = 0; i < 16; i++) {
		const uint8_t* r = slots + 0x148 + i * 0x7c;
		const char* name = (const char*)slots + (i + 3) * 0x7c;
		XLS_LOG_INFO("mods: slots: %2d ai %u name \"%.20s\" %08x %08x %08x %08x", i, r[0x69], name, *(uint32_t*)r, *(uint32_t*)(r + 4), *(uint32_t*)(r + 8), *(uint32_t*)(r + 12));
	}
}

bool BotMatchEvent(uint8_t* screen, int event, int value)
{
	if (event != 992 && event != 993) {
		return false;
	}
	if (event == 992 && value == 1) {
		g_hasEntry = g_input != nullptr;
		XLS_LOG_INFO("mods: bot match: entry added to the Multiplayer screen.");
		return true;
	}
	if (g_hasEntry && Live(screen)) {
		if (event == 993) {
			Choose(screen);
		}
		else {
			Int(screen, kIndex) = kEntry;
			Int(screen, kHelpMsg) = -1;
			Sound(screen, 7);
		}
	}
	return true;
}

bool BotMatchPatchFlash(std::vector<uint8_t>* gfx)
{
	return FlashAddRootScript(gfx, std::vector<uint8_t>(std::begin(kBotMatchScript), std::end(kBotMatchScript)), "Bot Match entry");
}

void BotMatchInstall(unsigned char* base)
{
	if (GameBuild() < 0) {
		return;
	}
	g_vas = &kBotMatchVas[GameBuild()];
	g_base = base;
	g_input = (ScreenFn)Detour(At(g_vas->input), kInputPrologue, sizeof(kInputPrologue), (void*)&Input);
	g_scene = g_input ? (ScreenFn)Detour(At(g_vas->scene), kScenePrologue, sizeof(kScenePrologue), (void*)&Scene) : nullptr;
	if (!g_scene) {
		g_input = nullptr; // (a hooked Input with no entry passes straight through)
		XLS_LOG_WARN("mods: bot match: Multiplayer screen code not recognised, no Bot Match entry.");
		return;
	}
	XLS_LOG_INFO("mods: bot match: Multiplayer screen hooked.");
}

}
