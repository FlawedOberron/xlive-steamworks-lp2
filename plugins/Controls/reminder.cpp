#include "plugin.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

// Pause menu key reminder (PDA, flash\pda\id700_PC, Control and VS tabs), keyboard only: the lists
// follow the Key Bindings page (menu.cpp): the same headers, the same order, and the Controls rules'
// keys on the rows they move.
//
// uIdPdaGFx::vf44 (DX11 FUN_00f2eb20, DX9 FUN_00f1ed50) builds the on-foot list and vf43
// (FUN_00f2ef70 / FUN_00f1f1a0) the list of one VS type: 10 rows "<ACT_..> name" from the scroll
// at +0x1bb0, sent with _root.hmsetPad2 / vssetPad2 (10 strings), then the scroll arrows with
// setArrowUD (2 numbers, 0 shown / 2 hidden) while the tab is up (+0x1ba0 1 / 3). With a keyboard
// (message manager +0x484 != 0) and the normal list (+0x1c1d == 0) ours build the rows instead;
// the game's ACT tags become the bound keys through its tag pass (FUN_00849730 / FUN_0083a2e0).
// The Control page (OPTIONS > Control, the on-foot list) shows the ridden VS's rows while in a VS
// (+0x1c28: that VS's type, set when the PDA opens); the VS page keeps its own VS choice.
// Row counts: the on-foot scroll input has 0x1b (27 rows) in three places, set to ours; the VS input
// asks FUN_00f2d3d0 / FUN_00f1d600 (the type's row count), hooked.

