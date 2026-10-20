#include "core/discord.h"

#include "api/xsession.h"
#include "core/config.h"
#include "core/content.h"
#include "core/json.h"
#include "core/log.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"
#include "mods/packages.h"

#include <windows.h>

#include <atomic>
#include <ctime>
#include <string>

namespace xls {

namespace {

// Discord's IPC: frames of u32 opcode, u32 length, JSON. Discord listens on the named pipe
// \\.\pipe\discord-ipc-N on Windows and on the unix socket $XDG_RUNTIME_DIR/discord-ipc-N on
// Linux. Under Wine / Proton there is no pipe and Proton's Wine has no AF_UNIX (WSAEAFNOSUPPORT,
// Proton 9 to 11), so the Linux socket is opened with Linux system calls (int 0x80, as
// rpc-bridge does; the DLL is 32-bit). Wine passes XDG_RUNTIME_DIR through.
enum Op : uint32_t { kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4 };

// i386 Linux system calls (only reached under Wine on Linux)
long LinuxCall(long n, long a, long b, long c)
{
	long r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
}
enum : long { kSysGetPid = 20, kSysRead = 3, kSysWrite = 4, kSysClose = 6, kSysPoll = 168, kSysSocket = 359, kSysConnect = 362 };

bool UnderWine()
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

// The pid Discord watches (it drops the activity once that process is gone): under Wine the
// Linux one, as Wine's GetCurrentProcessId is its own numbering.
unsigned long DiscordPid()
{
	return UnderWine() ? (unsigned long)LinuxCall(kSysGetPid, 0, 0, 0) : GetCurrentProcessId();
}

class Connection {
public:
	~Connection() { Close(); }

	bool Open()
	{
		for (int i = 0; i < 10; i++) {
			wchar_t name[64];
			swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
			HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (pipe != INVALID_HANDLE_VALUE) {
				m_pipe = pipe;
				XLS_LOG_INFO("discord: connected to %ls.", name);
				return true;
			}
		}
		char runtime[260] = {};
		if (!GetEnvironmentVariableA("XDG_RUNTIME_DIR", runtime, sizeof(runtime)) || runtime[0] != '/') {
			return false;
		}
		if (!UnderWine()) {
			return false;
		}
		// the native client, then the Flatpak and Snap ones
		for (const char* sub : {"", "/app/com.discordapp.Discord", "/snap.discord", "/app/dev.vencord.Vesktop"}) {
			for (int i = 0; i < 10; i++) {
				std::string path = FormatA("%s%s/discord-ipc-%d", runtime, sub, i);
				struct {
					uint16_t family;
					char path[108];
				} addr = {1, {}}; // AF_UNIX
				if (path.size() >= sizeof(addr.path)) {
					continue;
				}
				memcpy(addr.path, path.c_str(), path.size());
				long fd = LinuxCall(kSysSocket, 1, 1, 0); // AF_UNIX, SOCK_STREAM
				if (fd < 0) {
					XLS_LOG_WARN("discord: Linux socket() failed (%ld).", fd);
					return false;
				}
				if (LinuxCall(kSysConnect, fd, (long)&addr, (long)(2 + path.size() + 1)) == 0) {
					m_fd = (int)fd;
					XLS_LOG_INFO("discord: connected to %s (Linux socket).", path.c_str());
					return true;
				}
				LinuxCall(kSysClose, fd, 0, 0);
			}
		}
		return false;
	}

	bool IsOpen() const { return m_pipe != INVALID_HANDLE_VALUE || m_fd >= 0; }

	void Close()
	{
		if (m_pipe != INVALID_HANDLE_VALUE) {
			CloseHandle(m_pipe);
			m_pipe = INVALID_HANDLE_VALUE;
		}
		if (m_fd >= 0) {
			LinuxCall(kSysClose, m_fd, 0, 0);
			m_fd = -1;
		}
	}

