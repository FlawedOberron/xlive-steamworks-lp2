// Lost Planet 2 specific mods, enabled from the "mods" section of the configuration.
#pragma once

namespace xls::mods {

// Called from DllMain. Starts a worker that waits for SteamStub to decrypt the game's code and
// then applies the enabled code patches.
void Start();

}
