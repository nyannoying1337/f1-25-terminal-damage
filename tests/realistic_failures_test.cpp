// Standalone Windows test. No game hooks, sockets or patch installation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <bit>
#include <cassert>
#include "test_util.hpp"

namespace {
uint8_t (*brake_wheels)[0x200] = nullptr;
const uint32_t *brake_keys = nullptr;
void set_torque_raw(int w, float v)
{
    uint32_t e = std::bit_cast<uint32_t>(v) ^ brake_keys[w];
    memcpy(brake_wheels[w] + 0xf8, &e, 4);
}
using tu::close_to;
}  // namespace

int main()
{
    SwoftyApi api{};
    api.log = tu::quiet_log;
    swofty::api = &api;

    // ---- Model: every era's shares add up, the V8 era has no turbo or MGU-H ----
    for (const fm::Era &e : fm::eras) {
        int total = 0;
        for (int i = 0; i < fm::n_outcomes; ++i)
            total += e.share[i];
        assert(total == 1000);
    }
    for (const fm::Season &s : fm::seasons) {
        assert(s.era >= 0 && s.era < int(sizeof fm::eras / sizeof fm::eras[0]));
        // Over a full Grand Prix the chance of a failure is the season's measured rate.
        double p = 1.0 - std::exp(-fm::hazard_per_second(s) * fm::gp_minutes * 60.0);
        assert(close_to(p, s.race_chance, 1e-9));
    }
    const fm::Era &v8 = fm::era_of(fm::season_of("2012"));
    assert(v8.share[fm::mguh] == 0 && v8.share[fm::tc] == 0);
    assert(&fm::season_of("nonsense") == &fm::seasons[fm::n_seasons - 1]);
    assert(fm::season_of("2017").race_chance == 0.150);

    // pick_outcome follows the shares.
    const fm::Era &e14 = fm::era_of(fm::season_of("2014"));
    int count[fm::n_outcomes]{};
    for (int i = 0; i < 100000; ++i)
        ++count[fm::pick_outcome(e14, (i + 0.5) / 100000.0)];
    for (int o = 0; o < fm::n_outcomes; ++o)
        assert(std::abs(count[o] - e14.share[o] * 100) <= 1);

    // Exponential timing: the median is ln 2 / hazard; a shorter race sees proportionally fewer failures.
    const fm::Season &s14 = fm::season_of("2014");
    double median = fm::seconds_until_failure(s14, 0.5);
    assert(close_to(median, std::log(2.0) / fm::hazard_per_second(s14), 1e-6));
    assert(fm::seconds_until_failure(s14, 0.0) >= 1e17);
    double short_race = 1.0 - std::exp(-fm::hazard_per_second(s14) * 0.25 * fm::gp_minutes * 60.0);
    assert(short_race > 0.03 && short_race < 0.035);

    // Nursing: normal driving keeps the full rate, strong lift and coast stretches the time.
    assert(fm::nursing_factor(0.65) == 1.0);
    assert(close_to(fm::nursing_factor(0.30), 0.5, 1e-9));
    assert(fm::nursing_factor(0.45) < 1.0 && fm::nursing_factor(0.45) > 0.5);
    assert(fm::warning_of(fm::suspension).chance == 0.0);

    // ---- Warning signs: a power course per part on the failure clock, the brakes fade, nothing before ----
    auto warn = [](int outcome) {
        reset_session();
        gp_clock = 100;
        gp_at = 300;
        gp_warn = {};
        gp_warn.outcome = outcome;
        gp_warn.warn_at = 200;
        gp_warning_tick(0.016);
        assert(!gp_warn.announced && power_warn.kind == pl::none);
        gp_clock = 200;
        gp_warning_tick(0.016);
        assert(gp_warn.announced);
    };
    warn(fm::ice);
    assert((power_warn.kind == pl::leak || power_warn.kind == pl::dead_cylinder) && power_warn.length == 100);
    gp_clock = 290;  // 90 % of the lead: the leak cuts out, the cylinder is gone
    gp_warning_tick(0.016);
    assert(std::fabs(power_warn.t - 90) < 1e-9 && pl::steady_loss(power_warn) > 0.05f);
    warn(fm::es);
    assert(power_warn.kind == pl::battery_derate && pl::steady_loss(power_warn) == 0.0f);
    gp_clock = 250;
    gp_warning_tick(0.016);
    assert(std::fabs(pl::steady_loss(power_warn) - power_warn.depth / 2) < 1e-4f);
    assert(pl::power(power_warn, 150) == 1.0f && pl::power(power_warn, 300) < 1.0f);
    warn(fm::brakes);
    assert(power_warn.kind == pl::none && brake_fade.load() == 0.0f);
    gp_clock = 250;
    gp_warning_tick(0.016);
    assert(std::fabs(brake_fade.load() - 0.225f) < 1e-6f);
    // Power on track: the course times the partial fault, the panel's loss; full power in the pit lane.
    warn(fm::mguk);
    player_vehicle = 3;
    player_pit = 1;
    gp_clock = 299;
    gp_warning_tick(0.016);
    power_tick(0.016);
    assert(std::fabs(shown_power_loss.load() - power_warn.depth) < 1e-5f && shown_power_kind.load() == pl::mguk_loss);
    assert(std::fabs(warning_cap.load() - std::round((1.0f - power_warn.depth) * 100.0f) / 100.0f) < 1e-6f);
    player_pit = 2;
    power_tick(0.016);
    assert(warning_cap.load() == 1.0f);
    // A reset clears it all. power_tick recomputes warning_cap and shown_power_loss from power_warn, so they start at
    // a value power_tick would not write and are checked right after the reset.
    warning_cap = 0.5f;
    shown_power_loss = 0.25f;
    reset_session();
    assert(warning_cap.load() == 1.0f && shown_power_loss.load() == 0.0f);
    power_tick(0.016);
    assert(gp_warn.outcome == -1 && warning_cap.load() == 1.0f && gp_at == -1 && power_warn.kind == pl::none &&
           brake_fade.load() == 0.0f && shown_power_loss.load() == 0.0f);

    // ---- Partial power faults: MGU-K for the rest of the race, a misfire fixed in the pits, the derate clears ----
    {
        reset_session();
        player_vehicle = 3;
        player_pit = 1;
        gp_clock = 100;
        gp_fault_at = 100;
        gp_fault_type_rolled = fm::f_mguk_loss;
        gp_fault_tick();
        assert(power_fault.kind == pl::mguk_loss && power_fault.length == 0 && fault_request.load() == 0);
        for (int k = 0; k < 200; ++k)
            power_tick(1.0);
        assert(power_fault.kind == pl::mguk_loss && shown_power_loss.load() >= 0.16f);
        reset_session();
        player_vehicle = 3;
        player_pit = 1;
        start_power_fault(fm::f_misfire, 1200, false);
        power_tick(1.0);
        assert(power_fault.kind == pl::misfire);
        player_pit = 2;  // in the pits: a power cycle
        power_tick(1.0);
        player_pit = 1;
        power_tick(1.0);
        assert(power_fault.kind == pl::none && warning_cap.load() == 1.0f);
        start_power_fault(fm::f_battery_derate, 100, true);
        assert(power_fault.kind == pl::battery_derate && power_fault.build_s == 15);
        for (int k = 0; k < 120; ++k)
            power_tick(1.0);
        assert(power_fault.kind == pl::none);  // cleared after its length
        // MGU-K lost: a third of the cars get overheated rear brakes 10-20 min later.
        int overheated = 0;
        for (int k = 0; k < 300; ++k) {
            start_power_fault(fm::f_mguk_loss, 0, false);
            overheated += power_fault_brake_at >= 600 && power_fault_brake_at <= 1200;
        }
        // The dice are seeded from the tick count and the process id, so every run differs: 300 draws at one third
        // (mean 100, sigma 8.2), the bounds are +-5.5 sigma (70..130 failed about once in 3400 runs).
        assert(overheated > 55 && overheated < 145);
        while (power_fault_brake_at < 0)
            start_power_fault(fm::f_mguk_loss, 0, false);
        power_fault.t = power_fault_brake_at - 0.5;
        power_tick(1.0);
        assert(rear_brake_loss_target.load() >= 0.35f && rear_brake_loss_target.load() <= 0.60f && power_fault_brake_at < 0);
        reset_session();
    }

    // ---- Planning: the outcome and its warning lead come from the era ----
    for (int i = 0; i < 200; ++i) {
        gp_clock = 25;
        gp_at = -1;
        gp_plan_realistic();
        assert(gp_at > gp_clock);
        assert(gp_warn.outcome >= 0 && gp_warn.outcome < fm::n_outcomes);
        fm::Warning w = fm::warning_of(gp_warn.outcome);
        if (gp_warn.warn_at < 1e17)
            assert(gp_at - gp_warn.warn_at <= w.max_lead_s + 1e-6);
        assert(gp_warn.outcome != fm::suspension || gp_warn.warn_at >= 1e17);
    }

    // ---- Partial failures: shares add up, the V8 era has no ERS modes, durations make sense ----
    for (const fm::FaultEra &fe : fm::fault_eras) {
        int total = 0;
        for (int i = 0; i < fm::n_fault_types; ++i)
            total += fe.share[i];
        assert(total == 1000 && fe.share[0] == 0 && fe.share[4] == 0 && fe.share[6] == 0 &&
               fe.share[fm::f_rear_brakes] == 150);
        assert(total == 1000);
        for (int i = 0; i < 1000; ++i) {
            int t = fm::pick_fault(fe, (i + 0.5) / 1000.0);
            assert(fe.share[t] > 0);
            fm::FaultSpec spec = fm::fault_spec(t);
            assert(spec.max_duration >= spec.min_duration && spec.min_duration > 0);
        }
    }
    assert(fm::fault_era_of(fm::season_of("2013")).share[fm::f_ers_modes] == 0);
    assert(fm::seconds_until_fault(fm::season_of("2012"), 0.0) >= 1e17);
    assert(close_to(fm::fault_race_chance(fm::season_of("2014")), 0.7 * 0.126, 1e-12));
    // 2026: new era without MGU-H, the least reliable season since 2017.
    assert(fm::era_of(fm::season_of("2026")).share[fm::mguh] == 0 && fm::season_of("2026").race_chance == 0.125);
    // About 40 % of failures give warning signs (weighted by the era's shares).
    double warned = 0;
    for (int o = 0; o < fm::n_outcomes; ++o)
        warned += fm::era_of(fm::season_of("2024")).share[o] / 1000.0 * fm::warning_of(o).chance;
    assert(warned > 0.3 && warned < 0.45);

    // A partial failure request packs type, duration, unit and gear for the game thread.
    reset_session();
    gp_clock = 100;
    gp_fault_at = 100;
    gp_fault_type_rolled = fm::f_gear_jammed;
    gp_fault_tick();
    uint64_t req = fault_request.load();
    assert((req & 0xff) == uint64_t(fm::f_gear_jammed) && ((req >> 40) & 1) == 1);
    double dur = double((req >> 8) & 0xffffffff) / 10.0;
    assert(dur >= 8 && dur <= 30);
    assert(gp_fault_at >= 1e17);
    // Missing gear names a gearbox index between 2nd (3) and 7th (8); slow shifts carry a magnitude of 100-200.
    for (int i = 0; i < 50; ++i) {
        gp_fault_at = 100;
        gp_fault_type_rolled = i % 2 ? fm::f_missing_gear : fm::f_slow_shift;
        gp_fault_tick();
        uint64_t r = fault_request.exchange(0);
        uint64_t gear = (r >> 48) & 0xff, mag = (r >> 56) & 0xff;
        if (i % 2)
            assert(gear >= 3 && gear <= 8 && mag == 0);
        else
            assert(gear == 0xff && mag >= 3 && mag <= 15);
    }
    // Severity tiers: mild 60 % (3-6x), moderate 30 % (6-10x), severe 10 % (10-15x), every size reachable.
    int tiers[3]{}, lo = 99, hi = 0;
    for (int i = 0; i < 1000; ++i) {
        fm::ShiftSeverity sv = fm::slow_shift_severity((i + 0.5) / 1000.0, (i * 7 % 1000 + 0.5) / 1000.0);
        ++tiers[sv.name[0] == 'm' && sv.name[1] == 'i' ? 0 : sv.name[0] == 'm' ? 1 : 2];
        lo = std::min(lo, sv.scale);
        hi = std::max(hi, sv.scale);
    }
    assert(tiers[0] == 600 && tiers[1] == 300 && tiers[2] == 100 && lo == 3 && hi == 15);
    assert(fm::slow_shift_severity(0.0, 0.999999).scale == 6 && fm::slow_shift_severity(0.95, 0.0).scale == 10);
    // Slow shifts: 100-200 give 10-20 times the shift time, the game's own 36.6 about 3.7, never below 1.
    assert(slow_shift_scale(150) == 15.0f && slow_shift_scale(100) == 10.0f && slow_shift_scale(200) == 20.0f);
    assert(slow_shift_scale(0) == 1.0f && slow_shift_scale(1000) == 20.0f);
    slow_shift_tick(0);
    fault_request = req;
    // Without a failure object the game thread drops it (logged), nothing crashes.
    register_fault(0);
    assert(fault_request.load() == 0);

    // ---- Rear brake failure: weak (45-70 % of the rear braking gone) or grabbing (1.6-2.1x), half each ----
    assert(close_to(fm::rear_brake_change(0, 0.2), 0.45, 1e-12) && close_to(fm::rear_brake_change(1, 0.2), 0.70, 1e-12));
    assert(close_to(fm::rear_brake_change(0, 0.8), -0.60, 1e-12) && close_to(fm::rear_brake_change(1, 0.8), -1.10, 1e-12));
    {
        reset_session();
        start_rear_brake_fault(0.5, 0);
        assert(rear_brake_loss_target.load() == 0.5f && rear_brake_until.load() == 0);
        // The brake step (physics thread) sees the rear wheels' max torque (+0xf8 XOR the key at +0x100) scaled for
        // the player's car only, and the game's value is back after it.
        alignas(16) static uint8_t bcar[0x600]{}, sys[0x100]{}, wheels[4][0x200]{}, sim[0x40]{}, other_sim[0x40]{},
            other_car[0x600]{};
        uintptr_t sp = reinterpret_cast<uintptr_t>(sys), bc = reinterpret_cast<uintptr_t>(bcar),
                  oc = reinterpret_cast<uintptr_t>(other_car);
        memcpy(bcar + 0x580, &sp, 8);
        memcpy(other_car + 0x580, &sp, 8);
        memcpy(sim + 0x18, &bc, 8);
        memcpy(other_sim + 0x18, &oc, 8);
        static const uint32_t keys[4] = {0x12345678u, 0x9abcdef0u, 0x0badf00du, 0xdeadbeefu};
        brake_wheels = wheels;
        brake_keys = keys;
        auto torque = [&](int w) {
            uint32_t e;
            memcpy(&e, wheels[w] + 0xf8, 4);
            return std::bit_cast<float>(e ^ keys[w]);
        };
        for (int w = 0; w < 4; ++w) {
            uintptr_t wp = reinterpret_cast<uintptr_t>(wheels[w]);
            memcpy(sys + 0x70 + 8 * w, &wp, 8);
            memcpy(wheels[w] + 0x100, &keys[w], 4);
            set_torque_raw(w, w < 2 ? 1000.0f : 3000.0f);
        }
        static float seen[4];
        orig_brake_step = [](void *, float) {
            for (int w = 0; w < 4; ++w) {
                uint32_t e;
                memcpy(&e, brake_wheels[w] + 0xf8, 4);
                seen[w] = std::bit_cast<float>(e ^ brake_keys[w]);
            }
        };
        brake_fault_tick(bc);  // game thread: the player's car and the factor
        assert(brake_car.load() == bc && rear_brake_factor.load() == 0.5f);
        on_brake_step(sim, 1.0f / 60);
        assert(seen[0] == 500.0f && seen[1] == 500.0f && seen[2] == 3000.0f && seen[3] == 3000.0f);
        assert(torque(0) == 1000.0f && torque(1) == 1000.0f);  // the game's value back after the step
        on_brake_step(other_sim, 1.0f / 60);  // another car: untouched
        assert(seen[0] == 1000.0f && seen[1] == 1000.0f);
        // The next tick's split writes a new value (bias change): it is scaled the same, nothing accumulates.
        set_torque_raw(0, 800.0f);
        on_brake_step(sim, 1.0f / 60);
        assert(seen[0] == 400.0f && seen[1] == 500.0f && torque(0) == 800.0f);
        // A wheel the game skips (+0x138 set) stays as it is.
        wheels[1][0x138] = 1;
        on_brake_step(sim, 1.0f / 60);
        assert(seen[1] == 1000.0f);
        wheels[1][0x138] = 0;
        // A timed one (debug) ends: full torque.
        rear_brake_until = 1;  // already over
        brake_fault_tick(bc);
        assert(rear_brake_loss_target.load() == 0 && rear_brake_factor.load() == 1.0f);
        on_brake_step(sim, 1.0f / 60);
        assert(seen[0] == 800.0f && seen[1] == 1000.0f);
        // Grabbing: 80 % more at the rear.
        start_rear_brake_fault(-0.8, 0);
        brake_fault_tick(bc);
        on_brake_step(sim, 1.0f / 60);
        assert(std::fabs(seen[0] - 1440.0f) < 1e-3f && std::fabs(seen[1] - 1800.0f) < 1e-3f && seen[2] == 3000.0f);
        // Failing brakes fade on top: x1.8 x (1 - 0.45).
        brake_fade = 0.45f;
        brake_fault_tick(bc);
        on_brake_step(sim, 1.0f / 60);
        assert(std::fabs(seen[1] - 1000.0f * 1.8f * 0.55f) < 1e-2f);
        reset_session();
        assert(rear_brake_loss_target.load() == 0 && rear_brake_factor.load() == 1.0f && brake_fade.load() == 0.0f);
    }

    // ---- Gearbox decline: stages at half and four fifths, then the gearbox fails ----
    assert(fm::decline_stage(0.0) == 0 && fm::decline_stage(0.5) == 1 && fm::decline_stage(0.8) == 2 &&
           fm::decline_stage(1.0) == 3);
    for (int i = 0; i < 100; ++i) {
        double u = (i + 0.5) / 100.0;
        int h0 = fm::decline_shift_scale(fm::decline_hydraulics, 0, u), h1 = fm::decline_shift_scale(fm::decline_hydraulics, 1, u),
            h2 = fm::decline_shift_scale(fm::decline_hydraulics, 2, u);
        assert(h0 >= 3 && h0 <= 6 && h1 >= 6 && h1 <= 10 && h2 >= 10 && h2 <= 15);
        assert(fm::decline_shift_scale(fm::decline_lost_gear, 0, u) == 1);
    }
    for (int path = 0; path < 2; ++path) {
        reset_session();
        player_vehicle = 0;
        player_pit = 1;
        failure_mgr = 1;  // only checked, not read: the failure goes to the game's breakdown sequence
        start_gearbox_decline(100, path);
        gearbox_decline_tick(1);
        uint64_t r = fault_request.exchange(0);
        assert((r & 0xff) == uint64_t(path == 0 ? fm::f_slow_shift : fm::f_missing_gear));
        assert(((r >> 8) & 0xffffffff) == 9990 && ((r >> 40) & 1) == 0);
        if (path == 0)
            assert(decline_shift_scale.load() >= 3 && decline_shift_scale.load() <= 6 && ((r >> 56) & 0xff) >= 3);
        else
            assert(decline_shift_scale.load() == 0 && ((r >> 48) & 0xff) >= 3 && ((r >> 48) & 0xff) <= 8);
        // The clock stands in the pit lane.
        player_pit = 2;
        gearbox_decline_tick(60);
        assert(gearbox_decline.stage == 0 && gearbox_decline.clock == 1);
        player_pit = 1;
        gearbox_decline_tick(54);
        assert(gearbox_decline.stage == 1 && decline_shift_scale.load() >= 6 && decline_shift_scale.load() <= 10);
        gearbox_decline_tick(30);
        assert(gearbox_decline.stage == 2 && decline_shift_scale.load() >= 10);
        assert(breakdown_request.load() == 0);
        gearbox_decline_tick(20);
        assert(gearbox_decline.path == -1 && decline_shift_scale.load() == 0);
        assert(breakdown_request.load() == 1u && breakdown_part[0].load() == fm::gearbox);
    }
    // A retired car ends it without a failure.
    reset_session();
    player_vehicle = 0;
    start_gearbox_decline(100, 0);
    player_broken = true;
    gearbox_decline_tick(1);
    assert(gearbox_decline.path == -1 && fault_request.load() == 0);
    player_broken = false;
    failure_mgr = 0;

    // ---- AI cars: the failure objects of the manager, era failures queued for the game thread ----
    reset_session();
    alignas(16) uint8_t mgr[0x200]{}, objs[3][0x100]{};
    alignas(16) uint64_t entries[6]{};
    for (int i = 0; i < 3; ++i) {
        objs[i][0x78] = uint8_t(i * 4);  // vehicles 0, 4, 8
        objs[i][0xa0] = 7;
        entries[i * 2 + 1] = reinterpret_cast<uint64_t>(objs[i]);
    }
    uint64_t b = reinterpret_cast<uint64_t>(entries), e = b + sizeof entries;
    memcpy(mgr + 0x198, &b, 8);
    memcpy(mgr + 0x1a0, &e, 8);
    failure_mgr = reinterpret_cast<uintptr_t>(mgr);
    assert(failure_obj_of(4) == reinterpret_cast<uintptr_t>(objs[1]) && failure_obj_of(5) == 0);
    int seen = 0;
    for_each_failure_obj([&](int, uintptr_t) { ++seen; });
    assert(seen == 3);
    player_vehicle = 0;
    gp_clock = 30;
    gp_ai_failure_tick();
    assert(ai_failures_rolled);
    for (int v = 0; v < 32; ++v)
        assert(ai_fail_at[v] > 30 && ai_fail_part[v] != fm::suspension && ai_fail_part[v] != fm::brakes);
    // Due: AI cars 4 and 8 get queued, the player's car (0) never through this path.
    for (double &t : ai_fail_at)
        t = 31;
    gp_clock = 32;
    gp_ai_failure_tick();
    assert(breakdown_request.load() == ((1u << 4) | (1u << 8)));
    // 0.75.1: due slots without a car are dropped after a walk that saw cars (else the walk would run every frame);
    // the player's slot stays as it was. An unreadable manager keeps a due slot for the next frame.
    assert(ai_fail_at[0] == 31 && ai_fail_at[4] >= 1e17 && ai_fail_at[8] >= 1e17);
    for (int v = 1; v < 32; ++v)
        assert(ai_fail_at[v] >= 1e17);
    ai_fail_at[12] = 31;
    uint64_t none = 0;
    memcpy(mgr + 0x198, &none, 8);
    gp_ai_failure_tick();
    assert(ai_fail_at[12] == 31 && breakdown_request.load() == ((1u << 4) | (1u << 8)));
    memcpy(mgr + 0x198, &b, 8);
    gp_ai_failure_tick();
    assert(ai_fail_at[12] >= 1e17 && breakdown_request.load() == ((1u << 4) | (1u << 8)));
    // Nothing due (the next one in 10 minutes): nothing queued, the slot keeps its time.
    ai_fail_at[8] = 632;
    gp_ai_failure_tick();
    assert(ai_fail_at[8] == 632 && breakdown_request.load() == ((1u << 4) | (1u << 8)));
    ai_fail_at[8] = 1e18;
    // The game thread arms them without the lap (lap 0, fraction 0 is due in any lap); car 0 stays untouched.
    breakdown_part[4] = 2;
    breakdown_part[8] = 6;
    objs[1][0xa4] = 9;
    arm_breakdowns();
    assert(breakdown_request.load() == 0 && objs[1][0xa0] == 2 && objs[2][0xa0] == 6 && objs[0][0xa0] == 7);
    assert(objs[1][0xa1] == 0 && objs[1][0xa4] == 0 && objs[1][0xb1] == 0);
    // A sequence already running (state 1-4) is left alone.
    objs[1][0xa1] = 2;
    breakdown_part[4] = 0;
    breakdown_request = 1u << 4;
    arm_breakdowns();
    assert(objs[1][0xa0] == 2 && objs[1][0xa1] == 2);
    failure_mgr = 0;

    // ---- 0.75.3: the pause menu stops the failure clocks: on_frame keeps real seconds, the game stops updating the
    // player's car. 0.75.4: the clocks follow that update, not the speed read (it runs only when a feature needs the
    // speed, so with suspension damage off the clocks stood in 0.75.3) ----
    {
        bool was_ended = session_ended;
        uint64_t was_not_before = telemetry_not_before.load(), was_update = player_update_ms.load(),
                 was_ms = game_speed_ms.load();
        session_ended = false;
        telemetry_not_before = 0;
        game_speed_ms = 0;  // nothing reads the speed: suspension damage off, the car healthy
        player_update_ms = GetTickCount64();
        assert(drive_seconds(0.016) == 0.016);  // driving, or standing on the grid: the game updates the car
        player_update_ms = GetTickCount64() - 2000;  // no car update for 2 s: paused
        assert(drive_seconds(0.016) == 0.0);
        player_update_ms = GetTickCount64();
        telemetry_not_before = GetTickCount64() + 1;  // the last update is from before this session
        assert(drive_seconds(0.016) == 0.0);
        telemetry_not_before = 0;
        session_ended = true;  // between sessions
        assert(drive_seconds(0.016) == 0.0);
        session_ended = was_ended;
        telemetry_not_before = was_not_before;
        player_update_ms = was_update;
        game_speed_ms = was_ms;
    }

    // ---- 0.75.5: Career gets the realistic failures too: the AI plan and the partial fault plan no longer switch
    // off while a Career is loaded (before, every AI slot was 1e18 and the partial fault came after 1e18 s) ----
    {
        static SwoftyCareer cr{};
        cr.state = [](SwoftyCareerState *out) {
            *out = {};
            out->loaded = 1;
            return int(SWOFTY_ASSET_OK);
        };
        uint32_t was_version = api.version;
        api.version = 7;
        api.career = &cr;
        swofty::career::State st{};
        assert(swofty::career::state(&st) == swofty::career::Result::ok && st.loaded);  // the test really is in Career
        reset_session();
        player_vehicle = 0;
        gp_clock = 30;
        failure_mgr = 1;  // the plan needs a failure manager; nothing is due, so nothing walks it
        gp_ai_failure_tick();
        failure_mgr = 0;
        assert(ai_failures_rolled);
        for (int v = 0; v < 32; ++v)
            assert(ai_fail_at[v] > 30 && ai_fail_at[v] < 1e17);
        gp_fault_at = -1;
        gp_fault_tick();
        assert(gp_fault_at > 30 && gp_fault_at < 1e17);

        // The game's own temporary fault for a human driver (record wheel byte 0x63) is not registered in Career with
        // Realistic: its caller gets a scratch record. AI records (phase B) and every record outside Career go through.
        static int registered;
        registered = 0;
        orig_failure_register = [](void *, const void *) -> void * {
            ++registered;
            return nullptr;
        };
        alignas(8) uint8_t player_rec[0x34] = {}, ai_rec[0x34] = {};
        player_rec[0x30] = 0xff;
        player_rec[0x31] = 0x63;
        ai_rec[0x31] = 0x02;
        void *kept = on_failure_register(nullptr, player_rec);
        assert(kept != nullptr && registered == 0);
        assert(on_failure_register(nullptr, ai_rec) == nullptr && registered == 1);
        api.career = nullptr;  // Grand Prix: the game's records are untouched
        assert(on_failure_register(nullptr, player_rec) == nullptr && registered == 2);
        orig_failure_register = nullptr;
        api.version = was_version;
        reset_session();
    }

    // ---- 0.75.5: the 100 % test fails two AI cars as well (15-60 s after the clock starts), never the player's ----
    {
        uint32_t was_version = api.version;
        auto was_setting = api.setting;
        auto was_watch = api.watch_setting;
        api.version = 4;
        api.setting = [](const char *id, uint32_t type, SwoftySettingValue *out) -> int {
            if (strcmp(id, "gp_failures") || type != SWOFTY_SETTING_CHOICE)
                return 0;
            out->type = type;
            out->choice = "100";
            return 1;
        };
        api.watch_setting = [](const char *, SwoftySettingFn, void *) -> int { return 0; };
        alignas(16) static uint8_t mgr[0x200]{}, objs[4][0x100]{};
        alignas(16) static uint64_t entries[8]{};
        for (int i = 0; i < 4; ++i) {
            objs[i][0x78] = uint8_t(i * 4);  // vehicles 0, 4, 8, 12
            entries[i * 2 + 1] = reinterpret_cast<uint64_t>(objs[i]);
        }
        uint64_t b = reinterpret_cast<uint64_t>(entries), e = b + sizeof entries;
        memcpy(mgr + 0x198, &b, 8);
        memcpy(mgr + 0x1a0, &e, 8);
        reset_session();
        player_vehicle = 0;
        gp_clock = 30;
        failure_mgr = reinterpret_cast<uintptr_t>(mgr);
        gp_ai_failure_tick();
        assert(ai_failures_rolled);
        int planned = 0;
        for (int v = 0; v < 32; ++v) {
            if (ai_fail_at[v] >= 1e17)
                continue;
            ++planned;
            assert(v != 0 && v % 4 == 0 && ai_fail_at[v] >= 45 && ai_fail_at[v] <= 90);
        }
        assert(planned == 2);
        // One AI car only: no plan yet (the cars are not all there), tried again on the next frame.
        reset_session();
        player_vehicle = 0;
        gp_clock = 30;
        uint64_t two = b + 2 * 0x10;
        memcpy(mgr + 0x1a0, &two, 8);  // vehicles 0 and 4 only: the player's and one AI car
        gp_ai_failure_tick();
        assert(!ai_failures_rolled);
        failure_mgr = 0;
        api.setting = was_setting;
        api.watch_setting = was_watch;
        api.version = was_version;
        reset_session();
    }

    // ---- 0.75.5: the 100 % test breaks only parts the Failure season's cars have (it broke an MGU-H in a 2026 car) ----
    {
        static const char *era_id = "2026";
        uint32_t was_version = api.version;
        auto was_setting = api.setting;
        auto was_watch = api.watch_setting;
        api.version = 4;
        api.setting = [](const char *id, uint32_t type, SwoftySettingValue *out) -> int {
            if (strcmp(id, "failure_era") || type != SWOFTY_SETTING_CHOICE)
                return 0;
            out->type = type;
            out->choice = era_id;
            return 1;
        };
        api.watch_setting = [](const char *, SwoftySettingFn, void *) -> int { return 0; };
        for (const char *y : {"2026", "2025", "2018", "2014", "2012"}) {
            era_id = y;
            const fm::Era &era = fm::era_of(fm::season_of(y));
            bool seen[fm::gearbox + 1] = {};
            for (int k = 0; k < 800; ++k) {
                int p = test_failure_part();
                assert(p >= fm::ice && p <= fm::gearbox && era.share[p] > 0);
                seen[p] = true;
            }
            for (int i = fm::ice; i <= fm::gearbox; ++i)
                assert(seen[i] == (era.share[i] > 0));  // every part the cars have comes up, none they lack
        }
        assert(fm::era_of(fm::season_of("2026")).share[fm::mguh] == 0);
        api.setting = was_setting;
        api.watch_setting = was_watch;
        api.version = was_version;
    }

    // ---- 0.75.6: every car's failure rate follows its team (the plans scale with team_factors_now) ----
    {
        uint32_t was_version = api.version;
        auto was_setting = api.setting;
        auto was_watch = api.watch_setting;
        api.version = 4;
        api.setting = [](const char *id, uint32_t type, SwoftySettingValue *out) -> int {
            if (strcmp(id, "failure_era") || type != SWOFTY_SETTING_CHOICE)
                return 0;
            out->type = type;
            out->choice = "2026";
            return 1;
        };
        api.watch_setting = [](const char *, SwoftySettingFn, void *) -> int { return 0; };
        const fm::Season &season = fm::season_of("2026");
        // 22 cars, two a team: cars 0 and 1 Aston Martin, 2 and 3 Cadillac, 4 and 5 Red Bull, the rest Mercedes-powered.
        const char *teams[11] = {"game/aston", "game/cadillac", "game/redbull", "game/mercedes", "game/ferrari",
                                 "game/mclaren", "game/williams", "game/alpine", "game/rb", "game/audi", "game/haas"};
        {
            std::lock_guard<std::mutex> lock(team_mu);
            team_of_car.assign(32, std::string());
            for (int v = 0; v < 22; ++v)
                team_of_car[v] = teams[v / 2];
        }
        team_ids_ready = true;
        team_pending = false;
        int matched = 0;
        uint32_t present = 0;
        std::array<double, 32> tf = team_factors_now(season, &matched, &present);
        assert(matched == 22 && present == (1u << 22) - 1 && tf[22] == 1.0);
        double sum = 0;
        for (int v = 0; v < 22; ++v)
            sum += tf[v];
        assert(close_to(sum, 22.0, 1e-9));
        assert(tf[0] > tf[2] && tf[2] > tf[18] && tf[18] > tf[8] && tf[4] == tf[5]);  // Aston > Cadillac > Audi > Red Bull
        assert(team_scale(season, 2) == tf[2] && team_scale(season, 40) == 1.0 && team_scale(season, -1) == 1.0);
        // R&D durability: cars 0 and 1 (Aston Martin) have every Durability upgrade, the rest of the teams have no data;
        // only the team that researched changes (the AI teams' research counts the same way).
        {
            std::vector<double> before(tf.begin(), tf.end());
            {
                std::lock_guard<std::mutex> lock(team_mu);
                team_key_of_car.assign(32, 0);
                for (int v = 0; v < 22; ++v)
                    team_key_of_car[v] = uint32_t(476 + v / 2) << 16;
                rnd_shares.assign(1, RndShare{team_key_of_car[0], 52, 52});
            }
            std::array<double, 32> with = team_factors_now(season);
            assert(close_to(with[0], 0.5 * before[0], 1e-9) && close_to(with[0], with[1], 1e-12));
            for (int v = 2; v < 22; ++v)
                assert(close_to(with[v], before[v], 1e-12));
            std::string with_text = team_rates_text(season);
            assert(with_text.find("(R&D 0.50)") != std::string::npos);
            // 28 of 52 done: 23 of the 47 beyond the start state, factor 0.76. The start state (5 done), fewer than
            // that, and no nodes counted leave the team alone.
            {
                std::lock_guard<std::mutex> lock(team_mu);
                rnd_shares.assign(1, RndShare{team_key_of_car[0], 28, 52});
            }
            assert(team_rates_text(season).find("(R&D 0.76)") != std::string::npos);
            for (RndShare s : {RndShare{team_key_of_car[0], 5, 52}, RndShare{team_key_of_car[0], 2, 52},
                               RndShare{team_key_of_car[0], 5, 0}}) {
                {
                    std::lock_guard<std::mutex> lock(team_mu);
                    rnd_shares.assign(1, s);
                }
                assert(team_rates_text(season).find("(R&D") == std::string::npos);
                std::array<double, 32> same = team_factors_now(season);
                for (int v = 0; v < 22; ++v)
                    assert(close_to(same[v], before[v], 1e-12));
            }
            {
                std::lock_guard<std::mutex> lock(team_mu);
                rnd_shares.clear();
                team_key_of_car.assign(32, 0);
            }
            std::array<double, 32> back = team_factors_now(season);
            for (int v = 0; v < 22; ++v)
                assert(close_to(back[v], before[v], 1e-12));
        }
        // The rates of a session are logged once: when the teams are known and, in a Career, once the R&D data has been
        // read. Not between sessions.
        {
            bool was_ended = session_ended;
            session_ended = false;
            rates_logged = false;
            team_ids_ready = false;
            rnd_shares_read = false;
            team_rates_log_tick();
            assert(!rates_logged.load());  // the teams are not known yet
            team_ids_ready = true;
            team_rates_log_tick();
            assert(rates_logged.load());  // outside a Career there is no R&D data to wait for
            rates_logged = false;
            static SwoftyCareer in_career{};
            in_career.state = [](SwoftyCareerState *out) {
                *out = {};
                out->loaded = 1;
                return int(SWOFTY_ASSET_OK);
            };
            uint32_t was_version = api.version;
            api.version = 7;
            api.career = &in_career;
            team_rates_log_tick();
            assert(!rates_logged.load());  // a Career: its R&D data comes first
            rnd_shares_read = true;
            session_ended = true;
            team_rates_log_tick();
            assert(!rates_logged.load());  // between sessions
            session_ended = false;
            team_rates_log_tick();
            assert(rates_logged.load());
            api.career = nullptr;
            api.version = was_version;
            rates_logged = false;
            rnd_shares_read = false;
            session_ended = was_ended;
        }
        // The participants text read by hand: a string team, an object team, spaces, the human flag.
        {
            std::vector<std::string> got;
            std::string raw = "[{\"car\":0,\"driver\":\"game/x\",\"team\":\"game/mercedes\",\"vehicle\":\"v\",\"human\":false},"
                              "{\"car\": 16, \"team\" : {\"id\": \"mods/cadillac\", \"x\": 1}, \"human\": true},"
                              "{\"car\":3,\"human\":false}]";
            int count = scan_participants_json(raw, [&](int car, std::string team, bool human) {
                got.push_back(std::to_string(car) + "=" + team + (human ? "!" : ""));
            });
            assert(count == 3 && got.size() == 3);
            assert(got[0] == "0=game/mercedes" && got[1] == "16=mods/cadillac!" && got[2] == "3=");
            assert(scan_participants_json("[]", [](int, std::string, bool) {}) == 0);
            assert(scan_participants_json("", [](int, std::string, bool) {}) == 0);
        }
        std::string text = team_rates_text(season);
        assert(text.find("aston x") == 0 && text.find("cadillac x") != std::string::npos);

        // The AI plan: over many rolls the share of each car's slot that fails within the race follows its factor.
        failure_mgr = 1;  // the plan needs a failure manager; nothing is due, so nothing walks it
        player_vehicle = 20;
        const int rolls = 3000;
        int hit[22] = {};
        for (int r = 0; r < rolls; ++r) {
            ai_failures_rolled = false;
            gp_clock = 30;
            gp_ai_failure_tick();
            assert(ai_failures_rolled);
            for (int v = 0; v < 22; ++v)
                hit[v] += ai_fail_at[v] < 30 + fm::gp_minutes * 60.0;
        }
        for (int v : {0, 2, 4, 18}) {
            double p = std::fmin(0.95, season.race_chance * tf[v]);
            double sigma = std::sqrt(p * (1 - p) / rolls);
            assert(std::fabs(hit[v] / double(rolls) - p) < 5 * sigma);
        }
        failure_mgr = 0;

        // The plan waits for the participants of a session that has just started (8 s at most), then goes on without.
        reset_session();
        {
            std::lock_guard<std::mutex> lock(team_mu);
            team_of_car.assign(32, std::string());
        }
        team_ids_ready = false;
        team_pending = true;
        team_pending_at = GetTickCount64();
        failure_mgr = 1;
        gp_clock = 30;
        gp_ai_failure_tick();
        assert(!ai_failures_rolled);
        team_pending_at = GetTickCount64() - 9000;
        gp_ai_failure_tick();
        assert(ai_failures_rolled);
        for (int v = 0; v < 32; ++v)  // no team known: every car has the season's own rate, 1.0
            assert(ai_fail_at[v] > 30);
        failure_mgr = 0;
        team_pending = false;
        assert(team_scale(season, 3) == 1.0);
        api.setting = was_setting;
        api.watch_setting = was_watch;
        api.version = was_version;
        reset_session();
    }

    std::puts("realistic failures ok");
}
