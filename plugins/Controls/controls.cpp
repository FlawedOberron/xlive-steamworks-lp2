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
// Pointer to the options object; its +0x68 holds the 45 key bindings (u32: virtual-key code, or
// mouse code | 0x80000000), copied there when the key config menu closes.
const uint32_t kOptionsPtrs[] = {0x1852238 /* DX11 */, 0x1842198 /* DX9 */};
// The game's slot -> action bits table: 45 x {u64 bits, u64 default binding}. Context-free: the VS
// code reads the same bits differently (slot 8 grenade 0x200 = VS secondary, slot 13 action 1 0x1 =
// get out, slot 12 change weapon 0x8 = ascend, slot 18 T-ENG w2 0x40000, ...).
const uint32_t kSlotBits[] = {0x1645040 /* DX11 */, 0x1634f08 /* DX9 */};
const uint32_t kSetsOffset = 0x3b8 - 0x3b0; // from ecx (pad+0x3b0)
const int kSetCount = 5;

constexpr uint64_t W2(uint32_t bits) { return (uint64_t)bits << 32; }

enum Context { kFoot, kVs, kFly, kScoped, kContextCount };
const char* const kContextNames[kContextCount] = {"foot", "vs", "fly", "scoped"};

// An action as the pad shows it: any bit of test means the button is down; set is what the game
// itself produces for it; clear is what a rule removes (bits shared with other actions are left
// out: VS zoom also sets 0x1, which get out shares). slot: the action's key config slot, which is
// also its prompt tag index (FUN_00848110).
struct Action {
	const char* name;
	uint64_t test;
	uint64_t set;
	uint64_t clear;
	int slot; // key config slot = prompt tag index (-1: none), for the on-screen prompts
};

// From the 2026-10-16 pad rounds (lp2-tools/padmap.py, the user's bindings).
const Action kFootActions[] = {
	{"jump", 0x2, 0x2, 0x2, 19},
	{"grapple", 0x4, 0x4, 0x4, 10},
	{"next_weapon", 0x8, 0x8, 0x8, 12},
	{"crouch", 0x20, 0x20, 0x20, 9},
	{"fire", 0x100, 0x100, 0x100, 7},
	{"grenade", 0x200, 0x200 | W2(0x400000), 0x200 | W2(0x400000), 8},
	{"reload", 0x400, 0x400 | 0x800 | W2(0x200000), 0x400 | 0x800 | W2(0x200000), 28},
	{"action", 0x2000, 0x2000, 0x2000, 13},
	{"quick_turn", 0x10000, 0x10000 | 0x20000, 0x10000 | 0x20000, 6},
	{"zoom", 0x100000, 0x100000, 0x100000, 29},
	{"melee", W2(0x20), W2(0x20), W2(0x20), 11},
	{"gesture", W2(0x100), W2(0x100), W2(0x100), 20},
	{"teng", W2(0x40000), W2(0x40000), W2(0x40000), 18}, // 0x40000 (2026-10-17 bit log; the pad round had 0x4000)
	{"dodge", W2(0x8000000), W2(0x8000000), W2(0x8000000), 36},
};
const Action kVsActions[] = {
	{"fire", 0x100, 0x100, 0x100, 7},
	{"secondary", 0x200, 0x200 | W2(0x400000), 0x200 | W2(0x400000), 8}, // the grenade button
	{"grenade", 0x200, 0x200 | W2(0x400000), 0x200 | W2(0x400000), 8}, // same button as secondary
	{"zoom", 0x100000, 0x100000, 0x100000, 29},
	// Get out is the Action 1 button (slot 13, bit 0x1).
	{"get_out", 0x1, 0x1, 0x1, 13},
	// Transforming VSs (e.g. type 0x9a) transform on the jump bit, which is boost in others. Nothing
	// is cleared: a transform key adds the bit, Space keeps doing what it does.
	{"transform", 0x2, 0x2, 0, -1},
	{"brake", 0x20, 0x20, 0x20, 9},
	{"crouch", 0x20, 0x20, 0x20, 9},
	{"reload", 0x400, 0x400 | 0x800 | W2(0x200000), 0x400 | 0x800 | W2(0x200000), 28},
	{"quick_turn", 0x10000, 0x10000 | 0x20000 | W2(0x80000), 0x10000 | 0x20000 | W2(0x80000), 6},
	{"dash", W2(0x10000000), W2(0x10000000), W2(0x10000000), 37},
	{"boost", 0x2, 0x2 | 0x10 | 0x1000 | W2(0x40 | 0x80), 0x2, 19},
	{"jump", 0x2, 0x2 | 0x10 | 0x1000 | W2(0x40 | 0x80), 0x2, 19},
};

