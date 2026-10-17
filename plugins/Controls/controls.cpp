#include "plugin.h"

#include <windows.h>

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

// Split controls: the same button can mean different actions on foot and in a VS.
//
// Each frame the pad update (DX11 FUN_00f8a980, DX9 FUN_00f7a9e0; thiscall, ecx = pad+0x3b0, one
// caller) copies the player's action bits from the input manager into five u64 sets at pad+0x3b8
// (held), +0x3c0 (pressed), +0x3c8, +0x3d0 (released) and +0x3d8. Word 1 is the low dword, word 2
// the high one. The VS code reads the same sets but gives some bits other meanings (the grenade
// bits fire the VS's second weapon). The call is redirected through PadUpdate, which runs the game's
// update and then rewrites the bits for the current context: a rule "dest <- source" makes action
// dest follow the input of action source and drops source's own bits, in all five sets alike, so
// held/pressed/released stay consistent and gamepads work the same way. Example, zoom on right
// click and grenade on G, but right click fires the VS's second weapon and G zooms in a VS:
//   controls.json: {"vs": {"secondary": "zoom", "zoom": "grenade"}}
// A source can also be a key or mouse button ("key:F", "key:MOUSE_X1"): then the action follows
// that key directly (read with GetAsyncKeyState while the game window is in front), with our own
// press/release edges, and the key's own game binding is left alone. In a VS the game only reports
// a few actions, so this is the way to put a VS action on any key. Keyboard and mouse only.
//
// Context: the pad's unit (pad+0x480) and what it is riding (unit+0x40, as the game's own zoom
// check reads it, DX11 0x9397af). Riding a unit of category 2 (byte +0x1f, the two-weapon units)
// counts as being in a VS. Unverified in game: "log": true logs every change of these so the test
// can confirm it.
//
//   lea ecx,[esi+0x3b0]; call update; call ...    (8d 8e b0 03 00 00 e8 ........ e8)

