// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Game lap and race length, heavy damage tick, gp_failure_tick.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Lap (0-based) the game counts for the player, read like the failure update does (getter 0x46046a0 with the failure
// object's car lookup). -1 when it cannot be read.
int game_lap() { return game_lap_of(failure_planner_obj.load()); }
// The lap (0-based) of the car whose failure object this is.
int game_lap_of(uintptr_t obj)
{
    uint64_t c98 = 0, c20 = 0;
    uint8_t idx = 0xff;
    if (!obj || !guarded_read(obj + 0x98, c98) || !c98 || !guarded_read(c98 + 0x20, c20) || !c20 || !guarded_read(obj + 0x78, idx) || idx == 0xff) {
        static uintptr_t logged = 0;
        if (obj && logged != obj) {
            logged = obj;
            swofty::log("[td-probe] game lap: no lap for failure object %p (+0x98 %p, +0x20 %p, vehicle %u)",
                        reinterpret_cast<void *>(obj), reinterpret_cast<void *>(c98), reinterpret_cast<void *>(c20),
                        unsigned(idx));
        }
        return -1;
    }
    // A fixed address of this build: call it only when its first instructions match (push rsi; sub rsp, 0x20;
    // movzx r8d, dl; mov rax, [rcx+0x38]).
    static const auto getter = [] {
        constexpr uint8_t prologue[] = {0x56, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x0F, 0xB6, 0xC2, 0x48, 0x8B, 0x41, 0x38};
        uintptr_t at = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x46046a0;
        uint8_t code[sizeof prologue] = {};
        bool ok = guarded_copy(at, code, sizeof code) && !memcmp(code, prologue, sizeof prologue);
        if (!ok)
            swofty::log("[td-probe] game failure: lap getter not found (other game build?), the 10 min fallback applies");
        return ok ? reinterpret_cast<uintptr_t (*)(uint64_t, uint32_t)>(at) : nullptr;
    }();
    if (!getter)
        return -1;
    uintptr_t a = getter(c20, idx);
    uint16_t l1 = 0, l2 = 0;
    if (!a || !guarded_read(a + 8, l1) || !guarded_read(a + 0x10, l2))
        return -1;
    return int(int16_t(l1 ^ l2));
}

// The race's laps from Swofty's telemetry, else from the session context (session_laps), else from the game's race
// settings (game_race_laps, the laps the game plans its failures over); 0 when none has them.
int race_total_laps(const char **source = nullptr)
{
    swofty::telemetry::SessionState st{};
    st.version = swofty::telemetry::version;
    if (swofty::telemetry::session(&st) == swofty::telemetry::Result::ok && st.total_laps > 0 && st.total_laps < 200) {
        if (source)
            *source = "Swofty telemetry";
        return int(st.total_laps);
    }
    if (int laps = session_laps.load(); laps > 0) {
        if (source)
            *source = "session context";
        return laps;
    }
    if (int laps = game_race_laps.load(); laps > 0 && laps < 200) {
        if (source)
            *source = "the game's race settings";
        return laps;
    }
    return 0;
}

// The fixed-chance modes' failure point: on its lap, after the planned share of a lap (the last full lap's time, 90 s
// on the first lap); then the time trigger in gp_failure_tick takes over. The lap is read twice a second of driving.
void gp_point_tick()
{
    static double next_read = 0;
    if (gp_point.lap < 0) {
        next_read = 0;
        return;
    }
    if (gp_clock < next_read)
        return;
    next_read = gp_clock + 0.5;
    int lap = game_lap();
    if (lap < 0)
        return;
    if (lap != gp_lap_seen) {
        if (gp_lap_seen >= 0 && lap == gp_lap_seen + 1)
            gp_lap_time = gp_clock - gp_lap_started;
        gp_lap_seen = lap;
        gp_lap_started = gp_clock;
    }
    if (lap < gp_point.lap)
        return;
    double lap_time = gp_lap_time > 20 ? gp_lap_time : 90.0;
    gp_at = lap > gp_point.lap ? gp_clock : std::max(gp_clock, gp_lap_started + gp_point.in_lap * lap_time);
    swofty::log("[td-probe] gp failures: the planned lap %d has come, the part fails in %.0f s", gp_point.lap + 1,
                gp_at - gp_clock);
    gp_point = {};
}

