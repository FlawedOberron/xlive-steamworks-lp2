// LP2 mods: the jeep's driving model (see jeep.h). Forces are in kg cm / s^2 (cm, not metres: the game's unit).
#include "mods/jeep/jeep.h"

#include <math.h>

namespace xls::jeep {

namespace {

const float kPi = 3.14159265f;

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3& operator+=(Vec3& a, const Vec3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
Vec3& operator-=(Vec3& a, const Vec3& b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(const Vec3& a) { return sqrtf(Dot(a, a)); }
Vec3 Normalise(const Vec3& a)
{
	float l = Length(a);
	return l > 1e-6f ? a * (1.0f / l) : Vec3{0, 0, 0};
}
float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float Wrap(float a)
{
	while (a > kPi) a -= 2 * kPi;
	while (a < -kPi) a += 2 * kPi;
	return a;
}

Quat Mul(const Quat& a, const Quat& b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat Normalise(const Quat& q)
{
	float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return l > 1e-6f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}

Quat FromYaw(float yaw)
{
	return {0, sinf(yaw / 2), 0, cosf(yaw / 2)};
}

// the box's inertia (kg cm^2), body axes
Vec3 Inertia(const Params& p)
{
	const Vec3& h = p.halfSize;
	float k = p.mass / 3.0f;
	return {k * (h.y * h.y + h.z * h.z), k * (h.x * h.x + h.z * h.z), k * (h.x * h.x + h.y * h.y)};
}

struct Body {
	const Params& p;
	State& s;
	Vec3 force, torque; // world, about the centre of mass
	Vec3 com;           // world
	Vec3 inertia;

	Vec3 PointVel(const Vec3& at) const { return s.vel + Cross(s.angVel, at - com); }
	void Push(const Vec3& f, const Vec3& at)
	{
		force += f;
		torque += Cross(at - com, f);
	}
	// world-space inverse inertia applied to a vector
	Vec3 InvInertia(const Vec3& v) const
	{
		Vec3 b = Unrotate(s.rot, v);
		return Rotate(s.rot, {b.x / inertia.x, b.y / inertia.y, b.z / inertia.z});
	}
	// an impulse at a point (contacts)
	void Impulse(const Vec3& j, const Vec3& at)
	{
		s.vel += j * (1.0f / p.mass);
		s.angVel += InvInertia(Cross(at - com, j));
	}
};

void Substep(const Params& p, State* s, const Input& in, float dt, RaycastFn ray, void* user)
{
	Body b{p, *s, {}, {}, ToWorld(*s, p.centreOfMass), Inertia(p)};
	const Vec3 up = Rotate(s->rot, {0, 1, 0});
	const Vec3 forward = Rotate(s->rot, {0, 0, 1});
	s->speed = Dot(s->vel, forward);

	// steering: towards where the driver looks (reversing: the other way), less lock at speed, rate limited
	float speedRatio = Clamp(fabsf(s->speed) / p.topSpeed, 0.0f, 1.0f);
	float lock = (p.maxSteer + (p.highSpeedSteer - p.maxSteer) * speedRatio) * kPi / 180.0f;
	float want;
	if (in.lookSteer) {
		float diff = Wrap(in.lookYaw - Yaw(s->rot));
		bool reversing = s->speed < -80.0f || (in.throttle < 0 && s->speed < 150.0f);
		if (reversing) {
			diff = -diff;
		}
		want = Clamp(diff * 1.4f, -lock, lock);
		if (in.throttle == 0 && fabsf(s->speed) < 100.0f) {
			want = 0; // standing still: the driver looks around without the wheels following
		}
	}
	else {
		want = Clamp(in.steer, -1.0f, 1.0f) * lock;
	}
	float rate = p.steerRate * kPi / 180.0f * dt;
	s->steerAngle += Clamp(want - s->steerAngle, -rate, rate);

	// wheels: spring, damper, then the tyre in the ground's plane (friction circle on the load)
	s->groundedWheels = 0;
	const float reach = p.restLength + p.travel + p.wheelRadius;
	RayHit hits[4];
	for (int i = 0; i < 4; i++) {
		Vec3 mount = ToWorld(*s, s->wheels[i].mount);
		hits[i] = ray(user, mount, mount - up * reach);
		s->wheels[i].compression = hits[i].hit ? p.restLength - (hits[i].fraction * reach - p.wheelRadius) : -p.travel;
	}
	for (int i = 0; i < 4; i++) {
		Wheel& w = s->wheels[i];
		Vec3 mount = ToWorld(*s, w.mount);
		const RayHit& hit = hits[i];
		// anti-roll bar: wheels 0/1 and 2/3 are left/right pairs
		float roll = p.antiRoll * (w.compression - s->wheels[i ^ 1].compression);
		w.grounded = hit.hit;
		if (!hit.hit) {
			w.compression = -p.travel;
			w.load = 0;
			continue;
		}
		s->groundedWheels++;
		w.contact = hit.pos;
		w.normal = hit.normal;
		Vec3 pv = b.PointVel(mount);
		float spring = p.springRate * w.compression;
		if (w.compression > p.travel) {
			spring += p.springRate * 6.0f * (w.compression - p.travel); // bump stop
		}
		float load = spring + roll - p.damping * Dot(pv, up);
		load = load < 0 ? 0 : load;
		w.load = load;
		b.Push(up * load, mount);

		Vec3 n = hit.normal;
		float steer = w.front ? s->steerAngle : 0.0f;
		Vec3 heading = Rotate(s->rot, {sinf(steer), 0, cosf(steer)});
		Vec3 f = Normalise(heading - n * Dot(heading, n));
		Vec3 side = Cross(n, f); // to the right of f (+Y up, +Z forward, +X right)
		Vec3 cv = b.PointVel(hit.pos);
		float vLong = Dot(cv, f), vLat = Dot(cv, side);
		float quarter = p.mass / 4.0f;
		float grip = p.grip * ((!w.front && in.handbrake) ? p.rearSlideGrip : 1.0f);
		float latGrip = grip * (1.0f + (p.highSpeedGrip - 1.0f) * speedRatio);
		// sideways: cancel the slip over a few substeps
		float lat = -vLat * quarter / dt * 0.35f;
		// along: drive (all four), brake, rolling
		float lng = -vLong * quarter * (0.015f + 0.00001f * fabsf(vLong)); // rolling + air
		float t = Clamp(in.throttle, -1.0f, 1.0f);
		if (t > 0 && s->speed < p.topSpeed) {
			float v = Clamp(s->speed / p.topSpeed, 0.0f, 1.0f);
			lng += p.driveForce / 4.0f * t * (1.0f - v * v * v);
		}
		else if (t < 0 && s->speed > 60.0f) {
			lng += -p.brakeForce / 4.0f * -t;
		}
		else if (t < 0 && -s->speed < p.reverseSpeed) {
			lng += p.driveForce / 4.0f * 0.6f * t * sqrtf(1.0f - Clamp(-s->speed / p.reverseSpeed, 0.0f, 1.0f));
		}
		if (!w.front && in.handbrake) {
			lng = -vLong * quarter / dt * 0.2f;
		}
		// friction ellipse: sideways up to latGrip, along up to grip
		float latLimit = latGrip * load, lngLimit = grip * load;
		float e = (latLimit > 0 ? (lat / latLimit) * (lat / latLimit) : 0) + (lngLimit > 0 ? (lng / lngLimit) * (lng / lngLimit) : 0);
		if (e > 1.0f) {
			float k = 1.0f / sqrtf(e);
			lat *= k;
			lng *= k;
		}
		b.Push(side * lat + f * lng, hit.pos);
		w.spin = vLong / p.wheelRadius;
	}
	s->airborne = s->groundedWheels == 0;

	// gravity, air control, a little levelling in the air
	b.force += Vec3{0, -p.gravity * p.mass, 0};
	if (s->airborne) {
		Vec3 control = Rotate(s->rot, {in.airPitch * p.airPitch, 0, -in.airRoll * p.airRoll});
		s->angVel += control * dt;
		s->angVel += Cross(up, {0, 1, 0}) * (p.levelling * dt);
	}

	// righting itself: on its side or roof, slow, for a while (or a player asked)
	bool tipped = up.y < 0.6f && Length(s->vel) < 250.0f && s->groundedWheels < 3;
	s->upsideTime = tipped ? s->upsideTime + dt : 0.0f;
	if ((tipped && s->upsideTime > p.flipAfter) || (in.flip && up.y < 0.7f)) {
		Vec3 axis = Normalise(Cross(up, {0, 1, 0}));
		if (Length(axis) < 0.5f) {
			axis = Rotate(s->rot, {0, 0, 1}); // on its roof: roll over sideways
		}
		s->angVel = axis * 4.0f;
		s->vel.y = 520.0f;
		s->upsideTime = 0;
	}

	// integrate (semi-implicit)
	s->vel += b.force * (dt / p.mass);
	Vec3 tb = Unrotate(s->rot, b.torque), wb = Unrotate(s->rot, s->angVel);
	Vec3 iw = {b.inertia.x * wb.x, b.inertia.y * wb.y, b.inertia.z * wb.z};
	Vec3 gyro = Cross(wb, iw);
	Vec3 alpha = {(tb.x - gyro.x) / b.inertia.x, (tb.y - gyro.y) / b.inertia.y, (tb.z - gyro.z) / b.inertia.z};
	s->angVel += Rotate(s->rot, alpha) * dt;
	s->angVel = s->angVel * (1.0f - (s->airborne ? 0.15f : 0.9f) * dt);
	s->pos += s->vel * dt;
	Quat w{s->angVel.x, s->angVel.y, s->angVel.z, 0};
	Quat dq = Mul(w, s->rot);
	s->rot = Normalise(Quat{s->rot.x + dq.x * 0.5f * dt, s->rot.y + dq.y * 0.5f * dt, s->rot.z + dq.z * 0.5f * dt, s->rot.w + dq.w * 0.5f * dt});

	// the chassis against the world: its corners and edge middles, from the centre out
	Body c{p, *s, {}, {}, ToWorld(*s, p.centreOfMass), Inertia(p)};
	const Vec3& h = p.halfSize;
	const Vec3 points[] = {{-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {-h.x, -h.y, h.z}, {h.x, -h.y, h.z}, {-h.x, h.y, -h.z}, {h.x, h.y, -h.z},
	                       {-h.x, h.y, h.z},   {h.x, h.y, h.z},   {0, 0, h.z},        {0, 0, -h.z},      {h.x, 0, 0},        {-h.x, 0, 0}, {0, h.y, 0}};
	for (const Vec3& local : points) {
		Vec3 at = ToWorld(*s, local);
		RayHit hit = ray(user, c.com, at);
		if (!hit.hit) {
			continue;
		}
		Vec3 n = hit.normal;
		float depth = Dot(hit.pos - at, n);
		if (depth <= 0) {
			continue;
		}
		s->pos += n * (depth * 0.8f);
		c.com = ToWorld(*s, p.centreOfMass);
		Vec3 r = hit.pos - c.com;
		Vec3 pv = c.PointVel(hit.pos);
		float vn = Dot(pv, n);
		if (vn >= 0) {
			continue;
		}
		float k = 1.0f / p.mass + Dot(n, Cross(c.InvInertia(Cross(r, n)), r));
		float j = -(1.0f + 0.25f) * vn / k;
		Vec3 tangent = pv - n * vn;
		Vec3 friction = Length(tangent) > 1e-3f ? Normalise(tangent) * -fminf(Length(tangent) / k, 0.6f * j) : Vec3{};
		c.Impulse(n * j + friction, hit.pos);
	}
}

} // namespace

Vec3 Rotate(const Quat& q, const Vec3& v)
{
	Vec3 u{q.x, q.y, q.z};
	Vec3 t = Cross(u, v) * 2.0f;
	return v + t * q.w + Cross(u, t);
}

Vec3 Unrotate(const Quat& q, const Vec3& v)
{
	return Rotate({-q.x, -q.y, -q.z, q.w}, v);
}

Vec3 ToWorld(const State& s, const Vec3& local)
{
	return s.pos + Rotate(s.rot, local);
}

float Yaw(const Quat& q)
{
	Vec3 f = Rotate(q, {0, 0, 1});
	return atan2f(f.x, f.z);
}

void Init(Params* p, State* s, const Vec3& pos, float yaw)
{
	if (p->springRate <= 0) {
		p->springRate = p->mass * p->gravity / 4.0f / 15.0f; // 15 cm sag at rest
	}
	if (p->antiRoll <= 0) {
		p->antiRoll = p->springRate * 2.0f;
	}
	if (p->damping <= 0) {
		p->damping = 2.0f * sqrtf(p->springRate * p->mass / 4.0f) * 0.45f;
	}
	if (p->driveForce <= 0) {
		p->driveForce = p->mass * p->gravity * 0.75f;
	}
	if (p->brakeForce <= 0) {
		p->brakeForce = p->mass * p->gravity * 1.1f;
	}
	*s = State{};
	s->pos = pos;
	s->rot = FromYaw(yaw);
	const float xs[4] = {-p->wheelX, p->wheelX, -p->wheelX, p->wheelX};
	const float zs[4] = {p->wheelZ, p->wheelZ, -p->wheelZ, -p->wheelZ};
	for (int i = 0; i < 4; i++) {
		s->wheels[i].mount = {xs[i], p->mountY, zs[i]};
		s->wheels[i].front = i < 2;
	}
}

void Step(const Params& p, State* s, const Input& in, float dt, RaycastFn ray, void* user)
{
	int steps = (int)ceilf(dt / (1.0f / 240.0f));
	steps = steps < 1 ? 1 : steps > 16 ? 16 : steps;
	for (int i = 0; i < steps; i++) {
		Substep(p, s, in, dt / steps, ray, user);
	}
}

RunOverResult RunOver(const Params& p, State* s, const Unit& u)
{
	RunOverResult r;
	Vec3 centre = u.pos + Vec3{0, u.height * 0.5f, 0};
	Vec3 local = Unrotate(s->rot, centre - s->pos);
	const Vec3& h = p.halfSize;
	if (fabsf(local.x) > h.x + u.radius || fabsf(local.z) > h.z + u.radius || fabsf(local.y) > h.y + u.height * 0.5f + 20.0f) {
		return r;
	}
	// the jeep's velocity where it meets the unit, towards the unit (sideways and up-down: a glancing hit counts less)
	Vec3 contact = ToWorld(*s, {Clamp(local.x, -h.x, h.x), Clamp(local.y, -h.y, h.y), Clamp(local.z, -h.z, h.z)});
	Vec3 jv = s->vel + Cross(s->angVel, contact - s->pos);
	Vec3 away = centre - contact;
	away.y = 0;
	if (Length(away) < 1.0f) {
		away = Vec3{jv.x, 0, jv.z};
	}
	Vec3 n = Normalise(away);
	r.closing = Dot(jv - u.vel, n);
	if (r.closing <= 0) {
		return r;
	}
	if (r.closing < p.runOverMin) {
		r.impact = Impact::Push;
		r.unitVel = u.vel + n * (r.closing * 1.05f);
		return r;
	}
	Vec3 carry = Vec3{jv.x, 0, jv.z} * 0.35f;
	if (r.closing < p.runOverKill) {
		float t = (r.closing - p.runOverKnock) / (p.runOverKill - p.runOverKnock);
		r.impact = r.closing < p.runOverKnock ? Impact::Push : Impact::Knock;
		r.damage = r.impact == Impact::Knock ? 0.2f + 0.7f * Clamp(t, 0.0f, 1.0f) : 0.05f;
		r.unitVel = n * (r.closing * 1.1f) + carry + Vec3{0, r.impact == Impact::Knock ? 350.0f : 120.0f, 0};
	}
	else {
		r.impact = Impact::Kill;
		r.damage = 1.0f;
		r.unitVel = n * (r.closing * 1.3f) + carry + Vec3{0, 600.0f, 0};
	}
	// momentum shared: a soldier barely slows a jeep
	s->vel -= n * (r.closing * u.mass / (p.mass + u.mass));
	return r;
}

int DefaultSeats(Seat* out, int max)
{
	const Seat seats[] = {
	    {SeatRole::Driver, {-40, 10, 30}, {-160, -40, 30}, -100, 100, -30, 40},
	    {SeatRole::Gunner, {0, 70, -110}, {0, -40, -280}, -180, 180, -20, 55},   // the turret turns all the way round
	    {SeatRole::Passenger, {40, 10, 30}, {160, -40, 30}, -20, 160, -30, 50}, // aims out of its side and forward
	};
	int n = 0;
	for (const Seat& seat : seats) {
		if (n < max) {
			out[n++] = seat;
		}
	}
	return n;
}

} // namespace xls::jeep
