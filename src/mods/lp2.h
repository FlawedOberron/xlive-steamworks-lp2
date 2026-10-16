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

// Re-applies the import hooks if something (SteamStub) restored the import table.
void OverlayVerify();

}
