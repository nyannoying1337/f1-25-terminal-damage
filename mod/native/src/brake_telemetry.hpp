// SPDX-License-Identifier: MIT
// Brake telemetry for the debug test run: the AI drives, so the log has to say how it brakes. Per braking zone:
// entry and lowest speed, peak deceleration, the rear wheels' share of the brake torque, each wheel's lowest rim
// speed against the car's speed (a locked wheel drops towards 0), the body slip angle and the yaw rate. Slides are
// counted anywhere. The power check (0.51.115): the mean acceleration at full throttle on a straight per speed band,
// so a step with less engine power shows against the steps without (0.51.114 log: the top speeds alone depend on
// the straight). Pure logic; the probe feeds it samples read from the game (terminal_damage_probe.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace btel {

// One physics sample of the player's car. Wheels in the game's order RL, RR, FL, FR.
struct sample {
    double dt = 0;        // s since the last sample
    float v_long = 0;     // m/s along the car's forward axis
    float v_lat = 0;      // m/s sideways
    float yaw_rate = 0;   // rad/s about the car's up axis
    float rim[4] = {};    // wheel rim speed, m/s (angular speed x radius)
    float torque[4] = {}; // brake torque applied now
    float throttle = 0;   // 0..1 (car+0x188)
};

// Full-throttle acceleration per speed band. Above 200 km/h an F1 car is limited by its power, not its grip, and
// with less power the drag takes a bigger share: 19 % less power gives about 28 % less acceleration at 220 km/h and
// about half at 280 km/h.
constexpr int n_power_bands = 4;
constexpr float power_band_kmh[n_power_bands + 1] = {200, 240, 270, 300, 400};
constexpr float full_throttle = 0.98f;
constexpr float straight_yaw = 0.1f;  // rad/s: 0.8 g sideways at 280 km/h, so fast corners do not count
struct power_bands {
    double dv[n_power_bands] = {}, t[n_power_bands] = {};
    static constexpr double min_time = 0.5;  // s in a band for a value
    bool has(int b) const { return t[b] >= min_time; }
    float accel(int b) const { return has(b) ? float(dv[b] / t[b]) : 0.0f; }
    void add(const power_bands &o)
    {
        for (int b = 0; b < n_power_bands; ++b) {
            dv[b] += o.dv[b];
            t[b] += o.t[b];
        }
    }
    void feed(float v_before, float v_now, double dt)
    {
        float kmh = (v_before + v_now) * 1.8f;  // the mean speed in km/h
        for (int b = 0; b < n_power_bands; ++b)
            if (kmh >= power_band_kmh[b] && kmh < power_band_kmh[b + 1]) {
                dv[b] += v_now - v_before;
                t[b] += dt;
            }
    }
};

// The bands as text: "200-240 km/h 5.58 (4.1 s) -17 %, 240-270 km/h -, ...", with the change against ref where both
// have a value (ref null: none). Returns the length.
inline int format_bands(char *out, int size, const power_bands &p, const power_bands *ref)
{
    int at = 0;
    out[0] = 0;
    for (int b = 0; b < n_power_bands && at < size - 64; ++b) {
        at += std::snprintf(out + at, size - at, "%s%.0f-%.0f km/h ", b ? ", " : "", double(power_band_kmh[b]),
                            double(power_band_kmh[b + 1]));
        if (!p.has(b)) {
            at += std::snprintf(out + at, size - at, "-");
            continue;
        }
        at += std::snprintf(out + at, size - at, "%.2f (%.1f s)", double(p.accel(b)), p.t[b]);
        if (ref && ref->has(b) && ref->accel(b) > 0.05f)
            at += std::snprintf(out + at, size - at, " %+.0f %%", (double(p.accel(b)) / double(ref->accel(b)) - 1) * 100);
    }
    return at;
}

// A run's steps for the summary at its end: a step that ran twice (the run paused and went on with it) adds up.
struct power_run {
    static constexpr int max_steps = 16;
    struct entry {
        const char *name = nullptr;
        bool power = false;
        power_bands p;
    };
    entry e[max_steps];
    int n = 0;
    void add(const char *name, bool power, const power_bands &p)
    {
        for (int i = 0; i < n; ++i)
            if (e[i].power == power && e[i].name && name && !std::strcmp(e[i].name, name)) {
                e[i].p.add(p);
                return;
            }
        if (n < max_steps)
            e[n++] = {name, power, p};
    }
    // All steps without a power fault.
    power_bands reference() const
    {
        power_bands r;
        for (int i = 0; i < n; ++i)
            if (!e[i].power)
                r.add(e[i].p);
        return r;
    }
};

