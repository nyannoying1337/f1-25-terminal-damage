// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Realistic Grand Prix failures: plan, AI cars, faults, warnings.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// ---- Realistic Grand Prix failures (gp_failures "realistic", failure_era) and their warning signs ----
double uniform01()
{
    static std::mt19937_64 rng{GetTickCount64() * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId()};
    return double(rng() >> 11) * (1.0 / 9007199254740992.0);
}

void gp_plan_realistic()
{
    const fm::Season &season = fm::season_of(failure_era);
    double u = uniform01();
    double scale = team_scale(season, player_vehicle.load());  // your team's rate against the season's average (1: unknown)
    player_plan_epoch = team_epoch.load();
    double in = fm::seconds_until_failure(season, u > 0 ? u : 1e-12, scale);
    gp_at = gp_clock + in;
    gp_warn = {};
    gp_warn.outcome = fm::pick_outcome(fm::era_of(season), uniform01());
    fm::Warning w = fm::warning_of(gp_warn.outcome);
    double lead = 0;
    if (uniform01() < w.chance) {
        lead = w.min_lead_s + uniform01() * (w.max_lead_s - w.min_lead_s);
        gp_warn.warn_at = std::max(gp_clock, gp_at - lead);
    }
    // Logged as minutes of driving: a full Grand Prix is about 90, so most sessions end before.
    swofty::log("[td-probe] gp failures (%.*s %s, %.1f %% per Grand Prix, your team x%.2f = %.1f %%): %s would fail after "
                "%.0f more minutes of driving, %s",
                int(season.id.size()), season.id.data(), fm::era_of(season).name, season.race_chance * 100.0,
                scale, std::fmin(0.95, season.race_chance * scale) * 100.0, fm::outcome_name(gp_warn.outcome), in / 60.0,
                lead > 0 ? "with warning signs before" : "without warning");
}

