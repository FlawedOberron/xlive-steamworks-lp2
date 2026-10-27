// Options > Network (the title / pause Options ring menu, flash\option\id500_PC): Share Custom Parts,
// Receive Custom Parts and Upload Speed, saved to xlive_steamworks.json.
//
// Our script (flash/netmenu.as, added to the menu's Flash like the create-match rows) puts a sixth
// entry under the game's five and runs its panel itself. It talks to us through the menu's
// ExternalInterface calls. Every Flash screen receives those in uPlanetFlash::vf17 (DX11 FUN_00f4b1a0,
// DX9 FUN_00f3b470; thiscall(screen, movie, name, args, count), args = Scaleform values of 12 bytes:
// +4 type (3 number, a float at +8; 4 string)); "evnt" calls go on to the screen's vf23 (or vf24 for
// a few sound events). Ours are first numbers the game never sends:
//   990, 0            the panel was built: send the texts
//   991, row, step    left / right on a row
// We answer by calling the script's _root.lp2NetSet(share, receive, speed) through the menu's Flash
// call (the one rules.cpp uses for its rows). The script's texts are MsgOption messages 401-409,
// which NetMenuPatchMessages adds to every language of the message file (packages.cpp, with the
// map names).

#include "mods/lp2.h"
#include "mods/flash/netmenu.h"
#include "mods/parts.h"

#include "core/config.h"
#include "core/log.h"
#include "core/net.h"

#include <windows.h>

#include <string>
#include <vector>

namespace xls::mods {

namespace {

struct FlashArg {
	uint8_t flag;
	uint32_t type; // 3 number (float), 4 string
	const char* text;
};

typedef bool(__attribute__((thiscall)) * FlashCallFn)(uint8_t* menu, int zero, const char* name, const FlashArg* args, int count);
typedef void(__attribute__((thiscall)) * FlashCallbackFn)(uint8_t* screen, void* movie, void* name, const uint8_t* args, uint32_t count);

struct NetMenuVas {
	uint32_t callback;  // every Flash screen's ExternalInterface receiver (uPlanetFlash::vf17)
	uint32_t flashCall;  // a menu's Flash call: thiscall(menu, 0, name, args, count)
};
const NetMenuVas kNetMenuVas[] = {
	{0xf4b1a0, 0xf4a390}, // DX11
	{0xf3b470, 0xf3a660}, // DX9
};
// sub esp,0x18; push ebx; push ebp; push esi; mov esi,ecx
const uint8_t kCallbackPrologue[] = {0x83, 0xec, 0x18, 0x53, 0x55, 0x56, 0x8b, 0xf1};

const int kEventHello = 990;
const int kEventStep = 991;

// Upload Speed values, KB/s per peer (0 = auto: Steam between 1 and 16 MB/s, net.cpp)
const uint32_t kRates[] = {0, 256, 512, 1024, 2048, 4096, 8192, 16384};
const char* const kRateNames[] = {"", // Auto: the script shows its own (translated) word
                                   "256 KB/s", "512 KB/s", "1 MB/s", "2 MB/s", "4 MB/s", "8 MB/s", "16 MB/s"};
const int kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));

FlashCallbackFn g_callbackOriginal = nullptr;
FlashCallFn g_flashCall = nullptr;

int RateIndex(uint32_t kb)
{
	int best = 0;
	for (int i = 0; i < kRateCount; i++) {
		if (kRates[i] == kb) {
			return i;
		}
		if (kRates[i] <= kb) {
			best = i; // a hand-set rate between ours shows as the next lower one
		}
	}
	return best;
}

void Push(uint8_t* screen)
{
	const Config& c = Cfg();
	FlashArg args[4] = {{0, 4, c.partsShare ? "1" : "0"}, {0, 4, c.partsReceive ? "1" : "0"}, {0, 4, kRateNames[RateIndex(c.sendRateKBytes)]},
	                    {0, 4, c.updateAuto ? "1" : "0"}};
	if (!g_flashCall || !g_flashCall(screen, 0, "lp2NetSet", args, 4)) {
		XLS_LOG_WARN("mods: network menu: the Options Flash has no lp2NetSet (entry not added).");
	}
}

void Step(int row, int step)
{
	const Config& c = Cfg();
	bool share = c.partsShare, receive = c.partsReceive, update = c.updateAuto;
	uint32_t rate = c.sendRateKBytes;
	if (row == 0) {
		share = !share;
	}
	else if (row == 1) {
		receive = !receive;
	}
	else if (row == 2) {
		rate = kRates[(RateIndex(rate) + kRateCount + (step < 0 ? -1 : 1)) % kRateCount];
	}
	else if (row == 3) {
		update = !update; // update.cpp reads it at the next start
	}
	else {
		return;
	}
	bool partsChanged = share != c.partsShare || receive != c.partsReceive;
	bool rateChanged = rate != c.sendRateKBytes;
	ConfigSetNetwork(share, receive, rate, update);
	if (partsChanged) {
		PartsNetworkChanged();
	}
	if (rateChanged) {
		NetApplySendRate();
	}
}

