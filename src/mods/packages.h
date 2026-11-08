#pragma once

// Mod packages: map .zip files in the Maps folder (packages.cpp). Parts: parts.cpp.

#include <math.h>

#include <array>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace xls {
class Json;
}

namespace xls::mods {

// A volume's box turned about its centre by its map.json "yaw" (degrees: the editor's turn about y, game axes):
// turn = cos, sin. Points go into the box's frame, then the min / max test (top: false = no ceiling, for water).
inline void SetTurn(float turn[2], double yawDegrees)
{
	double a = yawDegrees * 3.14159265358979323846 / 180.0;
	turn[0] = (float)cos(a);
	turn[1] = (float)sin(a);
}

inline bool InBox(const float min[3], const float max[3], const float turn[2], const float* p, bool top = true)
{
	float cx = (min[0] + max[0]) * 0.5f, cz = (min[2] + max[2]) * 0.5f;
	float dx = p[0] - cx, dz = p[2] - cz;
	float x = cx + dx * turn[0] - dz * turn[1], z = cz + dx * turn[1] + dz * turn[0];
	return x >= min[0] && x <= max[0] && z >= min[2] && z <= max[2] && p[1] >= min[1] && (!top || p[1] <= max[1]);
}

struct GravityVolume {
	float min[3], max[3]; // cm, game axes, before the turn
	float turn[2] = {1.0f, 0.0f}; // "yaw" (InBox)
	float gravity;        // multiplier for characters inside
	int sid = -1;         // script volume number (scripts.cpp switches it), -1 = always on
};

struct PostVolume {
	float min[3], max[3];  // cm, game axes, before the turn
	float turn[2] = {1.0f, 0.0f}; // "yaw" (InBox)
	float tint[3];         // multiplies the screen's red, green, blue
	float brightness;      // 1 = unchanged
	float saturation;      // 1 = unchanged, 0 = grey
	float contrast;        // 1 = unchanged
	float fade;            // seconds to fade in on entering / out on leaving, 0 = instant
	int sid = -1;         // script volume number, -1 = always on
};

// map.json gravity and screen settings, applied by gravity.cpp / post.cpp while the map is played.
struct WaterVolume {
	float min[3], max[3]; // cm, game axes, before the turn; the water's surface is the top
	float turn[2] = {1.0f, 0.0f}; // "yaw" (InBox)
	int sid = -1;         // script volume number, -1 = always on
};

struct DamageVolume {
	float min[3], max[3];  // cm, game axes, before the turn
	float turn[2] = {1.0f, 0.0f}; // "yaw" (InBox)
	float player = 0.0f, vs = 0.0f; // % of full health per second
	bool playerKill = false, vsKill = false; // at once
	int sid = -1;
};

// A one-way volume (oneway.cpp): what it holds back may cross it only along its arrow (the box's local -z, the
// editor's: world (-sin yaw, 0, -cos yaw)); moving the other way through it is stopped (characters) or removed (shells).
enum { kOnewayPlayers = 1, kOnewayVs = 2, kOnewayBullets = 4, kOnewayGrenades = 8, kOnewayAkrid = 16 };
struct OnewayVolume {
	float min[3], max[3];  // cm, game axes, before the turn
	float turn[2] = {1.0f, 0.0f}; // "yaw" (InBox)
	int flags = 0;         // kOneway*
	int sid = -1;
};

// A force volume (force.cpp): pushes what it affects (the kOneway* flags). "force" is cm/s^2 for kForceContinuous (an
// acceleration while inside), cm/s for kForceImpulse (added once on entering) and kForceVelocity (replaces the velocity
// on entering: lifts, man cannons); game axes, already turned by the editor's yaw.
enum { kForceContinuous = 0, kForceImpulse = 1, kForceVelocity = 2 };
struct ForceVolume {
	float min[3], max[3];  // cm, game axes, before the turn
	float turn[2] = {1.0f, 0.0f}; // "yaw" (InBox)
	int flags = 0;         // kOneway*
	int mode = kForceContinuous;
	float force[3] = {0.0f, 0.0f, 0.0f};
	int sid = -1;
};

// Visual scripting (map.json "script", scripts.cpp; the editor's scripts.gd, lp2mapc.script_manifest).
enum ScriptType { kScriptStart, kScriptEnter, kScriptTimer, kScriptDelay, kScriptFlipflop, kScriptCounter, kScriptSet,
	kScriptIf, kScriptVolume, kScriptLog, kScriptRandom, kScriptMath, kScriptCompare, kScriptSetValue, kScriptPlayers,
	kScriptList, kScriptListRemove, kScriptListGet, kScriptListCount, kScriptForEach, kScriptPlayerCheck, kScriptPlayerGet,
	kScriptPlayerAct, kScriptLoop, kScriptFindMarkers, kScriptGetMarker, kScriptSpawn,
	kScriptTeleport, kScriptGetPos, kScriptVector, kScriptSwitch, kScriptSequence, kScriptGate, kScriptOnce, kScriptBranch,
	kScriptChance, kScriptCooldown, kScriptZoneOutline,
	// game modes (.lp2mode, modes.cpp / scripts.cpp ModeEvent): events, then actions and data
	kModeMatchStart, kModeRoundStart, kModeRoundEnd, kModeWorldUpdate, kModeOnKill, kModeOnDeath, kModeOnSpawn,
	kModeSpawnPlayer, kModeHoldPlayer, kModeEliminate, kModeRespawnRules, kModeQueue, kModeScore, kModePlayerInfo,
	kModeTeamList, kModeEndRound, kModeEndMatch, kModeShowText,
	// mode -> map: the mode's "Fire map event" (text = the event's name, player) runs the map script's "On mode event" nodes
	// of that name (they set their player variable)
	kModeFireEvent, kScriptOnModeEvent, kScriptTypes };
const int kScriptIns = 5, kScriptOuts = 8;
struct ScriptTypeInfo {
	const char* name;
	const char* ins[kScriptIns];   // input ports, by number (null: none)
	const char* outs[kScriptOuts]; // output ports
};
inline const ScriptTypeInfo kScriptTypeInfo[kScriptTypes] = {
	{"start", {}, {"then"}},
	{"enter", {}, {"entered", "left"}},
	{"timer", {"start", "stop"}, {"tick"}},
	{"delay", {"in"}, {"then"}},
	{"flipflop", {"in"}, {"A", "B"}},
	{"counter", {"in", "reset"}, {"reached"}},
	{"set", {"in"}, {"then"}},
	{"if", {"in"}, {"on", "off"}},
	{"volume", {"on", "off", "toggle"}, {"then"}},
	{"log", {"in"}, {"then"}},
	{"random", {"in"}, {"then"}},
	{"math", {"in"}, {"then"}},
	{"compare", {"in"}, {"yes", "no"}},
	{"setvalue", {"in"}, {"then"}},
	{"players", {"in"}, {"then"}},
	{"list", {"add", "remove", "clear"}, {"then"}},
	{"listremove", {"in"}, {"then"}},
	{"listget", {"in"}, {"found", "none"}},
	{"listcount", {"in"}, {"then"}},
	{"foreach", {"in"}, {"each", "done"}},
	{"playercheck", {"in"}, {"yes", "no"}},
	{"playerget", {"in"}, {"then"}},
	{"playeract", {"in"}, {"then"}},
	{"loop", {"in"}, {"each", "done"}},
	{"findmarkers", {"in"}, {"found", "none"}},
	{"getmarker", {"in"}, {"found", "none"}},
	{"spawn", {"in"}, {"spawned", "skipped"}},
	{"teleport", {"in"}, {"moved", "skipped"}},
	{"getpos", {"in"}, {"then", "none"}},
	{"vector", {"in"}, {"then"}},
	{"switch", {"in"}, {"1", "2", "3", "4", "5", "6", "default"}},
	{"sequence", {"in"}, {"1", "2", "3", "4", "5", "6"}},
	{"gate", {"in", "open", "close", "toggle"}, {"then"}},
	{"once", {"in", "reset"}, {"then"}},
	{"branch", {"in"}, {"1", "2", "3", "4", "5", "6"}},
	{"chance", {"in"}, {"yes", "no"}},
	{"cooldown", {"in", "reset"}, {"then", "blocked"}},
	{"zoneoutline", {"show", "hide"}, {"then"}},
	{"matchstart", {}, {"then"}},
	{"roundstart", {}, {"then"}},
	{"roundend", {}, {"then"}},
	{"worldupdate", {}, {"tick"}},
	{"onkill", {}, {"then"}},
	{"ondeath", {}, {"then"}},
	{"onspawn", {}, {"then"}},
	{"spawnplayer", {"in"}, {"spawned", "skipped"}},
	{"holdplayer", {"in"}, {"then"}},
	{"eliminate", {"in"}, {"then"}},
	{"respawnrules", {"in"}, {"then"}},
	{"queue", {"push", "push front", "pop", "remove", "clear"}, {"then", "empty"}},
	{"score", {"add", "set"}, {"then"}},
	{"playerinfo", {"in"}, {"then"}},
	{"teamlist", {"in"}, {"then"}},
	{"endround", {"in"}, {}},
	{"endmatch", {"in"}, {}},
	{"showtext", {"in"}, {"then"}},
	{"modeevent", {"in"}, {"then"}},
	{"onmodeevent", {}, {"then"}},
};
// Variables' kinds ("var_kinds"): a number; a player (its match slot, -1 nobody); a list of players; a script marker
// (its number in "script_markers", -1 none); a list of markers.
enum ScriptVarKind { kVarNumber, kVarPlayer, kVarList, kVarMarker, kVarMarkers, kVarVector };
// vector ("vecop"): vectors are x, y, z (cm, game axes) and a yaw (degrees, the markers' convention). A, B: vectors (B
// a number for scale / rotate / set*); marker: the frame of tolocal (world -> the marker's own: where in it) / fromlocal
enum ScriptVecOp { kVecAdd, kVecSub, kVecScale, kVecRotate, kVecToLocal, kVecFromLocal, kVecLength, kVecGetX, kVecGetY,
	kVecGetZ, kVecGetYaw, kVecSetX, kVecSetY, kVecSetZ, kVecSetYaw, kVecOps };
inline const char* const kScriptVecOps[kVecOps] = {"add", "sub", "scale", "rotate", "tolocal", "fromlocal", "length",
	"getx", "gety", "getz", "getyaw", "setx", "sety", "setz", "setyaw"};
// playercheck / listremove conditions ("check"); a: the threshold of the "below" ones
enum ScriptCheck { kCheckGone, kCheckDead, kCheckGoneOrDead, kCheckAlive, kCheckHealthBelow, kCheckTengBelow, kChecks };
inline const char* const kScriptChecks[kChecks] = {"gone", "dead", "gone_or_dead", "alive", "health_below", "teng_below"};
// playerget ("stat")
enum ScriptStat { kStatHealth, kStatHealthPercent, kStatHealthMax, kStatTeng, kStatTengMax, kStats };
inline const char* const kScriptStats[kStats] = {"health", "health_percent", "health_max", "teng", "teng_max"};
// playeract ("action"); a: the amount
enum ScriptAction { kActDamage, kActDamagePercent, kActKill, kActAddTeng, kActRemoveTeng, kActTeleport, kActions };
inline const char* const kScriptActions[kActions] = {"damage", "damage_percent", "kill", "add_teng", "remove_teng",
	"teleport"};   // teleport: the teleport node's (to its marker), not playeract's
// math / compare operators ("op")
enum ScriptOp { kOpAdd, kOpSub, kOpMul, kOpDiv, kOpMin, kOpMax, kOpMod, kOpEq, kOpNe, kOpLt, kOpLe, kOpGt, kOpGe, kOps };
inline const char* const kScriptOps[kOps] = {"+", "-", "*", "/", "min", "max", "mod", "==", "!=", "<", "<=", ">", ">="};
// the volume settings a script may change ("setting"), each read through ScriptValue by its module
enum ScriptSetting { kSetGravity, kSetDamagePlayer, kSetDamageVs, kSetBrightness, kSetSaturation, kSetContrast,
	kSetTintR, kSetTintG, kSetTintB, kSettings };
inline const char* const kScriptSettings[kSettings] = {"gravity", "damage_player", "damage_vs", "brightness",
	"saturation", "contrast", "tint_r", "tint_g", "tint_b"};
struct ScriptLink {
	int node, in;   // the target node, its input port number
};
struct ScriptOperand {   // a number, or a variable's value
	int var = -1;
	float num = 0.0f;
};
struct ScriptNode {
	int type = kScriptStart;
	int sid = -1;          // enter / volume / setvalue: script volume number
	int who = 0;           // enter: 0 anyone, 1 players on foot, 2 VS
	int var = -1;          // set / if / random / math / listcount / playerget / loop: the variable
	int player = -1;       // enter (who went in / out) / list / listget / foreach (players or markers: the list's) /
	                       // playercheck / playerget / playeract; getmarker: the marker
	int list = -1;         // players / list / listremove / listget / listcount / foreach / findmarkers: the list variable
	int check = kCheckGone; // playercheck / listremove
	int stat = kStatHealth; // playerget
	int action = kActDamage; // playeract
	bool random = false;   // listget: a random entry rather than number a (from 1); getmarker: a random match;
	                       // spawn: anywhere in the marker's box rather than its floor centre
	int spawnKind = 4;     // spawn: kSpawnWeapon / kSpawnVs (lp2.h)
	std::vector<int> items; // spawn: one picked at random (weapon pickup items / VS types); player: the marker;
	                       // a: the chance (%)
	int marker = -1;       // teleport: the marker variable (player: who; random: anywhere in its box)
	bool face = true;      // teleport: turn to the marker's arrow (the vector's yaw)
	int vec = -1;          // teleport: to this vector variable rather than the marker
	bool fromMarker = false; // getpos: the marker's position (else the player's)
	int vecOp = kVecAdd;   // vector
	bool toggle = false;   // set: toggle (0 <-> 1) rather than a
	int value = 2;         // volume start state (unused: start_off)
	int target = 1;        // counter; branch: how many of its outputs it picks from
	int op = kOpAdd;       // math / compare
	int setting = -1;      // setvalue
	bool integer = false;  // random: whole numbers
	ScriptOperand a, b;    // set: a; random: a..b; math / compare: a op b; setvalue: a
	float seconds = 1.0f;  // timer / delay / cooldown; setvalue: fade
	float maxSeconds = 0.0f; // timer / delay: above seconds: a random time between them
	bool repeat = false, autostart = false; // timer; gate: autostart = starts open
	uint32_t rgb = 0xffcc33; // zoneoutline / showtext: "colour" (rrggbb)
	int victim = -1;       // onkill: the victim's player variable (player: the killer)
	bool alive = false;    // teamlist: only those in play (not held / eliminated)
	int info = 0;          // playerinfo: 0 team, 1 score, 2 kills, 3 deaths (this round), 4 lives left
	int scoreOf = 1;       // score / endround / endmatch: 0 the player, 1 their team, 2 their opponents (endround)
	int showTo = 0;        // showtext: 0 all, 1 the player, 2 their team
	int style = 0;         // showtext: 0 banner, 1 top, 2 hud
	bool manual = false;   // respawnrules: only by Spawn player
	std::string font;      // showtext
	std::string text;      // log ({name}: a variable's value); findmarkers / getmarker: the name (* and ? wildcards)
	std::vector<float> cases; // switch: the value of each case port (1..6)
	std::vector<ScriptLink> out[kScriptOuts]; // by output port number
};
struct ScriptBox {
	float min[3], max[3];
	float turn[2] = {1.0f, 0.0f};
	int sid;
};
struct ScriptMarker {   // "script_markers" (the editor's marker volumes), by name in natural order
	std::string name;      // lower case
	float pos[3];          // floor centre, cm
	float size[3];         // its box (cm), for "anywhere inside"
	float yaw;             // degrees, the editor's (facing -z turned by it)
};
struct ScriptGraph {
	std::vector<ScriptNode> nodes;
	std::vector<ScriptBox> boxes;   // "script_volumes": every box of the volumes the script uses
	std::vector<int> startOff;      // volumes switched off at the start
	std::vector<std::string> varNames;
	std::vector<int> varKinds;      // ScriptVarKind per variable
	std::vector<ScriptMarker> markers;
	std::vector<std::pair<int, float>> varInit; // a game mode's settings: variable, value
	int volumes = 0, vars = 0;
};
void ParseScriptGraph(const Json& js, const std::wstring& label, ScriptGraph* out);

// The bots' zones and links (map.json "nav_zones", lp2navmap.zones_and_links): bots.cpp routes zone to zone.
enum NavKind { kNavWalk, kNavJump, kNavDrop, kNavGrapple, kNavLaunch };
struct NavZone {
	std::string name;
	float pos[3];  // a spot on the walkable surface near its middle, cm
	float size[3]; // its bounds' size, cm
	std::vector<std::string> tags;  // the zone volume's "zone_tags" (lower case): e.g. koth_start
	std::vector<std::vector<std::array<float, 3>>> outline; // its outer edges: closed loops on the floor, cm
};
struct NavLink {
	int from, to;
	int kind;      // NavKind
	float a[3];    // where it starts (the ledge, the grapple's firing spot, the launch volume's floor centre), cm
	float b[3];    // where it ends (the landing, the grapple's lip)
	float length;  // the grapple's reach needed, cm (0 for the others)
};
struct NavZones {
	std::vector<NavZone> zones;
	std::vector<NavLink> links;
};

struct MapPhysics {
	bool jumpPacks = false;   // "jump_packs": NEOS's stage flag (the air boost, the AI's rules) without its x0.3
	                          // gravity; older "low_gravity" = jump packs and "gravity" x0.3
	bool underwater = false;  // "underwater": lighter characters below the map's water level (stage flag)
	float gravity = 1.0f;     // "gravity": multiplier for characters and physics objects
	std::vector<GravityVolume> volumes; // "gravity_volumes": [{"min": [x,y,z], "max": [x,y,z], "gravity": f}]
	std::vector<PostVolume> post;       // "post_volumes": [{"min", "max", "tint": [r,g,b], "brightness", "saturation", "contrast", "fade"}]
	std::vector<WaterVolume> water;     // "water_volumes": [{"min": [x,y,z], "max": [x,y,z]}] (water.cpp)
	std::vector<DamageVolume> damage;   // "damage_volumes": [{"min", "max", "player", "vs", "player_kill", "vs_kill"}] (damage.cpp)
	std::vector<OnewayVolume> oneway;   // "oneway_volumes": [{"min", "max", "players", "vs", "bullets", "grenades", "akrid"}] (oneway.cpp)
	std::vector<ForceVolume> force;     // "force_volumes": [{"min", "max", "mode", "force": [x,y,z], flags as oneway's}] (force.cpp)
	ScriptGraph script;                 // "script", "script_volumes" (scripts.cpp)
	NavZones nav;                       // "nav_zones" (bots.cpp)
};

struct MapPackage {
	std::wstring package; // file name without .zip (downloaded ones: "remote-<hash>")
	std::string name;     // shown in the map picker
	int base = 0;         // online map whose preload list (vehicles, weapons, objects) it uses
	int number = 0;       // map number the package's files were built for
	int size = -1;        // 0 small, 1 medium, 2 large, -1 = the base map's
	int slot = 0;         // online map id it plays as now (13 or 14), 0 = not bound
	std::wstring thumbnail; // unpacked thumbnail.rTexture of its binding (the create-match picture), or empty
	std::string hash;     // FNV-1a 64 of the zip (16 hex): the package's identity between players
	std::wstring zip;     // the zip it was loaded from
	bool remote = false;  // downloaded from another player (ModCache\maps\remote\), never bound by default
	std::vector<std::string> preload; // extra autoArc archives ("uVs50", "uWp63") added to the base map's preload list
	MapPhysics physics;   // gravity settings (gravity.cpp)
};

// Scans the package folders and unpacks what changed (once; thread-safe).
void PackagesLoad();
// The packages bound to the map slots right now (copies; slot set).
std::vector<MapPackage> PackageMaps();
// The gravity settings of the package bound to slot (13 / 14); false when none is bound.
bool PackagesSlotPhysics(int slot, MapPhysics* out);
// Whether a package is bound to online map slot (13 / 14) right now.
bool PackagesSlotBound(int slot);
// The create-match map picker: slot 13 holds one of our own packages at a time. Steps the choice
// by step (+1 / -1), or with enter picks the first (step > 0) or last one; false when there is no
// package that way (the picker then moves on to the stock maps).
bool PackagesPick(int step, bool enter);
// The name of the package bound to slot 13 / 14 (stays valid), or nullptr.
const char* PackagesSlotName(int slot);
// The name for slot in a "13=<name>\n14=<name>" value (another player's published map names), or "".
std::string PackagesNameIn(const std::string& value, int slot);
// The bound package's file for rel (lower game-relative path, "nativepc\..."), if it has one.
bool PackagesMapFile(const std::wstring& rel, std::wstring* out);
// A generated replacement for the shared game file rel (lower game-relative path), built from
// source (the file the game would otherwise open), or nullptr when nothing needs changing.
const std::wstring* PackagesGenerated(const std::wstring& rel, const std::wstring& source);

}
