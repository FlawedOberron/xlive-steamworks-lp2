#include "core/transfer.h"

#include "core/log.h"
#include "core/steam.h"
#include "core/utils.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace xls {

namespace {

// Messages on their own SteamNetworkingMessages channel (net.cpp uses UDP ports below 0x10000
// and 0x10000 for control), all reliable (so in order):
//   request  {type 1, id, u8 kind length, kind, u8 hash length, hash}   requester -> owner
//   header   {type 2, id, u64 size}  (kUnavailable: no such content)    owner -> requester
//   data     {type 3, id, u32 index, chunk}                              owner -> requester
//   ack      {type 4, id, u32 chunks received}                           requester -> owner
//   cancel   {type 5, id}                                                either way
// The owner keeps at most kWindow chunks unacknowledged (well inside Steam's send buffer).
const int kTransferChannel = 0x10001;
const uint32_t kChunk = 64 * 1024;
const uint32_t kWindow = 24; // 1.5 MB in flight: inside the 4 MB send buffer net.cpp sets
const uint64_t kUnavailable = ~0ull;
const int kMaxServing = 4;

enum : uint8_t { kRequest = 1, kHeader = 2, kData = 3, kAck = 4, kCancel = 5 };

#pragma pack(push, 1)
struct Head {
	uint8_t type;
	uint8_t pad[3];
	uint32_t id;
};
#pragma pack(pop)

SteamNetworkingIdentity Identity(uint64_t steamId)
{
	SteamNetworkingIdentity identity;
	identity.SetSteamID(CSteamID(steamId));
	return identity;
}

std::mutex g_loopMutex;
std::set<uint64_t> g_loopPeers;
std::deque<std::pair<uint64_t, std::vector<uint8_t>>> g_loop; // (peer, message) for the pump
struct Throttle {
	uint32_t rate = 0;   // bytes per second, 0 = as fast as it goes
	double budget = 0;   // bytes it may deliver now (refilled at rate, may dip below 0)
	DWORD refilled = 0;
};
std::map<uint64_t, Throttle> g_loopThrottle; // loopback peer -> its speed

bool Send(uint64_t peer, const std::vector<uint8_t>& message)
{
	{
		std::lock_guard<std::mutex> lock(g_loopMutex);
		if (g_loopPeers.count(peer)) {
			g_loop.emplace_back(peer, message);
			return true;
		}
	}
	for (int tries = 0; tries < 200; tries++) {
		if (!SteamReady() || !SteamNetworkingMessages()) {
			return false;
		}
		EResult r = SteamNetworkingMessages()->SendMessageToUser(Identity(peer), message.data(), (uint32)message.size(),
		                                                          k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession, kTransferChannel);
		if (r == k_EResultOK) {
			return true;
		}
		if (r != k_EResultLimitExceeded) {
			XLS_LOG_DEBUG("transfer: send to %llu failed (%d).", (unsigned long long)peer, (int)r);
			return false;
		}
		Sleep(50); // send buffer full
	}
	return false;
}

std::vector<uint8_t> Message(uint8_t type, uint32_t id, const void* body = nullptr, size_t size = 0)
{
	std::vector<uint8_t> m(sizeof(Head) + size);
	Head h = {type, {}, id};
	memcpy(m.data(), &h, sizeof(h));
	if (size) {
		memcpy(m.data() + sizeof(Head), body, size);
	}
	return m;
}

std::mutex g_mutex;
std::condition_variable g_changed;
std::map<std::string, TransferProvider> g_providers;

// --- requester side ---
struct Fetch {
	uint64_t peer;
	FILE* file = nullptr;
	uint64_t maxBytes;
	uint64_t size = kUnavailable;
	bool header = false;
	uint64_t received = 0;
	uint32_t chunks = 0;
	DWORD lastProgress;
	int result = 0; // 1 done, -1 failed
	std::string why;
};
std::map<uint32_t, Fetch*> g_fetches;

// --- owner side ---
struct Serve {
	uint64_t peer;
	uint32_t id;
	uint32_t acked = 0;
	bool cancelled = false;
	DWORD lastAck;
};
std::map<std::pair<uint64_t, uint32_t>, Serve*> g_serves;

struct ServeJob {
	Serve* serve;
	std::string kind, hash;
};

DWORD WINAPI ServeJobThread(LPVOID param)
{
	std::unique_ptr<ServeJob> job((ServeJob*)param);
	Serve* s = job->serve;
	TransferProvider provider;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_providers.find(job->kind);
		if (it != g_providers.end()) {
			provider = it->second;
		}
	}
	std::shared_ptr<TransferSource> source = provider ? provider(job->hash) : nullptr;
	uint64_t size = source ? source->size : kUnavailable;
	Send(s->peer, Message(kHeader, s->id, &size, sizeof(size)));
	DWORD start = GetTickCount();
	bool ok = source != nullptr;
	if (source) {
		XLS_LOG_INFO("transfer: sending %s %s (%llu bytes) to %llu.", job->kind.c_str(), job->hash.c_str(), (unsigned long long)size, (unsigned long long)s->peer);
		uint32_t chunks = (uint32_t)((size + kChunk - 1) / kChunk);
		std::vector<uint8_t> buffer(sizeof(Head) + 4 + kChunk);
		for (uint32_t index = 0; index < chunks && ok; index++) {
			{
				std::unique_lock<std::mutex> lock(g_mutex);
				while (!s->cancelled && index >= s->acked + kWindow) {
					if (GetTickCount() - s->lastAck > 30000) {
						s->cancelled = true;
						XLS_LOG_WARN("transfer: %llu stopped acknowledging %s %s.", (unsigned long long)s->peer, job->kind.c_str(), job->hash.c_str());
						break;
					}
					g_changed.wait_for(lock, std::chrono::milliseconds(200));
				}
				if (s->cancelled) {
					ok = false;
					break;
				}
			}
			uint64_t offset = (uint64_t)index * kChunk;
			uint32_t length = (uint32_t)std::min<uint64_t>(kChunk, size - offset);
			Head h = {kData, {}, s->id};
			memcpy(buffer.data(), &h, sizeof(h));
			memcpy(buffer.data() + sizeof(h), &index, 4);
			if (!source->read(offset, length, buffer.data() + sizeof(h) + 4)) {
				XLS_LOG_WARN("transfer: cannot read %s %s at %llu.", job->kind.c_str(), job->hash.c_str(), (unsigned long long)offset);
				Send(s->peer, Message(kCancel, s->id));
				ok = false;
				break;
			}
			ok = Send(s->peer, std::vector<uint8_t>(buffer.begin(), buffer.begin() + sizeof(h) + 4 + length));
		}
		if (ok) {
			XLS_LOG_INFO("transfer: sent %s %s to %llu in %lu ms.", job->kind.c_str(), job->hash.c_str(), (unsigned long long)s->peer, GetTickCount() - start);
		}
	}
	else {
		XLS_LOG_INFO("transfer: %llu asked for %s %s, which we don't have.", (unsigned long long)s->peer, job->kind.c_str(), job->hash.c_str());
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_serves.erase({s->peer, s->id});
	delete s;
	return 0;
}

void OnRequest(uint64_t peer, uint32_t id, const uint8_t* body, size_t size)
{
	if (size < 1 || size < 1u + body[0] + 1 || size < 2u + body[0] + body[1 + body[0]]) {
		return;
	}
	std::string kind((const char*)body + 1, body[0]);
	std::string hash((const char*)body + 2 + body[0], body[1 + body[0]]);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_serves.size() >= kMaxServing || g_serves.count({peer, id})) {
		uint64_t none = kUnavailable;
		Send(peer, Message(kHeader, id, &none, sizeof(none)));
		return;
	}
	Serve* s = new Serve{peer, id, 0, false, GetTickCount()};
	g_serves[{peer, id}] = s;
	HANDLE t = CreateThread(nullptr, 0, ServeJobThread, new ServeJob{s, kind, hash}, 0, nullptr);
	if (t) {
		CloseHandle(t);
	}
}