// Our MsgOption messages (the script's lp2NetEnglish has the same order): the entry, the three row
// labels, Auto, the help lines of the entry and the rows, then the Update Automatically row's label
// and help line. Per language block of the message file: 0 Japanese, 1 English, 2 French, 3 Spanish, 4 German, 5 Italian, 6 Korean, 10 Russian,
// 11 Polish; the rest (the game's unused Czech, Hungarian, Slovak) get English.
const uint32_t kMsgBase = 401; // the file's message count: ours follow the game's
const int kMsgCount = 11;
struct NetTexts {
	uint32_t language;
	const char* text[kMsgCount];
};
const NetTexts kTexts[] = {
	{1, {"NETWORK", "Share Custom Parts", "Receive Custom Parts", "Upload Speed", "Auto",
	     "Custom parts sharing and upload speed for online play.",
	     "Let other players download and see your custom parts.",
	     "Download and show other players' custom parts.",
	     "Upload speed to each player for custom parts and maps. Lower it on a slow connection.",
	     "Update Automatically", "Download and install new versions of the mod when the game starts."}},
	{0, {"ネットワーク", "カスタムパーツを共有", "カスタムパーツを受信", "アップロード速度", "自動",
	     "オンラインでのカスタムパーツの共有とアップロード速度を設定します。",
	     "他のプレイヤーがあなたのカスタムパーツをダウンロードして表示できます。",
	     "他のプレイヤーのカスタムパーツをダウンロードして表示します。",
	     "カスタムパーツとマップを各プレイヤーへ送る速度です。回線が遅い場合は下げてください。",
	     "自動アップデート", "ゲーム起動時にMODの新しいバージョンをダウンロードしてインストールします。"}},
	{2, {"RÉSEAU", "Partager mes pièces", "Recevoir les pièces", "Vitesse d'envoi", "Auto",
	     "Partage des pièces personnalisées et vitesse d'envoi en ligne.",
	     "Permet aux autres joueurs de télécharger et de voir vos pièces personnalisées.",
	     "Télécharge et affiche les pièces personnalisées des autres joueurs.",
	     "Vitesse d'envoi vers chaque joueur pour les pièces et les cartes. Réduisez-la si votre connexion est lente.",
	     "Mise à jour auto", "Télécharge et installe les nouvelles versions du mod au lancement du jeu."}},
	{3, {"RED", "Compartir mis piezas", "Recibir piezas", "Velocidad de subida", "Auto",
	     "Uso compartido de piezas personalizadas y velocidad de subida en línea.",
	     "Permite que otros jugadores descarguen y vean tus piezas personalizadas.",
	     "Descarga y muestra las piezas personalizadas de otros jugadores.",
	     "Velocidad de subida a cada jugador para piezas y mapas. Redúcela si tu conexión es lenta.",
	     "Actualizar automáticamente", "Descarga e instala las nuevas versiones del mod al iniciar el juego."}},
	{4, {"NETZWERK", "Eigene Teile teilen", "Fremde Teile empfangen", "Upload-Tempo", "Auto",
	     "Teilen eigener Teile und Upload-Tempo im Online-Spiel.",
	     "Andere Spieler können deine eigenen Teile herunterladen und sehen.",
	     "Lädt die eigenen Teile anderer Spieler herunter und zeigt sie an.",
	     "Upload-Tempo pro Spieler für Teile und Karten. Bei langsamer Verbindung verringern.",
	     "Automatisch aktualisieren", "Lädt neue Versionen der Mod beim Spielstart herunter und installiert sie."}},
	{5, {"RETE", "Condividi le mie parti", "Ricevi parti", "Velocità di upload", "Auto",
	     "Condivisione delle parti personalizzate e velocità di upload online.",
	     "Consente agli altri giocatori di scaricare e vedere le tue parti personalizzate.",
	     "Scarica e mostra le parti personalizzate degli altri giocatori.",
	     "Velocità di upload verso ogni giocatore per parti e mappe. Riducila con una connessione lenta.",
	     "Aggiornamento automatico", "Scarica e installa le nuove versioni della mod all'avvio del gioco."}},
	{6, {"네트워크", "커스텀 파츠 공유", "커스텀 파츠 수신", "업로드 속도", "자동",
	     "온라인 플레이의 커스텀 파츠 공유와 업로드 속도를 설정합니다.",
	     "다른 플레이어가 당신의 커스텀 파츠를 다운로드하고 볼 수 있습니다.",
	     "다른 플레이어의 커스텀 파츠를 다운로드하여 표시합니다.",
	     "커스텀 파츠와 맵을 각 플레이어에게 보내는 속도입니다. 연결이 느리면 낮추십시오.",
	     "자동 업데이트", "게임 시작 시 모드의 새 버전을 다운로드하여 설치합니다."}},
	{10, {"СЕТЬ", "Делиться деталями", "Получать детали", "Скорость отдачи", "Авто",
	      "Обмен своими деталями и скорость отдачи в сетевой игре.",
	      "Другие игроки смогут скачивать и видеть ваши детали.",
	      "Скачивать и показывать детали других игроков.",
	      "Скорость отдачи деталей и карт каждому игроку. Уменьшите при медленном соединении.",
	      "Автообновление", "Загружать и устанавливать новые версии мода при запуске игры."}},
	{11, {"SIEĆ", "Udostępniaj części", "Odbieraj części", "Prędkość wysyłania", "Auto",
	      "Udostępnianie własnych części i prędkość wysyłania w grze online.",
	      "Inni gracze mogą pobierać i widzieć twoje własne części.",
	      "Pobiera i pokazuje własne części innych graczy.",
	      "Prędkość wysyłania części i map do każdego gracza. Zmniejsz przy wolnym łączu.",
	      "Aktualizuj automatycznie", "Pobiera i instaluje nowe wersje moda przy uruchomieniu gry."}},
};