	bool Send(uint32_t op, const std::string& json)
	{
		std::string frame(8, '\0');
		uint32_t length = (uint32_t)json.size();
		memcpy(&frame[0], &op, 4);
		memcpy(&frame[4], &length, 4);
		frame += json;
		if (!WriteAll(frame.data(), frame.size())) {
			Close();
			return false;
		}
		return true;
	}

	// One frame if one arrives within timeoutMs. false = nothing (or the connection dropped).
	bool Receive(uint32_t* op, std::string* json, DWORD timeoutMs)
	{
		if (!Wait(timeoutMs)) {
			return false;
		}
		uint32_t header[2];
		if (!ReadAll(header, 8) || header[1] > (1u << 20)) {
			Close();
			return false;
		}
		json->resize(header[1]);
		if (header[1] && !ReadAll(&(*json)[0], header[1])) {
			Close();
			return false;
		}
		*op = header[0];
		return true;
	}

private:
	bool Wait(DWORD timeoutMs)
	{
		if (m_fd >= 0) {
			struct {
				int fd;
				short events, revents;
			} p = {m_fd, 1 /* POLLIN */, 0};
			return LinuxCall(kSysPoll, (long)&p, 1, (long)timeoutMs) > 0;
		}
		for (DWORD waited = 0;; waited += 50) {
			DWORD available = 0;
			if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr)) {
				Close();
				return false;
			}
			if (available >= 8) {
				return true;
			}
			if (waited >= timeoutMs) {
				return false;
			}
			Sleep(50);
		}
	}

	bool WriteAll(const char* data, size_t size)
	{
		while (size) {
			int done = 0;
			if (m_fd >= 0) {
				done = (int)LinuxCall(kSysWrite, m_fd, (long)data, (long)size);
			}
			else {
				DWORD written = 0;
				done = WriteFile(m_pipe, data, (DWORD)size, &written, nullptr) ? (int)written : -1;
			}
			if (done <= 0) {
				return false;
			}
			data += done;
			size -= done;
		}
		return true;
	}

	bool ReadAll(void* out, size_t size)
	{
		char* p = (char*)out;
		while (size) {
			int done = 0;
			if (m_fd >= 0) {
				done = (int)LinuxCall(kSysRead, m_fd, (long)p, (long)size);
			}
			else {
				DWORD read = 0;
				done = ReadFile(m_pipe, p, (DWORD)size, &read, nullptr) ? (int)read : -1;
			}
			if (done <= 0) {
				return false;
			}
			p += done;
			size -= done;
		}
		return true;
	}

	HANDLE m_pipe = INVALID_HANDLE_VALUE;
	int m_fd = -1; // Linux socket (under Wine)
};

std::string Quote(const std::string& s)
{
	std::string out = "\"";
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += (char)c;
		}
		else if (c < 0x20) {
			out += FormatA("\\u%04x", c);
		}
		else {
			out += (char)c;
		}
	}
	return out + "\"";
}

std::atomic<bool> g_stop{false};
std::atomic<bool> g_started{false};
HANDLE g_thread = nullptr;
uint64_t g_nonce = 0;

std::string Nonce()
{
	return Quote(FormatA("%llu", (unsigned long long)++g_nonce));
}

// LP2's online rules as the host advertises them in the lobby (session properties): match mode,
// its sub-rule and the map, named as the create-match menu does (MsgOnline 72.., 79.., 126..).
const char* const kModes[] = {"Elimination", "Team Elimination", "Data Post Battle", "Akrid Egg Battle", "Fugitive", "Battle Series"};
const char* const kSubRules[][5] = {
	{"Battle Gauge", "Kills", "Points"},
	{"Battle Gauge", "Kills", "Points", "VS Annihilator"},
	{"Post Grab", "Counter Grab"},
};
const char* const kMaps[] = {"Desecration", "Turbulent Jungle", "Kraftwerk", "The Great Outdoors", "Dockyard Battle",
                             "Thunderpeal Precipice", "Dual Complex", "NEOS", "Helix", "Friction", "Pirate Fortress",
                             "Back to the Island", "Cube", nullptr, nullptr, "Frozen Wasteland"};
