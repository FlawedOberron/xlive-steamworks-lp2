#include "plugin.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

// Key configuration menu: the "Controls" page is regrouped under headers (Shared, On Foot, Scoped,
// VS) and gains a "Hold to aim" On/Off row. LP2DX11.exe only for now.
//
// The page (flash\option\id501) is driven by a table of {u32 label, u32 slot} rows at 0x1636c70
// (35 rows; label = index into the u32 message id table 0x1636dd0, slot = key binding slot, 0x2e and
// 0x2f = the mouse speed / sensitivity numbers). The game reads the table through the slot field
// (0x1636c74) in nine places, which are pointed at our table, and its row count 0x23 in two
// (the list input FUN_00f06f30 and the number editor FUN_00f069f0). The reset to defaults
// (FUN_00f060a0) keeps the original table, so it only ever touches real slots.
//
// Our rows use slots the game never has: headers and the toggle. Hooks:
//   FUN_00f056a0 (draw the 8 visible rows) is replaced: the same three ActionScript calls
//     (setList / setListData / setArrowState via FUN_00f04d90 / 04e10 / 04f40) with our texts.
//   FUN_00f074b0 (state 2, list) and FUN_00f069f0 (state 6, number rows): when a move lands on a
//     header, the handler runs again with the same input, so the cursor steps over it; left/right
//     on the toggle row flips it.
//   FUN_00f06480 (Enter: start key capture): ignored on headers, flips the toggle.
// Mouse speed and sensitivity stay the first two rows, where the number editor expects them.

