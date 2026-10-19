#include "mods/lp2.h"

#include "core/log.h"

#include <windows.h>

#include <stdint.h>

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
#endif
	return EXCEPTION_CONTINUE_SEARCH;
}

}

void CrashLogInstall()
{
	AddVectoredExceptionHandler(0, Handler);
}

}