namespace controls {

namespace {

const uint8_t kSiteBytes[] = {0x8d, 0x8e, 0xb0, 0x03, 0x00, 0x00, 0xe8, 0, 0, 0, 0, 0xe8};
const uint8_t kSiteMask[] = {1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1};
const uint32_t kSiteRvas[] = {0xb8d96c /* DX11 */, 0xb7d9cc /* DX9 */};
const uint32_t kSetsOffset = 0x3b8 - 0x3b0; // from ecx (pad+0x3b0)
const int kSetCount = 5;

constexpr uint64_t W2(uint32_t bits) { return (uint64_t)bits << 32; }

enum Context { kFoot, kVs, kContextCount };
const char* const kContextNames[kContextCount] = {"foot", "vs"};

// An action as the pad shows it: any bit of test means the button is down; set is what the game
// itself produces for it; clear is what a rule removes. Bits shared with other actions are left
// out of clear (VS zoom also sets 0x1, which get out shares; keepIf keeps those when present).
struct Action {
	const char* name;
	uint64_t test;
	uint64_t set;
	uint64_t clear;
	uint64_t clearIfNot; // also cleared, unless a keepIf bit is present
	uint64_t keepIf;
};

// From the 2026-10-16 pad rounds (lp2-tools/padmap.py, the user's bindings).
const Action kFootActions[] = {
	{"jump", 0x2, 0x2, 0x2},
	{"grapple", 0x4, 0x4, 0x4},
	{"next_weapon", 0x8, 0x8, 0x8},
	{"crouch", 0x20, 0x20, 0x20},
	{"fire", 0x100, 0x100, 0x100},
	{"grenade", 0x200, 0x200 | W2(0x400000), 0x200 | W2(0x400000)},
	{"reload", 0x400, 0x400 | 0x800 | W2(0x200000), 0x400 | 0x800 | W2(0x200000)},
	{"action", 0x2000, 0x2000, 0x2000},
	{"quick_turn", 0x10000, 0x10000 | 0x20000, 0x10000 | 0x20000},
	{"zoom", 0x100000, 0x100000, 0x100000},
	{"melee", W2(0x20), W2(0x20), W2(0x20)},
	{"gesture", W2(0x100), W2(0x100), W2(0x100)},
	{"teng", W2(0x4000), W2(0x4000), W2(0x4000)},
	{"dodge", W2(0x8000000), W2(0x8000000), W2(0x8000000)},
};
const Action kVsActions[] = {
	{"fire", 0x100, 0x100, 0x100},
	{"secondary", 0x200, 0x200 | W2(0x400000), 0x200 | W2(0x400000)},
	{"grenade", 0x200, 0x200 | W2(0x400000), 0x200 | W2(0x400000)}, // same button as secondary
	{"zoom", 0x100000, 0x100000 | 0x1, 0x100000, 0x1, 0x10 | 0x1000},
	{"brake", 0x20, 0x20, 0x20},
	{"crouch", 0x20, 0x20, 0x20},
	{"reload", 0x400, 0x400 | 0x800 | W2(0x200000), 0x400 | 0x800 | W2(0x200000)},
	{"quick_turn", 0x10000, 0x10000 | 0x20000 | W2(0x80000), 0x10000 | 0x20000 | W2(0x80000)},
	{"dash", W2(0x10000000), W2(0x10000000), W2(0x10000000)},
	{"boost", 0x2, 0x2 | 0x10 | 0x1000 | W2(0x40 | 0x80), 0x2},
	{"jump", 0x2, 0x2 | 0x10 | 0x1000 | W2(0x40 | 0x80), 0x2},
};

struct Rule {
	const Action* dest;
	const Action* source; // nullptr when the source is a key
	int vk = 0;           // virtual-key code of a key source
	bool down = false;    // key state at the previous pad update
	bool wasDown = false;
};
std::vector<Rule> g_rules[kContextCount];

using UpdateFn = void(__attribute__((thiscall)) *)(void*);
UpdateFn g_update = nullptr;

const Action* FindAction(Context context, const std::string& name)
{
	const Action* list = context == kVs ? kVsActions : kFootActions;
	size_t count = context == kVs ? sizeof(kVsActions) / sizeof(kVsActions[0]) : sizeof(kFootActions) / sizeof(kFootActions[0]);
	for (size_t i = 0; i < count; i++) {
		if (_stricmp(list[i].name, name.c_str()) == 0) {
			return &list[i];
		}
	}
	return nullptr;
}

struct KeyName {
	const char* name;
	int vk;
};
const KeyName kKeyNames[] = {
	{"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"ENTER", VK_RETURN}, {"BACKSPACE", VK_BACK},
	{"SHIFT", VK_SHIFT}, {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},
	{"CTRL", VK_CONTROL}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
	{"ALT", VK_MENU}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"CAPSLOCK", VK_CAPITAL},
	{"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END},
	{"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
	{"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
	{"MOUSE_L", VK_LBUTTON}, {"MOUSE_R", VK_RBUTTON}, {"MOUSE_M", VK_MBUTTON},
	{"MOUSE_X1", VK_XBUTTON1}, {"MOUSE4", VK_XBUTTON1}, {"MOUSE_X2", VK_XBUTTON2}, {"MOUSE5", VK_XBUTTON2},
	{",", VK_OEM_COMMA}, {".", VK_OEM_PERIOD}, {"-", VK_OEM_MINUS}, {"=", VK_OEM_PLUS},
	{";", VK_OEM_1}, {"/", VK_OEM_2}, {"`", VK_OEM_3}, {"[", VK_OEM_4}, {"\\", VK_OEM_5},
	{"]", VK_OEM_6}, {"'", VK_OEM_7},
};

// "key:<name>": a letter or digit, F1-F24, NUM0-NUM9, a name above, or a hex code (0x41).
int ParseKey(const std::string& source)
{
	if (_strnicmp(source.c_str(), "key:", 4) != 0) {
		return 0;
	}
	std::string name = source.substr(4);
	for (char& c : name) {
		c = (char)toupper((unsigned char)c);
	}
	if (name.size() == 1 && (isalpha((unsigned char)name[0]) || isdigit((unsigned char)name[0]))) {
		return name[0];
	}
	if (name.size() >= 2 && name[0] == 'F' && isdigit((unsigned char)name[1])) {
		int n = atoi(name.c_str() + 1);
		return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
	}
	if (name.size() == 4 && name.compare(0, 3, "NUM") == 0 && isdigit((unsigned char)name[3])) {
		return VK_NUMPAD0 + (name[3] - '0');
	}
	if (name.size() > 2 && name[0] == '0' && name[1] == 'X') {
		int vk = (int)strtol(name.c_str() + 2, nullptr, 16);
		return vk > 0 && vk < 256 ? vk : 0;
	}
	for (const KeyName& k : kKeyNames) {
		if (name == k.name) {
			return k.vk;
		}
	}
	return -1; // "key:" with an unknown name
}

bool GameInFront()
{
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
}

}

bool ControlsLoadRules()
{
	bool any = false;
	for (const Remap& remap : g_settings.remaps) {
		int context = -1;
		for (int c = 0; c < kContextCount; c++) {
			if (_stricmp(remap.context.c_str(), kContextNames[c]) == 0) {
				context = c;
			}
		}
		if (context < 0) {
			LOG_WARN("unknown context \"%s\" (use foot or vs).", remap.context.c_str());
			continue;
		}
		const Action* dest = FindAction((Context)context, remap.dest);
		int vk = ParseKey(remap.source);
		if (vk < 0) {
			LOG_WARN("%s: unknown key \"%s\".", kContextNames[context], remap.source.c_str());
			continue;
		}
		const Action* source = vk ? nullptr : FindAction((Context)context, remap.source);
		if (!dest || (!vk && !source)) {
			LOG_WARN("%s: unknown action \"%s\".", kContextNames[context], (!dest ? remap.dest : remap.source).c_str());
			continue;
		}
		Rule rule;
		rule.dest = dest;
		rule.source = source;
		rule.vk = vk;
		g_rules[context].push_back(rule);
		if (vk) {
			LOG_INFO("%s: %s <- key 0x%02x (%s).", kContextNames[context], dest->name, vk, remap.source.c_str() + 4);
		}
		else {
			LOG_INFO("%s: %s <- %s.", kContextNames[context], dest->name, source->name);
		}
		any = true;
	}
	return any;
}

namespace {

bool Alive(const uint8_t* unit)
{
	if (!unit) {
		return false;
	}
	uint32_t state = *(const uint32_t*)(unit + 4) & 7;
	return state == 1 || state == 2;
}

Context CurrentContext(uint8_t* pad)
{
	uint8_t* unit = *(uint8_t**)(pad + 0x480);
	uint8_t* ride = Alive(unit) ? *(uint8_t**)(unit + 0x40) : nullptr;
	bool vs = Alive(ride) && ride != unit && ride[0x1f] == 2;
	if (g_settings.log) {
		static uint8_t* lastUnit = nullptr;
		static uint8_t* lastRide = nullptr;
		if (unit != lastUnit || ride != lastRide) {
			lastUnit = unit;
			lastRide = ride;
			LOG_INFO("pad %p unit %p (id %08x) ride %p (id %08x) -> %s.", pad, unit,
			             Alive(unit) ? *(uint32_t*)(unit + 0x1c) : 0, ride, Alive(ride) ? *(uint32_t*)(ride + 0x1c) : 0,
			             kContextNames[vs ? kVs : kFoot]);
		}
	}
	return vs ? kVs : kFoot;
}

void Remap(uint64_t* sets, const std::vector<Rule>& rules)
{
	uint64_t snap[kSetCount];
	memcpy(snap, sets, sizeof(snap));
	for (int k = 0; k < kSetCount; k++) {
		uint64_t clear = 0;
		for (const Rule& r : rules) {
			for (const Action* a : {r.dest, r.source}) {
				if (!a) {
					continue; // key source: its game binding is left alone
				}
				clear |= a->clear;
				if (!(snap[k] & a->keepIf)) {
					clear |= a->clearIfNot;
				}
			}
		}
		uint64_t out = snap[k] & ~clear;
		for (const Rule& r : rules) {
			bool on;
			if (r.source) {
				on = (snap[k] & r.source->test) != 0;
			}
			else {
				bool pressed = r.down && !r.wasDown;
				bool released = !r.down && r.wasDown;
				// held, pressed, +0x3c8 (taken as pressed), released, +0x3d8 (taken as changed)
				const bool byKind[kSetCount] = {r.down, pressed, pressed, released, pressed || released};
				on = byKind[k];
			}
			if (on) {
				out |= r.dest->set;
			}
		}
		sets[k] = out;
	}
}

void __attribute__((thiscall)) PadUpdate(uint8_t* self)
{
	g_update(self);
	uint8_t* pad = self - 0x3b0;
	// Key sources keep their state in every context, so entering or leaving a VS while holding the
	// key gives no false press.
	bool front = GameInFront();
	for (std::vector<Rule>& list : g_rules) {
		for (Rule& r : list) {
			if (r.vk) {
				r.wasDown = r.down;
				r.down = front && (GetAsyncKeyState(r.vk) & 0x8000) != 0;
			}
		}
	}
	const std::vector<Rule>& rules = g_rules[CurrentContext(pad)];
	if (!rules.empty()) {
		Remap((uint64_t*)(self + kSetsOffset), rules);
	}
}

bool SiteMatches(const uint8_t* at)
{
	for (size_t i = 0; i < sizeof(kSiteBytes); i++) {
		if (kSiteMask[i] && at[i] != kSiteBytes[i]) {
			return false;
		}
	}
	return true;
}

}

bool ControlsTryInstall(unsigned char* base)
{
	uint8_t* site = nullptr;
	for (uint32_t rva : kSiteRvas) {
		if (InImage(base, rva, sizeof(kSiteBytes)) && SiteMatches(base + rva)) {
			site = base + rva;
			break;
		}
	}
	if (!site) {
		return false;
	}
	uint8_t* call = site + 6;
	g_update = (UpdateFn)(call + 5 + *(int32_t*)(call + 1));
	int32_t rel = (int32_t)((uint8_t*)&PadUpdate - (call + 5));
	DWORD old = 0;
	if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	memcpy(call + 1, &rel, 4);
	VirtualProtect(call + 1, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), call + 1, 4);
	LOG_INFO("pad update hooked at 0x%p (game update 0x%p).", call, (void*)g_update);
	return true;
}

}
