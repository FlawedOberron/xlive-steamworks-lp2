#pragma once

// Self-update (update.cpp): a release build (built with XLS_RELEASE_TAG, the GitHub release tag
// "test-YYYY-MM-DD[letter]") asks GitHub for the latest release at start; when that is newer it
// downloads its zip and puts the new xlive.dll in place for the next launch (the running one is
// renamed to xlive.dll.old, removed at the next start). Start-up waits up to 10 s for this, so a
// downloaded update is announced ("restart to install") before the title screen and the save load.
// Development builds (no tag) only log what the latest is. "update": {"auto": false} turns it off.
namespace xls {

void UpdateStart();

}
