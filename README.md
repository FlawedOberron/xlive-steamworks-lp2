# Lost Planet 2 (Recolonised)

Online play and a mod loader for **Lost Planet 2** on Steam: a replacement `xlive.dll` that swaps
the dead Games for Windows LIVE for Steamworks, with quality-of-life options, extra match rules and
custom map packages on top.

## Thanks

This project is built on **[xlive-steamworks](https://github.com/sokie/xlive-steamworks) by sokie**
(MIT), the GFWL → Steamworks replacement that gets LP2's multiplayer working again: sign-in,
lobbies, invites, networking, achievements and saves all come from sokie's work. Its original
README, with how every GFWL call maps to Steam, is kept in
[docs/XLIVE-STEAMWORKS.md](docs/XLIVE-STEAMWORKS.md).

Also thanks to [rlabrecque's Steamworks SDK mirror](https://github.com/rlabrecque/SteamworksSDK),
which the build fetches.

## Download

From the [latest release](https://github.com/FlawedOberron/xlive-steamworks-lp2/releases/latest) (also in the
Releases box on the repo page), take the zip for your copy of the game:

- `lp2-recolonised-test.zip`: the **Steam** copy.
- `lp2-recolonised-test-retail.zip`: the **retail disc (DVD)**, already set up for Spacewar (app 480).

Older test builds are on the [Releases page](https://github.com/FlawedOberron/xlive-steamworks-lp2/releases).

> **Experimental.** Expect bugs. Everyone in a match needs the same build for the match rules to apply
> to them.

## Before installing

**Back up your save(s).** Coming from GFWL, your progress is in a folder named after your GFWL
gamertag (where to find it: [Your save](#your-save)). The dll never moves or deletes saves (when it
converts one it keeps the original next to it), but copy the whole `LOST PLANET 2` save folder somewhere safe first anyway. If you
had more than one GFWL profile, that backs them all up.

## Memory limit

Lost Planet 2 is a 32-bit game. On Windows it can use at most **2 GB**; Steam's DRM rejects an exe
patched to be large address aware, so this can't be raised. On Linux, Proton gives it **4 GB** by
default. Map packages and part sets only load when they are used (the bound map, the parts players
wear), but very large custom maps or many detailed part sets in one match can still run out on
Windows. If the game crashes loading a custom map on Windows and not on Linux, this is the likely cause.

## Install

Steam copy or retail disc (DVD): both play online together. The two versions install differently.

### Steam copy (`lp2-recolonised-test.zip`)

1. Steam → Lost Planet 2 → Manage → **Browse local files**.
2. Unzip everything in the zip into that folder, and replace `xlive.dll` when asked.
3. Linux / Steam Deck: set the launch option `WINE_LARGE_ADDRESS_AWARE=1 %command%`
   (Windows: see [Memory](docs/LP2-MODS.md#memory-large-address-aware-recommended)).
4. Play either the DirectX 9 or the DirectX 11 version. Online play uses Steam lobbies, shared with retail DVD
   copies (see [Spacewar](#steam-and-disc-copies-together-spacewar)): invite through Steam or Discord.

**Undo:** delete `xlive_relay.exe`, `steam_api.dll` and `xlive_steamworks.json`, then use Steam's "Verify
integrity of game files" to put Capcom's `xlive.dll` back. (A game update does that too; unzip ours again
afterwards.)

### Retail disc (DVD) (`lp2-recolonised-test-retail.zip`)

You need Steam installed and signed in (any account; you don't need to own Lost Planet 2 on Steam).

1. Install the game from the disc as normal. On Linux, run the disc's `setup.exe` through Wine,
   or add it to Steam as a non-Steam game and run it under Proton.
2. Unzip everything in the zip into the install folder (by default
   `C:\Program Files (x86)\Capcom\Lost Planet 2`). It is already set up for the disc version:
   `steam_appid.txt` and `xlive_steamworks.json` point at Spacewar (app 480). There is no `xlive.dll` to
   replace there; the disc version uses the GFWL one installed in Windows.
3. Steam → **Add a Game** → **Add a Non-Steam Game** → browse to `LP2DX9.exe` or `LP2DX11.exe` in
   that folder.
4. Linux / Steam Deck: in the shortcut's Properties, under Compatibility, pick Proton Experimental,
   and set the launch option `WINE_LARGE_ADDRESS_AWARE=1 %command%`.
5. Start the game from Steam. Steam shows you as playing Spacewar; that's expected (see
   [Spacewar](#steam-and-disc-copies-together-spacewar)).

**Undo:** delete the six files from the zip. The GFWL `xlive.dll` in Windows is used again.

**Problems:** [open an issue](https://github.com/FlawedOberron/xlive-steamworks-lp2/issues) and attach `xlive_steamworks.log` from the game folder.

**If the game crashes:** [open an issue](https://github.com/FlawedOberron/xlive-steamworks-lp2/issues) with `xlive_steamworks.log` from the game folder, or `xlive_steamworks.prev.log` if you've relaunched since (the previous session's log is kept, one back). The crash details (address and call stack) are at the end. Say what you were doing (menu or map, mode, hosting or joining, whether a map or part set was downloading) and which test build you're on.

## Your save

LP2 keeps one save per profile name, in a folder named after it:

- Windows: `Documents\CAPCOM\LOST PLANET 2\<name>\Lostplanet2.Lostplanet2Save-capcom`
- Linux / Steam Deck (Proton): `steamapps/compatdata/45750/pfx/drive_c/users/steamuser/Documents/CAPCOM/LOST PLANET 2/<name>/`
  in the Steam library that holds the game

Under GFWL `<name>` was your GFWL gamertag. With this dll it is your Steam name (cut to 15 characters,
non-English letters become `_`), so the game would otherwise start you on a fresh save.

**What the import does:** on start, if the folder for your Steam name has no save yet, the dll copies
the most recently saved file from any other profile folder into it, and logs
`saves: imported the save of <old> into <new>`. It only ever copies: the old folder and its save
are left as they were, and nothing happens once your Steam-name folder has a save. If you had
several GFWL profiles, it picks the one played last; to use a different one, close the game and copy
that profile's `Lostplanet2.Lostplanet2Save-capcom` over the one in your Steam-name folder.

**Saves from other versions of the game** (an older or newer build, or a GFWL save that was moved to
another PC) are converted on start, so the game no longer reports them as damaged. The original file
is kept next to it as `Lostplanet2.Lostplanet2Save-capcom.original`, and the log says
`saves: ... converted to this version's format`. To use someone else's save, close the game and put
it in your Steam-name folder as `Lostplanet2.Lostplanet2Save-capcom` (chat apps may cut the name).

**To restore** a backup, close the game and copy it back over.

## What it adds

- **Online play** through Steam lobbies, invites and Discord ("Playing Lost Planet 2 (Recolonised)").
- **Match rules** in Create Match: default secondary weapon, unlimited ammo / grenades, ammo supply
  (Dry to Excessive). The host's rules apply to everyone.
- **Map packages** (`Maps\`): extra maps in the create-match picker.
- **File mods** (`Mods\`) and **code plugins**, loaded from the game folder.
- DirectX 9 and DirectX 11 both get every feature.

In progress: Survival and Team Survival (limited respawns, last player / team standing, spectator
camera, also for training's AI Enemy Battle), custom character parts, sharing maps and parts with the
lobby. The full guide, folder layout and settings are in [docs/LP2-MODS.md](docs/LP2-MODS.md).

## Steam and disc copies together (Spacewar)

We want one Lost Planet 2 community, not two: players with the Steam release and players with the
retail DVD should find and join each other's matches.

Disc copies aren't Steam products, so they run under **Spacewar** (Steam app 480), Valve's public
test app that any Steam account can use, and their lobbies live there. Steam copies join them by
default:

- `xlive.dll` starts `xlive_relay.exe` (in the release zip, and installed by the auto-updater), a
  small helper that signs into Steam as Spacewar. The helper handles only lobbies and in-match
  messages.
- Everything else stays on your own Lost Planet 2 Steam app: ownership, your Steam profile and
  Discord status.
- `"network_app_id"` in the `steam` section of `xlive_steamworks.json` picks the app for lobbies.
  It is 480 by default; set it to 0 to use Lost Planet 2's own app instead (Steam players only).
- If the helper is missing or can't start, lobbies fall back to Lost Planet 2's own app.
- The two builds' version numbers differ (Steam 1.000, disc 1.100). `xlive.dll` matches them so
  they accept each other.

Keeping cross-play on Spacewar is deliberate. We don't want to interfere with the networking of
vanilla Lost Planet 2, so cross-play lobbies and traffic stay off the game's own Steam app. Our
lobby keys also carry their own `xl_` prefix.

## No game files

Releases contain only this project's `xlive.dll`, Valve's `steam_api.dll` redistributable and an
example settings file. Nothing from Capcom is shipped: anything the mods change is patched at run
time from your own copy of the game.

## Building

The dll is 32-bit (LP2 is a 32-bit game). The Steamworks SDK is fetched at a pinned revision unless
`-DSTEAMWORKS_SDK_DIR=path/to/sdk` is given.

Windows (Visual Studio):

```
cmake -B build -G "Visual Studio 16 2019" -A Win32
cmake --build build --config Release
```

Linux (MinGW-w64 cross compiler, `i686-w64-mingw32-g++`):

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw32.cmake
cmake --build build
```

The dll lands in `bin/xlive.dll`, with `steam_api.dll` and the example configuration next to it.
More build options (tests, smoke tool) are in [docs/XLIVE-STEAMWORKS.md](docs/XLIVE-STEAMWORKS.md#building).

## License

MIT, like xlive-steamworks (see [LICENSE](LICENSE)). `src/third-party` carries its own licenses. The
Steamworks SDK is Valve's and is not redistributed here. Lost Planet 2 © Capcom; this project is not
affiliated with Capcom, Microsoft or Valve.
