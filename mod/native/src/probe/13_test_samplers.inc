// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Debug samplers: shift timing, throttle, brake telemetry.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Test run, game thread: time every shift (gearbox +0x680 current gear, +0x684 target) to the frame, and count the
// gear each shift ends in (a missing gear must never show up).
void test_shift_tick(uintptr_t car)
{
    static bool shifting = false, up = false;
    static std::chrono::steady_clock::time_point began;
    if (!test_run_on.load()) {
        shifting = false;
        return;
    }
    if (test_shifts.reset.exchange(false)) {
        for (int i = 0; i < 2; ++i) {
            test_shifts.count[i] = 0;
            test_shifts.total_us[i] = 0;
            test_shifts.max_us[i] = 0;
        }
        for (auto &g : test_shifts.gear_seen)
            g = 0;
    }
    uintptr_t gearbox = 0;
    uint32_t gear = 0, target = 0;
    if (!guarded_read(car + 0x538, gearbox) || !gearbox || !guarded_read(gearbox + 0x680, gear) ||
        !guarded_read(gearbox + 0x684, target) || gear > 11 || target > 11)
        return;
    auto now = std::chrono::steady_clock::now();
    if (!shifting && target != gear) {
        shifting = true;
        up = target > gear;
        began = now;
    } else if (shifting && target == gear) {
        shifting = false;
        uint32_t us = uint32_t(std::min<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - began).count(), 10000000));
        int k = up ? 0 : 1;
        ++test_shifts.count[k];
        test_shifts.total_us[k] += us;
        if (us > test_shifts.max_us[k].load())
            test_shifts.max_us[k] = us;
        ++test_shifts.gear_seen[gear];
    }
}

// Share of the last minute at full throttle (car+0x188, after the addon's cap), for nursing a failing part.
void sample_throttle(uintptr_t car)
{
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    double dt = last ? double(now - last) / 1000.0 : 0;
    last = now;
    float throttle = 0;
    if (dt <= 0 || dt > 0.5 || player_pit.load() != 1 || !guarded_read(car + 0x188, throttle))
        return;
    float share = full_throttle_share.load();
    share += float(1.0 - std::exp(-dt / 60.0)) * ((throttle >= 0.98f ? 1.0f : 0.0f) - share);
    full_throttle_share = share;
}

// Test run brake telemetry (game thread). The game's D-BOX motion output (0x144728690) reads the car like this:
// body = [[car+0x558]+0x340]+0x18 with the orientation rows sideways +0x190, up +0x1a0, forward +0x1b0, angular
// velocity +0x210 and velocity +0x220 (world, m/s); per wheel (sys = car+0x580, +0x70.. RL RR FL FR) the rim speed
// wheel+0x188 (angular speed) x wheel+0x3c (radius), a brake value +0x45c it divides by the bias-split maximum,
// and the contact point +0x320. The brake torque applied now is wheel+0x108 XOR +0x110 (brake step 0x1446dc480).
btel::tracker brake_tel;
std::atomic<const char *> brake_summary_step{nullptr};
std::atomic<bool> brake_summary_power{false};  // the step that ended had a power fault (for the power check)
std::atomic<bool> brake_tel_reset{false};
std::atomic<bool> power_summary_due{false};  // the run stopped: log its power steps against all steps without one

