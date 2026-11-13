#include "mods/lp2.h"

#include "core/log.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <exception>
#include <malloc.h>
#include <new>
#include <typeinfo>

// Crash log: a vectored handler that writes faults (access violations and the like) to the log with
// the module + offset of the faulting instruction, the registers and the return addresses on the
// stack, in the exe's preferred-base terms (0x400000) so they match the disassembly. It sees
// first-chance exceptions, so a fault the game handles itself is logged too (rare); it never
// handles anything.

namespace xls::mods {

// What this DLL itself has allocated (its C++ allocations and ImGui's), so the memory lines can tell our share
// from the game's: counted by the operator new / delete at the end of this file.
std::atomic<long long> g_ownBytes{0};
std::atomic<long> g_ownBlocks{0};

void* CountedMalloc(size_t size)
{
	void* p = malloc(size ? size : 1);
	if (p) {
		g_ownBytes += (long long)_msize(p);
		g_ownBlocks++;
	}
	return p;
}

void CountedFree(void* p)
{
	if (p) {
		g_ownBytes -= (long long)_msize(p);
		g_ownBlocks--;
		free(p);
	}
}

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
	uint64_t imageMb, mappedMb, privateMb, above2gMb; // committed by kind; everything in use above 0x80000000
	uintptr_t bigBase[3];                              // the largest allocations (reserved + committed), biggest first
	uint64_t bigMb[3];
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
	uint64_t free = 0, largest = 0, reserved = 0, committed = 0, image = 0, mapped = 0, priv = 0, above = 0;
	uintptr_t allocBase = 0;
	uint64_t allocSize = 0;
	auto endAlloc = [&] {
		for (int i = 0; i < 3; i++) {
			if (allocSize > (s.bigMb[i] << 20)) {
				for (int j = 2; j > i; j--) {
					s.bigMb[j] = s.bigMb[j - 1];
					s.bigBase[j] = s.bigBase[j - 1];
				}
				s.bigMb[i] = allocSize >> 20;
				s.bigBase[i] = allocBase;
				break;
			}
		}
	};
	MEMORY_BASIC_INFORMATION mbi;
	while (at < end && VirtualQuery((void*)at, &mbi, sizeof mbi) == sizeof mbi) {
		if (mbi.State == MEM_FREE) {
			free += mbi.RegionSize;
			largest = mbi.RegionSize > largest ? mbi.RegionSize : largest;
		}
		else {
			if ((uintptr_t)mbi.AllocationBase != allocBase) {
				endAlloc();
				allocBase = (uintptr_t)mbi.AllocationBase;
				allocSize = 0;
			}
			allocSize += mbi.RegionSize;
			if ((uintptr_t)mbi.BaseAddress >= 0x80000000u) {
				above += mbi.RegionSize;
			}
			if (mbi.State == MEM_RESERVE) {
				reserved += mbi.RegionSize;
			}
			else {
				committed += mbi.RegionSize;
				(mbi.Type == MEM_IMAGE ? image : mbi.Type == MEM_MAPPED ? mapped : priv) += mbi.RegionSize;
			}
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
	endAlloc();
	s.imageMb = image >> 20;
	s.mappedMb = mapped >> 20;
	s.privateMb = priv >> 20;
	s.above2gMb = above >> 20;
	return s;
}

void LogSpace(const char* why, const Space& s)
{
	XLS_LOG_INFO("memory (%s): %llu MB free of %llu MB (largest free block %llu MB), %llu MB committed, %llu MB reserved.", why,
	             (unsigned long long)s.freeMb, (unsigned long long)s.limitMb, (unsigned long long)s.largestMb,
	             (unsigned long long)s.committedMb, (unsigned long long)s.reservedMb);
	char big[160] = "";
	for (int i = 0, n = 0; i < 3 && s.bigMb[i]; i++) {
		HMODULE owner = nullptr;
		char name[MAX_PATH] = "";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       (LPCSTR)s.bigBase[i], &owner) && owner) {
			GetModuleFileNameA(owner, name, sizeof name);
		}
		const char* base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
		n += snprintf(big + n, sizeof big - n, "%s%llu MB at %08x%s%s", i ? ", " : "", (unsigned long long)s.bigMb[i],
		              (unsigned)s.bigBase[i], *base ? " " : "", base);
		if (n >= (int)sizeof big) {
			break;
		}
	}
	XLS_LOG_INFO("memory (%s): committed: %llu MB heap, %llu MB code, %llu MB files; %llu MB in use above 2 GB (none of that "
	             "fits without Large Address Aware); largest allocations: %s.", why, (unsigned long long)s.privateMb,
	             (unsigned long long)s.imageMb, (unsigned long long)s.mappedMb, (unsigned long long)s.above2gMb, big);
	XLS_LOG_INFO("memory (%s): this DLL's own allocations: %.1f MB in %ld blocks (the rest is the game, the graphics driver "
	             "and other libraries).", why, (double)g_ownBytes.load() / (1024.0 * 1024.0), g_ownBlocks.load());
}

// Whether the upper 2 GB can be used: an LAA exe on 64-bit Windows, or Wine told to treat it as one
// (WINE_LARGE_ADDRESS_AWARE=1, which leaves the exe's flag alone). Wine reports 4095 MB either way and keeps the
// upper half as one reserved block when it can't be used.
bool UpperHalfUsable(uint64_t limitMb)
{
	if (limitMb < 3000) {
		return false;
	}
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery((void*)0x80000000u, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_RESERVE &&
	    (uintptr_t)mbi.AllocationBase == 0x80000000u && mbi.RegionSize >= 0x70000000u) {
		return false;
	}
	return true;
}

DWORD WINAPI SpaceThread(LPVOID)
{
	Sleep(3000); // the log opens after we're started
	{
		HMODULE exe = GetModuleHandleW(nullptr);
		bool laa = false;
		if (exe) {
			auto* dos = (IMAGE_DOS_HEADER*)exe;
			auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
			laa = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
		}
		Space s = MeasureSpace();
		bool upper = UpperHalfUsable(s.limitMb);
		XLS_LOG_INFO("memory: the exe is %sLarge Address Aware; this process can use %s.", laa ? "" : "not ",
		             upper ? (laa ? "4 GB" : "4 GB (the system treats it as Large Address Aware)")
		                   : "2 GB: the game stops with ERR08 when that runs out, see the Large Address Aware section of LP2-MODS.md");
	}
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

// Every C++ allocation of this DLL (libstdc++ is linked statically, so these replace its operators for our code
// only; the game has its own allocator).
void* operator new(size_t size)
{
	void* p = xls::mods::CountedMalloc(size);
	if (!p) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](size_t size) { return operator new(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return xls::mods::CountedMalloc(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return xls::mods::CountedMalloc(size); }
void operator delete(void* p) noexcept { xls::mods::CountedFree(p); }
void operator delete[](void* p) noexcept { xls::mods::CountedFree(p); }
void operator delete(void* p, size_t) noexcept { xls::mods::CountedFree(p); }
void operator delete[](void* p, size_t) noexcept { xls::mods::CountedFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { xls::mods::CountedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { xls::mods::CountedFree(p); }
