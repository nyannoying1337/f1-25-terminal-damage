// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Partial power faults, gp_break, gearbox decline.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// ---- Partial power faults (MGU-K lost, battery derate, misfire) and the engine power each frame ----
void start_power_fault(int type, double length, bool quick)
{
    int kind = fm::power_kind_of_fault(type);
    if (kind == pl::none)
        return;
    power_fault = pl::start(kind, length, uniform01, false, quick);
    power_fault_pitted = false;
    power_fault_test = quick;
    power_fault_brake_at = !quick && kind == pl::mguk_loss && uniform01() < fm::mguk_brake_chance
                               ? fm::mguk_brake_after(uniform01()) : -1.0;
    char how[96] = "", until[32] = "to the flag";
    if (kind == pl::battery_derate)
        snprintf(how, sizeof how, " above 260 km/h, building over %.0f s, worst until %.0f s", power_fault.build_s,
                 power_fault.hold_s);
    else if (kind == pl::mguk_loss && power_fault.intermittent)
        snprintf(how, sizeof how, ", comes and goes");
    else if (kind == pl::mguk_loss && power_fault.build_s > 0)
        snprintf(how, sizeof how, ", half of it for the first %.0f s", power_fault.build_s);
    else if (kind == pl::misfire)
        snprintf(how, sizeof how, ", rough running");
    if (length > 0)
        snprintf(until, sizeof until, "for %.0f s", length);
    swofty::log("[td-probe] fault: %s: %.0f %% less power%s, %s", fm::fault_name(type), double(power_fault.depth) * 100.0,
                how, until);
    if (power_fault_brake_at >= 0)
        swofty::log("[td-probe] fault: the rear brakes will overheat after %.0f s without the MGU-K", power_fault_brake_at);
    const char *text = kind == pl::mguk_loss ? "We have lost the MGU-K, about 160 horsepower down. Keep going."
                       : kind == pl::battery_derate ? "Battery temperatures are high, it is derating on the straights. "
                                                      "Lift and coast to cool it."
                                                    : "We have a misfire, about 80 horsepower down. We are looking for a fix.";
    radio_say(kind == pl::mguk_loss ? radio_mgu : kind == pl::battery_derate ? radio_ers_derating : radio_engine_b);
    if (swofty::hud::available())
        swofty::hud::toast("ENGINEER", text, 6);
}

void end_power_fault(const char *why)
{
    if (power_fault.kind == pl::none)
        return;
    swofty::log("[td-probe] fault: %s over after %.0f s (%s)", pl::kind_name(power_fault.kind), power_fault.t, why);
    // A test step's fault ends quietly: the next step starts at once with its own radio, and the "fixed" line queued
    // behind it (0.51.114 log: it played 6 s into the misfire step).
    if (!power_fault_test && (power_fault.kind == pl::misfire || power_fault.kind == pl::battery_derate)) {
        radio_say(radio_fault_fixed);
        if (swofty::hud::available())
            swofty::hud::toast("ENGINEER", power_fault.kind == pl::misfire ? "Misfire fixed, full power back."
                                                                          : "Battery temperatures are normal again, "
                                                                            "full deployment.", 5);
    }
    power_fault = {};
    power_fault_brake_at = -1;
    power_fault_test = false;
}

// Frame thread: the player's engine power share as an engine.power_scale layer (a scale below the engine kill's
// set, so a dead engine stays dead), or as the throttle cap when the core has no car layers.
void apply_power(float scale)
{
    namespace live = swofty::cars::live;
    static live::Layer layer;
    static float applied = 1.0f;
    static int applied_car = -1;
    static bool layers_ok = true, verified = false;
    int car = session_ended ? -1 : player_vehicle.load();
    scale = car < 0 ? 1.0f : std::clamp(std::round(scale * 100.0f) / 100.0f, 0.0f, 1.0f);  // whole percent
    if (!layers_ok) {
        warning_cap = scale;
        return;
    }
    if (scale == applied && car == applied_car)
        return;
    applied_car = car;
    if (scale >= 1.0f) {
        layer.release();
        applied = 1.0f;
        return;
    }
    layer = live::Layer::scale(live::Cars::car(live::Car(uint32_t(car))), "engine.power_scale", scale, 50);
    if (!layer.ok()) {
        layers_ok = false;
        warning_cap = scale;
        swofty::log("[td-probe] power: no engine.power_scale layer (result %d), the throttle cap takes the power loss",
                    int(layer.result()));
        return;
    }
    applied = scale;
    if (!verified) {
        verified = true;
        auto v = live::Car(uint32_t(car)).get(live::Field::engine_power_scale);
        swofty::log("[td-probe] power: engine.power_scale layer on car %d, now %.2f (asked %.2f)", car, v ? double(*v) : -1.0,
                    double(scale));
    }
}

