#include "mods/lp2.h"

#include "core/log.h"

#include <windows.h>

#include <stdint.h>
#include <exception>
#include <typeinfo>

// Crash log: a vectored handler that writes faults (access violations and the like) to the log with
// the module + offset of the faulting instruction, the registers and the return addresses on the
// stack, in the exe's preferred-base terms (0x400000) so they match the disassembly. It sees
// first-chance exceptions, so a fault the game handles itself is logged too (rare); it never
// handles anything.

namespace xls::mods {

namespace {

volatile LONG g_logged = 0;
const LONG kMaxLogged = 8;

// "LP2DX11.exe 0x8652c0" (exe: preferred-base address) or "steam_api.dll+0x1234"
void Describe(uintptr_t addr, char* out, size_t size)
{
	HMODULE mod = nullptr;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod) {
		char path[MAX_PATH];
		if (GetModuleFileNameA(mod, path, sizeof(path))) {
			const char* slash = strrchr(path, '\\');
			lstrcpynA(name, slash ? slash + 1 : path, sizeof(name));
		}
		uintptr_t rva = addr - (uintptr_t)mod;
		if (mod == GetModuleHandleW(nullptr)) {
			_snprintf(out, size, "%s 0x%x", name, (unsigned)(rva + 0x400000));
		}
		else {
			_snprintf(out, size, "%s+0x%x", name, (unsigned)rva);
		}
	}
	else {
		_snprintf(out, size, "0x%08x", (unsigned)addr);
	}
	out[size - 1] = 0;
}

bool IsCode(uintptr_t addr)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (addr < 0x10000 || !VirtualQuery((void*)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) {
		return false;
	}
	return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
{
	DWORD code = info->ExceptionRecord->ExceptionCode;
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_STACK_OVERFLOW:
	case EXCEPTION_PRIV_INSTRUCTION:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case 0xC0000409: // stack buffer overrun / fail fast
		break;
	default:
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (InterlockedIncrement(&g_logged) > kMaxLogged) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	char where[MAX_PATH + 32];
	Describe((uintptr_t)info->ExceptionRecord->ExceptionAddress, where, sizeof(where));
	const ULONG_PTR* p = info->ExceptionRecord->ExceptionInformation;
	if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
		XLS_LOG_ERROR("crash: access violation (%s 0x%08x) at %s, thread %u.", p[0] == 0 ? "read" : p[0] == 1 ? "write" : "execute",
		              (unsigned)p[1], where, (unsigned)GetCurrentThreadId());
	}
	else {
		XLS_LOG_ERROR("crash: exception 0x%08x at %s, thread %u.", (unsigned)code, where, (unsigned)GetCurrentThreadId());
	}
#if defined(_M_IX86) || defined(__i386__)
	const CONTEXT* c = info->ContextRecord;
	XLS_LOG_ERROR("crash: eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x esp %08x", (unsigned)c->Eax, (unsigned)c->Ebx,
	              (unsigned)c->Ecx, (unsigned)c->Edx, (unsigned)c->Esi, (unsigned)c->Edi, (unsigned)c->Ebp, (unsigned)c->Esp);
	// return addresses: stack words that point into executable image memory (a call trail, with some noise)
	const uintptr_t* sp = (const uintptr_t*)c->Esp;
	int found = 0;
	for (int i = 0; i < 512 && found < 12; i++) {
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
			break;
		}
		if (IsCode(sp[i])) {
			Describe(sp[i], where, sizeof(where));
			XLS_LOG_ERROR("crash:   [esp+0x%03x] %s", i * 4, where);
			found++;
		}
	}
	// Raw memory, for crashes the trail can't explain (a jump through a bad pointer): the stack top, the code at eip and
	// what each register points at. A first word inside the exe's data is that object's vtable.
	auto dump = [](const char* name, uintptr_t at, int words) {
		if (at < 0x10000 || IsBadReadPtr((const void*)at, words * 4)) {
			return;
		}
		const uint32_t* p = (const uint32_t*)at;
		for (int i = 0; i < words; i += 8) {
			XLS_LOG_ERROR("crash: %s %08x: %08x %08x %08x %08x %08x %08x %08x %08x", name, (unsigned)(at + i * 4), p[i], p[i + 1], p[i + 2], p[i + 3], p[i + 4], p[i + 5], p[i + 6], p[i + 7]);
		}
	};
	dump("esp", c->Esp, 32);
	dump("eip", c->Eip - 16, 16);
	dump("ebx", c->Ebx, 16);
	dump("ecx", c->Ecx, 16);
	dump("edx", c->Edx, 16);
	dump("esi", c->Esi, 16);
	dump("edi", c->Edi, 16);
	dump("ebp", c->Ebp, 16);
#endif
	return EXCEPTION_CONTINUE_SEARCH;
}

}

