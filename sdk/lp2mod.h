/* Lost Planet 2 code plugins for xlive-steamworks.
 *
 * A plugin is a 32-bit DLL inside a mod: Mods\<Name>\*.dll at the top of the folder (or the top of
 * the zip). xlive.dll loads every plugin of every enabled mod on its own thread at startup, before
 * the game runs, then calls lp2mod_load once and lp2mod_poll repeatedly. Removing the mod (or
 * listing it in "mods": {"disabled": [...]}) leaves the game untouched.
 *
 * The game's code is encrypted (SteamStub) until shortly after start and is decrypted in place, so
 * patches are found by polling: lp2mod_poll runs about every millisecond until it returns nonzero
 * or 30 seconds pass. title_started() turns 1 when the game first calls into xlive, by which time
 * all of its code is decrypted, so a final full search can be done then.
 *
 * Everything here is plain C so plugins can be built with any compiler.
 */
#ifndef LP2MOD_H
#define LP2MOD_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LP2MOD_HOST_VERSION 1

enum {
	LP2MOD_EXE_UNKNOWN = 0,
	LP2MOD_EXE_DX9 = 9,
	LP2MOD_EXE_DX11 = 11,
};

enum {
	LP2MOD_LOG_ERROR = 0,
	LP2MOD_LOG_WARN = 1,
	LP2MOD_LOG_INFO = 2,
	LP2MOD_LOG_DEBUG = 3,
};

typedef struct LP2ModHost {
	uint32_t size;           /* sizeof(LP2ModHost) as the host knows it; newer fields go at the end */
	uint32_t version;        /* LP2MOD_HOST_VERSION */
	unsigned char* game_base; /* base of LP2DX11.exe / LP2DX9.exe */
	int game_exe;            /* LP2MOD_EXE_* */
	const wchar_t* mod_dir;  /* the mod's folder (unpacked folder for a zip), ending in a backslash */
	const char* mod_name;    /* UTF-8 */
	/* Writes to xlive_steamworks.log, prefixed with the mod name. printf format. */
	void (*log)(int level, const char* format, ...);
	/* Copies bytes over code or read-only data (VirtualProtect + flush). Returns 1 on success. */
	int (*write_code)(void* at, const void* bytes, size_t size);
	/* 1 once the game has called XLiveInitialize: its code is fully decrypted. */
	int (*title_started)(void);
} LP2ModHost;

/* Exported by the plugin (undecorated names, cdecl). */
typedef int (*LP2ModLoadFn)(const LP2ModHost* host); /* "lp2mod_load": required. Nonzero = failed, unloaded. */
typedef int (*LP2ModPollFn)(void);                   /* "lp2mod_poll": optional. Nonzero = done. */

#ifdef __cplusplus
}
#endif

#endif
