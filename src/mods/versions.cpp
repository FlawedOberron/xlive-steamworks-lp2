#include "mods/lp2.h"

#include "core/config.h"
#include "core/log.h"
#include "mods/parts.h"

#include <windows.h>

#include <stdint.h>
#include <ctype.h>
#include <string.h>

#include <string>

// Compatible game versions: the game drops sessions whose host runs another version ("Unable to Find Match",
// ERRUVer / ERRDVer) with an equality check against its own version string (" 1.000" in the Steam exes, " 1.100"
// in the disc exes; DX9 Steam FUN_00fe8ca0, disc 0xfe8f80, DX11 Steam 0xff9040, disc 0xff9370): cdecl(remote
// 8-byte version or null) -> 1 if equal (or the exe is named LostPlanet2.exe). Found in every build as the function
// starting `sub esp,0x124; push 0x104` that pushes the version string. The hook also answers 1 when both versions are
// in sessions.compatible_versions; the 1.0 and 1.1 builds have the same class sizes and data files.

namespace xls::mods {

namespace {

typedef int(__cdecl* VersionEqualFn)(const char*);
VersionEqualFn g_original = nullptr;
std::string g_own;

const uint8_t kPrologue[] = {0x81, 0xec, 0x24, 0x01, 0x00, 0x00, 0x68, 0x04, 0x01, 0x00, 0x00};
const size_t kStolen = 6; // sub esp,0x124

std::string Trim(const char* text, size_t max)
{
	std::string s(text, strnlen(text, max));
	size_t a = s.find_first_not_of(' ');
	return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(' ') - a + 1);
}

bool Listed(const std::string& version)
{
	for (const std::string& v : Cfg().compatibleVersions) {
		if (Trim(v.c_str(), v.size()) == version) {
			return true;
		}
	}
	return false;
}

int __cdecl VersionEqual(const char* remote)
{
	if (remote) {
		std::string theirs = Trim(remote, 8);
		if (theirs != g_own && Listed(g_own) && Listed(theirs)) {
			static std::string logged;
			if (logged != theirs) {
				logged = theirs;
				XLS_LOG_INFO("mods: versions: version %s treated as compatible with ours (%s).", theirs.c_str(), g_own.c_str());
			}
			return 1;
		}
	}
	return g_original(remote);
}

bool Section(uint8_t* base, const char* name, uint8_t** start, size_t* size)
{
	auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
		if (strncmp((const char*)section->Name, name, 8) == 0) {
			*start = base + section->VirtualAddress;
			*size = section->Misc.VirtualSize;
			return true;
		}
	}
	return false;
}

}

void VersionsInstall(unsigned char* base)
{
	if (Cfg().compatibleVersions.empty()) {
		return;
	}
	uint8_t *text, *rdata;
	size_t textSize, rdataSize;
	if (!Section(base, ".text", &text, &textSize) || !Section(base, ".rdata", &rdata, &rdataSize)) {
		return;
	}
	// the version string: "\0 1.ddd\0"
	for (uint8_t* s = rdata + 1; s + 8 <= rdata + rdataSize; s++) {
		if (s[-1] != 0 || memcmp(s, " 1.", 3) != 0 || s[6] != 0 || !isdigit(s[3]) || !isdigit(s[4]) || !isdigit(s[5])) {
			continue;
		}
		uint8_t push[5] = {0x68};
		uint32_t va = (uint32_t)(uintptr_t)s;
		memcpy(push + 1, &va, 4);
		for (uint8_t* at = text; at + 5 <= text + textSize; at++) {
			if (memcmp(at, push, 5) != 0) {
				continue;
			}
			for (uint8_t* fn = at; fn > at - 0x40; fn--) {
				if (memcmp(fn, kPrologue, sizeof(kPrologue)) != 0) {
					continue;
				}
				g_own = Trim((const char*)s, 8);
				g_original = (VersionEqualFn)Detour(fn, kPrologue, kStolen, (void*)&VersionEqual);
				if (g_original) {
					XLS_LOG_INFO("mods: versions: ours is %s, %s compatible (%zu listed).", g_own.c_str(),
						Listed(g_own) ? "listed versions are" : "not listed, so no others are", Cfg().compatibleVersions.size());
				}
				return;
			}
		}
	}
	XLS_LOG_WARN("mods: versions: version check not found in this game build.");
}

}