// AI cars: each car fails with the season's rate on the same drive clock (a full Grand Prix is about 90 minutes of
// driving), the part from the era's shares; suspension and brake failures, which the game's sequence cannot do, are
// left out of the draw (their share is zeroed): the part is a power unit part or the gearbox. Career too since 0.75.5
// (the game's own rolls, with the teams' R&D, are removed at the scheduler).
void gp_ai_failure_tick()
{
    if (!failure_mgr.load())
        return;
    const fm::Season &season = fm::season_of(failure_era);
    // The team ids came after the plan (they were late): make it again, as long as no AI failure has started.
    if (ai_failures_rolled && ai_plan_epoch != team_epoch.load() && !ai_failure_started && gp_clock < 900.0 &&
        std::string_view(gp_failures) != "100") {
        ai_failures_rolled = false;
        swofty::log("[td-probe] gp failures: the teams are known now, the AI plan is made again");
    }
    if (!ai_failures_rolled && std::string_view(gp_failures) == "100") {
        // The 100 % test (0.75.5): two AI cars fail 15-60 s into the race too, the first an engine, the second a
        // gearbox, so one short race shows the AI path (the game's breakdown sequence) also in Career.
        int cars[32], n = 0, player = player_vehicle.load();
        for_each_failure_obj([&](int v, uintptr_t) {
            if (v != player && v >= 0 && v < 32 && n < 32)
                cars[n++] = v;
        });
        if (n < 2)
            return;  // the cars are not there yet: try again next frame
        ai_failures_rolled = true;
        for (double &t : ai_fail_at)
            t = 1e18;
        int picked[2] = {-1, -1};
        for (int k = 0; k < 2; ++k) {
            int i = int(uniform01() * n) % n, v = cars[i];
            cars[i] = cars[--n];
            ai_fail_at[v] = gp_clock + 15.0 + uniform01() * 45.0;
            ai_fail_part[v] = k == 0 ? fm::ice : fm::gearbox;
            picked[k] = v;
        }
        swofty::log("[td-probe] gp failures (100, test): AI car %d (%s) fails after %.0f s, AI car %d (%s) after %.0f s",
                    picked[0], fm::outcome_name(fm::ice), ai_fail_at[picked[0]] - gp_clock, picked[1],
                    fm::outcome_name(fm::gearbox), ai_fail_at[picked[1]] - gp_clock);
    }
    if (!ai_failures_rolled) {
        // A session has just started: wait for the participants (each team's rate) for 8 s at most.
        if (team_pending.load() && !team_ids_ready.load() && GetTickCount64() < team_pending_at.load() + 8000)
            return;
        ai_failures_rolled = true;
        const fm::Era &era = fm::era_of(season);
        fm::Era pu = era;
        pu.share[fm::suspension] = pu.share[fm::brakes] = 0;
        int matched = 0;
        uint32_t present = 0;
        std::array<double, 32> tf = team_factors_now(season, &matched, &present);
        int planned = 0, cars = 0;
        for (int v = 0; v < 32; ++v) {
            ai_fail_at[v] = gp_clock + fm::seconds_until_failure(season, std::max(uniform01(), 1e-12), tf[v]);
            ai_fail_part[v] = fm::pick_outcome(pu, uniform01());
            if (present && !(present & (1u << v)))
                continue;  // no car in this slot
            ++cars;
            planned += ai_fail_at[v] < gp_clock + fm::gp_minutes * 60.0;
        }
        ai_plan_epoch = team_epoch.load();
        swofty::log("[td-probe] gp failures: AI cars use the %.*s rate by team, %d of %d cars would fail within a full "
                    "race distance", int(season.id.size()), season.id.data(), planned, cars);
        if (present)
            swofty::log("[td-probe] gp failures: team rates for %.*s (%d of %d cars with a known team): %s",
                        int(season.id.size()), season.id.data(), matched, cars, team_rates_text(season).c_str());
        else
            swofty::log("[td-probe] gp failures: the teams are not known, every car gets the season's average rate");
    }
    int player = player_vehicle.load();
    // Walk the cars only once a failure is due: the walk reads every car's failure object (two guarded reads a car,
    // about 25 us a frame in a 22-car race, 0.75.1 test at Montreal), and most of a race nothing is due.
    double due = 1e18;
    for (int v = 0; v < 32; ++v)
        if (v != player)
            due = std::min(due, ai_fail_at[v]);
    if (gp_clock < due)
        return;
    uint32_t seen = 0;
    for_each_failure_obj([&](int v, uintptr_t) {
        seen |= 1u << v;
        if (v == player || ai_fail_at[v] >= 1e17 || gp_clock < ai_fail_at[v])
            return;
        ai_fail_at[v] = 1e18;
        ai_failure_started = true;
        swofty::log("[td-probe] gp failures: AI car %d, %s fails", v, fm::outcome_name(ai_fail_part[v]));
        breakdown_part[v] = uint8_t(ai_fail_part[v]);
        breakdown_request.fetch_or(1u << v);
    });
    // A due slot without a car (fewer than 32 cars) would bring the walk back every frame: it is dropped, but only
    // when the walk saw cars (an unreadable manager keeps every slot for the next frame).
    if (seen)
        for (int v = 0; v < 32; ++v)
            if (v != player && !(seen & (1u << v)) && gp_clock >= ai_fail_at[v])
                ai_fail_at[v] = 1e18;
}

// One partial failure per session at most, with the era's chance, as the game's own temporary faults. Career too
// since 0.75.5: a registered fault is written into the career save like the game's own (its "last fault" data).
void gp_fault_tick()
{
    if (gp_fault_at < 0) {
        const fm::Season &season = fm::season_of(failure_era);
        const fm::FaultEra &fe = fm::fault_era_of(season);
        double in = fm::seconds_until_fault(season, std::max(uniform01(), 1e-12),
                                            team_scale(season, player_vehicle.load()));
        fault_plan_epoch = team_epoch.load();
        gp_fault_at = gp_clock + in;
        gp_fault_type_rolled = fm::pick_fault(fe, uniform01());
        swofty::log("[td-probe] gp failures: partial failure (%.1f %% per Grand Prix): %s after %.0f more minutes",
                    fm::fault_race_chance(season) * 100.0, fm::fault_name(gp_fault_type_rolled), in / 60.0);
    }
    if (gp_fault_at >= 1e17 || gp_clock < gp_fault_at)
        return;
    gp_fault_at = 1e18;
    fm::FaultSpec spec = fm::fault_spec(gp_fault_type_rolled);
    double duration = spec.min_duration + uniform01() * (spec.max_duration - spec.min_duration);
    // Missing gear: 2nd to 7th (gearbox index 3-8); slow shifts: a mild, moderate or severe shift time (the game's own
    // roll is too small to notice).
    uint64_t gear = gp_fault_type_rolled == fm::f_missing_gear ? uint64_t(3 + int(uniform01() * 6) % 6) : 0xff;
    fm::ShiftSeverity sev = fm::slow_shift_severity(uniform01(), uniform01());
    uint64_t magnitude = gp_fault_type_rolled == fm::f_slow_shift ? uint64_t(sev.scale) : 0;
    if (gp_fault_type_rolled == fm::f_rear_brakes) {
        start_rear_brake_fault(fm::rear_brake_change(uniform01(), uniform01()), 0);
        return;
    }
    if (fm::power_kind_of_fault(gp_fault_type_rolled) != pl::none) {
        start_power_fault(gp_fault_type_rolled, spec.seconds ? duration : 0.0, false);
        return;
    }
    fault_request = uint64_t(gp_fault_type_rolled) | (uint64_t(duration * 10.0) << 8) |
                    (uint64_t(spec.seconds ? 1 : 0) << 40) | (gear << 48) | (magnitude << 56);
    swofty::log("[td-probe] gp failures: partial failure now: %s", fm::fault_name(gp_fault_type_rolled));
}