uint32_t Read32(const std::vector<uint8_t>& f, size_t at)
{
	return f[at] | f[at + 1] << 8 | f[at + 2] << 16 | (uint32_t)f[at + 3] << 24;
}

void Write32(std::vector<uint8_t>* out, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		out->push_back((uint8_t)(v >> (i * 8)));
	}
}

bool Number(const uint8_t* arg, int* out)
{
	if (*(const uint32_t*)(arg + 4) != 3) {
		return false;
	}
	*out = (int)*(const float*)(arg + 8);
	return true;
}

void __attribute__((thiscall)) FlashCallback(uint8_t* screen, void* movie, void* name, const uint8_t* args, uint32_t count)
{
	int event = 0;
	if (args && count >= 1 && Number(args, &event) && (event == kEventHello || event == kEventStep)) {
		if (event == kEventHello) {
			XLS_LOG_INFO("mods: network menu: entry added, sending its texts.");
		}
		int row = 0, step = 0;
		if (event == kEventStep && (count < 3 || !Number(args + 12, &row) || !Number(args + 24, &step))) {
			return;
		}
		if (event == kEventStep) {
			Step(row, step);
		}
		Push(screen);
		return;
	}
	g_callbackOriginal(screen, movie, name, args, count);
}

}

bool NetMenuPatchFlash(std::vector<uint8_t>* gfx)
{
	return FlashAddRootScript(gfx, std::vector<uint8_t>(std::begin(kNetMenuScript), std::end(kNetMenuScript)), "Network entry");
}

// rFlashMessage: 'MES\0', u32 version, u32 name length, name + '\0', then per language: u32 language,
// u32 count, count x (u32 length, UTF-8, '\0'). Ours are appended to blocks of exactly kMsgBase
// messages (or replace ours in a file already patched).
bool NetMenuPatchMessages(std::vector<uint8_t>* file)
{
	const std::vector<uint8_t>& f = *file;
	if (f.size() < 12 || memcmp(f.data(), "MES\0", 4) != 0) {
		return false;
	}
	size_t at = 12 + (size_t)Read32(f, 8) + 1;
	std::vector<uint8_t> out(f.begin(), f.begin() + at);
	while (at + 8 <= f.size()) {
		uint32_t language = Read32(f, at), count = Read32(f, at + 4);
		if (count < kMsgBase || count > kMsgBase + kMsgCount) {
			return false; // not the message file we know
		}
		const NetTexts* texts = &kTexts[0];
		for (const NetTexts& t : kTexts) {
			if (t.language == language) {
				texts = &t;
			}
		}
		Write32(&out, language);
		Write32(&out, kMsgBase + kMsgCount);
		at += 8;
		for (uint32_t i = 0; i < kMsgBase; i++) {
			if (at + 4 > f.size() || at + 4 + Read32(f, at) + 1 > f.size()) {
				return false;
			}
			size_t length = Read32(f, at);
			out.insert(out.end(), f.begin() + at, f.begin() + at + 4 + length + 1);
			at += 4 + length + 1;
		}
		for (uint32_t i = kMsgBase; i < count; i++) { // ours from an earlier run: replaced
			if (at + 4 > f.size() || at + 4 + Read32(f, at) + 1 > f.size()) {
				return false;
			}
			at += 4 + Read32(f, at) + 1;
		}
		for (int i = 0; i < kMsgCount; i++) {
			size_t length = strlen(texts->text[i]);
			Write32(&out, (uint32_t)length);
			out.insert(out.end(), texts->text[i], texts->text[i] + length);
			out.push_back(0);
		}
	}
	if (at != f.size() || out == f) {
		return false;
	}
	file->swap(out);
	return true;
}

void NetMenuInstall(unsigned char* base)
{
	for (const NetMenuVas& vas : kNetMenuVas) {
		uint8_t* call = base + (vas.callback - 0x400000);
		if (memcmp(call, kCallbackPrologue, sizeof(kCallbackPrologue)) != 0) {
			continue;
		}
		g_flashCall = (FlashCallFn)(base + (vas.flashCall - 0x400000));
		g_callbackOriginal = (FlashCallbackFn)Detour(call, kCallbackPrologue, sizeof(kCallbackPrologue), (void*)&FlashCallback);
		if (g_callbackOriginal) {
			XLS_LOG_INFO("mods: network menu: Flash calls hooked at 0x%08X.", vas.callback);
			return;
		}
	}
	XLS_LOG_WARN("mods: network menu: Options code not recognised, no Network entry.");
}

}