void gp_plan_realistic();
void gp_fault_tick();
void gp_ai_failure_tick();
void gp_warning_tick(double dt);
void start_power_fault(int type, double length, bool quick);
void gp_break(int outcome);
void gearbox_decline_tick(double dt);
// Black flag with an orange disc (FIA Appendix H 2.5.4.1 e): shown at the line, the driver must stop at his pit on the
// next lap. The window after the game's failure is over once the lap after the failure's lap is over (laps 0-based,
// -1 unknown: then only the 10-minute fallback ends it).
constexpr bool pit_window_over(int failure_lap, int now_lap) { return failure_lap >= 0 && now_lap > failure_lap + 1; }
// A race car with only aero or body damage on track and no pit window (the damage came in the pit lane, the car
// drove through its box, or a flashback undid the pit entry or part of the damage) is called in (again).
void heavy_damage_tick()
{
    uint32_t mask = terminal_mask.load();
    constexpr uint32_t out = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 10) | (1u << 11);
    if (!keep_control || !player_broken || !mask || (mask & 0x1f) || game_failure_deadline.load() ||
        pit_call_ended.load() || player_pit.load() != 1 || session_ended || retire_in.load() != -1.0 ||
        dsq_pending || confirmed_retired_vehicle.load() >= 0 || red_flag_repair_wait.load() ||
        red_flag_screen_open.load() || (player_controller_flags.load() & out) || practice_like())
        return;
    heavy_damage_call(mask);
}
// The part the 100 % test breaks: any power unit part or the gearbox that the Failure season's cars have (a 2026 car
// has no MGU-H, a V8 from 2012-2013 no turbo or MGU-H; 0.75.5: the test broke an MGU-H in a 2026 car).
int test_failure_part()
{
    const fm::Era &era = fm::era_of(fm::season_of(failure_era));
    int present[fm::gearbox + 1], n = 0;
    for (int i = fm::ice; i <= fm::gearbox; ++i)
        if (era.share[i] > 0)
            present[n++] = i;
    return n ? present[std::min(n - 1, int(uniform01() * n))] : int(fm::ice);
}