void power_tick(double dt)
{
    bool on_track = !session_ended && player_vehicle.load() >= 0 && !player_broken && !retire_requested;
    bool driving = on_track && player_pit.load() == 1;
    if (power_fault.kind != pl::none) {
        if (!on_track) {
            // nothing: the car is out or the session over (reset_session clears it)
        } else if (player_pit.load() != 1) {
            power_fault_pitted = true;
        } else if (power_fault_pitted && power_fault.kind == pl::misfire) {
            end_power_fault("a power cycle in the pits fixed it");
        } else {
            double nurse = power_fault.kind == pl::battery_derate ? fm::nursing_factor(full_throttle_share.load()) : 1.0;
            pl::advance(power_fault, dt * nurse, uniform01);
            if (power_fault.length > 0 && power_fault.t >= power_fault.length) {
                end_power_fault("cleared");
            } else if (power_fault_brake_at >= 0 && power_fault.t >= power_fault_brake_at) {
                power_fault_brake_at = -1;
                if (rear_brake_loss_target.load() == 0) {
                    swofty::log("[td-probe] fault: without the MGU-K the rear brakes overheat");
                    start_rear_brake_fault(0.35 + 0.25 * uniform01(), 0);
                }
            }
        }
    }
    power_speed_wanted = power_warn.kind == pl::battery_derate || power_fault.kind == pl::battery_derate;
    float v = float(std::max(0, player_speed()));
    float scale = driving ? pl::power(power_warn, v) * pl::power(power_fault, v) : 1.0f;
    float lw = pl::steady_loss(power_warn), lf = pl::steady_loss(power_fault);
    shown_power_loss = on_track ? 1.0f - (1.0f - lw) * (1.0f - lf) : 0.0f;
    shown_power_kind = !on_track ? int(pl::none) : lf >= lw && power_fault.kind != pl::none ? power_fault.kind : power_warn.kind;
    shown_power_cuts = on_track && (pl::cutting(power_warn) || pl::cutting(power_fault));
    // The log: every change of the steady power by 2 % or more.
    static float logged = 1.0f;
    float steady = (1.0f - lw) * (1.0f - lf);
    if (std::fabs(steady - logged) >= 0.02f || (steady == 1.0f && logged != 1.0f)) {
        swofty::log("[td-probe] power: engine at %.0f %% (%s %.0f %%, %s %.0f %%)%s", double(steady) * 100.0,
                    pl::kind_name(power_warn.kind), double(lw) * 100.0, pl::kind_name(power_fault.kind),
                    double(lf) * 100.0, power_speed_wanted.load() ? ", the derate only above 230-260 km/h" : "");
        logged = steady;
    }
    apply_power(scale);
}

void gp_break(int outcome)
{
    swofty::log("[td-probe] gp failures: %s fails now", fm::outcome_name(outcome));
    if (outcome >= fm::ice && outcome <= fm::gearbox) {
        int v = player_vehicle.load();
        if (v >= 0 && v < 32 && failure_mgr.load()) {
            breakdown_part[v] = uint8_t(outcome);
            breakdown_request.fetch_or(1u << v);
        } else {
            trigger_failure(outcome);
        }
    } else if (outcome == fm::suspension) {
        susp::collapse(int(uniform01() * 4) & 3, "mechanical failure");
        radio_say(radio_mech_dnf);
    } else if (outcome == fm::brakes) {
        if (swofty::hud::available())
            swofty::hud::toast("BRAKE FAILURE", "Retire the car", 4);
        radio_say(radio_mech_dnf);
        player_broken = true;
        retire_in = 3.0;
    }
}