namespace controls {

namespace {

const uint32_t kSlotHeader = 0x40; // + header index
const uint32_t kSlotHoldToAim = 0x50;

struct Row {
	uint32_t label; // game label index, or text index for ours
	uint32_t slot;
};

const char* const kHeaders[] = {"- SHARED -", "- ON FOOT -", "- SCOPED -", "- VS -"};

// Game rows: label index (into 0x1636dd0), slot. From the original table.
const Row kRows[] = {
	{39, 0x2e}, {40, 0x2f}, // mouse speed, mouse sensitivity
	{0, kSlotHeader + 0},
	{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {6, 5}, {5, 6}, // forward, back, left, right, walk, turn L/R
	{7, 7}, {16, 0x1c}, {17, 0x13}, {26, 0x1f},             // shoot, reload, jump, focus camera
	{0, kSlotHeader + 1},
	{8, 8}, {9, 9}, {10, 10}, {11, 11}, {12, 12},             // grenade, crouch, anchor, melee, change weapon
	{13, 13}, {14, 14}, {15, 15},                             // action 1-3
	{27, 0x20}, {28, 0x21}, {44, 0x23},                       // night vision, harmonizer, merged/support
	{18, 0x14}, {19, 0x15}, {20, 0x16}, {21, 0x17}, {22, 0x18}, {23, 0x19}, {24, 0x1a}, {25, 0x1b}, // emotes
	{0, kSlotHeader + 2},
	{43, 0x1d}, {0, kSlotHoldToAim},                          // zoom in, hold to aim
	{0, kSlotHeader + 3},
	{42, 0x26}, {41, 0x27},                                   // reload VS right / left
};
const size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
Row g_table[kRowCount + 8]; // padding: the original table was followed by more rows

// LP2DX11.exe addresses.
const uint32_t kTableSlot = 0x1636c74;
const uint32_t kTableRefs[] = {0xf0522c, 0xf05b07, 0xf05dc9, 0xf06955, 0xf06a0a, 0xf06be1, 0xf07283, 0xf074d0};
const uint32_t kCountMov = 0xf06f82; // mov ecx,0x23; lea edx,[ecx-0x1f] (edx = 4, the capture state!)
const uint32_t kCountCmp = 0xf06a46; // cmp edx,0x23
const uint32_t kDraw = 0xf056a0;
const uint32_t kStateList = 0xf074b0;
const uint32_t kStateNumber = 0xf069f0;
const uint32_t kStartCapture = 0xf06480;
const uint32_t kLabelIds = 0x1636dd0;
const uint32_t kMsgPc = 0x1373294;  // "MsgPC" message file name
const uint32_t kMsgMgr = 0x18e0860; // message / input manager pointer
const uint32_t kSetList = 0xf04d90, kSetListData = 0xf04e10, kSetArrowState = 0xf04f40;
const uint32_t kMessage = 0x77b280, kKeyName = 0xf04b60;

const uint8_t kDrawPrologue[] = {0x83, 0xec, 0x20, 0x53, 0x55, 0x56, 0x8b, 0xf1};
const uint8_t kListPrologue[] = {0x83, 0xec, 0x0c, 0x56, 0x6a, 0x16, 0x6a, 0x4a, 0x8b, 0xf1};
const uint8_t kNumberPrologue[] = {0x83, 0xec, 0x40, 0x53, 0x55, 0x8b, 0x2d, 0x60, 0x08, 0x8e, 0x01};
const uint8_t kCapturePrologue[] = {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x6a, 0x00};

using ScreenFn = void(__attribute__((thiscall)) *)(uint8_t*);
using CaptureFn = void(__attribute__((thiscall)) *)(uint8_t*, int);
using RowTextFn = void(__attribute__((thiscall)) *)(uint8_t*, int, const char*, int);
using ArrowFn = void(__attribute__((thiscall)) *)(uint8_t*, int, int, int);
using MessageFn = const char*(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t, uint32_t);
using KeyNameFn = const char*(__cdecl*)(uint32_t*);

ScreenFn g_list = nullptr;
ScreenFn g_number = nullptr;
CaptureFn g_capture = nullptr;

int& State(uint8_t* screen) { return *(int*)(screen + 0x190); }
int& Cursor(uint8_t* screen) { return *(int*)(screen + 0x198); }
int& Visible(uint8_t* screen) { return *(int*)(screen + 0x19c); }

bool IsHeader(int row)
{
	return row >= 0 && row < (int)kRowCount && g_table[row].slot >= kSlotHeader && g_table[row].slot < kSlotHeader + 0x10;
}

const Row* RowAt(int row)
{
	return row >= 0 && row < (int)kRowCount ? &g_table[row] : nullptr;
}

void __attribute__((thiscall)) Draw(uint8_t* screen)
{
	auto setList = (RowTextFn)(uintptr_t)kSetList;
	auto setData = (RowTextFn)(uintptr_t)kSetListData;
	auto setArrow = (ArrowFn)(uintptr_t)kSetArrowState;
	int first = Cursor(screen) - Visible(screen);
	for (int r = 0; r < 8; r++) {
		const Row* row = RowAt(first + r);
		if (!row) {
			setList(screen, r, "", 1);
			setData(screen, r, "", 0);
			setArrow(screen, r, 0, 0);
			continue;
		}
		if (row->slot >= kSlotHeader && row->slot < kSlotHeader + 0x10) {
			setList(screen, r, kHeaders[row->slot - kSlotHeader], 1);
			setData(screen, r, "", 0);
			setArrow(screen, r, 0, 0);
			continue;
		}
		if (row->slot == kSlotHoldToAim) {
			setList(screen, r, "Hold to aim", 1);
			setData(screen, r, g_settings.holdToAim ? "ON" : "OFF", 1);
			setArrow(screen, r, 0, 0);
			continue;
		}
		void* messages = *(void**)(uintptr_t)kMsgMgr;
		uint32_t id = ((uint32_t*)(uintptr_t)kLabelIds)[row->label];
		const char* label = ((MessageFn)(uintptr_t)kMessage)(messages, kMsgPc, id, 0x16);
		setList(screen, r, label ? label : "", 1);
		if (row->slot == 0x2e || row->slot == 0x2f) {
			char number[16];
			snprintf(number, sizeof(number), "%d", screen[row->slot == 0x2e ? 0x1d2 : 0x1d3] + 1);
			setData(screen, r, number, 1);
			setArrow(screen, r, 0, 0);
			continue;
		}
		const char* key = ((KeyNameFn)(uintptr_t)kKeyName)((uint32_t*)(screen + 0x1e0 + row->slot * 4));
		setData(screen, r, key ? key : "", 1);
		bool conflict = *(int*)(screen + 0x294 + row->slot * 4) == 1;
		setArrow(screen, r, conflict, conflict);
	}
}

void ToggleHoldToAim(uint8_t* screen)
{
	g_settings.holdToAim = !g_settings.holdToAim;
	SaveHoldToAim();
	Draw(screen);
	LOG_INFO("menu: hold to aim %s.", g_settings.holdToAim ? "on" : "off");
}

// The mouse: +0x154 is the id of the element under the pointer (rows 0x1fe..0x205 = the 8 visible
// rows, 0 = Default, 1 = Back, 0x213/0x214 = scroll arrows), +0x15c bit 0 a click. Hovering a row
// moves the cursor there every frame, so a header under the pointer would hold the cursor. The
// pointer over a header counts as over nothing.
void IgnoreHeaderUnderMouse(uint8_t* screen)
{
	uint32_t hover = *(uint32_t*)(screen + 0x154);
	if (hover - 0x1fe < 8 && IsHeader(Cursor(screen) - Visible(screen) + (int)(hover - 0x1fe))) {
		*(uint32_t*)(screen + 0x154) = 0xffff;
		*(uint32_t*)(screen + 0x15c) &= ~1u;
	}
}

// Runs a state handler; if the cursor moved onto a header, runs it again (same input, same move).
void RunSkippingHeaders(uint8_t* screen)
{
	IgnoreHeaderUnderMouse(screen);
	for (int pass = 0; pass < 4; pass++) {
		int before = Cursor(screen);
		if (State(screen) == 6) {
			g_number(screen);
		}
		else if (State(screen) == 2) {
			g_list(screen);
		}
		else {
			return;
		}
		int now = Cursor(screen);
		if (now == before || !IsHeader(now)) {
			return;
		}
	}
}

bool LeftOrRight()
{
	uint8_t* input = *(uint8_t**)(uintptr_t)kMsgMgr;
	return input && (*(uint32_t*)(input + 0x330) & 0x300) != 0;
}

void __attribute__((thiscall)) ListState(uint8_t* screen)
{
	const Row* row = RowAt(Cursor(screen));
	if (row && row->slot == kSlotHoldToAim && LeftOrRight()) {
		ToggleHoldToAim(screen);
		return;
	}
	RunSkippingHeaders(screen);
}

void __attribute__((thiscall)) NumberState(uint8_t* screen)
{
	RunSkippingHeaders(screen);
}

void __attribute__((thiscall)) StartCapture(uint8_t* screen, int state)
{
	const Row* row = RowAt(Cursor(screen));
	LOG_INFO("menu: state %d requested at row %d (slot 0x%x), from state %d.", state, Cursor(screen), row ? row->slot : 0xffff, State(screen));
	if (state == 4 && row && (row->slot >= kSlotHeader || row->slot == kSlotHoldToAim)) {
		if (row->slot == kSlotHoldToAim) {
			ToggleHoldToAim(screen);
		}
		return; // headers: nothing to bind
	}
	g_capture(screen, state);
}

// Detours fn (whose first `length` bytes are position-independent) to detour; returns a trampoline
// that runs the original.
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

bool MenuTryInstall(unsigned char* base)
{
	if (g_host->game_exe != LP2MOD_EXE_DX11) {
		return true; // DX9 not mapped yet
	}
	auto at = [&](uint32_t va) { return base + (va - 0x400000); };
	// Every site must look exactly as expected, else leave the menu alone.
	for (uint32_t ref : kTableRefs) {
		uint8_t* p = at(ref);
		bool found = false;
		for (int i = 0; i < 8 && !found; i++) {
			found = *(uint32_t*)(p + i) == kTableSlot;
		}
		if (!found) {
			return false;
		}
	}
	const uint8_t mov23[] = {0xb9, 0x23, 0x00, 0x00, 0x00, 0x8d, 0x51, 0xe1};
	const uint8_t cmp23[] = {0x83, 0xfa, 0x23};
	if (memcmp(at(kCountMov), mov23, sizeof(mov23)) != 0 || memcmp(at(kCountCmp), cmp23, sizeof(cmp23)) != 0 ||
	    memcmp(at(kDraw), kDrawPrologue, sizeof(kDrawPrologue)) != 0 || memcmp(at(kStateList), kListPrologue, sizeof(kListPrologue)) != 0 ||
	    memcmp(at(kStateNumber), kNumberPrologue, sizeof(kNumberPrologue)) != 0 ||
	    memcmp(at(kStartCapture), kCapturePrologue, sizeof(kCapturePrologue)) != 0) {
		return false;
	}
	memcpy(g_table, kRows, sizeof(kRows));
	for (size_t i = kRowCount; i < kRowCount + 8; i++) {
		g_table[i] = {0, kSlotHeader + 0xf};
	}
	uint32_t slotField = (uint32_t)(uintptr_t)&g_table[0].slot;
	for (uint32_t ref : kTableRefs) {
		uint8_t* p = at(ref);
		for (int i = 0; i < 8; i++) {
			if (*(uint32_t*)(p + i) == kTableSlot) {
				g_host->write_code(p + i, &slotField, 4);
				break;
			}
		}
	}
	uint32_t count = (uint32_t)kRowCount;
	uint8_t count8 = (uint8_t)kRowCount;
	g_host->write_code(at(kCountMov) + 1, &count, 4);
	// The compiler derives the key capture state (4) from the count: keep edx = 4.
	int8_t stateFromCount = (int8_t)(4 - (int)kRowCount);
	g_host->write_code(at(kCountMov) + 7, &stateFromCount, 1);
	g_host->write_code(at(kCountCmp) + 2, &count8, 1);
	uint8_t jmp[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
	int32_t rel = (int32_t)((uint8_t*)&Draw - (at(kDraw) + 5));
	memcpy(jmp + 1, &rel, 4);
	g_host->write_code(at(kDraw), jmp, sizeof(jmp));
	g_list = (ScreenFn)Detour(at(kStateList), sizeof(kListPrologue), (void*)&ListState);
	g_number = (ScreenFn)Detour(at(kStateNumber), sizeof(kNumberPrologue), (void*)&NumberState);
	g_capture = (CaptureFn)Detour(at(kStartCapture), sizeof(kCapturePrologue), (void*)&StartCapture);
	LOG_INFO("menu: Controls page regrouped (%u rows).", (unsigned)kRowCount);
	return true;
}

}
