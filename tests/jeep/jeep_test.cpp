// The jeep's driving model (src/mods/jeep) on a test world: flat ground, a ramp, a wall. Native build:
//   g++ -O2 -std=c++17 -I src tests/jeep/jeep_test.cpp src/mods/jeep/jeep.cpp -o /tmp/jeep_test && /tmp/jeep_test
// Prints each check and PASS / FAIL; exits 1 if any failed.
#include "mods/jeep/jeep.h"

#include <math.h>
#include <stdio.h>

using namespace xls::jeep;

namespace {

struct World {
	bool ramp = false;  // z 3000..4200 rises to 260 cm, then a drop back to the ground
	bool wall = false;  // a wall across z 9000..9200, 600 cm high
};

float Ground(const World& w, float x, float z)
{
	(void)x;
	if (w.ramp && z > 3000 && z < 4200) {
		return (z - 3000) / 1200.0f * 260.0f;
	}
	return 0.0f;
}

bool Solid(const World& w, const Vec3& p)
{
	if (w.wall && p.z > 9000 && p.z < 9200 && p.y < 600) {
		return true;
	}
	return p.y < Ground(w, p.x, p.z);
}

RayHit Ray(void* user, const Vec3& from, const Vec3& to)
{
	const World& w = *(const World*)user;
	RayHit r;
	Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
	float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
	int steps = (int)(len / 2.0f) + 1;
	Vec3 prev = from;
	if (Solid(w, from)) {
		return r; // starts inside: no hit (as a game raycast from inside geometry)
	}
	for (int i = 1; i <= steps; i++) {
		float t = (float)i / steps;
		Vec3 p{from.x + d.x * t, from.y + d.y * t, from.z + d.z * t};
		if (Solid(w, p)) {
			// bisect between prev and p
			float a = (float)(i - 1) / steps, b = t;
			for (int k = 0; k < 12; k++) {
				float m = (a + b) / 2;
				Vec3 q{from.x + d.x * m, from.y + d.y * m, from.z + d.z * m};
				(Solid(w, q) ? b : a) = m;
			}
			r.hit = true;
			r.fraction = b;
			r.pos = {from.x + d.x * b, from.y + d.y * b, from.z + d.z * b};
			if (w.wall && prev.z <= 9000 && r.pos.z >= 8999 && r.pos.y < 600) {
				r.normal = {0, 0, -1};
			}
			else if (w.wall && prev.z >= 9200 && r.pos.z <= 9201 && r.pos.y < 600) {
				r.normal = {0, 0, 1};
			}
			else if (w.wall && prev.y >= 600 && r.pos.z > 9000 && r.pos.z < 9200) {
				r.normal = {0, 1, 0};
			}
			else {
				float e = 5.0f;
				float hx = Ground(w, r.pos.x + e, r.pos.z) - Ground(w, r.pos.x - e, r.pos.z);
				float hz = Ground(w, r.pos.x, r.pos.z + e) - Ground(w, r.pos.x, r.pos.z - e);
				Vec3 n{-hx, 2 * e, -hz};
				float l = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
				r.normal = {n.x / l, n.y / l, n.z / l};
			}
			return r;
		}
		prev = p;
	}
	return r;
}

int g_failed = 0;

void Check(bool ok, const char* what)
{
	printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
	g_failed += ok ? 0 : 1;
}

float Up(const State& s)
{
	return Rotate(s.rot, {0, 1, 0}).y;
}

float Speed(const State& s)
{
	return sqrtf(s.vel.x * s.vel.x + s.vel.y * s.vel.y + s.vel.z * s.vel.z);
}

void Run(const Params& p, State* s, const Input& in, float seconds, World* w)
{
	for (int i = 0; i < (int)(seconds * 60); i++) {
		Step(p, s, in, 1.0f / 60.0f, Ray, w);
	}
}

const float kDeg = 3.14159265f / 180.0f;

} // namespace

