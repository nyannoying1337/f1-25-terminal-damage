#pragma once
// Engine power over time while a power unit part fails or derates (0.51.114). Pure model, unit tested. The addon
// applies power() as a Swofty engine.power_scale layer: the game's drive torque times the share, at every throttle
// position (the throttle cap before it only limited the pedal, and the game's pedal-to-torque map is not linear).
//
// Values from timing data and reports (re/failure-realism-research.md, sections 1 and 5):
// - MGU-K lost: it gives 120 kW (160 hp) of 760-1000 hp, so the car keeps 0.75-0.84 of its power. A step within a
//   lap, in some cases after a lap at half effect, then constant to the flag; 1 of 6 real cases cut in and out
//   (Grosjean, Brazil 2019). Canada 2014: +2.8 s a lap for 5 laps, then +1.2-2.5 s. A test should see -15 to
//   -25 km/h at the end of the longest straight.
// - Battery or power unit temperature derate (Russell, Bahrain 2024): deployment cut on the straights only (speed
//   trap -11 km/h, lap time 0-0.5 s), worst from lap 4 to 15 after the warning, eased from lap 16, gone by lap 40.
// - Partial misfire (Leclerc, Canada 2024): about 80 hp, -4 to -10 km/h, about 0.3 s a lap, rough running, fixed
//   by a power cycle after about 15 laps.
// - Dead cylinder (Leclerc, Bahrain 2019): the loss grew over 3 laps (+0.1, +2.4, +4.4 s), then 4.6-6.5 s a lap and
//   "25 kph slower on the straights"; early in a race such cars retired within 2-4 laps (Vettel, Suzuka 2017).
// - Oil or water leak (Ocon, Mexico 2022; Piastri, Austin 2023): no power loss at first, then lift and coast and a
//   turned-down engine (a safer mode keeps 0.89-0.95), out when the temperatures keep rising.
// - Turbo (Rosberg, Abu Dhabi 2014): getting worse, +1.2 s, then +3.8 s, about +3 s, about +6 s at the end.
// - MGU-H lost: the MGU-K can spend 4 MJ a lap but braking refills only 2 MJ, so after 1-2 laps it works on about
//   half of each lap (+0.5-2 s a lap).
// - Control electronics: short cut-outs, mostly without warning.
// On a drag-limited straight top speed goes with power^(1/3): 0.82 is -6 %, about -20 km/h at 320 km/h.
#include <algorithm>
#include <cmath>