// Grand Prix failures (modes without the game's failure options): once the player has driven a while, roll once per
// session whether a part fails and when, then break a random part the same way the debug failure does. Also ends the
// window after the game's own scripted failure: disqualify (or retire) a car that neither pitted nor retired in time.
void gp_failure_tick(double dt)
{
    // The game ended the session for your car (NOT_IN_SESSION, the results screen and its replay follow): the failure
    // plans, the pit window and the gearbox decline stand. Swofty's session.end comes only when the results are left
    // (0.75.8 Career log: NOT_IN_SESSION 07:37:38, session.end 07:45:33), and resetting a retired car at NOT_IN_SESSION
    // puts player_broken, retire_requested and player_pit back, so the clock ran on and an AI car's planned failure was
    // armed 37 s into the results screen. reset_session and a race restart clear the flag with the other flags.
    if (player_controller_flags.load() & (1u << 3))
        return;
    gearbox_decline_tick(dt);
    heavy_damage_tick();
    if (uint64_t window = game_failure_deadline.load()) {
        // The 10-minute fallback counts driving time: not the pause menu (the game does not update the car there).
        static uint64_t window_seen = 0;
        static double window_driven = 0;
        if (window != window_seen) {
            window_seen = window;
            window_driven = 0;
        }
        if (player_car_running())
            window_driven += dt;
        if (session_ended || confirmed_retired_vehicle.load() >= 0 || player_vehicle.load() < 0) {
            game_failure_deadline = 0;
        } else if (player_controller_flags.load() & ((1u << 10) | (1u << 11))) {
            // Red flag or session suspension: the game holds the car and its teleport ends this window.
        } else if (player_pit.load() != 1) {
            // In the pit lane: the car is allowed to be there, the garage handling takes over.
            game_failure_deadline = 0;
            swofty::log("[td-probe] %s: car reached the pits, no disqualification", pit_call_name());
        } else {
            int start = game_failure_lap.load(), now_lap = start >= 0 ? game_lap() : -1;
            bool lap_over = pit_window_over(start, now_lap);
            // The game's lap could not be read: your line crossings count instead (the second one ends the next lap).
            if (start < 0 && game_failure_crossing.load() >= 0)
                lap_over = player_crossings.load() > game_failure_crossing.load() + 1;
            if (lap_over || window_driven >= 600.0) {
                game_failure_deadline = 0;
                pit_call_ended = true;
                const char *why = lap_over ? "the next lap ended" : "10 minutes of driving passed";
                if (std::string_view(game_failure_timeout) == "dsq") {
                    swofty::log("[td-probe] %s: %s without pitting or retiring, disqualifying the car", pit_call_name(),
                                why);
                    if (swofty::hud::available())
                        swofty::hud::toast("DISQUALIFIED", "Ignored the call to the pits", 4);
                    dsq_pending = true;
                } else {
                    swofty::log("[td-probe] %s: %s without pitting or retiring, retiring the car", pit_call_name(), why);
                    if (swofty::hud::available())
                        swofty::hud::toast("RETIRED", pit_call_reason.load() ? "Heavy damage" : "Mechanical failure", 4);
                    player_broken = true;
                    retire_in = 0.0;
                }
            }
        }
    }
    std::string_view mode = gp_failures;
    bool realistic = mode == "realistic";
    int percent = realistic ? 1 : mode == "off" ? 0 : choice_int(mode, 0);
    // The mode changed mid-session (a preset or by hand): the planned failure goes and the new mode plans its own.
    // Leaving Realistic also ends its warning signs (their power loss and brake fade froze before) and its partial
    // fault plan. A part that already failed stays failed; a gearbox decline under way runs to its end.
    static int last_plan = -1;  // 1000 realistic, else the percent (0 off); -1 before the first frame
    if (int plan = realistic ? 1000 : percent; plan != last_plan) {
        if (last_plan == 1000) {
            power_warn = {};
            brake_fade = 0.0f;
            shown_warning = -1;
            gp_warn = {};
            gp_fault_at = -1;
        }
        if (last_plan >= 0) {
            if (gp_at >= 0)
                swofty::log("[td-probe] gp failures: the mode is now %.*s, the planned failure is dropped",
                            int(mode.size()), mode.data());
            gp_at = -1;
            gp_point = {};
        }
        last_plan = plan;
    }
    if (percent <= 0) {
        gp_clock = 0; gp_at = -1;
        gp_point = {};
        return;
    }
    if (session_ended || practice_like() || player_broken || player_vehicle.load() < 0 || player_pit.load() != 1 ||
        pending_failure.load() || retire_requested)
        return;  // the clocks stand in the pit lane or once the car is out (power_tick gives full power there)
    // Standing since the session (or the race) began: the Career weekend menu, not driving yet. An unknown speed (-1)
    // does not hold the clocks.
    if (!player_has_moved) {
        if (int kmh = player_speed(); kmh >= 0 && kmh < moved_kmh)
            return;
        player_has_moved = true;
        swofty::log("[td-probe] gp failures: your car is moving, the failure clocks run");
    }
    // Nursing: during the warning signs the part wears slower with less time at full throttle.
    bool warning = realistic && gp_at >= 0 && gp_at < 1e17 && gp_clock >= gp_warn.warn_at;
    gp_clock += warning ? dt * fm::nursing_factor(full_throttle_share.load()) : dt;
    if (realistic) {
        // The team ids came after your plan (late): make it again while no warning sign has started.
        if (gp_at >= 0 && player_plan_epoch != team_epoch.load() && gp_clock < 900.0 && !warning) {
            gp_at = -1;
            gp_warn = {};
        }
        if (gp_fault_at >= 0 && fault_plan_epoch != team_epoch.load() && gp_clock < 900.0 && gp_fault_at > gp_clock + 5.0)
            gp_fault_at = -1;
        if (gp_at < 0 && gp_clock > 20.0)
            gp_plan_realistic();
        gp_fault_tick();
        gp_ai_failure_tick();
        gp_warning_tick(dt);
        if (gp_at >= 0 && gp_clock >= gp_at && gp_at < 1e17) {
            gp_at = 1e18;
            power_warn = {};
            gp_break(gp_warn.outcome);
        }
        return;
    }
    if (gp_at < 0 && gp_clock > 20.0) {
        // The chance is for the whole race; the failure comes anywhere between 5 % and 95 % of its laps (fm::race_point).
        // Without the lap count the old window stays: 1 to 15 minutes of driving. The rolls used to come from the tick
        // count, so they were tied to each other.
        bool fails = percent >= 100 || uniform01() < percent / 100.0;
        int laps = fails && percent < 100 ? race_total_laps() : 0;
        gp_point = {};
        if (!fails)
            gp_at = 1e18;
        else if (percent >= 100)
            gp_at = gp_clock + 15.0 + uniform01() * 45.0;
        else if (laps > 0) {
            gp_point = fm::race_point(uniform01(), laps);
            gp_at = 1e18;  // gp_point_tick sets the time on its lap
        } else
            gp_at = gp_clock + 60.0 + uniform01() * 840.0;
        if (gp_point.lap >= 0)
            swofty::log("[td-probe] gp failures (%.*s): a part will fail in lap %d of %d", int(mode.size()), mode.data(),
                        gp_point.lap + 1, laps);
        else
            swofty::log("[td-probe] gp failures (%.*s): %s", int(mode.size()), mode.data(),
                        gp_at >= 1e17 ? "no failure this race"
                        : percent >= 100 ? "a part will fail within a minute (test)"
                                         : "a part will fail within 15 minutes of driving (the race length is unknown)");
    }
    if (percent >= 100)
        gp_ai_failure_tick();  // the 100 % test fails two AI cars as well (0.75.5)
    gp_point_tick();
    if (gp_at >= 0 && gp_clock >= gp_at && gp_at < 1e17) {
        gp_at = 1e18;
        int part = test_failure_part();
        swofty::log("[td-probe] gp failures: breaking part %d", part);
        trigger_failure(part);
    }
}