int main()
{
	Params base;
	State s;

	printf("settle (dropped from 1.5 m, 3 s):\n");
	{
		World w;
		Params p = base;
		Init(&p, &s, {0, 150, 0}, 0);
		Run(p, &s, Input{}, 3.0f, &w);
		printf("    y %.1f, speed %.1f cm/s, up %.3f, wheels %d, sag %.1f cm\n", s.pos.y, Speed(s), Up(s), s.groundedWheels, s.wheels[0].compression);
		Check(Speed(s) < 5 && Up(s) > 0.99f && s.groundedWheels == 4, "comes to rest level on four wheels");
	}

	printf("acceleration (flat, full throttle):\n");
	{
		World w;
		Params p = base;
		Init(&p, &s, {0, 60, 0}, 0);
		Run(p, &s, Input{}, 1.0f, &w);
		Input in;
		in.throttle = 1;
		float at[3];
		Run(p, &s, in, 2.0f, &w);
		at[0] = s.speed;
		Run(p, &s, in, 2.0f, &w);
		at[1] = s.speed;
		Run(p, &s, in, 4.0f, &w);
		at[2] = s.speed;
		printf("    %.0f / %.0f / %.0f cm/s at 2 / 4 / 8 s (top %.0f), up %.3f\n", at[0], at[1], at[2], p.topSpeed, Up(s));
		Check(at[2] > p.topSpeed * 0.88f && at[2] < p.topSpeed * 1.05f, "reaches about top speed");
		Check(Up(s) > 0.97f, "stays level");
		Input brake;
		brake.throttle = -1;
		float before = s.pos.z;
		int t = 0;
		while (s.speed > 30 && t < 600) {
			Step(p, &s, brake, 1.0f / 60.0f, Ray, &w);
			t++;
		}
		printf("    brakes to a stop in %.1f s over %.1f m\n", t / 60.0f, (s.pos.z - before) / 100.0f);
		Check(t / 60.0f < 4.0f, "brakes within 4 s");
	}

	printf("look steering (at speed, look 90 degrees right):\n");
	{
		World w;
		Params p = base;
		Init(&p, &s, {0, 60, 0}, 0);
		Input in;
		in.throttle = 1;
		Run(p, &s, in, 4.0f, &w);
		in.lookYaw = 90 * kDeg;
		float minUp = 1;
		for (int i = 0; i < 180; i++) {
			Step(p, &s, in, 1.0f / 60.0f, Ray, &w);
			minUp = fminf(minUp, Up(s));
		}
		float yaw = Yaw(s.rot) / kDeg;
		printf("    heading %.1f deg after 3 s, speed %.0f, lowest up %.3f\n", yaw, s.speed, minUp);
		Check(fabsf(yaw - 90) < 15, "turns to where the driver looks");
		Check(minUp > 0.8f, "doesn't roll over in the turn");
	}

	printf("handbrake (same turn, rear sliding):\n");
	{
		World w;
		float rates[2];
		for (int hb = 0; hb < 2; hb++) {
			Params p = base;
			Init(&p, &s, {0, 60, 0}, 0);
			Input in;
			in.throttle = 1;
			Run(p, &s, in, 3.0f, &w);
			in.lookSteer = false;
			in.steer = 1;
			in.handbrake = hb == 1;
			Run(p, &s, in, 0.8f, &w);
			rates[hb] = fabsf(s.angVel.y) / kDeg;
		}
		printf("    yaw rate %.0f deg/s without, %.0f with\n", rates[0], rates[1]);
		Check(rates[1] > rates[0] * 1.15f, "handbrake turns it tighter");
	}

	printf("ramp jump (2.6 m ramp at full speed):\n");
	{
		World w;
		w.ramp = true;
		Params p = base;
		Init(&p, &s, {0, 60, 0}, 0);
		Input in;
		in.throttle = 1;
		float maxY = 0, air = 0, minUp = 1;
		for (int i = 0; i < 60 * 8; i++) {
			Step(p, &s, in, 1.0f / 60.0f, Ray, &w);
			maxY = fmaxf(maxY, s.pos.y);
			air += s.airborne ? 1.0f / 60.0f : 0.0f;
			if (s.pos.z > 4200) {
				minUp = fminf(minUp, Up(s));
			}
		}
		printf("    peak %.0f cm, %.2f s in the air, lowest up after the ramp %.3f, now up %.3f, speed %.0f\n", maxY, air, minUp, Up(s), s.speed);
		Check(air > 0.4f, "leaves the ground off the ramp");
		Check(Up(s) > 0.95f && minUp > 0.5f, "lands on its wheels");
	}

	printf("wall (full speed into a wall):\n");
	{
		World w;
		w.wall = true;
		Params p = base;
		Init(&p, &s, {0, 60, 0}, 0);
		Input in;
		in.throttle = 1;
		float maxZ = 0;
		for (int i = 0; i < 60 * 10; i++) {
			Step(p, &s, in, 1.0f / 60.0f, Ray, &w);
			maxZ = fmaxf(maxZ, s.pos.z + p.halfSize.z);
		}
		printf("    front reached z %.0f (wall at 9000), speed now %.0f, up %.3f\n", maxZ, s.speed, Up(s));
		Check(maxZ < 9060, "doesn't go through the wall");
	}

	printf("flip recovery (on its roof):\n");
	{
		World w;
		Params p = base;
		Init(&p, &s, {0, 120, 0}, 0);
		s.rot = {0, 0, 1, 0}; // 180 degrees about Z
		Run(p, &s, Input{}, 6.0f, &w);
		printf("    up %.3f after 6 s\n", Up(s));
		Check(Up(s) > 0.95f, "rights itself");
	}

	printf("run over (a standing soldier 6 m ahead):\n");
	{
		const float speeds[] = {300, 700, 1200, 2000};
		const char* names[] = {"none", "push", "knock", "kill"};
		Impact got[4];
		for (int i = 0; i < 4; i++) {
			World w;
			Params p = base;
			Init(&p, &s, {0, 60, 0}, 0);
			Run(p, &s, Input{}, 1.0f, &w);
			s.vel = {0, 0, speeds[i]};
			Unit u;
			u.pos = {30, 0, 600};
			RunOverResult r;
			for (int f = 0; f < 120 && r.impact == Impact::None; f++) {
				Input in; // coasting: the speed it was given
				Step(p, &s, in, 1.0f / 60.0f, Ray, &w);
				r = RunOver(p, &s, u);
			}
			got[i] = r.impact;
			printf("    %4.0f cm/s: %s, closing %.0f, damage %.2f, thrown (%.0f, %.0f, %.0f)\n", speeds[i], names[(int)r.impact], r.closing, r.damage,
			       r.unitVel.x, r.unitVel.y, r.unitVel.z);
		}
		Check(got[0] == Impact::Push && got[1] == Impact::Push && got[2] == Impact::Knock && got[3] == Impact::Kill, "push / push / knock / kill by speed");
	}

	printf("%s (%d failed)\n", g_failed ? "FAILED" : "all passed", g_failed);
	return g_failed ? 1 : 0;
}
