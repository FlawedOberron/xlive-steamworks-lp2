// LP2 mods: the jeep's driving model. An original light 4x4 with arcade handling: springy raycast suspension, lots of
// grip, a little slide, steering that turns the jeep towards where the driver looks, air control and flip recovery,
// and run-over impacts that scale with speed. Seats: driver, gunner (turret), passenger.
//
// Plain C++ with no game types: the game side (planned, notes/jeep-plan.md) feeds it the ground through a raycast
// callback, the driver's input, and the units near it, then copies the body's pose onto the VS it rides on.
// Units: cm, seconds, kg; +Y is up (the game's axes). tests/jeep/jeep_test.cpp drives it on flat ground, a ramp and a wall.
#pragma once

#include <stdint.h>

namespace xls::jeep {

struct Vec3 {
	float x = 0, y = 0, z = 0;
};

struct Quat { // rotation, w last
	float x = 0, y = 0, z = 0, w = 1;
};

// The world, as the jeep sees it: the first hit along from -> to. Static geometry only (units are RunOver's).
struct RayHit {
	bool hit = false;
	Vec3 pos, normal;
	float fraction = 1.0f; // along from -> to
};
typedef RayHit (*RaycastFn)(void* user, const Vec3& from, const Vec3& to);

enum class SeatRole : uint8_t { Driver, Gunner, Passenger };

struct Seat {
	SeatRole role;
	Vec3 mount;          // body space: where the rider sits
	Vec3 exit;           // body space: where getting out puts them
	float yawMin, yawMax; // degrees, body space, the aim it allows (gunner: the turret's arc; passenger: a side cone)
	float pitchMin, pitchMax;
};

struct Params {
	float mass = 1400.0f;              // kg
	Vec3 halfSize = {95, 55, 210};     // chassis half extents (cm): 1.9 m wide, 4.2 m long
	Vec3 centreOfMass = {0, -40, 0};   // body space, low (below the wheel mounts): it slides rather than rolls
	float wheelX = 85, wheelZ = 135;   // wheel mounts at (+-x, mountY, +-z)
	float mountY = -20;
	float wheelRadius = 45;
	float restLength = 55;             // spring at rest (mount to wheel centre)
	float travel = 35;                 // how far it can compress past rest, and drop below it
	float springRate = 0;              // kg/s^2 per wheel (forces are kg cm/s^2); 0 = sized to hold the jeep at rest (Init)
	float damping = 0;                 // per wheel; 0 = from springRate (Init)
	float antiRoll = 0;                // anti-roll bars (left / right pairs); 0 = from springRate (Init)
	float topSpeed = 2600;             // cm/s forward (about 94 km/h)
	float reverseSpeed = 900;
	float driveForce = 0;              // all four wheels together; 0 = from mass (about 0.75 g)
	float brakeForce = 0;              // 0 = from mass (about 1.1 g)
	float grip = 1.1f;                 // tyre friction (load times this)
	float highSpeedGrip = 0.9f;        // sideways grip at top speed (times grip): it drifts wide instead of tipping
	float rearSlideGrip = 0.55f;       // rear grip while the handbrake is held
	float maxSteer = 32;               // degrees at the front wheels, at low speed
	float highSpeedSteer = 17;         // degrees at top speed
	float steerRate = 160;             // degrees per second the wheels turn
	float airPitch = 2.2f, airRoll = 1.6f; // air control (rad/s^2 per full input)
	float levelling = 1.2f;            // airborne: rights itself a little (rad/s^2 per rad off level)
	float gravity = 980;               // cm/s^2 (the map's)
	float flipAfter = 1.5f;            // s on its side or roof, slow, before it rights itself
	float runOverMin = 450;            // cm/s closing speed: below this a unit is only pushed aside
	float runOverKnock = 900;          // above: knocked down and hurt
	float runOverKill = 1500;          // above: killed outright (thrown far)
};

struct Input {
	float throttle = 0;   // -1 (reverse / brake) .. 1
	bool handbrake = false;
	bool lookSteer = true; // steer towards lookYaw (the driver's camera); false: steer is the stick
	float lookYaw = 0;     // radians, world (0 = +Z, positive towards +X)
	float steer = 0;       // -1 (left) .. 1 (right), when !lookSteer
	float airPitch = 0, airRoll = 0; // -1..1 while airborne
	bool flip = false;     // someone asked to right it (a player next to it pressing action)
};

struct Wheel {
	Vec3 mount;          // body space
	bool front = false;
	bool grounded = false;
	float compression = 0; // cm past rest (negative: hanging)
	float load = 0;      // N, the spring + damper force
	Vec3 contact, normal;
	float spin = 0;      // rad/s, for the model
};

struct State {
	Vec3 pos;            // body origin, world
	Quat rot;
	Vec3 vel;            // cm/s
	Vec3 angVel;         // rad/s, world
	Wheel wheels[4];
	float steerAngle = 0; // radians, + = right
	int groundedWheels = 0;
	float upsideTime = 0;
	bool airborne = false;
	float speed = 0;      // forward cm/s (signed)
};

void Init(Params* params, State* state, const Vec3& pos, float yaw);
void Step(const Params& params, State* state, const Input& input, float dt, RaycastFn ray, void* user);

// A unit near the jeep: a standing capsule.
struct Unit {
	Vec3 pos;      // feet
	Vec3 vel;
	float radius = 40, height = 180;
	float mass = 90;
};

enum class Impact : uint8_t { None, Push, Knock, Kill };

struct RunOverResult {
	Impact impact = Impact::None;
	float closing = 0;  // cm/s, jeep towards the unit
	Vec3 unitVel;       // what to give the unit (push / knock / throw)
	float damage = 0;   // 0..1 of a soldier's life
};

// Does the moving jeep hit this unit now, and how hard. Also slows the jeep a little (momentum shared). A unit
// should be checked once per contact (the game side keeps a short per-unit cool-down).
RunOverResult RunOver(const Params& params, State* state, const Unit& unit);

// The default seats: driver front left, gunner standing at the turret behind, passenger front right.
int DefaultSeats(Seat* out, int max);

// Helpers (also for the game side).
Vec3 Rotate(const Quat& q, const Vec3& v);
Vec3 Unrotate(const Quat& q, const Vec3& v);
Vec3 ToWorld(const State& s, const Vec3& local);
float Yaw(const Quat& q);

} // namespace xls::jeep