void Fail(Fetch* f, const std::string& why)
{
	if (f->result == 0) {
		f->result = -1;
		f->why = why;
	}
}

void OnMessage(uint64_t peer, const uint8_t* data, size_t size)
{
	if (size < sizeof(Head)) {
		return;
	}
	Head h;
	memcpy(&h, data, sizeof(h));
	const uint8_t* body = data + sizeof(Head);
	size_t bodySize = size - sizeof(Head);
	if (h.type == kRequest) {
		OnRequest(peer, h.id, body, bodySize);
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	if (h.type == kAck || (h.type == kCancel && g_serves.count({peer, h.id}))) {
		auto it = g_serves.find({peer, h.id});
		if (it != g_serves.end()) {
			if (h.type == kAck && bodySize >= 4) {
				memcpy(&it->second->acked, body, 4);
				it->second->lastAck = GetTickCount();
			}
			else {
				it->second->cancelled = true;
			}
			g_changed.notify_all();
		}
		return;
	}
	auto it = g_fetches.find(h.id);
	if (it == g_fetches.end() || it->second->peer != peer || it->second->result != 0) {
		return;
	}
	Fetch* f = it->second;
	f->lastProgress = GetTickCount();
	if (h.type == kHeader && bodySize >= 8 && !f->header) {
		memcpy(&f->size, body, 8);
		f->header = true;
		if (f->size == kUnavailable) {
			Fail(f, "the player does not have it (or is busy)");
		}
		else if (f->size > f->maxBytes) {
			Fail(f, FormatA("too big (%llu bytes, limit %llu)", (unsigned long long)f->size, (unsigned long long)f->maxBytes));
			Send(peer, Message(kCancel, h.id));
		}
		else if (f->size == 0) {
			f->result = 1;
		}
	}
	else if (h.type == kData && bodySize >= 4 && f->header) {
		uint32_t index;
		memcpy(&index, body, 4);
		size_t length = bodySize - 4;
		if (index != f->chunks || f->received + length > f->size || (length != kChunk && f->received + length != f->size)) {
			Fail(f, "chunks out of order");
			Send(peer, Message(kCancel, h.id));
		}
		else if (fwrite(body + 4, 1, length, f->file) != length) {
			Fail(f, "cannot write the file");
			Send(peer, Message(kCancel, h.id));
		}
		else {
			f->received += length;
			f->chunks++;
			Send(peer, Message(kAck, h.id, &f->chunks, 4));
			if (f->received == f->size) {
				f->result = 1;
			}
		}
	}
	else if (h.type == kCancel) {
		Fail(f, "cancelled by the player");
	}
	g_changed.notify_all();
}

DWORD WINAPI PumpThread(LPVOID)
{
	SteamNetworkingMessage_t* messages[32];
	for (;;) {
		int count = SteamReady() && SteamNetworkingMessages() ? SteamNetworkingMessages()->ReceiveMessagesOnChannel(kTransferChannel, messages, 32) : 0;
		for (int i = 0; i < count; i++) {
			OnMessage(messages[i]->m_identityPeer.GetSteamID64(), (const uint8_t*)messages[i]->m_pData, (size_t)messages[i]->m_cbSize);
			messages[i]->Release();
		}
		std::deque<std::pair<uint64_t, std::vector<uint8_t>>> loop;
		{
			std::lock_guard<std::mutex> lock(g_loopMutex);
			DWORD now = GetTickCount();
			for (auto& t : g_loopThrottle) {
				if (t.second.rate) {
					t.second.budget = std::min<double>(t.second.budget + (now - t.second.refilled) * (double)t.second.rate / 1000, t.second.rate);
					t.second.refilled = now;
				}
			}
			// a throttled peer's messages wait their turn (in order: once one waits, so do the rest)
			std::deque<std::pair<uint64_t, std::vector<uint8_t>>> waiting;
			for (auto& m : g_loop) {
				Throttle& t = g_loopThrottle[m.first];
				if (t.rate && t.budget <= 0) {
					waiting.push_back(std::move(m));
					continue;
				}
				t.budget -= t.rate ? (double)m.second.size() : 0;
				loop.push_back(std::move(m));
			}
			g_loop.swap(waiting);
		}
		for (const auto& m : loop) {
			OnMessage(m.first, m.second.data(), m.second.size());
		}
		count += (int)loop.size();
		Sleep(count ? 0 : 5);
	}
}

void EnsurePump()
{
	static std::once_flag once;
	std::call_once(once, [] {
		HANDLE t = CreateThread(nullptr, 0, PumpThread, nullptr, 0, nullptr);
		if (t) {
			CloseHandle(t);
		}
	});
}

}

