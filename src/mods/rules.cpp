#include "mods/lp2.h"
#include "mods/package.h"
#include "mods/parts.h"

#include "api/xsession.h"
#include "core/config.h"
#include "core/log.h"
#include "core/memberdata.h"
#include "core/steam.h"
#include "core/utils.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

// Extra match rules the stock game doesn't have. The host's rules hold in its lobby: each player
// publishes its own as member data "rules" ("ammo=1;grenades=0"), and everyone applies the lobby
// owner's (outside lobbies, their own). Each player's weapons run on its own PC, so every player
// applies them locally.
//
// Ammo: the weapon's add-ammo function (DX11 FUN_00c7aa20, thiscall(weapon, s16 delta) -> ok) takes
// a shot / throw as a negative delta off the magazine (+0x20fc) and the total (+0x20fe; DX9 both
// -0x10). Weapon kind at +0x1e: 8 and 0x15-0x19 are grenades (magazine = total).
//   Unlimited ammo: the total is put back after a shot, so the magazine still empties and reloads.
//   Unlimited grenades: both are put back.
//
// Secondary: a player spawns (DX11 FUN_008fb130, thiscall(spawner, bool remote, ...), player index at
// spawner+0xb0) with the weapons whose ids are in its player info (game->+0x28[index]) at +0x10 +
// slot * 4: slot 0 the weapon in hand, 1 grenades, 7 the carried second weapon (empty at spawn;
// picking a weapon up moves the one in hand there). An empty slot 7 gets the secondary's id first.
// Ids are 0x04KK0000, KK the weapon kind (0x0c sidearm).
//
// Create-match rows: the rules bytes (0x38, saved with the match settings) use offsets up to 0x2d;
// ours are 0x2e secondary (0 none, then kSecondaryKinds), 0x2f unlimited ammo, 0x30 unlimited grenades. The
// row table (DX11 0x17e9840) holds per category (16 x 0x23 bytes: main, sub, then per page a row
// count and 10 rules offsets) which offset each row edits; the Settings 1 page (page 1, 7 rows in 14
// categories) gets ours as rows 7-9. The Flash (id221) has its own rows: OnlineRule.sub1ItemArray
// [category] lists item ids, offsetArray[item] = [MsgOnline value base, DataNo]; the label is
// MsgOnline 38 + item. Ours are items 500-502 (labels 538-540, served by RulesMessage), values
// pushed as DataNo 75-77 next to the game's rules.

