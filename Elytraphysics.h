// Elytraphysics.h - pure C++ port of Minecraft (Java) elytra / firework / kinetic-damage maths.
// No GTA or plugin-sdk dependency, so it can be unit-tested anywhere.
//
// Conventions
//   * Z is up (GTA convention).  Minecraft's Y axis maps to Z here.
//   * Velocity is in blocks per 20 Hz tick, the unit the Minecraft formulas use.
//   * `s` is the fraction of a 20 Hz tick that elapsed this frame (s = 1 reproduces
//     Minecraft exactly; smaller values give frame-rate independent stepping).
#pragma once
#include <algorithm>
#include <cmath>

namespace mcphys {

struct V3 {
    double x = 0, y = 0, z = 0;
};

constexpr double kGravity = 0.08;  // blocks / tick^2

inline double HorizontalLength(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline double Length(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// One elytra flight step. `look` is the view direction (normalised internally).
inline V3 ElytraStep(V3 v, V3 look, double s = 1.0) {
    const double ll = Length(look);
    if (ll < 1e-9) return v;
    look = {look.x / ll, look.y / ll, look.z / ll};

    const double lookH  = HorizontalLength(look);  // == cos(pitch) for a unit vector
    const double speedH = HorizontalLength(v);     // horizontal speed before this step
    const double f1     = lookH * lookH;           // cos^2(pitch) * min(1, |look| / 0.4)

    v.z += kGravity * (-1.0 + f1 * 0.75) * s;

    if (v.z < 0.0 && lookH > 0.0) {                // falling: convert some descent into forward speed
        const double d = v.z * -0.1 * f1 * s;
        v.x += look.x * d / lookH;
        v.y += look.y * d / lookH;
        v.z += d;
    }
    if (look.z > 0.0 && lookH > 0.0) {             // looking up: trade speed for height
        const double d = speedH * look.z * 0.04 * s;
        v.x += -look.x * d / lookH;
        v.y += -look.y * d / lookH;
        v.z += d * 3.2;
    }
    if (lookH > 0.0) {                             // steer horizontal velocity toward the view direction
        v.x += (look.x / lookH * speedH - v.x) * 0.1 * s;
        v.y += (look.y / lookH * speedH - v.y) * 0.1 * s;
    }

    const double dragH = std::pow(0.99, s);
    const double dragV = std::pow(0.98, s);
    v.x *= dragH;
    v.y *= dragH;
    v.z *= dragV;
    return v;
}

// One tick of an active firework rocket pushing the wearer along `look`.
inline V3 RocketStep(V3 v, V3 look, double s = 1.0) {
    const double ll = Length(look);
    if (ll < 1e-9) return v;
    look = {look.x / ll, look.y / ll, look.z / ll};
    v.x += (look.x * 0.1 + (look.x * 1.5 - v.x) * 0.5) * s;
    v.y += (look.y * 0.1 + (look.y * 1.5 - v.y) * 0.5) * s;
    v.z += (look.z * 0.1 + (look.z * 1.5 - v.z) * 0.5) * s;
    return v;
}

// Rocket lifetime in ticks: 10 * (1 + flightDuration) + nextInt(6) + nextInt(7).
inline int RocketLifetimeTicks(int flightDuration, int rand0to5, int rand0to6) {
    return 10 * (1 + flightDuration) + rand0to5 + rand0to6;
}

// Kinetic-energy damage (in Minecraft health points, 20 = full bar) when horizontal
// speed drops from `before` to `after` (blocks/tick) in a collision. <= 0 means no damage.
inline double KineticImpactDamage(double horizontalBefore, double horizontalAfter) {
    return (horizontalBefore - horizontalAfter) * 10.0 - 3.0;
}

}  // namespace mcphys