// Flying VSs: the VS actions plus these (ascend = the change weapon button, descend = the drop
// weapon button as the game's controls page lists them; descend's bit is from one test, unconfirmed).
const Action kFlyActions[] = {
	{"ascend", 0x8, 0x8, 0x8, 12},
	{"descend", W2(0x40000), W2(0x40000), W2(0x40000), 16},
};

// Scoped (on foot, zoomed in): the foot actions plus zoom steps (no pad bits: they step the zoom
// level through zoom.cpp).
const Action kScopedActions[] = {
	{"zoom_in", 0, 0, 0, -1},
	{"zoom_out", 0, 0, 0, -1},
};

// Mouse wheel "keys" (not virtual keys: read from the game's input, one pulse per notch).
const int kVkWheelUp = 0x10c;
const int kVkWheelDown = 0x10d;

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
uint32_t g_optionsPtr = 0;
uint32_t g_slotBits = 0;
uint32_t g_inputPtr = 0;   // input manager (FUN_00860f70 reads mouse codes from it), DX11 only for now
uint32_t g_mouseFn = 0;

// The game's "mouse code pressed this frame" (codes 12 / 13 = wheel up / down).
bool MousePressed(int code)
{
	uint8_t* input = g_inputPtr ? *(uint8_t**)(uintptr_t)g_inputPtr : nullptr;
	if (!input || !g_mouseFn) {
		return false;
	}
	return ((bool(__attribute__((thiscall)) *)(uint8_t*, int))(uintptr_t)g_mouseFn)(input, code);
}