// 0 ok, else why not: 1 physics chain, 2 wheel system, 3 body rows unreadable, 4 + w a wheel read, 8 not finite.
int read_brake_sample(uintptr_t car, btel::sample &s, float extra[4])
{
    uintptr_t physics = 0, rigid = 0, body = 0, sys = 0;
    if (!car || !guarded_read(car + 0x558, physics) || !physics || !guarded_read(physics + 0x340, rigid) || !rigid ||
        !guarded_read(rigid + 0x18, body) || !body)
        return 1;
    if (!guarded_read(car + 0x580, sys) || !sys)
        return 2;
    float f[0xa0 / 4];
    if (!guarded_copy(body + 0x190, f, sizeof f))
        return 3;
    // 0.51.112 log: the speed lies along +0x1b0 (forward), +0x190 is sideways.
    const float *side = f, *up = f + 4, *fwd = f + 8, *spin = f + (0x210 - 0x190) / 4, *vel = f + (0x220 - 0x190) / 4;
    auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    s.v_long = std::fabs(dot(vel, fwd));
    s.v_lat = dot(vel, side);
    s.yaw_rate = dot(spin, up);
    for (int w = 0; w < 4; ++w) {
        uintptr_t wheel = 0;
        float omega = 0, radius = 0, raw = 0;
        uint32_t enc = 0, key = 0;
        if (!guarded_read(sys + 0x70 + 8 * w, wheel) || !wheel || !guarded_read(wheel + 0x188, omega) ||
            !guarded_read(wheel + 0x3c, radius) || !guarded_read(wheel + 0x108, enc) ||
            !guarded_read(wheel + 0x110, key) || !guarded_read(wheel + 0x45c, raw))
            return 4 + w;
        s.rim[w] = omega * radius;
        s.torque[w] = std::bit_cast<float>(enc ^ key);
        extra[w] = raw;
    }
    guarded_read(car + 0x188, s.throttle);
    for (float x : {s.v_long, s.v_lat, s.yaw_rate, s.rim[0], s.rim[1], s.rim[2], s.rim[3], s.torque[0], s.torque[1],
                    s.torque[2], s.torque[3]})
        if (!std::isfinite(x))
            return 8;
    return 0;
}

// The run's full-throttle acceleration in the steps without a power fault: what the power steps compare with. Every
// step of the run for the summary at its end.
btel::power_bands power_reference;
btel::power_run power_run;

void log_brake_summary(const char *step, bool power_step)
{
    const btel::step_stats &st = brake_tel.st;
    swofty::log("[td-probe] test run: brakes in %s: %d zones, peak avg %.1f g, rear share avg %.0f %%, lock-ups front "
                "%d rear %d, slides %d, worst slip %.0f deg",
                step, st.zones, st.zones ? st.decel_sum / st.zones / btel::g : 0.0,
                st.zones ? st.share_sum / st.zones * 100.0 : 0.0, st.locks_front, st.locks_rear, st.slides,
                double(st.worst_slip_deg));
    // Power check: per band "accel m/s^2 over seconds", and for a power step the change against the reference.
    char text[320];
    btel::format_bands(text, sizeof text, st.power, power_step ? &power_reference : nullptr);
    swofty::log("[td-probe] test run: power in %s: full-throttle acceleration m/s^2 on the straights: %s%s", step, text,
                power_step ? " (against the steps without a power fault so far)" : "");
    if (!power_step)
        power_reference.add(st.power);
    power_run.add(step, power_step, st.power);
    brake_tel.st = {};
}

// The run is over: each power step against all its steps without a power fault, the later ones included (0.51.115
// log: the baseline right after the race start had the fullest battery, 8.3 m/s^2 at 200-240 km/h against 6.6-6.8 in
// the later steps, so "against the steps so far" overstated the MGU-K loss).
void log_power_summary()
{
    int refs = 0, powers = 0;
    for (int i = 0; i < power_run.n; ++i)
        ++(power_run.e[i].power ? powers : refs);
    if (powers && refs) {
        btel::power_bands ref = power_run.reference();
        char text[320];
        btel::format_bands(text, sizeof text, ref, nullptr);
        swofty::log("[td-probe] test run: power summary, the %d steps without a power fault: %s", refs, text);
        for (int i = 0; i < power_run.n; ++i)
            if (power_run.e[i].power) {
                btel::format_bands(text, sizeof text, power_run.e[i].p, &ref);
                swofty::log("[td-probe] test run: power summary, %s: %s", power_run.e[i].name, text);
            }
    }
    power_run = {};
}

