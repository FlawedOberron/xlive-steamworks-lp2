#include "mods/lp2.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace xls::mods {

namespace {

// A code patch located by byte pattern (mask 0 = wildcard) once SteamStub has decrypted .text.
struct CodePatch {
	const char* name;
	const bool Config::*enabled;
	const uint8_t* bytes;
	const uint8_t* mask;
	size_t size;
	// Where the pattern starts in the known builds (RVA), so waiting for decryption stays cheap.
	uint32_t knownRvas[2]; // LP2DX11.exe, LP2DX9.exe
	// Bytes to write: offset into the pattern and replacement.
	struct Write {
		size_t offset;
		uint8_t bytes[8];
		size_t size;
	} writes[2];
	size_t writeCount;
	bool done;
};

// Boot-logo state machine (DX11 FUN_008acfb0) state 0:
//   mov ecx,[abs]; cmp byte [ecx+0x1829],0; jz show_logos; pop edi; mov byte [esi+4],6
// NOPing the jz makes state 0 always continue to state 6 (logos done, title screen).
const uint8_t kSplashBytes[] = {0x8b, 0x0d, 0, 0, 0, 0, 0x80, 0xb9, 0x29, 0x18, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xec, 0x00, 0x00, 0x00, 0x5f, 0xc6, 0x46, 0x04, 0x06};
const uint8_t kSplashMask[] = {1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

// Lobby host input (DX11 FUN_00e969f0): on confirm, the host may start only when the lobby has
// more members than the minimum (1, or 2 when teams are fixed):
//   mov ecx,[esi+0x5b0]; mov eax,1; jz +5; mov eax,2; cmp ecx,eax; jle ignore
// Zeroing both minimums lets a lone host start (a private test box).
const uint8_t kSoloBytes[] = {0x8b, 0x8e, 0xb0, 0x05, 0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x74, 0x05, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x3b, 0xc8, 0x0f, 0x8e};
const uint8_t kSoloMask[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

// Match start (DX11 FUN_00e97690): counts the occupied member slots and refuses with alert 0x66
// unless more than one: cmp ecx,ebx; jg ok; mov ecx,[mgr]; push ebx; push edx; push 2; push 0x66.
const uint8_t kSoloSlotsBytes[] = {0x05, 0xf0, 0x01, 0x00, 0x00, 0x2b, 0xd3, 0x75, 0, 0x3b, 0xcb, 0x7f, 0, 0x8b, 0x0d, 0, 0, 0, 0, 0x53, 0x52, 0x6a, 0x02, 0x6a, 0x66};
const uint8_t kSoloSlotsMask[] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1};
// Match start (DX11 FUN_00e9af08 area): network member count must exceed one:
// cmp dword [eax+0xa208],1; jg ok; mov ecx,[mgr]; push 1; push ebx; push 2; push 0x66.
const uint8_t kSoloNetBytes[] = {0x83, 0xb8, 0x08, 0xa2, 0x00, 0x00, 0x01, 0x7f, 0, 0x8b, 0x0d, 0, 0, 0, 0, 0x6a, 0x01, 0x53, 0x6a, 0x02, 0x6a, 0x66};
const uint8_t kSoloNetMask[] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1};

CodePatch g_patches[] = {
	{"skip_splash", &Config::modSkipSplash, kSplashBytes, kSplashMask, sizeof(kSplashBytes), {0x4ad03f - 13, 0x49d88f - 13},
	 {{13, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, 6}}, 1, false},
	{"solo_start", &Config::modSoloStart, kSoloBytes, kSoloMask, sizeof(kSoloBytes), {0xa96c87, 0xa86bb7},
	 {{7, {0x00}, 1}, {14, {0x00}, 1}}, 2, false},
	{"solo_start (slots)", &Config::modSoloStart, kSoloSlotsBytes, kSoloSlotsMask, sizeof(kSoloSlotsBytes), {0xa976c6, 0xa875f6},
	 {{11, {0xeb}, 1}}, 1, false},
	{"solo_start (members)", &Config::modSoloStart, kSoloNetBytes, kSoloNetMask, sizeof(kSoloNetBytes), {0xa9af70, 0xa8aea0},
	 {{7, {0xeb}, 1}}, 1, false},
};

bool Matches(const uint8_t* at, const CodePatch& p)
{
	for (size_t i = 0; i < p.size; i++) {
		if (p.mask[i] && at[i] != p.bytes[i]) {
			return false;
		}
	}
	return true;
}

bool TextRange(uint8_t* base, uint8_t** start, size_t* size)
{
	auto* dos = (IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return false;
	}
	auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
		if (memcmp(section->Name, ".text", 6) == 0) {
			*start = base + section->VirtualAddress;
			*size = section->Misc.VirtualSize;
			return true;
		}
	}
	return false;
}

