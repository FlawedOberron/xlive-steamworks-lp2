#pragma once

// Whether everyone in the lobby has what the match needs (content.cpp). Each player publishes
// member data "content" = "ready" or "dl <percent>" from what the mods are still fetching (the
// host's maps, other players' part sets); a match start waits until every member is ready (or
// "sessions": {"start_wait_seconds"} passes). Members without it (no mod loader) count as ready.

#include <cstdint>
#include <functional>
#include <string>

namespace xls {

// Mods: a check for work in progress (a download being asked for, fetched, unpacked or built).
void ContentAddPending(std::function<bool()> pending);
// Starts publishing ours (once).
void ContentStart();
// Whether we have everything (no pending work).
bool ContentLocalReady();
// Whether every member of lobby is ready; *waiting = how many aren't, *percent = the lowest progress.
bool ContentLobbyReady(uint64_t lobby, int* waiting, int* percent);
// The session code: someone joined our lobby, or we joined one. For a moment the lobby counts as
// not ready, so the newcomer's sets (and theirs of ours / the host's map) can start downloading
// before a match start checks.
void ContentSettle();
// The session code: a match start is waiting on lobby (0 = none now).
void ContentStartWaiting(uint64_t lobby);
// A presence line while anything is downloading ("" = nothing): ours, or the others' a start waits on.
std::string ContentStatusText();

}
