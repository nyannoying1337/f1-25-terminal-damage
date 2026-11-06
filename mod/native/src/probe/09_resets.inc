// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Per-car, per-race and per-session resets.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Everything about the player's broken car; the player's vehicle id stays known.
void reset_car_state(bool preserve_retirement_audio, bool preserve_request_timeline, bool preserve_damage_timeline)
{
    if (!preserve_request_timeline) flashback_tracker.clear_lock();
    if (!preserve_damage_timeline) damage_flashback_tracker.clear_lock();
    replay_terminal_vehicle = -1;
    if (!preserve_retirement_audio) confirmed_retired_vehicle=-1;
    if (!preserve_retirement_audio) retired_drive_lock_vehicle = -1;
    if (!preserve_retirement_audio) pit_clearance_hold_vehicle = -1;
    int silent_vehicle = preserve_retirement_audio ? retired_engine_off_vehicle.load() : -1;
    retired_engine_off_vehicle = silent_vehicle;
    pending_radio_command = 0;
    throttle_hook.cap = retired_drive_lock_vehicle.load() == player_vehicle.load() && player_vehicle.load()>=0 ? 0.0f : 1.0f;
    engine_off_vehicle = engine_off_sound ? silent_vehicle : -1;
    engine_kill_vehicle = silent_vehicle;
    engine_off_standing = false;
    pending_failure = 0;
    garage_return_wanted = false;
    test_cap = -1.0f;
    car_dead = false;
    garage_withdrawn = false;
    garage_part_failed = false;
    garage_failed_types = 0;
    part_lock = false;
    exit_check_at = 0;
    terminal_mask = 0;
    pit_call_ended = false;
    limp_state = -1;
    limp_corner = -1;
    race_stop_left = 0;
    crash_corners = 0;
    garage_repair = 0;
    garage_service_corners = 0;
    garage_service_seconds = 0;
    garage_service_crack = false;
    repair_label(false);
    player_broken = false;
    retire_requested = false;
    player_pit = 1;
    still_for = 0;
    retire_in = -1.0;
    cancel_crew_work();
}

// The race's failures and faults: the plans, warning signs, power and brake faults, the gearbox decline, the pit
// window after the game's failure, and what the panel shows of them. A new session and a race restart (frame thread).
// Values are cleared, not ended: ending a fault plays its "fixed" radio line. The test run has its own restart path.
void reset_race_faults()
{
    game_failure_part = -1;
    game_failure_deadline = 0;
    game_failure_lap = -1;
    game_failure_crossing = -1;
    pit_call_reason = 0;
    pit_call_ended = false;
    pit_at_suspension = -1;
    red_flag_repair_wait = false;
    red_flag_state = 0;
    red_flag_toast_at = 0;
    red_flag_screen_open = false;
    red_flag_verdict = 0;
    red_flag_repair_deadline = 0;
    red_flag_fix_pending = false;
    red_flag_fix_now = false;
    live_terminal_state = -1;
    live_terminal_mask = 0;
    gp_clock = 0; gp_at = -1;
    gp_point = {};
    gp_lap_seen = -1;
    gp_lap_started = gp_lap_time = 0;
    gp_warn = {};
    warning_cap = 1.0f;
    power_warn = {};
    power_fault = {};
    power_fault_test = false;
    power_fault_brake_at = -1;
    power_fault_pitted = false;
    shown_power_loss = 0.0f;
    shown_power_kind = 0;
    shown_power_cuts = false;
    power_speed_wanted = false;
    gearbox_decline = {};
    decline_shift_scale = 0.0f;
    rear_brake_loss_target = 0.0f;
    rear_brake_until = 0;
    brake_fade = 0.0f;
    rear_brake_factor = 1.0f;
    radio_pending = 0;
    shown_shift_scale = 0.0f;
    shown_missing_gear = 11;
    shown_warning = -1;
    shown_decline_stage = -1;
    gp_fault_at = -1;
    gp_fault_type_rolled = -1;
    fault_request = 0;
    breakdown_request = 0;
    breakdown_watch = {};
    ai_failures_rolled = false;
    ai_failure_started = false;
}

void reset_session(bool preserve_retirement_audio = false)
{
    susp::request_reset("new session");
    ai_watch_mask = 0;
    ai_watch_asked = 0;
    reset_car_state(preserve_retirement_audio);
    reset_race_faults();
    test_run_reset();
    flashback_tracker.invalidate_session();damage_flashback_tracker.invalidate_session();
    player_controller_flags=0;
    owned_crew_instance=owned_crew_module=0;owned_crew_vehicle=-1;
    throttle_hook.car = 0;
    telemetry_not_before = GetTickCount64();
    swofty::mem::invalidate();
    for (int i = 0; i < max_vehicles; i++) {
        last_ai[i] = -1;
        last_reason[i] = -1;
        human[i] = false;
        observed_pit_state[i] = 0;
        pit_slot_of[i] = -1;
    }
    player_vehicle = -1;
}

void find_terminal_message()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_terminal_send_site, &site);
    if (n != 1) {
        swofty::log("[td-probe] terminal message: signature has %d hits, keep_control only drops the hand-over", n);
        return;
    }
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void *>(site + 0x0c), 4);
    terminal_vtable = reinterpret_cast<const void *>(site + 0x10 + int64_t(rel));
    swofty::log("[td-probe] terminal message vtable %p", terminal_vtable.load());
}

// The player's car as VehicleTerminalDamageSystem sees it, or 0.
uintptr_t car_of(uintptr_t sys, int vehicle)
{
    uint64_t count = 0;
    uintptr_t rec = 0, car = 0;
    if (vehicle < 0 || !sys || !guarded_read(sys + 0xc20, count) || uint64_t(vehicle) >= count ||
        !guarded_read(sys + 0x50 + uint64_t(vehicle) * 0x70 + 8, rec) || !rec ||
        !guarded_read(rec + 0x58, car))
        return 0;
    return car;
}

float fraction(std::string_view v)
{
    if (v.size() < 3 || v[0] != '0' || v[1] != '.')
        return 0;
    float f = 0, scale = 0.1f;
    for (char ch : v.substr(2)) {
        if (ch < '0' || ch > '9')
            return 0;
        f += float(ch - '0') * scale;
        scale *= 0.1f;
    }
    return f;
}