enum : uint32_t { kPropMode = 0x10000001, kPropSubRule = 0x10000002, kPropMap = 0x10000004 };

bool LobbyNumber(CSteamID lobby, uint32_t propertyId, long* out)
{
	const char* v = SteamMatchmaking()->GetLobbyData(lobby, FormatA("%sp%08x", Cfg().lobbyKeyPrefix.c_str(), propertyId).c_str());
	if (!v || !*v) {
		return false;
	}
	*out = strtol(v, nullptr, 10);
	return true;
}

std::string MapName(long map)
{
	for (const mods::MapPackage& m : mods::PackageMaps()) {
		if (m.slot == map) {
			return m.name;
		}
	}
	return map >= 0 && map < (long)(sizeof(kMaps) / sizeof(kMaps[0])) && kMaps[map] ? kMaps[map] : std::string();
}

// What we show; rebuilt every few seconds, sent when it changes.
std::string BuildActivity(int64_t started)
{
	std::string line = WideToUtf8(UserPresenceText(0).c_str());
	// the game says "Preparing to play" before and between matches, in the menus
	for (size_t at; (at = line.find("Preparing to play")) != std::string::npos;) {
		line.replace(at, 17, "In the menu");
	}
	std::string details = line.empty() ? "Starting up" : line, state;
	CSteamID lobby = SessionPresenceLobby();
	std::string party;
	if (lobby.IsValid() && SteamReady() && SteamMatchmaking()) {
		bool inGame = SessionPresenceInGame();
		long mode = -1, sub = -1, map = -1;
		if (LobbyNumber(lobby, kPropMode, &mode) && mode >= 0 && mode < 6) {
			std::string kind = line.substr(0, line.find(" - ")); // "Player Match", "Ranked Match"...
			details = (kind.empty() || kind == line ? std::string() : kind + ": ") + kModes[mode];
			if (mode < 3 && LobbyNumber(lobby, kPropSubRule, &sub) && sub >= 0 && sub < 5 && kSubRules[mode][sub]) {
				details += std::string(" (") + kSubRules[mode][sub] + ")";
			}
		}
		std::string mapName = LobbyNumber(lobby, kPropMap, &map) ? MapName(map) : std::string();
		static std::string lastRaw;
		std::string raw;
		for (uint32_t id = 0x10000001; id <= 0x10000008; id++) {
			long v = -1;
			raw += FormatA(" %x=%ld", id & 0xff, LobbyNumber(lobby, id, &v) ? v : -1);
		}
		if (raw != lastRaw) {
			XLS_LOG_DEBUG("discord: lobby properties%s (mode %ld, sub %ld, map %ld).", raw.c_str(), mode, sub, map);
			lastRaw = raw;
		}
		state = inGame ? (mapName.empty() ? "In a match" : mapName) : (mapName.empty() ? "In the lobby" : "Lobby: " + mapName);
		int members = SteamMatchmaking()->GetNumLobbyMembers(lobby);
		int limit = SteamMatchmaking()->GetLobbyMemberLimit(lobby);
		if (members > 0 && limit >= members) {
			std::string id = FormatA("%llu", (unsigned long long)lobby.ConvertToUint64());
			party = FormatA(",\"party\":{\"id\":%s,\"size\":[%d,%d]}", Quote("lp2-" + id).c_str(), members, limit);
			if (members < limit && SessionPresenceJoinable()) { // invite-only sessions: no Join
				party += ",\"secrets\":{\"join\":" + Quote(id) + "}";
			}
		}
	}
	std::string activity = "{\"details\":" + Quote(details);
	std::string content = ContentStatusText(); // downloads (ours, or the ones a match start waits on)
	if (!content.empty()) {
		state = content;
	}
	if (!state.empty()) {
		activity += ",\"state\":" + Quote(state);
	}
	activity += party;
	activity += FormatA(",\"timestamps\":{\"start\":%lld}", (long long)started);
	activity += ",\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Lost Planet 2 (Recolonised)\"}";
	return activity + "}";
}