void TransferRegister(const std::string& kind, TransferProvider provider)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_providers[kind] = std::move(provider);
	}
	EnsurePump();
}

bool TransferFetch(uint64_t steamId, const std::string& kind, const std::string& hash, uint64_t maxBytes, const std::wstring& path, uint32_t stallMs)
{
	EnsurePump();
	if (kind.size() > 255 || hash.size() > 255) {
		return false;
	}
	std::wstring part = path + L".part";
	EnsureDirectory(path.substr(0, path.find_last_of(L"\\/") + 1));
	FILE* file = _wfopen(part.c_str(), L"wb");
	if (!file) {
		XLS_LOG_WARN("transfer: cannot create %ls.", part.c_str());
		return false;
	}
	Fetch f{steamId, file, maxBytes};
	f.lastProgress = GetTickCount();
	uint32_t id;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		do {
			id = (uint32_t)GetTickCount() ^ ((uint32_t)rand() << 16) ^ (uint32_t)rand();
		} while (g_fetches.count(id));
		g_fetches[id] = &f;
	}
	std::vector<uint8_t> body;
	body.push_back((uint8_t)kind.size());
	body.insert(body.end(), kind.begin(), kind.end());
	body.push_back((uint8_t)hash.size());
	body.insert(body.end(), hash.begin(), hash.end());
	DWORD start = GetTickCount();
	XLS_LOG_INFO("transfer: asking %llu for %s %s.", (unsigned long long)steamId, kind.c_str(), hash.c_str());
	if (!Send(steamId, Message(kRequest, id, body.data(), body.size()))) {
		Fail(&f, "cannot reach the player");
	}
	uint64_t logged = 0;
	{
		std::unique_lock<std::mutex> lock(g_mutex);
		while (f.result == 0) {
			if (GetTickCount() - f.lastProgress > stallMs) {
				Fail(&f, "no progress");
				Send(steamId, Message(kCancel, id));
				break;
			}
			if (f.received >= logged + (8u << 20)) {
				logged = f.received;
				XLS_LOG_INFO("transfer: %s %s: %llu / %llu KB.", kind.c_str(), hash.c_str(), (unsigned long long)(f.received >> 10), (unsigned long long)(f.size >> 10));
			}
			g_changed.wait_for(lock, std::chrono::milliseconds(250));
		}
		g_fetches.erase(id);
	}
	fclose(file);
	if (f.result != 1) {
		XLS_LOG_WARN("transfer: %s %s from %llu failed: %s.", kind.c_str(), hash.c_str(), (unsigned long long)steamId, f.why.c_str());
		DeleteFileW(part.c_str());
		return false;
	}
	DeleteFileW(path.c_str());
	if (!MoveFileW(part.c_str(), path.c_str())) {
		DeleteFileW(part.c_str());
		return false;
	}
	DWORD ms = GetTickCount() - start;
	XLS_LOG_INFO("transfer: got %s %s (%llu bytes) in %lu ms (%llu KB/s).", kind.c_str(), hash.c_str(), (unsigned long long)f.size, ms,
	             (unsigned long long)(ms ? f.size / ms : 0));
	return true;
}

void TransferAddLoopback(uint64_t peer, uint32_t bytesPerSecond)
{
	std::lock_guard<std::mutex> lock(g_loopMutex);
	g_loopPeers.insert(peer);
	if (bytesPerSecond) {
		Throttle& t = g_loopThrottle[peer];
		t.rate = bytesPerSecond;
		t.refilled = GetTickCount();
		XLS_LOG_INFO("transfer: loopback %llu throttled to %u KB/s.", (unsigned long long)peer, bytesPerSecond / 1024);
	}
}

int TransferProgress(uint64_t* received, uint64_t* total)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	*received = *total = 0;
	for (const auto& kv : g_fetches) {
		if (kv.second->header && kv.second->size != kUnavailable) {
			*received += kv.second->received;
			*total += kv.second->size;
		}
	}
	return (int)g_fetches.size();
}

}