struct zone {
    float v_in = 0, v_min = 0;    // m/s
    double time = 0;              // s
    float peak_decel = 0;         // m/s^2
    float min_ratio[4] = {1, 1, 1, 1};  // lowest rim speed / car speed
    float max_slip_deg = 0, max_yaw = 0;
    double rear_torque = 0, all_torque = 0;  // summed over the zone
    float rear_share() const { return all_torque > 0 ? float(rear_torque / all_torque) : 0.0f; }
    bool locked(int w) const { return min_ratio[w] < lock_ratio; }
    static constexpr float lock_ratio = 0.5f;
};

struct step_stats {
    int zones = 0, locks_front = 0, locks_rear = 0, slides = 0;
    double decel_sum = 0, share_sum = 0;
    float worst_slip_deg = 0;
    power_bands power;
};

constexpr float g = 9.81f;
constexpr float zone_min_speed = 60 / 3.6f;  // a zone starts above 60 km/h
constexpr float slide_on_deg = 12, slide_off_deg = 5, slide_min_speed = 15;

inline float slip_deg(float v_long, float v_lat)
{
    return std::atan2(std::fabs(v_lat), std::fabs(v_long)) * 57.2957795f;
}

struct tracker {
    enum event { none = 0, zone_done = 1, slide_started = 2 };

    zone z;              // the current or last finished zone
    step_stats st;
    bool in_zone = false, sliding = false;
    double off_for = 0;  // s the brakes have been off inside a zone
    float prev_v = -1, decel = 0;
    float torque_max = 0;  // highest total brake torque seen: "braking" is above 5 % of it

    // Feed one sample; returns a bit set of events (zone_done: z holds it, slide_started).
    int feed(const sample &s)
    {
        int ev = none;
        if (s.dt <= 0 || s.dt > 0.5) {
            prev_v = s.v_long;
            return ev;
        }
        float before = prev_v;
        if (prev_v >= 0) {
            float a = float((prev_v - s.v_long) / s.dt);
            decel += float(1.0 - std::exp(-s.dt / 0.1)) * (a - decel);  // 0.1 s smoothing
        }
        prev_v = s.v_long;
        float total = 0;
        for (float t : s.torque)
            total += std::max(0.0f, t);
        torque_max = std::max(torque_max, total);
        bool braking = torque_max > 0 && total > 0.05f * torque_max;
        float slip = s.v_long > slide_min_speed ? slip_deg(s.v_long, s.v_lat) : 0.0f;

        if (!sliding && slip > slide_on_deg) {
            sliding = true;
            ++st.slides;
            ev |= slide_started;
        } else if (sliding && slip < slide_off_deg) {
            sliding = false;
        }
        st.worst_slip_deg = std::max(st.worst_slip_deg, slip);
        if (before >= 0 && !braking && s.throttle >= full_throttle && !sliding && slip < slide_off_deg &&
            std::fabs(s.yaw_rate) < straight_yaw)
            st.power.feed(before, s.v_long, s.dt);

        if (!in_zone && braking && s.v_long > zone_min_speed) {
            in_zone = true;
            off_for = 0;
            z = {};
            z.v_in = z.v_min = s.v_long;
        }
        if (!in_zone)
            return ev;
        z.time += s.dt;
        z.v_min = std::min(z.v_min, s.v_long);
        z.peak_decel = std::max(z.peak_decel, decel);
        z.max_slip_deg = std::max(z.max_slip_deg, slip);
        z.max_yaw = std::max(z.max_yaw, std::fabs(s.yaw_rate));
        if (braking) {
            off_for = 0;
            z.rear_torque += (std::max(0.0f, s.torque[0]) + std::max(0.0f, s.torque[1])) * s.dt;
            z.all_torque += total * s.dt;
            if (s.v_long > 10)
                for (int w = 0; w < 4; ++w)
                    z.min_ratio[w] = std::min(z.min_ratio[w], std::fabs(s.rim[w]) / s.v_long);
        } else {
            off_for += s.dt;
        }
        if (off_for >= 0.3 || s.v_long < 5) {
            in_zone = false;
            ++st.zones;
            st.decel_sum += z.peak_decel;
            st.share_sum += z.rear_share();
            st.locks_rear += z.locked(0) || z.locked(1);
            st.locks_front += z.locked(2) || z.locked(3);
            ev |= zone_done;
        }
        return ev;
    }
};

}
