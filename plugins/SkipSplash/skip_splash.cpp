// SkipSplash plugin: skip the boot logos, straight to the title screen.
//
// Boot-logo state machine (DX11 FUN_008acfb0, DX9 0x89d880 area) state 0:
//   mov ecx,[abs]; cmp byte [ecx+0x1829],0; jz show_logos; pop edi; mov byte [esi+4],6
// NOPing the jz makes state 0 always continue to state 6 (logos done, title screen). The logos
// show early, so the known sites are polled while SteamStub decrypts; other builds get one full
// search of .text once the game has started.

#include "../../sdk/lp2mod.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

namespace {

const uint8_t kBytes[] = {0x8b, 0x0d, 0, 0, 0, 0, 0x80, 0xb9, 0x29, 0x18, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xec, 0x00, 0x00, 0x00, 0x5f, 0xc6, 0x46, 0x04, 0x06};
const uint8_t kMask[] = {1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
const size_t kJzOffset = 13;
const uint32_t kKnownRvas[] = {0x4ad03f - kJzOffset /* DX11 */, 0x49d88f - kJzOffset /* DX9 */};

const LP2ModHost* g_host = nullptr;
uint8_t* g_text = nullptr;
size_t g_textSize = 0;

bool Matches(const uint8_t* at)
{
	for (size_t i = 0; i < sizeof(kBytes); i++) {
		if (kMask[i] && at[i] != kBytes[i]) {
			return false;
		}
	}
	return true;
}

bool FindText(uint8_t* base)
{
	auto* dos = (IMAGE_DOS_HEADER*)base;
	auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
		if (memcmp(section->Name, ".text", 6) == 0) {
			g_text = base + section->VirtualAddress;
			g_textSize = section->Misc.VirtualSize;
			return true;
		}
	}
	return false;
}

bool Patch(uint8_t* site)
{
	const uint8_t nops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	if (!g_host->write_code(site + kJzOffset, nops, sizeof(nops))) {
		g_host->log(LP2MOD_LOG_ERROR, "could not write the patch (%lu).", GetLastError());
		return true;
	}
	g_host->log(LP2MOD_LOG_INFO, "boot logos skipped (patched at 0x%p).", site + kJzOffset);
	return true;
}

}

extern "C" __declspec(dllexport) int lp2mod_load(const LP2ModHost* host)
{
	if (!host || host->version < 1 || host->size < sizeof(LP2ModHost)) {
		return 1;
	}
	g_host = host;
	if (!FindText(host->game_base)) {
		host->log(LP2MOD_LOG_WARN, "no .text section in the game module.");
		return 1;
	}
	return 0;
}

extern "C" __declspec(dllexport) int lp2mod_poll(void)
{
	for (uint32_t rva : kKnownRvas) {
		uint8_t* site = g_host->game_base + rva;
		if (site >= g_text && site + sizeof(kBytes) <= g_text + g_textSize && Matches(site)) {
			return Patch(site);
		}
	}
	if (!g_host->title_started()) {
		return 0;
	}
	for (uint8_t* at = g_text; at + sizeof(kBytes) <= g_text + g_textSize; at++) {
		if (Matches(at)) {
			return Patch(at);
		}
	}
	g_host->log(LP2MOD_LOG_WARN, "boot logo code not found in this game build.");
	return 1;
}