// Gearbox decline: stage 0 at the start, 1 at half, 2 at four fifths of the length, then the gearbox fails through the
// game's breakdown sequence. The first stage registers the game's own fault for its radio (slow shifts or the lost
// gear, for the rest of the race); the shift time comes from decline_shift_scale (slow_shift_tick).
void start_gearbox_decline(double length, int path)
{
    gearbox_decline = {};
    gearbox_decline.path = path;
    gearbox_decline.length = std::max(10.0, length);
    if (path == fm::decline_lost_gear)
        gearbox_decline.gear = 3 + int(uniform01() * 6) % 6;  // 2nd-7th
    swofty::log("[td-probe] gearbox decline: %s, the gearbox fails after %.0f s of driving",
                path == fm::decline_hydraulics ? "hydraulics (slow shifts)" : "lost gear", gearbox_decline.length);
}

void gearbox_decline_tick(double dt)
{
    gearbox_decline_state &d = gearbox_decline;
    if (d.path < 0)
        return;
    if (session_ended || player_broken || player_vehicle.load() < 0 || retire_requested) {
        d = {};
        decline_shift_scale = 0.0f;
        shown_decline_stage = -1;
        return;
    }
    if (player_pit.load() != 1 || pending_failure.load())
        return;  // the clock stands in the pit lane
    d.clock += dt * fm::nursing_factor(full_throttle_share.load());
    int stage = fm::decline_stage(d.clock / d.length);
    shown_decline_stage = stage < 3 ? stage : -1;
    if (stage == d.stage)
        return;
    d.stage = stage;
    if (stage >= 3) {
        swofty::log("[td-probe] gearbox decline: the gearbox fails now");
        shown_decline_stage = -1;
        d = {};
        decline_shift_scale = 0.0f;
        gp_break(fm::gearbox);
        return;
    }
    int scale = fm::decline_shift_scale(d.path, stage, uniform01());
    decline_shift_scale = scale > 1 ? float(scale) : 0.0f;
    static const char *const gear_names[] = {"reverse", "neutral", "1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th"};
    char text[128] = {};
    if (stage == 0) {
        // The game's fault for its radio: 999 laps (the rest of the race), started at once. A test run's decline asks
        // for three times its length in seconds instead: a run stopped early would leave slow shifts to the flag.
        uint64_t type = d.path == fm::decline_hydraulics ? fm::f_slow_shift : fm::f_missing_gear;
        uint64_t tenths = d.test ? uint64_t(std::min(9990.0, 30.0 * d.length)) : 9990;
        fault_request = type | (tenths << 8) | (uint64_t(d.test ? 1 : 0) << 40) | (uint64_t(d.gear) << 48) |
                        (uint64_t(d.path == fm::decline_hydraulics ? scale : 0) << 56);
        if (d.path == fm::decline_hydraulics)
            snprintf(text, sizeof text, "Gearbox pressure is dropping, shifts are slow. Short shift, stay off the kerbs.");
        else
            snprintf(text, sizeof text, "We have lost %s gear. Skip it, stay off the kerbs.",
                     d.gear < 10 ? gear_names[d.gear] : "a");
    } else if (stage == 1) {
        snprintf(text, sizeof text, "Gearbox is getting worse, shifts are rough. Short shift, no kerbs.");
        radio_say(radio_gearbox);
    } else {
        snprintf(text, sizeof text, "We are losing the gearbox. Keep it going as long as you can.");
        radio_say(radio_gearbox_jam);
    }
    swofty::log("[td-probe] gearbox decline: stage %d after %.0f of %.0f s, shift time x%d", stage, d.clock, d.length,
                scale);
    if (swofty::hud::available())
        swofty::hud::toast("ENGINEER", text, 6);
}
