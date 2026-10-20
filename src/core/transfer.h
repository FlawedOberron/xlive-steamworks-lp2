#pragma once

// Content transfer between players (transfer.cpp): a file-sized blob identified by (kind, hash),
// sent peer to peer over SteamNetworkingMessages on its own channel in reliable chunks. Used by
// the mod loader for map packages and part sets other players need. Data only: what arrives is
// written to a file, and the caller checks its hash before using it.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace xls {

// What a provider serves: size bytes, read piecewise (offset, length, out). Called on a
// transfer thread.
struct TransferSource {
	uint64_t size = 0;
	std::function<bool(uint64_t offset, uint32_t length, uint8_t* out)> read;
};
// Looks up content of this kind by hash; nullptr = we don't have it (the requester is told).
using TransferProvider = std::function<std::shared_ptr<TransferSource>(const std::string& hash)>;

void TransferRegister(const std::string& kind, TransferProvider provider);

// Downloads (kind, hash) from player steamId into path (replaced; removed on failure). Blocks the
// calling thread until done, refused, larger than maxBytes, or stalled for stallMs.
bool TransferFetch(uint64_t steamId, const std::string& kind, const std::string& hash, uint64_t maxBytes,
                   const std::wstring& path, uint32_t stallMs = 20000);

// Downloads running now: how many, and their bytes received / total (sizes not yet known count 0).
int TransferProgress(uint64_t* received, uint64_t* total);

}