namespace xls::mods {

namespace {

//
// The HUD shows the reserve through DX11 FUN_00c7a820 (thiscall(weapon, bool raw) -> s16): not raw,
// it answers -1 (drawn as the infinity sign, like the sidearm's) for weapons whose data says
// "infinite reserve"; with a rule on it answers -1 for the weapon too. The HUD's counter draw (DX11
// FUN_00eca930, thiscall(hud, slot, icon, count, style, counterOnly)) shows the infinity sign by
// style 6 (from a per-icon table), not by the count, so a count of -1 gets style 6.
struct RulesVas {
	uint32_t addAmmo;
	uint32_t reserve;
	uint32_t counter;
	uint32_t magazine; // the total follows it
	uint32_t spawn;
	uint32_t pushRules; // the create screen's push of every rule to its Flash: thiscall(screen, rules)
	uint32_t setNumber; // the screen's Flash number setter: thiscall(screen, dataNo, value)
	uint32_t rowTable;  // create-match rows (see RowTable)
};
const RulesVas kRulesVas[] = {
	{0xc7aa20, 0xc7a820, 0xeca930, 0x20fc, 0x8fb130, 0xe8c270, 0xf4a330, 0x17e9840}, // DX11
	{0xc6ae10, 0xc6ac10, 0xebab30, 0x20ec, 0x8eb7d0, 0xe7c1e0, 0xf3a600, 0x17d97e0}, // DX9
};
// push esi; push edi; mov edi,[esp+0xc]; test di,di; (then jge rel32, left in place)
const uint8_t kAddAmmoPrologue[] = {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x66, 0x85, 0xff};
const uint8_t kAddAmmoNext[] = {0x0f, 0x8d};
// cmp byte [esp+4],0 (then jne rel8, left in place)
const uint8_t kReservePrologue[] = {0x80, 0x7c, 0x24, 0x04, 0x00};
// sub esp,0x7c; push ebp; mov ebp,ecx
const uint8_t kCounterPrologue[] = {0x83, 0xec, 0x7c, 0x55, 0x8b, 0xe9};
const int kStyleInfinite = 6;

const int kOffsetSecondary = 0x2e;
const int kOffsetAmmo = 0x2f;
const int kOffsetGrenades = 0x30;
const int kRows = 3;
const int kRowOffsets[kRows] = {kOffsetSecondary, kOffsetAmmo, kOffsetGrenades};
// secondary values: none, then the weapon kinds of kSecondaryKinds
const int kSecondaryKinds[] = {0, 0x0c, 0x30, 0x31, 0x32, 0x33, 0x34};
const char* const kSecondaryNames[] = {"None", "Sidearm", "Standard", "Short", "Long", "Heavy", "Special"};
const int kSecondaryValues = (int)(sizeof(kSecondaryKinds) / sizeof(kSecondaryKinds[0]));
const int kRowValues[kRows] = {kSecondaryValues, 2, 2};
const int kRowItems[kRows] = {500, 501, 502};     // Flash item ids
const int kRowDataNo[kRows] = {75, 76, 77};
const int kMsgItemName = 38;                      // MsgOnline label of item i: 38 + i
const int kMsgSecondary = 600;                    // our value texts
const int kMsgOffOn = 362;                        // MsgOnline MSG_COMMON_OFF (Off, On)
const int kPageSettings1 = 1;
const int kStockRows = 7;

struct Rules {
	bool ammo = false;
	bool grenades = false;
	int secondary = 0; // weapon kind, 0 = none
};
const int kSlotSecond = 7;

std::mutex g_mutex;
std::map<uint64_t, Rules> g_memberRules;
std::atomic<bool> g_ammo{false};
std::atomic<bool> g_grenades{false};
std::atomic<int> g_secondary{0};
uint32_t g_magazine = 0;

std::string Format(const Rules& r)
{
	return FormatA("ammo=%d;grenades=%d;secondary=%d", r.ammo, r.grenades, r.secondary);
}

Rules Parse(const std::string& s)
{
	Rules r;
	r.ammo = s.find("ammo=1") != std::string::npos;
	r.grenades = s.find("grenades=1") != std::string::npos;
	size_t at = s.find("secondary=");
	r.secondary = at == std::string::npos ? 0 : atoi(s.c_str() + at + 10) & 0xff;
	return r;
}

std::atomic<bool> g_menuSeen{false};
std::atomic<int> g_menu[kRows];

Rules Own()
{
	Rules r;
	if (g_menuSeen) {
		r.ammo = g_menu[1] != 0;
		r.grenades = g_menu[2] != 0;
		r.secondary = kSecondaryKinds[g_menu[0] % kSecondaryValues];
		return r;
	}
	r.ammo = Cfg().rulesUnlimitedAmmo;
	r.grenades = Cfg().rulesUnlimitedGrenades;
	for (int i = 0; i < kSecondaryValues; i++) {
		if (_stricmp(Cfg().rulesSecondary.c_str(), kSecondaryNames[i]) == 0) {
			r.secondary = kSecondaryKinds[i];
		}
	}
	return r;
}

// Once a second: the lobby owner's rules (ours outside lobbies or until the owner's arrive).
DWORD WINAPI SyncThread(LPVOID)
{
	std::string logged, published = Format(Own());
	for (;; Sleep(1000)) {
		Rules r = Own();
		if (Format(r) != published) {
			published = Format(r);
			MemberDataSet("rules", published);
			Bytes save(published.begin(), published.end());
			WriteWholeFile(Cfg().modCacheDir + L"rules.txt", save);
		}
		CSteamID lobby = SessionPresenceLobby();
		if (lobby.IsValid() && SteamReady() && SteamMatchmaking()) {
			uint64_t host = LobbyOwner(SteamMatchmaking(), lobby).ConvertToUint64();
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = g_memberRules.find(host);
			if (host != SteamLocalId().ConvertToUint64() && it != g_memberRules.end()) {
				r = it->second;
			}
		}
		g_ammo = r.ammo;
		g_grenades = r.grenades;
		g_secondary = r.secondary;
		std::string now = Format(r);
		if (now != logged) {
			logged = now;
			XLS_LOG_INFO("mods: rules: unlimited ammo %s, unlimited grenades %s, secondary kind 0x%02x.", r.ammo ? "on" : "off", r.grenades ? "on" : "off", r.secondary);
		}
	}
}

using AddAmmoFn = uint32_t(__attribute__((thiscall)) *)(uint8_t*, int16_t);
AddAmmoFn g_addAmmoOriginal = nullptr;

bool Grenade(const uint8_t* weapon)
{
	uint8_t kind = weapon[0x1e];
	return kind == 8 || (kind >= 0x15 && kind <= 0x19);
}

using ReserveFn = uint32_t(__attribute__((thiscall)) *)(uint8_t*, uint32_t);
ReserveFn g_reserveOriginal = nullptr;

uint32_t __attribute__((thiscall)) Reserve(uint8_t* weapon, uint32_t raw)
{
	// not grenades: their counter works the count out from this and shows 0 for -1 (no infinity there)
	if (!(raw & 0xff) && weapon && !Grenade(weapon) && g_ammo) {
		return 0xffffffff;
	}
	return g_reserveOriginal(weapon, raw);
}

using CounterFn = void(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t, int32_t, uint32_t, uint32_t);
CounterFn g_counterOriginal = nullptr;

void __attribute__((thiscall)) Counter(void* hud, uint32_t slot, uint32_t icon, int32_t count, uint32_t style, uint32_t counterOnly)
{
	g_counterOriginal(hud, slot, icon, count, count < 0 ? kStyleInfinite : style, counterOnly);
}

// sub esp,0x10; push ebp; push esi; mov esi,ecx; then mov al,[esi+0xb0]
const uint8_t kSpawnPrologue[] = {0x83, 0xec, 0x10, 0x55, 0x56, 0x8b, 0xf1};
const uint8_t kSpawnIndex[] = {0x8a, 0x86, 0xb0, 0x00, 0x00, 0x00};
const size_t kSpawnGameAt = 0x1f; // mov ecx,[game] (8b 0d <addr>) at +0x1d
const uint8_t kSpawnGameOp[] = {0x8b, 0x0d};
using SpawnFn = uint32_t(__attribute__((thiscall)) *)(uint8_t*, uint32_t, void*, void*);
SpawnFn g_spawnOriginal = nullptr;
uint8_t** g_game = nullptr;

uint32_t __attribute__((thiscall)) Spawn(uint8_t* spawner, uint32_t remote, void* a, void* b)
{
	static uint32_t given[16]; // the id we put in each player's slot 7 (it stays in the player info)
	int kind = g_secondary;
	uint8_t index = spawner ? spawner[0xb0] : 0xff;
	if (!(remote & 0xff) && index < 16 && *g_game) {
		auto* info = *(uint8_t**)(*g_game + 0x28 + index * 4);
		auto* second = info ? (uint32_t*)(info + 0x10 + kSlotSecond * 4) : nullptr;
		uint32_t want = kind ? 0x04000000u | (uint32_t)kind << 16 : 0;
		if (second && given[index] && *second == given[index] && *second != want) {
			*second = 0; // ours from an earlier match with other rules
			given[index] = 0;
		}
		if (second && want && *second == 0) {
			*second = given[index] = want;
			XLS_LOG_INFO("mods: rules: player %u spawns with secondary 0x%08x.", index, want);
		}
	}
	return g_spawnOriginal(spawner, remote, a, b);
}

using PushRulesFn = void(__attribute__((thiscall)) *)(void*, uint8_t*);
using SetNumberFn = void(__attribute__((thiscall)) *)(void*, uint32_t, uint32_t);
PushRulesFn g_pushRulesOriginal = nullptr;
SetNumberFn g_setNumber = nullptr;
const uint8_t kPushRulesPrologue[] = {0x83, 0xec, 0x20, 0x53, 0x55, 0x56}; // sub esp,0x20; push ebx; push ebp; push esi

void __attribute__((thiscall)) PushRules(void* screen, uint8_t* rules)
{
	g_pushRulesOriginal(screen, rules);
	if (!rules) {
		return;
	}
	for (int i = 0; i < kRows; i++) {
		uint8_t v = rules[kRowOffsets[i]];
		if (v >= kRowValues[i]) {
			v = rules[kRowOffsets[i]] = 0; // saved before these rows existed
		}
		g_menu[i] = v;
		g_setNumber(screen, kRowDataNo[i], v);
	}
	g_menuSeen = true;
}

bool PatchRowTable(uint8_t* table)
{
	DWORD old = 0;
	if (!VirtualProtect(table, 16 * 0x23, PAGE_READWRITE, &old)) {
		return false;
	}
	int patched = 0;
	for (int c = 0; c < 16 && table[c * 0x23] != 0xff; c++) {
		uint8_t* page = table + c * 0x23 + 2 + kPageSettings1 * 0xb;
		if (page[0] == kStockRows) {
			for (int i = 0; i < kRows; i++) {
				page[1 + kStockRows + i] = (uint8_t)kRowOffsets[i];
			}
			page[0] = kStockRows + kRows;
			patched++;
		}
	}
	VirtualProtect(table, 16 * 0x23, old, &old);
	XLS_LOG_INFO("mods: rules: %d create-match categories got the rule rows.", patched);
	return patched > 0;
}

uint32_t __attribute__((thiscall)) AddAmmo(uint8_t* weapon, int16_t delta)
{
	if (delta >= 0 || !weapon) {
		return g_addAmmoOriginal(weapon, delta);
	}
	bool grenade = Grenade(weapon);
	if (!(grenade ? g_grenades : g_ammo)) {
		return g_addAmmoOriginal(weapon, delta);
	}
	auto* magazine = (int16_t*)(weapon + g_magazine);
	int16_t loaded = magazine[0];
	int16_t total = magazine[1];
	uint32_t ok = g_addAmmoOriginal(weapon, delta);
	if (grenade) {
		magazine[0] = loaded;
	}
	magazine[1] = total;
	return ok;
}

}

void RulesInstall(unsigned char* base)
{
	int build = -1;
	for (int b = 0; b < 2 && build < 0; b++) {
		const uint8_t* add = base + (kRulesVas[b].addAmmo - 0x400000);
		const uint8_t* reserve = base + (kRulesVas[b].reserve - 0x400000);
		if (memcmp(add, kAddAmmoPrologue, sizeof(kAddAmmoPrologue)) == 0 && memcmp(add + sizeof(kAddAmmoPrologue), kAddAmmoNext, sizeof(kAddAmmoNext)) == 0 &&
		    memcmp(reserve, kReservePrologue, sizeof(kReservePrologue)) == 0 &&
		    memcmp(base + (kRulesVas[b].counter - 0x400000), kCounterPrologue, sizeof(kCounterPrologue)) == 0 &&
		    memcmp(base + (kRulesVas[b].spawn - 0x400000), kSpawnPrologue, sizeof(kSpawnPrologue)) == 0 &&
		    memcmp(base + (kRulesVas[b].spawn - 0x400000) + sizeof(kSpawnPrologue), kSpawnIndex, sizeof(kSpawnIndex)) == 0 &&
		    memcmp(base + (kRulesVas[b].spawn - 0x400000) + kSpawnGameAt - 2, kSpawnGameOp, sizeof(kSpawnGameOp)) == 0) {
			build = b;
		}
	}
	if (build < 0) {
		XLS_LOG_WARN("mods: rules: add-ammo code not recognised, unlimited ammo / grenades unavailable.");
		return;
	}
	g_magazine = kRulesVas[build].magazine;
	g_addAmmoOriginal = (AddAmmoFn)Detour(base + (kRulesVas[build].addAmmo - 0x400000), kAddAmmoPrologue, sizeof(kAddAmmoPrologue), (void*)&AddAmmo);
	g_reserveOriginal = (ReserveFn)Detour(base + (kRulesVas[build].reserve - 0x400000), kReservePrologue, sizeof(kReservePrologue), (void*)&Reserve);
	g_counterOriginal = (CounterFn)Detour(base + (kRulesVas[build].counter - 0x400000), kCounterPrologue, sizeof(kCounterPrologue), (void*)&Counter);
	g_setNumber = (SetNumberFn)(uintptr_t)kRulesVas[build].setNumber;
	g_pushRulesOriginal = (PushRulesFn)Detour(base + (kRulesVas[build].pushRules - 0x400000), kPushRulesPrologue, sizeof(kPushRulesPrologue), (void*)&PushRules);
	if (g_pushRulesOriginal) {
		PatchRowTable(base + (kRulesVas[build].rowTable - 0x400000));
	}
	uint8_t* spawn = base + (kRulesVas[build].spawn - 0x400000);
	g_game = *(uint8_t***)(spawn + kSpawnGameAt);
	g_spawnOriginal = (SpawnFn)Detour(spawn, kSpawnPrologue, sizeof(kSpawnPrologue), (void*)&Spawn);
	if (!g_addAmmoOriginal || !g_reserveOriginal || !g_counterOriginal || !g_spawnOriginal) {
		XLS_LOG_WARN("mods: rules: add-ammo hook failed.");
		return;
	}
	MemberDataListen("rules", [](uint64_t steamId, XUID, const std::string& value) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_memberRules[steamId] = Parse(value);
	});
	Bytes saved;
	if (ReadWholeFile(Cfg().modCacheDir + L"rules.txt", &saved)) {
		Rules r = Parse(std::string(saved.begin(), saved.end()));
		for (int i = 0; i < kSecondaryValues; i++) {
			g_menu[0] = kSecondaryKinds[i] == r.secondary ? i : (int)g_menu[0];
		}
		g_menu[1] = r.ammo;
		g_menu[2] = r.grenades;
		g_menuSeen = true;
	}
	MemberDataSet("rules", Format(Own()));
	HANDLE t = CreateThread(nullptr, 0, SyncThread, nullptr, 0, nullptr);
	if (t) {
		CloseHandle(t);
	}
	XLS_LOG_INFO("mods: rules: hooked (%s).", build == 0 ? "DX11" : "DX9");
}


