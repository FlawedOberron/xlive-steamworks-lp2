#include "mods/lp2.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

namespace xls::mods {

namespace {

// A byte pattern; mask 0 bytes match anything.
struct Pattern {
	const uint8_t* bytes;
	const uint8_t* mask;
	size_t size;
};

// Boot-logo state machine (DX11 FUN_008acfb0, DX9 at 0x89d830), state 0:
//   mov ecx,[abs]; cmp byte [ecx+0x1829],0; jz show_logos; pop edi; mov byte [esi+4],6
// NOPing the jz makes state 0 always continue to state 6 (logos done, title screen).
const uint8_t kSplashBytes[] = {0x8b, 0x0d, 0, 0, 0, 0, 0x80, 0xb9, 0x29, 0x18, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xec, 0x00, 0x00, 0x00, 0x5f, 0xc6, 0x46, 0x04, 0x06};
const uint8_t kSplashMask[] = {1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
const Pattern kSplash = {kSplashBytes, kSplashMask, sizeof(kSplashBytes)};
const size_t kSplashPatchOffset = 13; // the jz
const size_t kSplashPatchSize = 6;
// Where the pattern sits in the known builds (RVA), so the wait loop stays cheap.
const uint32_t kSplashKnownRvas[] = {0x4ad03f - 13, 0x49d88f - 13}; // LP2DX11.exe, LP2DX9.exe

bool Matches(const uint8_t* at, const Pattern& p)
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

bool Patch(uint8_t* at, const uint8_t* bytes, size_t size)
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

bool ApplySkipSplash(uint8_t* site)
{
	const uint8_t nops[kSplashPatchSize] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	if (!Patch(site + kSplashPatchOffset, nops, sizeof(nops))) {
		XLS_LOG_ERROR("mods: skip_splash: VirtualProtect failed (%lu).", GetLastError());
		return false;
	}
	XLS_LOG_INFO("mods: skip_splash: patched boot logos at 0x%p.", site + kSplashPatchOffset);
	return true;
}

DWORD WINAPI Worker(LPVOID)
{
	ConfigEnsureLoaded();
	if (!Cfg().modSkipSplash) {
		return 0;
	}
	auto* base = (uint8_t*)GetModuleHandleW(nullptr);
	uint8_t* text = nullptr;
	size_t textSize = 0;
	if (!TextRange(base, &text, &textSize)) {
		XLS_LOG_WARN("mods: skip_splash: no .text section in the game module.");
		return 0;
	}
	// SteamStub decrypts .text in place before jumping to the game's entry point, which may be
	// after DllMain returns. Poll the known sites until the pattern appears.
	for (int tick = 0; tick < 30000; tick++) {
		for (uint32_t rva : kSplashKnownRvas) {
			uint8_t* site = base + rva;
			if (site >= text && site + kSplash.size <= text + textSize && Matches(site, kSplash)) {
				ApplySkipSplash(site);
				return 0;
			}
		}
		Sleep(1);
	}
	// Unknown build: by now the code is decrypted, so scan the whole section once.
	for (uint8_t* p = text; p + kSplash.size <= text + textSize; p++) {
		if (Matches(p, kSplash)) {
			ApplySkipSplash(p);
			return 0;
		}
	}
	XLS_LOG_WARN("mods: skip_splash: boot logo code not found, logos will play.");
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