namespace pl {

enum Kind : int {
    none,
    mguk_loss,       // the MGU-K gives nothing: a step to 0.78-0.84
    battery_derate,  // battery temperature: deployment cut above 230-260 km/h, builds over 2-10 laps, then clears
    misfire,         // partial misfire: 0.90-0.94 with rough dips, fixed after 10-20 laps or by a pit stop
    dead_cylinder,   // a cylinder drops out: 0.80-0.85 within 2-3 laps, rough running
    leak,            // oil or water leak: nothing in the first half, then a turned-down engine, cut-outs at the end
    turbo,           // the turbo loses boost: down to 0.70-0.80 at the end, surges (cut-outs) in the last 30 %
    mguh,            // MGU-H: the battery is not recharged, 0.90 from half the lead
    electrical,      // control electronics: cut-outs
    n_kinds
};

inline const char *kind_name(int k)
{
    static const char *const names[n_kinds] = {"none", "MGU-K lost", "battery derate", "misfire", "dead cylinder",
                                               "leak", "turbo", "MGU-H", "electrical"};
    return k >= 0 && k < n_kinds ? names[k] : "?";
}

struct State {
    int kind = none;
    double t = 0;          // seconds of driving since the onset
    double length = 0;     // seconds of the course: until the part fails or the fault clears; 0 = the rest of the race
    float depth = 0;       // the full loss: 1 - the power share left
    double build_s = 0;    // seconds to the full loss (the MGU-K's half-effect lap, the cylinder's ramp, the derate's build)
    double hold_s = 0;     // battery derate: full until here, then it eases until length
    bool intermittent = false;  // MGU-K: the loss comes and goes
    bool phase_on = true;
    double phase_end = 0;
    double next_cut = 0, cut_end = -1;  // the next cut-out or rough dip and the end of the current one
};

constexpr float cut_power = 0.15f;  // what is left during a cut-out
constexpr float rough_drop = 0.12f; // a rough dip of a misfiring engine takes this much more
constexpr float derate_from = 230.0f, derate_full = 260.0f;  // km/h: the battery derate's share of its depth

// Start a course. length: seconds until the part fails (warning signs) or the fault clears, 0 = to the flag.
// warning: the signs before a failure, which reach their full size at its end. quick: the test run's short version.
// r() gives uniform numbers in [0, 1).
template <class R> State start(int kind, double length, R &&r, bool warning = false, bool quick = false)
{
    State s;
    s.kind = kind;
    s.length = std::max(0.0, length);
    switch (kind) {
    case mguk_loss:
        s.depth = float(0.16 + 0.06 * r());
        // A lap at half effect first in half the cases (a warning gives at most a third of its lead to it).
        if (r() < 0.5)
            s.build_s = quick ? 10.0 : warning ? std::min(90.0, s.length / 3) : 90.0;
        s.intermittent = !warning && !quick && r() < 0.15;
        s.phase_end = s.intermittent ? 10.0 + 30.0 * r() : 0.0;
        s.phase_on = !s.intermittent;
        break;
    case battery_derate:
        // 0.84-0.90 at the top of the straights: Bahrain 2024 measured -11 km/h at the speed trap (0.89 by the cube
        // law); the research's 0.82 cap gave -13 to -20 km/h in the model.
        s.depth = float(0.10 + 0.06 * r());
        if (warning) {
            s.build_s = s.hold_s = s.length;  // worse and worse until the battery gives up
        } else {
            s.build_s = quick ? std::min(15.0, s.length / 2) : 180.0 + 720.0 * r();  // 2-10 laps
            s.hold_s = quick ? s.length * 0.7 : std::max(s.build_s, 1350.0);        // worst until about lap 15
        }
        break;
    case misfire:
        s.depth = float(0.06 + 0.04 * r());
        s.next_cut = 3.0 + 5.0 * r();
        break;
    case dead_cylinder:
        s.depth = float(0.15 + 0.05 * r());
        s.build_s = quick ? 15.0 : 180.0 + 90.0 * r();  // 2-3 laps
        if (s.length > 0)
            s.build_s = std::min(s.build_s, 0.5 * s.length);
        s.next_cut = 4.0 + 6.0 * r();
        break;
    case leak:
        s.depth = float(0.06 + 0.04 * r());
        break;
    case turbo:
        s.depth = float(0.20 + 0.10 * r());
        break;
    case mguh:
        s.depth = 0.10f;
        break;
    default:
        break;
    }
    return s;
}

// Share of the course done (0 for a course to the flag).
inline double progress(const State &s) { return s.length > 0 ? std::clamp(s.t / s.length, 0.0, 1.0) : 0.0; }

// Cut-outs (to cut_power): the leak's last fifth, the turbo's last 30 %, control electronics all the time.
inline bool cutting(const State &s)
{
    double x = progress(s);
    return (s.kind == leak && x >= 0.8) || (s.kind == turbo && x >= 0.7) || s.kind == electrical;
}
// Rough running: short dips of a misfiring engine.
inline bool rough(const State &s) { return s.kind == misfire || s.kind == dead_cylinder; }

// Seconds to the next cut-out: rarely at first, more often as the failure comes closer.
template <class R> double cut_gap(double left, R &&r)
{
    return left > 300 ? 30.0 + 40.0 * r() : left > 60 ? 12.0 + 14.0 * r() : 5.0 + 6.0 * r();
}

// Advance by dt seconds of driving.
template <class R> void advance(State &s, double dt, R &&r)
{
    if (s.kind == none || !(dt > 0))
        return;
    s.t += dt;
    if (s.intermittent && s.t >= s.phase_end) {
        s.phase_on = !s.phase_on;
        s.phase_end = s.t + (s.phase_on ? 3.0 + 5.0 * r() : 10.0 + 30.0 * r());
    }
    if (s.t < s.next_cut)
        return;
    if (rough(s)) {
        s.cut_end = s.t + 0.1 + 0.3 * r();
        s.next_cut = s.t + (s.kind == misfire ? 8.0 + 17.0 * r() : 5.0 + 10.0 * r());
    } else if (cutting(s)) {
        s.cut_end = s.t + 0.25 + 0.45 * r();
        s.next_cut = s.t + cut_gap(s.length > 0 ? s.length - s.t : 1e9, r);
    }
}

// Battery derate: how much of its depth is in force (0..1) at this point of the course.
inline double derate_level(const State &s)
{
    if (s.t < s.build_s)
        return s.build_s > 0 ? s.t / s.build_s : 1.0;
    if (s.t < s.hold_s || s.length <= s.hold_s)
        return 1.0;
    return std::max(0.0, 1.0 - (s.t - s.hold_s) / (s.length - s.hold_s));
}

// The loss without cut-outs, rough dips and the derate's speed share: what the damage panel shows.
inline float steady_loss(const State &s)
{
    double x = progress(s);
    switch (s.kind) {
    case mguk_loss:
        return s.t < s.build_s ? s.depth / 2 : s.depth;
    case battery_derate:
        return s.depth * float(derate_level(s));
    case misfire:
        return s.depth;
    case dead_cylinder:
        return s.depth * float(s.build_s > 0 ? std::min(1.0, s.t / s.build_s) : 1.0);
    case leak:
        return x < 0.5 ? 0.0f : s.depth;
    case turbo:
        return s.depth * float(x);
    case mguh:
        return s.depth * float(std::min(1.0, 2.0 * x));
    default:
        return 0.0f;
    }
}

// The engine power share now (1 = full) at v_kmh.
inline float power(const State &s, float v_kmh)
{
    if (s.kind == none)
        return 1.0f;
    float loss = steady_loss(s);
    if (s.kind == mguk_loss && s.intermittent && !s.phase_on)
        loss = 0;
    if (s.kind == battery_derate)
        loss *= std::clamp((v_kmh - derate_from) / (derate_full - derate_from), 0.0f, 1.0f);
    float p = 1.0f - loss;
    if (s.t < s.cut_end)
        p = rough(s) ? p - rough_drop : std::min(p, cut_power);
    return std::clamp(p, 0.0f, 1.0f);
}

// Top speed share on a drag-limited straight for a power share p.
inline double top_speed_share(double p) { return std::cbrt(std::max(0.0, p)); }

}  // namespace pl