namespace controls {

namespace {

struct ReminderAddrs {
	uint32_t foot, vs, vsCount;
	uint32_t footCaps[3]; // cmp reg, 0x1b
	uint32_t vsMessages;  // per VS type x 21 slots: MsgOption id - 0x86, 0x49 = none (keyboard = control type 0)
	uint32_t message;     // thiscall(manager, "file", id, 0x16)
	uint32_t msgMgr;
	uint32_t tags;        // thiscall(manager, text, index, 0): ACT tag index -> the bound key
	uint32_t invoke;      // thiscall(screen, result*, "path", args, count)
	uint32_t valueFree;
};
const ReminderAddrs kDx11 = {0xf2eb20, 0xf2ef70, 0xf2d3d0, {0xf330dc, 0xf334dc, 0xf33699}, 0x180f100,
                             0x77b280, 0x18e0860, 0x849730, 0xf4a390, 0x77a680};
const ReminderAddrs kDx9 = {0xf1ed50, 0xf1f1a0, 0xf1d600, {0xf2330c, 0xf2370c, 0xf238c9}, 0x17ff0a0,
                            0x76c0c0, 0x18ce774, 0x83a2e0, 0xf3a660, 0x76b4e0};
const ReminderAddrs* A = &kDx11;

const uint8_t kFootPrologue[] = {0x81, 0xec, 0xa4, 0x0a, 0x00, 0x00}; // sub esp, 0xaa4
const uint8_t kVsPrologue[] = {0xb8, 0x04, 0x20, 0x00, 0x00};        // mov eax, 0x2004 (stack probe follows)
const uint8_t kCountPrologue[] = {0x8b, 0x81, 0x90, 0x1b, 0x00, 0x00}; // mov eax, [ecx+0x1b90]

// A row: a header, one of the game's actions (ACT tag + message), or a Controls rule.
enum Kind { kHeader, kGame, kRule };
struct Line {
	Kind kind;
	int value;        // header index / message id / VS slot
	const char* tag;  // game: ACT tag; rule: context
	const char* text; // rule: dest
	const char* label;
};

// On foot: the game's 27 rows (tag, MsgOption id - 0x86; 0x3a..0x41 are MsgPC id - 0x28) regrouped.
const Line kFoot[] = {
	{kHeader, 0},
	{kGame, 4, "<ACT_FRONT><ACT_BACK><ACT_LEFT><ACT_RIGHT>"},
	{kGame, 43, "<ACT_DASH>"},
	{kGame, 56, "<ACT_TURN_L>"},
	{kGame, 57, "<ACT_TURN_R>"},
	{kGame, 1, "<ACT_SHOT>"},
	{kGame, 19, "<ACT_RELOAD>"},
	{kGame, 16, "<ACT_JUMP>"},
	{kHeader, 1},
	{kGame, 0, "<ACT_GRENADE>"},
	{kGame, 5, "<ACT_SIT>"},
	{kGame, 10, "<ACT_ANCHOR>"},
	{kGame, 15, "<ACT_STRIKE>"},
	{kGame, 11, "<ACT_CHWP_R>"},
	{kGame, 13, "<ACT_LAYDOWN_R>"},
	{kGame, 17, "<ACT_CHWP_L>"},
	{kGame, 92, "<ACT_NIGHTVIEW>"},
	{kGame, 66, "<ACT_HARMO>"},
	{kGame, 7, "<ACT_PDA>"},
	{kGame, 58, "<ACT_APL1>"},
	{kGame, 59, "<ACT_APL2>"},
	{kGame, 60, "<ACT_APL3>"},
	{kGame, 61, "<ACT_APL4>"},
	{kGame, 62, "<ACT_APL5>"},
	{kGame, 63, "<ACT_APL6>"},
	{kGame, 64, "<ACT_APL7>"},
	{kGame, 65, "<ACT_APL8>"},
	{kHeader, 2},
	{kGame, 18, "<ACT_AIM>"},
	{kGame, 6, "<ACT_ZMUP>"},
	{kRule, 0, "scoped", "zoom_in", "Zoom in"},
	{kRule, 0, "scoped", "zoom_out", "Zoom out"},
};
const int kFootCount = (int)(sizeof(kFoot) / sizeof(kFoot[0]));

// VS: the type's rows by slot (0..15, the game's ACT tag per slot below), regrouped. A rule (context,
// dest) on a slot replaces its key when set. Flying rows only while in a flyer.
const char* const kVsTags[16] = {"<ACT_SHOT>", "<ACT_TURN_L>", "<ACT_FRONT><ACT_BACK><ACT_LEFT><ACT_RIGHT>", "<ACT_PDA>",
                                 "<ACT_ZMUP>", "<ACT_NIGHTVIEW>", "<ACT_APL1>", "<ACT_GRENADE>", "<ACT_TURN_R>", "<ACT_YACT>",
                                 "<ACT_YACT><PC_PLUS><ACT_SHOT><ACT_GRENADE>", "<ACT_XACT>", "<ACT_BACT>", "<ACT_JUMP>",
                                 "<ACT_AIM>", "<ACT_RELOAD><PC_SLASH><ACT_RELOAD_L><ACT_RELOAD_R>"};
const Line kVs[] = {
	{kHeader, 0},
	{kGame, 2},  // move
	{kGame, 1},  // turn left
	{kGame, 8},  // turn right
	{kGame, 0},  // fire left weapon
	{kGame, 13}, // jump / boost
	{kHeader, 3},
	{kGame, 7, "vs", "secondary"}, // fire right weapon
	{kGame, 4, "vs", "zoom"},
	{kGame, 12, "vs", "get_out"},
	{kGame, 11, "vs", "transform"},
	{kGame, 15}, // reload
	{kGame, 9},  // (hold) pick up weapon
	{kGame, 10}, // drop weapon
	{kGame, 14}, // aim reticle
	{kGame, 5},  // night vision
	{kGame, 6},  // klaxon
	{kGame, 3},  // PDA
	{kHeader, 4},
	{kRule, 0, "fly", "ascend", "Ascend"},
	{kRule, 0, "fly", "descend", "Descend"},
	{kRule, 0, "fly", "transform", "Transform"},
	{kRule, 0, "fly", "dash", "Dash"},
};
const int kVsLines = (int)(sizeof(kVs) / sizeof(kVs[0]));

struct AsValue {
	void* object;
	uint32_t type; // 3 number (float), 4 string
	union {
		float number;
		const char* text;
	};
};
using ListFn = void(__attribute__((thiscall)) *)(uint8_t*, uint32_t);
using CountFn = int(__attribute__((thiscall)) *)(uint8_t*, uint32_t);
using MessageFn = const char*(__attribute__((thiscall)) *)(void*, const char*, uint32_t, uint32_t);
using TagsFn = bool(__attribute__((thiscall)) *)(void*, char*, uint32_t, uint32_t);
using InvokeFn = bool(__attribute__((thiscall)) *)(uint8_t*, void*, const char*, AsValue*, int);
using ValueFreeFn = void(__attribute__((thiscall)) *)(AsValue*);

ListFn g_foot = nullptr;
ListFn g_vs = nullptr;
CountFn g_vsCount = nullptr;

void* Manager() { return *(void**)(uintptr_t)A->msgMgr; }
bool Keyboard() { return *(int*)((uint8_t*)Manager() + 0x484) != 0; }
bool OurList(uint8_t* pda) { return Keyboard() && pda[0x1c1d] == 0; }
int& Scroll(uint8_t* pda) { return *(int*)(pda + 0x1bb0); }

const char* Message(const char* file, uint32_t id)
{
	const char* text = ((MessageFn)(uintptr_t)A->message)(Manager(), file, id, 0x16);
	return text ? text : "";
}

int VsMessage(uint32_t type, int slot)
{
	return ((const int*)(uintptr_t)A->vsMessages)[type * 0x15 + slot];
}

// The rows of a list, headers dropped when nothing follows them.
std::vector<std::string> FootRows()
{
	std::vector<std::string> rows;
	for (const Line& l : kFoot) {
		if (l.kind == kHeader) {
			rows.push_back(MenuHeader(l.value));
		}
		else if (l.kind == kGame) {
			bool emote = l.value >= 0x3a && l.value <= 0x41;
			rows.push_back(std::string(l.tag) + " " + Message(emote ? "MsgPC" : "MsgOption", l.value + (emote ? 0x28 : 0x86)));
		}
		else {
			rows.push_back(MenuRemapKey(l.tag, l.text) + " " + l.label);
		}
	}
	return rows;
}

std::vector<std::string> VsRows(uint32_t type)
{
	std::vector<std::string> rows;
	size_t header = (size_t)-1; // index of the last header, until a row follows it
	bool flyer = ControlsInFlyer();
	for (int i = 0; i < kVsLines; i++) {
		const Line& l = kVs[i];
		if (l.kind == kHeader) {
			if (header != (size_t)-1) {
				rows.pop_back();
			}
			if (l.value == 4 && !flyer) {
				break;
			}
			header = rows.size();
			rows.push_back(MenuHeader(l.value));
			continue;
		}
		std::string row;
		if (l.kind == kGame) {
			int id = VsMessage(type, l.value);
			if (id == 0x49) {
				continue;
			}
			std::string key = kVsTags[l.value];
			int sourceSlot = -1;
			if (l.tag && ControlsRuleFor(l.tag, l.text, &sourceSlot) >= 0) {
				key = MenuRemapKey(l.tag, l.text);
			}
			// The game greys out dropping the weapon on these types.
			const char* prefix = id == 0xd && ((type >= 4 && type <= 6) || type == 0x16) ? "----- / " : "";
			row = key + " " + prefix + Message("MsgOption", id + 0x86);
		}
		else {
			row = MenuRemapKey(l.tag, l.text) + " " + l.label;
		}
		rows.push_back(row);
		header = (size_t)-1;
	}
	if (header != (size_t)-1) {
		rows.pop_back();
	}
	return rows;
}

void Send(uint8_t* pda, const char* function, const std::vector<std::string>& rows, bool tabUp)
{
	int count = (int)rows.size();
	int& scroll = Scroll(pda);
	if (scroll > count - 10) {
		scroll = count > 10 ? count - 10 : 0;
	}
	if (scroll < 0) {
		scroll = 0;
	}
	char texts[10][256];
	AsValue args[10];
	for (int r = 0; r < 10; r++) {
		texts[r][0] = 0;
		if (scroll + r < count) {
			snprintf(texts[r], sizeof(texts[r]), "%s", rows[scroll + r].c_str());
			for (uint32_t tag = 0; tag < 0x2c; tag++) {
				((TagsFn)(uintptr_t)A->tags)(Manager(), texts[r], tag, 0);
			}
		}
		args[r] = {};
		args[r].type = 4;
		args[r].text = texts[r];
	}
	auto invoke = (InvokeFn)(uintptr_t)A->invoke;
	auto valueFree = (ValueFreeFn)(uintptr_t)A->valueFree;
	invoke(pda, nullptr, function, args, 10);
	AsValue arrows[2] = {};
	arrows[0].type = arrows[1].type = 3;
	arrows[0].number = tabUp && scroll > 0 ? 0.0f : 2.0f;
	arrows[1].number = tabUp && scroll + 10 < count ? 0.0f : 2.0f;
	invoke(pda, nullptr, "setArrowUD", arrows, 2);
	for (AsValue& v : arrows) {
		valueFree(&v);
	}
	for (AsValue& v : args) {
		valueFree(&v);
	}
}

void __attribute__((thiscall)) Foot(uint8_t* pda, uint32_t type)
{
	if (!OurList(pda)) {
		g_foot(pda, type);
		return;
	}
	uint8_t ridden = pda[0x1c28];
	bool vs = ControlsInVs() && ridden < 30;
	Send(pda, "hmsetPad2", vs ? VsRows(ridden) : FootRows(), *(int*)(pda + 0x1ba0) == 1);
}

void __attribute__((thiscall)) Vs(uint8_t* pda, uint32_t type)
{
	if (!OurList(pda)) {
		g_vs(pda, type);
		return;
	}
	Send(pda, "vssetPad2", VsRows(type & 0xff), *(int*)(pda + 0x1ba0) == 3);
}

int __attribute__((thiscall)) VsCount(uint8_t* pda, uint32_t type)
{
	if (!OurList(pda)) {
		return g_vsCount(pda, type);
	}
	return (int)VsRows(type & 0xff).size();
}

void* Detour(uint8_t* fn, size_t length, void* detour)
{
	auto* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tramp) {
		return nullptr;
	}
	memcpy(tramp, fn, length);
	tramp[length] = 0xe9;
	int32_t back = (int32_t)((fn + length) - (tramp + length + 5));
	memcpy(tramp + length + 1, &back, 4);
	FlushInstructionCache(GetCurrentProcess(), tramp, 32);
	uint8_t patch[16];
	memset(patch, 0x90, sizeof(patch));
	patch[0] = 0xe9;
	int32_t rel = (int32_t)((uint8_t*)detour - (fn + 5));
	memcpy(patch + 1, &rel, 4);
	if (!g_host->write_code(fn, patch, length)) {
		return nullptr;
	}
	return tramp;
}

}

