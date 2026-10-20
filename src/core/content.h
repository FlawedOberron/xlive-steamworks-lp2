#pragma once

// Whether everyone in the lobby has what the match needs (content.cpp). Each player publishes
// member data "content" = "ready" or "dl <file>/<files> <percent>" (the file being fetched of this
// batch, and its progress) from what the mods are still fetching (the
// host's maps, other players' part sets); a match start waits until every member is ready (or
// "sessions": {"start_wait_seconds"} passes). Members without it (no mod loader) count as ready.

#include <cstdint>
#include <functional>
#include <string>

namespace xls {

// Mods: how many downloads of theirs are in progress (being asked for, fetched, unpacked or built).
void ContentAddPending(std::function<int()> pending);
// Starts publishing ours (once).
void ContentStart();
// Whether we have everything (no pending work).
bool ContentLocalReady();
// Whether every member of lobby is ready; *waiting = how many aren't, *percent = the lowest progress.
bool ContentLobbyReady(uint64_t lobby, int* waiting, int* percent);
// Our download batch now: false when nothing is downloading, else the file being fetched (1-based),
// how many the batch has, and that file's progress.
bool ContentLocalProgress(int* file, int* files, int* percent);
// Whether a member published that they have everything (false: downloading, or no data: no mod loader).
bool ContentMemberReady(uint64_t steamId);
// A member's published progress (false: ready, or no data).
bool ContentMemberProgress(uint64_t steamId, int* file, int* files, int* percent);
// "Downloading 2/5 [45%]" for that progress.
std::string ContentProgressText(int file, int files, int percent);
// The session code: someone joined our lobby, or we joined one. For a moment the lobby counts as
// not ready, so the newcomer's sets (and theirs of ours / the host's map) can start downloading
// before a match start checks.
void ContentSettle();
// The session code: a match start is waiting on lobby (0 = none now).
void ContentStartWaiting(uint64_t lobby);
// A presence line while anything is downloading ("" = nothing): ours, or the others' a start waits on.
std::string ContentStatusText();

}