const Action* FindAction(Context context, const std::string& name)
{
	if (context == kFly) {
		for (const Action& a : kFlyActions) {
			if (_stricmp(a.name, name.c_str()) == 0) {
				return &a;
			}
		}
		context = kVs;
	}
	if (context == kScoped) {
		for (const Action& a : kScopedActions) {
			if (_stricmp(a.name, name.c_str()) == 0) {
				return &a;
			}
		}
		context = kFoot;
	}
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
	{"MOUSE_WUP", kVkWheelUp}, {"WHEEL_UP", kVkWheelUp}, {"MOUSE_WDOWN", kVkWheelDown}, {"WHEEL_DOWN", kVkWheelDown},
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

// The game's binding code for a virtual key (mouse buttons have their own codes).
uint32_t BindingCode(int vk)
{
	switch (vk) {
	case VK_LBUTTON:
		return 0x80000003;
	case VK_RBUTTON:
		return 0x80000004;
	case VK_MBUTTON:
		return 0x80000005;
	case kVkWheelUp:
		return 0x8000000c;
	case kVkWheelDown:
		return 0x8000000d;
	}
	return (uint32_t)vk;
}

// A key source takes its key over in its context: whatever the player bound to that key in the
// game's own config is dropped there (e.g. right click no longer zooms in a VS once it fires the
// second weapon; wheel down no longer swaps to T-ENG while scoped). Uses the player's bindings and
// the game's slot -> bits table, so it works for any assignment.
uint64_t NativeClear(Context, int vk)
{
	uint8_t* options = g_optionsPtr ? *(uint8_t**)(uintptr_t)g_optionsPtr : nullptr;
	if (!options || !g_slotBits) {
		return 0;
	}
	uint32_t code = BindingCode(vk);
	uint64_t clear = 0;
	for (int slot = 0; slot < 45; slot++) {
		if (*(uint32_t*)(options + 0x68 + slot * 4) == code) {
			clear |= *(uint64_t*)(uintptr_t)(g_slotBits + slot * 16);
		}
	}
	return clear;
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
			LOG_WARN("unknown context \"%s\" (use foot, vs or fly).", remap.context.c_str());
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
	// Flying VSs inherit the VS rules, scoped the foot rules, unless they set the action themselves.
	auto inherit = [](Context from, Context to) {
		for (const Rule& r : g_rules[from]) {
			bool overridden = false;
			for (const Rule& f : g_rules[to]) {
				overridden |= _stricmp(f.dest->name, r.dest->name) == 0;
			}
			if (!overridden) {
				g_rules[to].push_back(r);
			}
		}
	};
	inherit(kVs, kFly);
	inherit(kFoot, kScoped);
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
	bool fly = false;
	if (vs) {
		int type = ride[0x1e];
		for (int t : g_settings.flyTypes) {
			fly |= t == type;
		}
	}
	Context context = fly ? kFly : vs ? kVs : kFoot;
	if (context == kFoot && Alive(ride) && ZoomLevelOf(ride) > 0) {
		context = kScoped;
	}
	if (g_settings.log) {
		static uint8_t* lastUnit = nullptr;
		static uint8_t* lastRide = nullptr;
		if (unit != lastUnit || ride != lastRide) {
			lastUnit = unit;
			lastRide = ride;
			LOG_INFO("pad %p unit %p (id %08x) ride %p (id %08x) -> %s.", pad, unit,
			             Alive(unit) ? *(uint32_t*)(unit + 0x1c) : 0, ride, Alive(ride) ? *(uint32_t*)(ride + 0x1c) : 0,
			             kContextNames[context]);
		}
	}
	return context;
}

void ApplyRules(uint64_t* sets, const std::vector<Rule>& rules, uint64_t extraClear)
{
	uint64_t snap[kSetCount];
	memcpy(snap, sets, sizeof(snap));
	for (int k = 0; k < kSetCount; k++) {
		uint64_t clear = extraClear;
		for (const Rule& r : rules) {
			for (const Action* a : {r.dest, r.source}) {
				if (!a) {
					continue; // key source: its game binding is left alone
				}
				clear |= a->clear;
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

// --- On-screen prompts. The prompt refresh (DX11 FUN_00848110, DX9 FUN_00838cc0; thiscall, ecx =
// the message system) gives every prompt tag object (array at +0x4c4, count +0x4b8; tag index =
// key config slot) its key icon at +0xc: 0x24 + index of the key's virtual-key code in the key
// table, 0x84/0x85/0x86 for the left/right/middle mouse button, 0x8d for none; then sets +0x47c
// (redraw). The refresh is hooked at its start (8 relocatable bytes): after the game's refresh, the
// rules of the current context give each moved action the icon of its source. A context change
// runs the refresh again, so prompts follow on foot / in a VS.
struct PromptLayout {
	uint32_t refreshRva;
	uint32_t keyTable; // u32 virtual-key codes, 0x60 entries
};
const PromptLayout kPromptLayouts[] = {
	{0x448110, 0x1372d78}, // LP2DX11.exe
	{0x438cc0, 0x1362ba8}, // LP2DX9.exe
};
// sub esp,0x10; push ebx; push esi; push edi; mov edi,ecx
const uint8_t kRefreshPrologue[] = {0x83, 0xec, 0x10, 0x53, 0x56, 0x57, 0x8b, 0xf9};
const int kIconNone = 0x8d;

const PromptLayout* g_prompt = nullptr;
using RefreshFn = void(__attribute__((thiscall)) *)(uint8_t*);
RefreshFn g_refresh = nullptr; // trampoline to the game's refresh
uint8_t* volatile g_messageSystem = nullptr;
int g_promptContext = kFoot;

int IconOfKey(int vk)
{
	switch (vk) {
	case VK_LBUTTON:
		return 0x84;
	case VK_RBUTTON:
		return 0x85;
	case VK_MBUTTON:
		return 0x86;
	}
	const uint32_t* table = (const uint32_t*)(uintptr_t)g_prompt->keyTable;
	for (int i = 0; i < 0x60; i++) {
		if ((int)table[i] == vk) {
			return 0x24 + i;
		}
	}
	return kIconNone;
}

void ApplyPromptRules(uint8_t* system)
{
	const std::vector<Rule>& rules = g_rules[g_promptContext];
	uint8_t** tags = *(uint8_t***)(system + 0x4c4);
	int count = *(int*)(system + 0x4b8);
	if (rules.empty() || !tags || count <= 0) {
		return;
	}
	if (count > 64) {
		count = 64;
	}
	int icons[64];
	for (int i = 0; i < count; i++) {
		icons[i] = tags[i] ? *(int*)(tags[i] + 0xc) : kIconNone;
	}
	for (const Rule& r : rules) {
		int dest = r.dest->slot;
		if (dest < 0 || dest >= count || !tags[dest]) {
			continue;
		}
		int source = r.source ? r.source->slot : -1;
		*(int*)(tags[dest] + 0xc) = r.vk ? IconOfKey(r.vk) : (source >= 0 && source < count ? icons[source] : kIconNone);
	}
	system[0x47c] = 1;
}

void __attribute__((thiscall)) PromptRefresh(uint8_t* system)
{
	g_messageSystem = system;
	g_refresh(system);
	ApplyPromptRules(system);
}

void PromptContext(Context context)
{
	if (context != g_promptContext && g_messageSystem && g_refresh) {
		g_promptContext = context;
		PromptRefresh(g_messageSystem);
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
				r.down = front && (r.vk >= 0x100 ? MousePressed(r.vk - 0x100) : (GetAsyncKeyState(r.vk) & 0x8000) != 0);
			}
		}
	}
	Context context = CurrentContext(pad);
	PromptContext(context);
	const std::vector<Rule>& rules = g_rules[context];
	uint64_t* sets = (uint64_t*)(self + kSetsOffset);
	uint64_t before = sets[0];
	// Zoom steps (scoped): a press of the source, before the rules clear its bits.
	for (const Rule& r : rules) {
		bool in = strcmp(r.dest->name, "zoom_in") == 0;
		if (!in && strcmp(r.dest->name, "zoom_out") != 0) {
			continue;
		}
		bool pressed = r.source ? (sets[1] & r.source->test) != 0 : r.vk >= 0x100 ? r.down : (r.down && !r.wasDown);
		if (pressed) {
			ZoomRequest(in ? 1 : -1);
		}
	}
	if (!rules.empty()) {
		uint64_t extraClear = 0;
		for (const Rule& r : rules) {
			if (r.vk) {
				extraClear |= NativeClear(context, r.vk);
			}
		}
		ApplyRules(sets, rules, extraClear);
	}
	// With "log": true, every change of the held bits in a VS (game's, then after the rules), to
	// map buttons that are not known yet.
	static uint64_t lastHeld = 0;
	if (g_settings.log && context != kFoot && before != lastHeld) {
		lastHeld = before;
		LOG_INFO("%s held: game w1 %08x w2 %08x -> w1 %08x w2 %08x.", kContextNames[context], (uint32_t)before, (uint32_t)(before >> 32), (uint32_t)sets[0], (uint32_t)(sets[0] >> 32));
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
	g_optionsPtr = site == base + kSiteRvas[0] ? kOptionsPtrs[0] : kOptionsPtrs[1];
	g_slotBits = site == base + kSiteRvas[0] ? kSlotBits[0] : kSlotBits[1];
	if (site == base + kSiteRvas[0]) {
		g_inputPtr = 0x18de7f4; // LP2DX11.exe
		g_mouseFn = 0x860f70;
	}
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

bool PromptsTryInstall(unsigned char* base)
{
	uint8_t* fn = nullptr;
	for (const PromptLayout& L : kPromptLayouts) {
		if (InImage(base, L.refreshRva, sizeof(kRefreshPrologue)) && memcmp(base + L.refreshRva, kRefreshPrologue, sizeof(kRefreshPrologue)) == 0) {
			fn = base + L.refreshRva;
			g_prompt = &L;
			break;
		}
	}
	if (!fn) {
		return false;
	}
	auto* tramp = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tramp) {
		return false;
	}
	memcpy(tramp, kRefreshPrologue, sizeof(kRefreshPrologue));
	tramp[8] = 0xe9;
	int32_t back = (int32_t)((fn + 8) - (tramp + 13));
	memcpy(tramp + 9, &back, 4);
	FlushInstructionCache(GetCurrentProcess(), tramp, 16);
	g_refresh = (RefreshFn)tramp;
	uint8_t patch[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
	int32_t rel = (int32_t)((uint8_t*)&PromptRefresh - (fn + 5));
	memcpy(patch + 1, &rel, 4);
	if (!g_host->write_code(fn, patch, sizeof(patch))) {
		return false;
	}
	LOG_INFO("prompts: refresh hooked at 0x%p.", fn);
	return true;
}

// --- For the menu.

int ControlsActionSlot(const char* context, const char* name)
{
	for (int c = 0; c < kContextCount; c++) {
		if (_stricmp(context, kContextNames[c]) == 0) {
			const Action* a = FindAction((Context)c, name);
			return a ? a->slot : -1;
		}
	}
	return -1;
}

int ControlsRuleFor(const char* context, const char* dest, int* sourceSlot)
{
	for (const Remap& r : g_settings.remaps) {
		if (_stricmp(r.context.c_str(), context) == 0 && _stricmp(r.dest.c_str(), dest) == 0) {
			int vk = ParseKey(r.source);
			if (vk > 0) {
				return vk;
			}
			*sourceSlot = ControlsActionSlot(context, r.source.c_str());
			return 0;
		}
	}
	return -1;
}

std::string ControlsKeyName(int vk)
{
	char text[16];
	if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
		snprintf(text, sizeof(text), "key:%c", vk);
		return text;
	}
	if (vk >= VK_F1 && vk <= VK_F24) {
		snprintf(text, sizeof(text), "key:F%d", vk - VK_F1 + 1);
		return text;
	}
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
		snprintf(text, sizeof(text), "key:NUM%d", vk - VK_NUMPAD0);
		return text;
	}
	for (const KeyName& k : kKeyNames) {
		if (k.vk == vk) {
			return std::string("key:") + k.name;
		}
	}
	snprintf(text, sizeof(text), "key:0x%02X", vk);
	return text;
}

void ControlsSetKey(const char* context, const char* dest, int vk)
{
	auto& remaps = g_settings.remaps;
	for (size_t i = 0; i < remaps.size(); i++) {
		if (_stricmp(remaps[i].context.c_str(), context) == 0 && _stricmp(remaps[i].dest.c_str(), dest) == 0) {
			remaps.erase(remaps.begin() + i);
			break;
		}
	}
	if (vk > 0) {
		remaps.push_back({context, dest, ControlsKeyName(vk)});
	}
	for (std::vector<Rule>& list : g_rules) {
		list.clear();
	}
	ControlsLoadRules();
	SaveSettings();
	if (g_messageSystem && g_refresh) {
		PromptRefresh(g_messageSystem);
	}
}

bool ControlsMousePressed(int code)
{
	return MousePressed(code);
}

}
