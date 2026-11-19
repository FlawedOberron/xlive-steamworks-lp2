#include "mods/lp2.h"

#include "core/log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The game's network messages to our log. The network library singleton (sNetworkLibraryXbox, DX11 DAT_018de398, DX9
// DAT_018ce2d8) has three print methods its ~2600 call sites use: vtable +0x20 (this, level, format, ...) for stats
// ("NetworkConnectionStable[%d] 1min send %d/%d recv %d"), +0x24 (this, format, ...) for errors ("CRC error") and +0x28
// (this, format, ...) for warnings ("recv buf over", "Upstream disconnected", "no receive timeout"). In the shipped
// game all three are "xor eax, eax; ret" (DX9 0x7a2ab0): the messages go nowhere. They're cdecl (varargs, the caller
// pops), so a copy of the object's vtable with those three slots pointing here, swapped in, logs them. Wanted for the
// System Link "Communication Error - Connection Lost" (2026-11-13: nothing of ours logged before the game left).
// A thread checks once a second (the library is made at startup and may be remade), and re-swaps when needed.

namespace xls::mods {

namespace {

const uint32_t kLibrary[kBuildCount] = {0x18de398, 0x18ce2d8, 0x18df398, 0x18ce2d8}; // DX11, DX9, DX11 1.1, DX9 1.1
const int kSlots = 64;                       // copied (the class has fewer; the rest is read-only data, never called)
const int kSlotLevel = 0x20 / 4, kSlotError = 0x24 / 4, kSlotWarn = 0x28 / 4;
const int kMaxPerSecond = 10;

uint8_t*** g_library = nullptr;
uintptr_t g_codeLo = 0, g_codeHi = 0, g_base = 0;  // the exe's code: return addresses on the stack fall in it
void* g_vtable[kSlots];
volatile LONG g_second = 0, g_count = 0, g_dropped = 0;
char g_last[256];
CRITICAL_SECTION g_lock;

void Emit(const char* kind, int level, const char* format, va_list args)
{
	char text[512];
	vsnprintf(text, sizeof(text), format ? format : "(null)", args);
	size_t n = strlen(text);
	while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) {
		text[--n] = 0;
	}
	EnterCriticalSection(&g_lock);
	LONG now = (LONG)(GetTickCount() / 1000);
	if (now != g_second) {
		if (g_dropped) {
			XLS_LOG_INFO("game net: (%ld lines over %d a second not logged)", g_dropped, kMaxPerSecond);
		}
		g_second = now;
		g_count = 0;
		g_dropped = 0;
	}
	bool repeat = strncmp(text, g_last, sizeof(g_last) - 1) == 0;
	if (!repeat && g_count < kMaxPerSecond) {
		g_count++;
		snprintf(g_last, sizeof(g_last), "%s", text);
		LeaveCriticalSection(&g_lock);
		if (level >= 0) {
			XLS_LOG_INFO("game net %s %d: %s", kind, level, text);
		} else {
			XLS_LOG_INFO("game net %s: %s", kind, text);
		}
		return;
	}
	if (!repeat) {
		g_dropped++;
	}
	LeaveCriticalSection(&g_lock);
}

// "Communication Error - Connection Lost": the game closes its own connection (NetworkConnectionStable close-leave,
// DX11 0x452b50 / DX9 0x452b10, a virtual method) with nothing logged before it. Whoever asked is up the stack: every
// value there pointing into the exe's code is logged (as the exe's own addresses, 0x400000 base), most are return
// addresses. Once per second at most.
void __attribute__((noinline)) LogCallers(const char* why)
{
	static volatile LONG last = 0;
	LONG now = (LONG)(GetTickCount() / 1000);
	if (!g_codeLo || InterlockedExchange(&last, now) == now) {
		return;
	}
	uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	uintptr_t* top = (uintptr_t*)tib->StackBase;
	char text[1024];
	size_t n = 0;
	int found = 0;
	for (int i = 0; i < 2048 && sp + i < top && found < 40; i++) {
		uintptr_t v = sp[i];
		if (v >= g_codeLo && v < g_codeHi) {
			n += snprintf(text + n, sizeof(text) - n, " %x", (unsigned)(v - g_base + 0x400000));
			found++;
			if (n >= sizeof(text) - 16) {
				break;
			}
		}
	}
	text[n] = 0;
	XLS_LOG_INFO("game net: %s, code addresses up the stack (callers first):%s", why, text);
}

int __cdecl PrintLevel(void*, int level, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	Emit("info", level, format, args);
	va_end(args);
	if (format && strstr(format, "close-leave") && !strstr(format, "send control")) {
		LogCallers("the game closes a connection (close-leave)");
	}
	return 0;
}

int __cdecl PrintError(void*, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	Emit("error", -1, format, args);
	va_end(args);
	return 0;
}

int __cdecl PrintWarn(void*, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	Emit("warning", -1, format, args);
	va_end(args);
	return 0;
}

DWORD WINAPI Watch(void*)
{
	uint8_t* swapped = nullptr;
	for (;;) {
		uint8_t** library = *g_library;
		if (library && (uint8_t*)library != swapped) {
			void** vt = *(void***)library;
			if (vt && vt != g_vtable) {
				memcpy(g_vtable, vt, sizeof(g_vtable));
				g_vtable[kSlotLevel] = (void*)&PrintLevel;
				g_vtable[kSlotError] = (void*)&PrintError;
				g_vtable[kSlotWarn] = (void*)&PrintWarn;
				InterlockedExchangePointer((void**)library, (void*)g_vtable);
				XLS_LOG_INFO("mods: game net log: the network library's messages now go to this log (library %p).", (void*)library);
			}
			swapped = (uint8_t*)library;
		}
		Sleep(1000);
	}
	return 0;
}

}

void GameLogInstall(uint8_t* base, int build)
{
	InitializeCriticalSection(&g_lock);
	g_library = (uint8_t***)(base + (kLibrary[build] - 0x400000));
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
	IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
	g_base = (uintptr_t)base;
	g_codeLo = g_base + nt->OptionalHeader.BaseOfCode;
	g_codeHi = g_codeLo + nt->OptionalHeader.SizeOfCode;
	if (HANDLE t = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr)) {
		CloseHandle(t);
	}
}

}