bool RulesStep(int rule, int key, uint8_t* value)
{
	for (int i = 0; i < kRows; i++) {
		if (rule == kRowOffsets[i] && (key == 10 || key == 11)) {
			*value = (uint8_t)((*value + (key == 10 ? 1 : kRowValues[i] - 1)) % kRowValues[i]);
			g_menu[i] = *value;
			g_menuSeen = true;
			return true;
		}
	}
	return false;
}

const char* RulesMessage(uint32_t id)
{
	if (id >= (uint32_t)kMsgSecondary && id < (uint32_t)(kMsgSecondary + kSecondaryValues)) {
		return kSecondaryNames[id - kMsgSecondary];
	}
	switch (id) {
	case kMsgItemName + 500: return "Default Secondary";
	case kMsgItemName + 501: return "Unlimited Ammo";
	case kMsgItemName + 502: return "Unlimited Grenades";
	default: return nullptr;
	}
}

namespace {

// AVM1 bytecode for the rows (see the top of the file).
struct Avm1 {
	Bytes code;
	void Push(std::initializer_list<int> values)
	{
		Bytes p;
		for (int v : values) {
			p.push_back(7); // integer
			for (int k = 0; k < 4; k++) {
				p.push_back((uint8_t)(v >> (k * 8)));
			}
		}
		Action(0x96, p);
	}
	void Push(const char* text)
	{
		Bytes p{0}; // string
		p.insert(p.end(), text, text + strlen(text) + 1);
		Action(0x96, p);
	}
	void Action(uint8_t op, const Bytes& payload = Bytes())
	{
		code.push_back(op);
		if (op >= 0x80) {
			code.push_back((uint8_t)payload.size());
			code.push_back((uint8_t)(payload.size() >> 8));
			code.insert(code.end(), payload.begin(), payload.end());
		}
	}
};

Bytes RowsScript()
{
	const uint8_t kGetVariable = 0x1c, kGetMember = 0x4e, kSetMember = 0x4f, kCallMethod = 0x52, kPop = 0x17, kInitArray = 0x42;
	Avm1 a;
	for (int c = 0; c < 14; c++) { // the categories with the 7 stock rows (the C++ table agrees)
		a.Push({kRowItems[2], kRowItems[1], kRowItems[0], kRows});
		a.Push("OnlineRule");
		a.Action(kGetVariable);
		a.Push("sub1ItemArray");
		a.Action(kGetMember);
		a.Push({c});
		a.Action(kGetMember);
		a.Push("push");
		a.Action(kCallMethod);
		a.Action(kPop);
	}
	const int bases[kRows] = {kMsgSecondary, kMsgOffOn, kMsgOffOn};
	for (int i = 0; i < kRows; i++) {
		a.Push("OnlineRule");
		a.Action(kGetVariable);
		a.Push("offsetArray");
		a.Action(kGetMember);
		a.Push({kRowItems[i], kRowDataNo[i], bases[i], 2});
		a.Action(kInitArray);
		a.Action(kSetMember);
	}
	a.code.push_back(0); // ActionEnd
	return a.code;
}

}