void gp_warning_tick(double dt)
{
    (void)dt;
    int o = gp_warn.outcome;
    if (o < 0 || gp_at < 0 || gp_at >= 1e17 || gp_clock < gp_warn.warn_at) {
        power_warn = {};
        brake_fade = 0.0f;
        shown_warning = -1;
        return;
    }
    shown_warning = o == fm::gearbox ? -1 : o;
    double lead = std::max(1.0, gp_at - gp_warn.warn_at), t = gp_clock - gp_warn.warn_at;
    if (!gp_warn.announced) {
        gp_warn.announced = true;
        swofty::log("[td-probe] gp failures: warning signs for the %s start (%.0f s of driving left at full load)",
                    fm::outcome_name(o), gp_at - gp_clock);
        if (o == fm::gearbox) {
            // The gearbox declines in stages and fails at the end itself (its own clock, nursed the same way).
            start_gearbox_decline(gp_at - gp_clock, uniform01() < 0.5 ? fm::decline_hydraulics : fm::decline_lost_gear);
            gp_at = 1e18;
            return;
        }
        int kind = fm::warning_kind(o, uniform01());
        power_warn = pl::start(kind, lead, uniform01, true);
        const char *text = "";
        switch (kind) {
        case pl::leak: text = "Oil pressure is dropping. Lift and coast to save the engine."; break;
        case pl::dead_cylinder: text = "We have a misfire, the engine is losing a cylinder. Short shift, keep it going."; break;
        case pl::mguk_loss: text = "MGU-K temperatures are high, it is derating. Manage it."; break;
        case pl::battery_derate: text = "Battery temperatures are high, it is derating on the straights. Lift and coast."; break;
        case pl::turbo: text = "The turbo is losing boost. Lift and coast, short shift."; break;
        case pl::mguh: text = "MGU-H problem, the battery is not recharging. Manage the energy."; break;
        case pl::electrical: text = "We see an electrical problem. Keep going for now."; break;
        default:
            if (o == fm::brakes)
                text = "Brake temperatures are critical. Lift and coast into the braking zones.";
            break;
        }
        if (kind != pl::none) {
            pl::State end = power_warn;
            end.t = lead;
            swofty::log("[td-probe] power: warning signs as %s over %.0f s, %.0f %% less power at the end%s",
                        pl::kind_name(kind), lead, double(pl::steady_loss(end)) * 100.0,
                        pl::cutting(end) || pl::rough(end) ? ", with cut-outs or rough running" : "");
        }
        if (swofty::hud::available() && *text)
            swofty::hud::toast("ENGINEER", text, 6);
        static const int line_of[fm::n_outcomes] = {radio_engine_a, radio_mgu,     radio_turbo,
                                                    radio_energy_store, radio_turbo, radio_control_electronics,
                                                    radio_gearbox,  -1,            radio_brakes};
        radio_say(line_of[o]);
    }
    // The course runs on the failure clock, which nursing slows.
    if (power_warn.kind != pl::none && t > power_warn.t)
        pl::advance(power_warn, t - power_warn.t, uniform01);
    brake_fade = o == fm::brakes ? float(fm::brake_fade(t / lead)) : 0.0f;
}