// An uncaught C++ exception (out of memory, a bad container access) ends the game through std::terminate, which
// the exception handler doesn't see: say what it was before the process goes.
void Terminate()
{
	const char* what = "unknown";
	std::string type = "?";
	try {
		if (std::exception_ptr e = std::current_exception()) {
			std::rethrow_exception(e);
		}
		what = "no exception (std::terminate called directly)";
	}
	catch (const std::exception& e) {
		type = typeid(e).name();
		XLS_LOG_ERROR("crash: uncaught C++ exception %s (%s), thread %u.", type.c_str(), e.what(), (unsigned)GetCurrentThreadId());
		abort();
	}
	catch (...) {
	}
	XLS_LOG_ERROR("crash: std::terminate: %s, thread %u.", what, (unsigned)GetCurrentThreadId());
	abort();
}

namespace {

// Address space: LP2 is 32-bit, 2 GB unless the exe is Large Address Aware (4 GB on a 64-bit system). The game's
// "ERR08 : Memory overrun" (a failed CreateTexture2D / allocation) is most likely that running out, so the log shows
// the limit at start, then free space and the largest free block (big textures need one piece) every 30 s while it
// moved by 64 MB or more, and at every game message box.
struct Space {
	uint64_t limitMb, freeMb, largestMb, reservedMb, committedMb;
};

Space MeasureSpace()
{
	Space s{};
	MEMORYSTATUSEX ms{sizeof ms};
	if (GlobalMemoryStatusEx(&ms)) {
		s.limitMb = ms.ullTotalVirtual >> 20;
	}
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	uintptr_t at = (uintptr_t)si.lpMinimumApplicationAddress, end = (uintptr_t)si.lpMaximumApplicationAddress;
	uint64_t free = 0, largest = 0, reserved = 0, committed = 0;
	MEMORY_BASIC_INFORMATION mbi;
	while (at < end && VirtualQuery((void*)at, &mbi, sizeof mbi) == sizeof mbi) {
		if (mbi.State == MEM_FREE) {
			free += mbi.RegionSize;
			largest = mbi.RegionSize > largest ? mbi.RegionSize : largest;
		}
		else if (mbi.State == MEM_RESERVE) {
			reserved += mbi.RegionSize;
		}
		else {
			committed += mbi.RegionSize;
		}
		uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (next <= at) {
			break;
		}
		at = next;
	}
	s.freeMb = free >> 20;
	s.largestMb = largest >> 20;
	s.reservedMb = reserved >> 20;
	s.committedMb = committed >> 20;
	return s;
}

void LogSpace(const char* why, const Space& s)
{
	XLS_LOG_INFO("memory (%s): %llu MB free of %llu MB (largest free block %llu MB), %llu MB committed, %llu MB reserved.", why,
	             (unsigned long long)s.freeMb, (unsigned long long)s.limitMb, (unsigned long long)s.largestMb,
	             (unsigned long long)s.committedMb, (unsigned long long)s.reservedMb);
}

DWORD WINAPI SpaceThread(LPVOID)
{
	uint64_t lastFree = ~0ull, lowest = ~0ull;
	for (;; Sleep(30000)) {
		Space s = MeasureSpace();
		lowest = s.freeMb < lowest ? s.freeMb : lowest;
		if (lastFree == ~0ull || s.freeMb + 64 <= lastFree || s.freeMb >= lastFree + 64) {
			LogSpace(s.freeMb == lowest ? "now, the lowest so far" : "now", s);
			lastFree = s.freeMb;
		}
	}
	return 0;
}

}

void MemoryLogNow(const char* why)
{
	LogSpace(why, MeasureSpace());
}

namespace {

void SpaceStart()
{
	HMODULE exe = GetModuleHandleW(nullptr);
	bool laa = false;
	if (exe) {
		auto* dos = (IMAGE_DOS_HEADER*)exe;
		auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
		laa = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
	}
	Space s = MeasureSpace();
	XLS_LOG_INFO("memory: the exe is %sLarge Address Aware; this process can use %llu MB%s.", laa ? "" : "not ",
	             (unsigned long long)s.limitMb,
	             s.limitMb < 3000 ? " (2 GB: big mods can run out, see the LAA section of LP2-MODS.md)" : "");
	if (HANDLE t = CreateThread(nullptr, 0, SpaceThread, nullptr, 0, nullptr)) {
		CloseHandle(t);
	}
}

}

void CrashLogInstall()
{
	AddVectoredExceptionHandler(0, Handler);
	std::set_terminate(Terminate);
	SpaceStart();
}

}