// Inserts a DoAction with RowsScript before the root timeline's first ShowFrame.
bool RulesPatchMenuFlash(std::vector<uint8_t>* gfx)
{
	Bytes& f = *gfx;
	if (f.size() < 16 || memcmp(f.data(), "GFX", 3) != 0) {
		return false;
	}
	size_t at = 8;
	int nbits = f[at] >> 3;
	at += (5 + 4 * nbits + 7) / 8 + 4; // frame rect, rate, count
	while (at + 2 <= f.size()) {
		uint16_t head = (uint16_t)(f[at] | f[at + 1] << 8);
		uint32_t code = head >> 6, length = head & 0x3f;
		size_t body = at + 2;
		if (length == 0x3f) {
			if (body + 4 > f.size()) {
				return false;
			}
			length = *(const uint32_t*)&f[body];
			body += 4;
		}
		if (code == 0) {
			return false;
		}
		if (code == 1) { // ShowFrame
			Bytes script = RowsScript();
			Bytes tag;
			uint16_t h = (uint16_t)(12 << 6 | 0x3f); // DoAction, long form
			tag.push_back((uint8_t)h);
			tag.push_back((uint8_t)(h >> 8));
			for (int k = 0; k < 4; k++) {
				tag.push_back((uint8_t)(script.size() >> (k * 8)));
			}
			tag.insert(tag.end(), script.begin(), script.end());
			f.insert(f.begin() + at, tag.begin(), tag.end());
			*(uint32_t*)&f[4] = (uint32_t)f.size();
			XLS_LOG_INFO("mods: rules: create-match rows added to the menu Flash (%zu bytes of script).", script.size());
			return true;
		}
		at = body + length;
	}
	return false;
}

}