bool ReminderTryInstall(unsigned char* base)
{
	A = g_host->game_exe == LP2MOD_EXE_DX11 ? &kDx11 : &kDx9;
	auto at = [&](uint32_t va) { return base + (va - 0x400000); };
	if (memcmp(at(A->foot), kFootPrologue, sizeof(kFootPrologue)) != 0 || memcmp(at(A->vs), kVsPrologue, sizeof(kVsPrologue)) != 0 ||
	    memcmp(at(A->vsCount), kCountPrologue, sizeof(kCountPrologue)) != 0) {
		return false;
	}
	for (uint32_t cap : A->footCaps) {
		uint8_t* p = at(cap);
		if (p[0] != 0x83 || (p[1] & 0xf8) != 0xf8 || p[2] != 0x1b) {
			return false;
		}
	}
	uint8_t count = (uint8_t)kFootCount;
	for (uint32_t cap : A->footCaps) {
		g_host->write_code(at(cap) + 2, &count, 1);
	}
	g_foot = (ListFn)Detour(at(A->foot), sizeof(kFootPrologue), (void*)&Foot);
	g_vs = (ListFn)Detour(at(A->vs), sizeof(kVsPrologue), (void*)&Vs);
	g_vsCount = (CountFn)Detour(at(A->vsCount), sizeof(kCountPrologue), (void*)&VsCount);
	LOG_INFO("menu: pause menu key reminder follows the Key Bindings order.");
	return true;
}

}
