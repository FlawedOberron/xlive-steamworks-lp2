// Lost Planet 2 specific mods, enabled from the "mods" section of the configuration.
#pragma once

namespace xls::mods {

// Called from DllMain. Starts a worker that waits for SteamStub to decrypt the game's code and
// then applies the enabled code patches.
void Start();

// Hooks the game's file opens so files in enabled mod folders replace the game's own
// (mods/<name>/nativePC/... overrides <game>/nativePC/...). Called from DllMain.
void OverlayInstall();
// Extra online map slots declared by mod.json files (maps.cpp). MapsWanted loads them;
// MapsTryInstall patches the game once its code is decrypted and returns true when done.
bool MapsWanted();
bool MapsTryInstall(unsigned char* base);
void MapsPatchPickers(unsigned char* base);

// hold_zoom (zoom.cpp): returns true once the zoom check is hooked (LP2DX11.exe and LP2DX9.exe).
bool HoldZoomTryInstall(unsigned char* base);

// Split controls (controls.cpp): per-context action remaps from "mods": {"controls": ...}.
// ControlsTryInstall returns true once the pad update is hooked.
bool ControlsWanted();
bool ControlsTryInstall(unsigned char* base);

// Re-applies the import hooks if something (SteamStub) restored the import table.
void OverlayVerify();

}