bool Write(void* at, const void* bytes, size_t size)
{
	DWORD old = 0;
	if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) {
		return false;
	}
	memcpy(at, bytes, size);
	VirtualProtect(at, size, old, &old);
	FlushInstructionCache(GetCurrentProcess(), at, size);
	return true;
}

void Apply(CodePatch& p, uint8_t* site)
{
	for (size_t i = 0; i < p.writeCount; i++) {
		if (!Write(site + p.writes[i].offset, p.writes[i].bytes, p.writes[i].size)) {
			XLS_LOG_ERROR("mods: %s: VirtualProtect failed (%lu).", p.name, GetLastError());
			p.done = true;
			return;
		}
	}
	XLS_LOG_INFO("mods: %s: patched at 0x%p.", p.name, site);
	p.done = true;
}

// --- game_log: the game's debug logger (a global object whose print methods are empty stubs in
// the retail build) is pointed at our log. DX11 only for now: logger pointer and vtable slots.
const uintptr_t kLoggerPtrDx11 = 0x18de398;
const int kLoggerSlots[] = {0x20, 0x24, 0x28};
uint8_t* g_rdataStart = nullptr;
uint8_t* g_rdataEnd = nullptr;

int __cdecl GameLog(void*, const char* format, ...)
{
	// Only trust format strings that live in the game's read-only data.
	if ((uint8_t*)format < g_rdataStart || (uint8_t*)format >= g_rdataEnd) {
		return 0;
	}
	char text[1024];
	va_list args;
	va_start(args, format);
	_vsnprintf(text, sizeof(text) - 1, format, args);
	va_end(args);
	text[sizeof(text) - 1] = 0;
	size_t n = strlen(text);
	while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) {
		text[--n] = 0;
	}
	// The voice engine reports the same failure every frame; log repeats once with a count.
	static char last[1024];
	static unsigned repeats = 0;
	static CRITICAL_SECTION* lock = [] {
		auto* cs = new CRITICAL_SECTION;
		InitializeCriticalSection(cs);
		return cs;
	}();
	EnterCriticalSection(lock);
	if (strcmp(text, last) == 0) {
		repeats++;
		LeaveCriticalSection(lock);
		return 0;
	}
	if (repeats) {
		XLS_LOG_INFO("game: (previous message repeated %u more times)", repeats);
	}
	repeats = 0;
	memcpy(last, text, sizeof(last));
	LeaveCriticalSection(lock);
	XLS_LOG_INFO("game: %s", text);
	return 0;
}

bool HookGameLog(uint8_t* base)
{
	auto* dos = (IMAGE_DOS_HEADER*)base;
	auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
		if (memcmp(section->Name, ".rdata", 7) == 0) {
			g_rdataStart = base + section->VirtualAddress;
			g_rdataEnd = g_rdataStart + section->Misc.VirtualSize;
		}
	}
	auto* logger = *(uint8_t**)kLoggerPtrDx11;
	if (!logger || !g_rdataStart) {
		return false; // not created yet
	}
	auto* vtable = *(uint8_t**)logger;
	if (vtable < g_rdataStart || vtable >= g_rdataEnd) {
		XLS_LOG_WARN("mods: game_log: logger vtable 0x%p is not in .rdata, not hooking.", vtable);
		return true;
	}
	static const uint8_t kStub[] = {0x33, 0xc0, 0xc3}; // xor eax,eax; ret
	int hooked = 0;
	for (int slot : kLoggerSlots) {
		auto** entry = (uint8_t**)(vtable + slot);
		if (memcmp(*entry, kStub, sizeof(kStub)) != 0) {
			continue;
		}
		void* hook = (void*)&GameLog;
		if (Write(entry, &hook, sizeof(hook))) {
			hooked++;
		}
	}
	XLS_LOG_INFO("mods: game_log: hooked %d logger methods.", hooked);
	return true;
}

// --- solo_start (in match): the match monitor (DX11 FUN_0100e1a0) asks FUN_01008be0 whether the
// match can go on and ends it with reason 7 ("not enough players / a team is empty"). The one call
// is redirected through MatchViable, which turns reason 7 into "fine" and passes every other reason
// (disconnects, host loss) through unchanged.
//   cmp [ebx+0x25f],cl; jne far; mov ecx,ebp; call check; mov esi,eax; test esi,esi; je far
const uint8_t kViableBytes[] = {0x38, 0x8b, 0x5f, 0x02, 0x00, 0x00, 0x0f, 0x85, 0, 0, 0, 0, 0x8b, 0xcd, 0xe8, 0, 0, 0, 0, 0x8b, 0xf0, 0x85, 0xf6, 0x0f, 0x84};
const uint8_t kViableMask[] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1};
const size_t kViableCall = 14;
const uint32_t kViableRvas[] = {0xc0e4c0, 0xbfe0e0}; // LP2DX11.exe, LP2DX9.exe
using ViableFn = int(__cdecl*)();
ViableFn g_viableOriginal = nullptr;
bool g_viableDone = false;