void SetActivity(Connection& c, const std::string& activity)
{
	c.Send(kFrame, FormatA("{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":%s},\"nonce\":%s}",
	                       DiscordPid(), activity.c_str(), Nonce().c_str()));
}

void HandleFrame(Connection& c, uint32_t op, const std::string& text, bool* ready)
{
	if (op == kPing) {
		c.Send(kPong, text);
		return;
	}
	if (op == kClose) {
		XLS_LOG_WARN("discord: closed by Discord: %s", text.c_str());
		c.Close();
		return;
	}
	Json root;
	if (!Json::Parse(text, root, nullptr)) {
		return;
	}
	std::string cmd = root["cmd"].GetString(std::string()), evt = root["evt"].GetString(std::string());
	if (cmd == "DISPATCH" && evt == "READY") {
		*ready = true;
		XLS_LOG_INFO("discord: ready (user %s).", root["data"]["user"]["username"].GetString("?").c_str());
		c.Send(kFrame, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":" + Nonce() + "}");
	}
	else if (cmd == "DISPATCH" && evt == "ACTIVITY_JOIN") {
		std::string secret = root["data"]["secret"].GetString(std::string());
		CSteamID lobby(_strtoui64(secret.c_str(), nullptr, 10));
		XLS_LOG_INFO("discord: join requested, lobby %s.", secret.c_str());
		if (lobby.IsLobby()) {
			events::OnLobbyJoinRequested(lobby, k_steamIDNil);
		}
	}
	else if (evt == "ERROR") {
		XLS_LOG_WARN("discord: %s failed: %s", cmd.c_str(), root["data"]["message"].GetString("?").c_str());
	}
	else {
		XLS_LOG_DEBUG("discord: reply %.300s", text.c_str());
	}
}

DWORD WINAPI Worker(LPVOID)
{
	Connection c;
	const int64_t started = (int64_t)time(nullptr);
	DWORD nextTry = 0;
	bool loggedMissing = false;
	while (!g_stop) {
		if (!c.IsOpen()) {
			if (GetTickCount() < nextTry) {
				Sleep(500);
				continue;
			}
			nextTry = GetTickCount() + 20000;
			if (!c.Open()) {
				if (!loggedMissing) {
					XLS_LOG_INFO("discord: Discord is not running (or not reachable); retrying every 20 s.");
					loggedMissing = true;
				}
				continue;
			}
			c.Send(kHandshake, "{\"v\":1,\"client_id\":" + Quote(Cfg().discordAppId) + "}");
		}
		bool ready = false;
		std::string shown;
		DWORD nextUpdate = 0;
		while (!g_stop && c.IsOpen()) {
			uint32_t op;
			std::string text;
			if (c.Receive(&op, &text, 500)) {
				HandleFrame(c, op, text, &ready);
			}
			if (ready && GetTickCount() >= nextUpdate) {
				nextUpdate = GetTickCount() + 3000;
				std::string activity = BuildActivity(started);
				if (activity != shown) {
					SetActivity(c, activity);
					shown = activity;
					XLS_LOG_DEBUG("discord: activity %s", activity.c_str());
				}
			}
		}
		if (!g_stop) {
			XLS_LOG_INFO("discord: connection lost; retrying.");
		}
	}
	if (c.IsOpen()) {
		c.Send(kFrame, FormatA("{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu},\"nonce\":%s}", DiscordPid(), Nonce().c_str()));
	}
	return 0;
}

}

void DiscordStart()
{
	if (!Cfg().discordEnabled || Cfg().discordAppId.empty() || g_started.exchange(true)) {
		return;
	}
	g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

void DiscordStop()
{
	if (!g_thread) {
		return;
	}
	g_stop = true;
	WaitForSingleObject(g_thread, 2000);
	CloseHandle(g_thread);
	g_thread = nullptr;
}

}