void test_brake_tick(uintptr_t car)
{
    static std::chrono::steady_clock::time_point last{};
    static bool sanity_logged = false;
    static int last_fail = -1;
    static double sample_clock = 0, next_raw = 0;
    if (brake_tel_reset.exchange(false)) {
        brake_tel = {};
        power_reference = {};
        power_run = {};
        sanity_logged = false;
        last = {};
        last_fail = -1;
        sample_clock = next_raw = 0;
    }
    // The summary flag first: when it is set, the last step's summary was posted before it.
    bool summary_due = power_summary_due.exchange(false);
    if (const char *step = brake_summary_step.exchange(nullptr))
        log_brake_summary(step, brake_summary_power.load());
    if (summary_due)
        log_power_summary();
    if (!test_run_on.load())
        return;
    auto now = std::chrono::steady_clock::now();
    btel::sample smp;
    smp.dt = last == std::chrono::steady_clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - last).count();
    last = now;
    float extra[4] = {};
    if (int why = read_brake_sample(car, smp, extra); why) {
        if (why != last_fail)
            swofty::log("[td-probe] brake telemetry: no sample (reason %d: %s)", why,
                        why == 1 ? "physics chain" : why == 2 ? "wheel system" : why == 3 ? "body rows" :
                        why < 8 ? "wheel read" : "not finite");
        last_fail = why;
        return;
    }
    last_fail = 0;
    sample_clock += smp.dt;
    // Raw values: the first sample and every 20 s for the first two minutes, to check the offsets.
    if (!sanity_logged || (sample_clock >= next_raw && sample_clock < 130)) {
        next_raw = sample_clock + 20;
        sanity_logged = true;
        float throttle = -1, other = -1;
        guarded_read(car + 0x188, throttle);
        guarded_read(car + 0x18c, other);
        swofty::log("[td-probe] brake telemetry: %d km/h, v_long %.1f v_lat %.1f m/s, yaw %.2f rad/s, rim RL %.1f RR "
                    "%.1f FL %.1f FR %.1f m/s, torque %.0f %.0f %.0f %.0f, +0x45c %.2f %.2f %.2f %.2f, car+0x188 %.2f "
                    "+0x18c %.2f",
                    game_speed.load(), double(smp.v_long), double(smp.v_lat), double(smp.yaw_rate),
                    double(smp.rim[0]), double(smp.rim[1]), double(smp.rim[2]), double(smp.rim[3]),
                    double(smp.torque[0]), double(smp.torque[1]), double(smp.torque[2]), double(smp.torque[3]),
                    double(extra[0]), double(extra[1]), double(extra[2]), double(extra[3]), double(throttle),
                    double(other));
    }
    int ev = brake_tel.feed(smp);
    if (ev & btel::tracker::slide_started)
        swofty::log("[td-probe] brakes: slide at %.0f km/h, slip %.0f deg, yaw %.2f rad/s%s", smp.v_long * 3.6,
                    double(btel::slip_deg(smp.v_long, smp.v_lat)), double(smp.yaw_rate),
                    brake_tel.in_zone ? ", under braking" : "");
    if (ev & btel::tracker::zone_done) {
        const btel::zone &z = brake_tel.z;
        char locks[32] = {};
        static const char *const names[4] = {"RL", "RR", "FL", "FR"};
        for (int w = 0, at = 0; w < 4; ++w)
            if (z.locked(w))
                at += snprintf(locks + at, sizeof locks - at, "%s%s", at ? " " : "", names[w]);
        swofty::log("[td-probe] brakes: %.0f -> %.0f km/h in %.1f s, peak %.1f g, rear share %.0f %%, lowest wheel "
                    "speed RL %.2f RR %.2f FL %.2f FR %.2f, slip max %.0f deg, yaw max %.2f rad/s%s%s",
                    z.v_in * 3.6, z.v_min * 3.6, z.time, double(z.peak_decel / btel::g), z.rear_share() * 100.0,
                    double(z.min_ratio[0]), double(z.min_ratio[1]), double(z.min_ratio[2]), double(z.min_ratio[3]),
                    double(z.max_slip_deg), double(z.max_yaw), locks[0] ? ", LOCKED " : "", locks);
    }
}
