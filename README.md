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

Take `lp2-recolonised-test.zip` from the
[latest release](https://github.com/FlawedOberron/xlive-steamworks-lp2/releases/latest) (also in the
Releases box on the repo page). Older test builds are on the
[Releases page](https://github.com/FlawedOberron/xlive-steamworks-lp2/releases).

> **Experimental.** Expect bugs. Everyone in a match needs the same build for the match rules to apply
> to them.

## Install

1. Steam → Lost Planet 2 → Manage → **Browse local files**.
2. Rename the existing `xlive.dll` to `xlive.dll.capcom` (keeps a backup).
3. Copy `xlive.dll`, `steam_api.dll` and `xlive_steamworks.json` from the zip into that folder.
4. Linux / Steam Deck: set the launch option `WINE_LARGE_ADDRESS_AWARE=1 %command%`
   (Windows: see [Memory](docs/LP2-MODS.md#memory-large-address-aware-recommended)).
5. Play either the DirectX 9 or the DirectX 11 version. Online play uses Steam lobbies: invite through
   Steam or Discord.

**Undo:** delete the three files and rename `xlive.dll.capcom` back. Steam's "Verify integrity of game
files" (or a game update) also puts Capcom's `xlive.dll` back; copy ours again afterwards.

**Problems:** send `xlive_steamworks.log` from the game folder.

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