int __cdecl MatchViable()
{
	int reason = g_viableOriginal();
	return reason == 7 ? 0 : reason;
}

bool MatchesBytes(const uint8_t* at, const uint8_t* bytes, const uint8_t* mask, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		if (mask[i] && at[i] != bytes[i]) {
			return false;
		}
	}
	return true;
}

void HookViable(uint8_t* site)
{
	uint8_t* call = site + kViableCall;
	int32_t rel = *(int32_t*)(call + 1);
	g_viableOriginal = (ViableFn)(call + 5 + rel);
	int32_t newRel = (int32_t)((uint8_t*)&MatchViable - (call + 5));
	if (Write(call + 1, &newRel, sizeof(newRel))) {
		XLS_LOG_INFO("mods: solo_start (in match): hooked viability check 0x%p at 0x%p.", (void*)g_viableOriginal, call);
	}
	g_viableDone = true;
}

DWORD WINAPI Worker(LPVOID)
{
	ConfigEnsureLoaded();
	auto* base = (uint8_t*)GetModuleHandleW(nullptr);
	uint8_t* text = nullptr;
	size_t textSize = 0;
	if (!TextRange(base, &text, &textSize)) {
		XLS_LOG_WARN("mods: no .text section in the game module.");
		return 0;
	}
	char exeName[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exeName, MAX_PATH);
	bool dx11 = strstr(exeName, "LP2DX11") != nullptr;
	bool gameLogPending = Cfg().modGameLog && dx11;
	if (Cfg().modGameLog && !dx11) {
		XLS_LOG_WARN("mods: game_log is only mapped for LP2DX11.exe.");
	}

	size_t pending = 0;
	for (CodePatch& p : g_patches) {
		p.done = !(Cfg().*p.enabled);
		pending += p.done ? 0 : 1;
	}
	// SteamStub decrypts .text in place before jumping to the game's entry point, which may be
	// after DllMain returns. Poll the known sites until the patterns appear.
	g_viableDone = !Cfg().modSoloStart;
	bool mapsPending = MapsWanted();
	bool zoomPending = Cfg().modHoldZoom && dx11;
	if (Cfg().modHoldZoom && !dx11) {
		XLS_LOG_WARN("mods: hold_zoom is only mapped for LP2DX11.exe.");
	}
	for (int tick = 0; tick < 30000 && (pending || gameLogPending || !g_viableDone || mapsPending || zoomPending); tick++) {
		if (zoomPending && HoldZoomTryInstall(base)) {
			zoomPending = false;
		}
		if (mapsPending && MapsTryInstall(base)) {
			mapsPending = false;
		}
		if (!g_viableDone) {
			for (uint32_t rva : kViableRvas) {
				uint8_t* site = base + rva;
				if (site >= text && site + sizeof(kViableBytes) <= text + textSize && MatchesBytes(site, kViableBytes, kViableMask, sizeof(kViableBytes))) {
					HookViable(site);
					break;
				}
			}
		}
		for (CodePatch& p : g_patches) {
			if (p.done) {
				continue;
			}
			for (uint32_t rva : p.knownRvas) {
				uint8_t* site = base + rva;
				if (site >= text && site + p.size <= text + textSize && Matches(site, p)) {
					Apply(p, site);
					pending--;
					break;
				}
			}
		}
		if (gameLogPending && tick % 50 == 0 && HookGameLog(base)) {
			gameLogPending = false;
		}
		OverlayVerify();
		Sleep(1);
	}
	if (!g_viableDone) {
		for (uint8_t* at = text; at + sizeof(kViableBytes) <= text + textSize; at++) {
			if (MatchesBytes(at, kViableBytes, kViableMask, sizeof(kViableBytes))) {
				HookViable(at);
				break;
			}
		}
		if (!g_viableDone) {
			XLS_LOG_WARN("mods: solo_start (in match): viability check not found, matches will end at GO.");
		}
	}
	if (zoomPending) {
		XLS_LOG_WARN("mods: hold_zoom: zoom check not found in this build.");
	}
	if (mapsPending) {
		XLS_LOG_WARN("mods: maps: unknown game build, mod map slots not enabled.");
	}
	else {
		// The viability hook sits near the end of .text, so by now all of it is decrypted.
		MapsPatchPickers(base);
	}
	// Unknown build: by now the code is decrypted, so scan the whole section once.
	for (CodePatch& p : g_patches) {
		if (p.done) {
			continue;
		}
		uint8_t* found = nullptr;
		for (uint8_t* at = text; at + p.size <= text + textSize; at++) {
			if (Matches(at, p)) {
				found = at;
				break;
			}
		}
		if (found) {
			Apply(p, found);
		}
		else {
			XLS_LOG_WARN("mods: %s: code not found in this build, not applied.", p.name);
		}
	}
	return 0;
}

}

void Start()
{
	HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	}
}

}
