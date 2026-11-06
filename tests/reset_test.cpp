// Everything the three reset functions clear (probe/09_resets.inc): reset_car_state, reset_race_faults, reset_session.
// A field is set to a value that is not its default, the reset runs, the default is asserted. A reset statement that
// goes missing makes this test fail and the test names the field. The first half of every pair checks the premise
// too: before the reset each field is away from its default, so no check can pass by itself. reset_car_state and
// reset_race_faults run alone as well as through reset_session: reset_session calls both and would hide a field both
// of them clear (pit_call_ended).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include <string>
#include <vector>
#include "test_util.hpp"

namespace {
bool engine_off_sound_on = true;
int setting(const char *id, uint32_t type, SwoftySettingValue *out)
{
    if (strcmp(id, "engine_off_sound") || type != SWOFTY_SETTING_TOGGLE)
        return 0;
    out->type = type;
    out->toggle = engine_off_sound_on;
    return 1;
}
int watch(const char *, SwoftySettingFn, void *) { return 0; }

// The core's text overrides: the repair job's name is set while a suspension repair runs and cleared by the reset.
unsigned labels_set, labels_cleared;
int lang_available() { return 1; }
int lang_set(const char *, const char *key, const char *)
{
    assert(!strcmp(key, "lng_ft_detached_wheel"));
    ++labels_set;
    return SWOFTY_ASSET_OK;
}
int lang_clear(const char *, const char *key)
{
    assert(!strcmp(key, "lng_ft_detached_wheel"));
    ++labels_cleared;
    return SWOFTY_ASSET_OK;
}
const SwoftyLang spy_lang{lang_available, lang_set, lang_clear, nullptr, nullptr};

// One group of fields, each checked against its default: which are at it, which are away from it.
struct Fields {
    std::vector<std::string> at_default, away;
    void operator()(bool is_default, const char *what) { (is_default ? at_default : away).push_back(what); }
    size_t size() const { return at_default.size() + away.size(); }
};
#define AT_DEFAULT(fields, condition) (fields)((condition), #condition)

// The state before the reset: every field away from its default.
void expect_all_away(const Fields &f, const char *group)
{
    for (const std::string &what : f.at_default)
        printf("%s: already at its default before the reset, the test could not notice a missing reset: %s\n", group, what.c_str());
    assert(f.at_default.empty());
}
// The state after the reset: every field at its default.
void expect_all_default(const Fields &f, const char *group)
{
    for (const std::string &what : f.away)
        printf("%s: not reset: %s\n", group, what.c_str());
    assert(f.away.empty());
}

// ---- plain structs: every field away from its default, and the check that all of them are back ----
pl::State dirty_power_state()
{
    pl::State s;
    s.kind = pl::mguk_loss;
    s.t = 5;
    s.length = 100;
    s.depth = 0.3f;
    s.build_s = 2;
    s.hold_s = 3;
    s.intermittent = true;
    s.phase_on = false;
    s.phase_end = 9;
    s.next_cut = 7;
    s.cut_end = 8;
    return s;
}
// All the fields of a power course in one answer, for the dirty state "not one is at its default" counts too.
void power_state(Fields &f, const pl::State &s, const char *name)
{
    f(s.kind == pl::none, (std::string(name) + ".kind").c_str());
    f(s.t == 0, (std::string(name) + ".t").c_str());
    f(s.length == 0, (std::string(name) + ".length").c_str());
    f(s.depth == 0, (std::string(name) + ".depth").c_str());
    f(s.build_s == 0, (std::string(name) + ".build_s").c_str());
    f(s.hold_s == 0, (std::string(name) + ".hold_s").c_str());
    f(!s.intermittent, (std::string(name) + ".intermittent").c_str());
    f(s.phase_on, (std::string(name) + ".phase_on").c_str());
    f(s.phase_end == 0, (std::string(name) + ".phase_end").c_str());
    f(s.next_cut == 0, (std::string(name) + ".next_cut").c_str());
    f(s.cut_end == -1, (std::string(name) + ".cut_end").c_str());
}

void dirty_tracker(FlashbackTracker &t)
{
    t.session = 99;
    t.stamp = 5;
    t.deadline = 6;
    t.frame = 7;
    t.lock_frame = 8;
    t.target = 9;
    t.event_frame = 10;
    t.event_overall = 11;
    t.lock_known = t.pending = t.resumed = t.before_lock = true;
    t.damage_count = 3;
    t.damage_overflow = true;
}
// What clear_lock clears (the lock and the damage points) ...
bool lock_clean(const FlashbackTracker &t)
{
    return !t.lock_known && !t.pending && !t.resumed && t.damage_count == 0 && !t.damage_overflow;
}
bool lock_dirty(const FlashbackTracker &t)
{
    return t.lock_known && t.pending && t.resumed && t.damage_count == 3 && t.damage_overflow;
}
// ... and what only invalidate_session clears (the UDP session itself).
bool session_dirty(const FlashbackTracker &t)
{
    return t.session == 99 && t.stamp == 5 && t.deadline == 6 && t.frame == 7 && t.lock_frame == 8 && t.target == 9 &&
           t.event_frame == 10 && t.event_overall == 11 && t.before_lock;
}
bool session_clean(const FlashbackTracker &t)
{
    return t.session == 0 && t.stamp == 0 && t.deadline == 0 && t.frame == 0 && t.lock_frame == 0 && t.target == 0 &&
           t.event_frame == 0 && t.event_overall == 0 && !t.before_lock;
}

// ---- reset_car_state ----
void dirty_car_state()
{
    dirty_tracker(flashback_tracker);
    dirty_tracker(damage_flashback_tracker);
    replay_terminal_vehicle = 7;
    confirmed_retired_vehicle = 7;
    retired_drive_lock_vehicle = 7;
    pit_clearance_hold_vehicle = 7;
    retired_engine_off_vehicle = 7;
    pending_radio_command = 0xabcd;
    throttle_hook.cap = 0.25f;
    engine_off_vehicle = 7;
    engine_kill_vehicle = 7;
    engine_off_standing = true;
    pending_failure = 0x1234;
    garage_return_wanted = true;
    test_cap = 0.5f;
    car_dead = true;
    garage_withdrawn = true;
    garage_part_failed = true;
    garage_failed_types = 0x7f;
    part_lock = true;
    exit_check_at = 12345;
    terminal_mask = 0x1ff;
    pit_call_ended = true;
    limp_state = 1;
    limp_corner = 2;
    race_stop_left = 5.0;
    crash_corners = 0x2a;
    garage_repair = 2;
    garage_service_corners = 0xf;
    garage_service_seconds = 30.0f;
    garage_service_crack = true;
    repair_label(true);
    player_broken = true;
    retire_requested = true;
    player_pit = 9;
    still_for = 4.0;
    retire_in = 12.0;
    // cancel_crew_work
    crew_spawn_now = true;
    crew_spawn_wanted = true;
    crew_seq_t = 3.5;
    crew_request = 6;
    crew_request_in = 2.0;
    crew_msg = 3;
    crew_msg_in = 1.5;
    pending_crew = 4;
    crew_state_now = 5;
    sendoff_stage = 2;
    pushing = true;
    pushed = true;
    prepare_at_box = true;
    pit_stop_refused = true;
}

// cleared_before: labels_cleared when dirty_car_state ran (the reset gives the repair job the game's name again).
Fields car_state(unsigned cleared_before)
{
    Fields f;
    AT_DEFAULT(f, lock_clean(flashback_tracker));
    AT_DEFAULT(f, lock_clean(damage_flashback_tracker));
    AT_DEFAULT(f, replay_terminal_vehicle == -1);
    AT_DEFAULT(f, confirmed_retired_vehicle == -1);
    AT_DEFAULT(f, retired_drive_lock_vehicle == -1);
    AT_DEFAULT(f, pit_clearance_hold_vehicle == -1);
    AT_DEFAULT(f, retired_engine_off_vehicle == -1);
    AT_DEFAULT(f, pending_radio_command == 0);
    AT_DEFAULT(f, throttle_hook.cap == 1.0f);
    AT_DEFAULT(f, engine_off_vehicle == -1);
    AT_DEFAULT(f, engine_kill_vehicle == -1);
    AT_DEFAULT(f, !engine_off_standing);
    AT_DEFAULT(f, pending_failure == 0);
    AT_DEFAULT(f, !garage_return_wanted);
    AT_DEFAULT(f, test_cap == -1.0f);
    AT_DEFAULT(f, !car_dead);
    AT_DEFAULT(f, !garage_withdrawn);
    AT_DEFAULT(f, !garage_part_failed);
    AT_DEFAULT(f, garage_failed_types == 0);
    AT_DEFAULT(f, !part_lock);
    AT_DEFAULT(f, exit_check_at == 0);
    AT_DEFAULT(f, terminal_mask == 0);
    AT_DEFAULT(f, !pit_call_ended);
    AT_DEFAULT(f, limp_state == -1);
    AT_DEFAULT(f, limp_corner == -1);
    AT_DEFAULT(f, race_stop_left == 0);
    AT_DEFAULT(f, crash_corners == 0);
    AT_DEFAULT(f, garage_repair == 0);
    AT_DEFAULT(f, garage_service_corners == 0);
    AT_DEFAULT(f, garage_service_seconds == 0);
    AT_DEFAULT(f, !garage_service_crack);
    AT_DEFAULT(f, labels_cleared == cleared_before + 1);  // repair_label(false)
    AT_DEFAULT(f, !player_broken);
    AT_DEFAULT(f, !retire_requested);
    AT_DEFAULT(f, player_pit == 1);
    AT_DEFAULT(f, still_for == 0);
    AT_DEFAULT(f, retire_in == -1.0);
    AT_DEFAULT(f, !crew_spawn_now);
    AT_DEFAULT(f, !crew_spawn_wanted);
    AT_DEFAULT(f, crew_seq_t == -1.0);
    AT_DEFAULT(f, crew_request == 0);
    AT_DEFAULT(f, crew_request_in == -1.0);
    AT_DEFAULT(f, crew_msg == 0);
    AT_DEFAULT(f, crew_msg_in == -1.0);
    AT_DEFAULT(f, pending_crew == -1);
    AT_DEFAULT(f, crew_state_now == -1);
    AT_DEFAULT(f, sendoff_stage == 0);
    AT_DEFAULT(f, !pushing);
    AT_DEFAULT(f, !pushed);
    AT_DEFAULT(f, !prepare_at_box);
    AT_DEFAULT(f, !pit_stop_refused);
    return f;
}

// ---- reset_race_faults ----
void dirty_race_faults()
{
    game_failure_part = 3;
    game_failure_deadline = 5555;
    game_failure_lap = 8;
    game_failure_crossing = 9;
    pit_call_reason = 1;
    pit_call_ended = true;
    pit_at_suspension = 2;
    red_flag_repair_wait = true;
    red_flag_state = 0x1000;
    red_flag_toast_at = 77;
    red_flag_screen_open = true;
    red_flag_verdict = 2;
    red_flag_repair_deadline = 88;
    red_flag_fix_pending = true;
    red_flag_fix_now = true;
    live_terminal_state = 5;
    live_terminal_mask = 0x18;
    gp_clock = 321.0;
    gp_at = 400.0;
    gp_point.lap = 3;
    gp_point.in_lap = 0.5;
    gp_lap_seen = 4;
    gp_lap_started = 100.0;
    gp_lap_time = 90.0;
    gp_warn.outcome = 3;
    gp_warn.warn_at = 12.5;
    gp_warn.announced = true;
    gp_warn.next_cut = 4;
    gp_warn.cut_end = 6;
    warning_cap = 0.5f;
    power_warn = dirty_power_state();
    power_fault = dirty_power_state();
    power_fault_test = true;
    power_fault_brake_at = 900.0;
    power_fault_pitted = true;
    shown_power_loss = 0.3f;
    shown_power_kind = 2;
    shown_power_cuts = true;
    power_speed_wanted = true;
    gearbox_decline.path = 1;
    gearbox_decline.clock = 3;
    gearbox_decline.length = 60;
    gearbox_decline.stage = 2;
    gearbox_decline.gear = 4;
    gearbox_decline.test = true;
    decline_shift_scale = 2.5f;
    rear_brake_loss_target = 0.4f;
    rear_brake_until = 999;
    brake_fade = 0.5f;
    rear_brake_factor = 0.5f;
    radio_pending = 0x5;
    shown_shift_scale = 3.0f;
    shown_missing_gear = 4;
    shown_warning = 3;
    shown_decline_stage = 2;
    gp_fault_at = 50.0;
    gp_fault_type_rolled = 2;
    fault_request = 0xabc;
    breakdown_request = 0x8;
    breakdown_watch.obj = 0x1234;
    breakdown_watch.part = 2;
    breakdown_watch.at = 99;
    ai_failures_rolled = true;
    ai_failure_started = true;
}

Fields race_faults()
{
    Fields f;
    AT_DEFAULT(f, game_failure_part == -1);
    AT_DEFAULT(f, game_failure_deadline == 0);
    AT_DEFAULT(f, game_failure_lap == -1);
    AT_DEFAULT(f, game_failure_crossing == -1);
    AT_DEFAULT(f, pit_call_reason == 0);
    AT_DEFAULT(f, !pit_call_ended);
    AT_DEFAULT(f, pit_at_suspension == -1);
    AT_DEFAULT(f, !red_flag_repair_wait);
    AT_DEFAULT(f, red_flag_state == 0);
    AT_DEFAULT(f, red_flag_toast_at == 0);
    AT_DEFAULT(f, !red_flag_screen_open);
    AT_DEFAULT(f, red_flag_verdict == 0);
    AT_DEFAULT(f, red_flag_repair_deadline == 0);
    AT_DEFAULT(f, !red_flag_fix_pending);
    AT_DEFAULT(f, !red_flag_fix_now);
    AT_DEFAULT(f, live_terminal_state == -1);
    AT_DEFAULT(f, live_terminal_mask == 0);
    AT_DEFAULT(f, gp_clock == 0);
    AT_DEFAULT(f, gp_at == -1);
    AT_DEFAULT(f, gp_point.lap == -1);
    AT_DEFAULT(f, gp_point.in_lap == 0);
    AT_DEFAULT(f, gp_lap_seen == -1);
    AT_DEFAULT(f, gp_lap_started == 0);
    AT_DEFAULT(f, gp_lap_time == 0);
    AT_DEFAULT(f, gp_warn.outcome == -1);
    AT_DEFAULT(f, gp_warn.warn_at == 1e18);
    AT_DEFAULT(f, !gp_warn.announced);
    AT_DEFAULT(f, gp_warn.next_cut == 0);
    AT_DEFAULT(f, gp_warn.cut_end == 0);
    AT_DEFAULT(f, warning_cap == 1.0f);
    power_state(f, power_warn, "power_warn");
    power_state(f, power_fault, "power_fault");
    AT_DEFAULT(f, !power_fault_test);
    AT_DEFAULT(f, power_fault_brake_at == -1);
    AT_DEFAULT(f, !power_fault_pitted);
    AT_DEFAULT(f, shown_power_loss == 0.0f);
    AT_DEFAULT(f, shown_power_kind == 0);
    AT_DEFAULT(f, !shown_power_cuts);
    AT_DEFAULT(f, !power_speed_wanted);
    AT_DEFAULT(f, gearbox_decline.path == -1);
    AT_DEFAULT(f, gearbox_decline.clock == 0);
    AT_DEFAULT(f, gearbox_decline.length == 0);
    AT_DEFAULT(f, gearbox_decline.stage == -1);
    AT_DEFAULT(f, gearbox_decline.gear == 0xff);
    AT_DEFAULT(f, !gearbox_decline.test);
    AT_DEFAULT(f, decline_shift_scale == 0.0f);
    AT_DEFAULT(f, rear_brake_loss_target == 0.0f);
    AT_DEFAULT(f, rear_brake_until == 0);
    AT_DEFAULT(f, brake_fade == 0.0f);
    AT_DEFAULT(f, rear_brake_factor == 1.0f);
    AT_DEFAULT(f, radio_pending == 0);
    AT_DEFAULT(f, shown_shift_scale == 0.0f);
    AT_DEFAULT(f, shown_missing_gear == 11);
    AT_DEFAULT(f, shown_warning == -1);
    AT_DEFAULT(f, shown_decline_stage == -1);
    AT_DEFAULT(f, gp_fault_at == -1);
    AT_DEFAULT(f, gp_fault_type_rolled == -1);
    AT_DEFAULT(f, fault_request == 0);
    AT_DEFAULT(f, breakdown_request == 0);
    AT_DEFAULT(f, breakdown_watch.obj == 0);
    AT_DEFAULT(f, breakdown_watch.part == 0xff);
    AT_DEFAULT(f, breakdown_watch.at == 0);
    AT_DEFAULT(f, !ai_failures_rolled);
    AT_DEFAULT(f, !ai_failure_started);
    return f;
}

// ---- what only reset_session clears ----
void dirty_session()
{
    susp::reset_request = nullptr;
    ai_watch_mask = 0xffff;
    player_controller_flags = 0x800;
    owned_crew_instance = 0x1000;
    owned_crew_module = 0x2000;
    owned_crew_vehicle = 5;
    throttle_hook.car = 0x1234;
    telemetry_not_before = 1;  // long ago: only the reset can bring it up to the present
    test_run.active = true;
    test_run.ai = true;
    test_run.paused = true;
    test_run.decline_over = true;
    test_run.step = 3;
    test_run.reason_try = 1;
    test_run.clock = 5;
    test_run.step_clock = 2;
    test_run.speed_sum = 100;
    test_run.speed_time = 3;
    test_run.top_speed = 250;
    test_run.vehicle = 3;
    test_run_on = true;
    power_summary_due = false;
    for (int i = 0; i < max_vehicles; i++) {
        last_ai[i] = 5;
        last_reason[i] = 6;
        human[i] = true;
        observed_pit_state[i] = 3;
        pit_slot_of[i] = 7;
    }
    player_vehicle = 7;
}

// now_before and generation_before: the tick count and the memory cache generation when dirty_session ran.
Fields session_state(uint64_t now_before, uint32_t generation_before)
{
    Fields f;
    AT_DEFAULT(f, susp::reset_request.load() && !strcmp(susp::reset_request.load(), "new session"));
    AT_DEFAULT(f, ai_watch_mask == 0);
    AT_DEFAULT(f, session_clean(flashback_tracker));
    AT_DEFAULT(f, session_clean(damage_flashback_tracker));
    AT_DEFAULT(f, player_controller_flags == 0);
    AT_DEFAULT(f, owned_crew_instance == 0);
    AT_DEFAULT(f, owned_crew_module == 0);
    AT_DEFAULT(f, owned_crew_vehicle == -1);
    AT_DEFAULT(f, throttle_hook.car == 0);
    AT_DEFAULT(f, telemetry_not_before >= now_before);
    AT_DEFAULT(f, swofty::mem::detail::generation.load() != generation_before);  // the cached memory answers were dropped
    AT_DEFAULT(f, !test_run.active);
    AT_DEFAULT(f, !test_run.ai);
    AT_DEFAULT(f, !test_run.paused);
    AT_DEFAULT(f, !test_run.decline_over);
    AT_DEFAULT(f, test_run.step == -1);
    AT_DEFAULT(f, test_run.reason_try == 0);
    AT_DEFAULT(f, test_run.clock == 0);
    AT_DEFAULT(f, test_run.step_clock == 0);
    AT_DEFAULT(f, test_run.speed_sum == 0);
    AT_DEFAULT(f, test_run.speed_time == 0);
    AT_DEFAULT(f, test_run.top_speed == 0);
    AT_DEFAULT(f, test_run.vehicle == -1);
    AT_DEFAULT(f, !test_run_on);
    AT_DEFAULT(f, power_summary_due);  // a run that was active is summed up
    for (int i = 0; i < max_vehicles; i++) {
        AT_DEFAULT(f, last_ai[i] == -1);
        AT_DEFAULT(f, last_reason[i] == -1);
        AT_DEFAULT(f, !human[i]);
        AT_DEFAULT(f, observed_pit_state[i] == 0);
        AT_DEFAULT(f, pit_slot_of[i] == -1);
    }
    AT_DEFAULT(f, player_vehicle == -1);
    return f;
}
}  // namespace

int main()
{
    SwoftyApi api{};
    api.version = 8;
    api.log = tu::quiet_log;
    api.setting = setting;
    api.watch_setting = watch;
    api.lang = &spy_lang;
    swofty::api = &api;
    assert(engine_off_sound);
    size_t checked = 0;

    // ---- reset_car_state alone: the player's car, not the UDP session of the trackers ----
    dirty_car_state();
    unsigned cleared = labels_cleared;
    assert(labels_set == 1);  // dirty_car_state: the repair job reads "Suspension"
    expect_all_away(car_state(cleared), "reset_car_state");
    reset_car_state();
    expect_all_default(car_state(cleared), "reset_car_state");
    checked += car_state(cleared).size();
    assert(session_dirty(flashback_tracker) && session_dirty(damage_flashback_tracker));  // only the lock goes

    // ---- reset_race_faults alone ----
    dirty_race_faults();
    expect_all_away(race_faults(), "reset_race_faults");
    reset_race_faults();
    expect_all_default(race_faults(), "reset_race_faults");
    checked += race_faults().size();

    // ---- reset_session: all of it, the car, the race and the session ----
    dirty_car_state();
    dirty_race_faults();
    dirty_session();
    cleared = labels_cleared;
    uint64_t now_before = GetTickCount64();
    uint32_t generation_before = swofty::mem::detail::generation.load();
    expect_all_away(car_state(cleared), "reset_session (car)");
    expect_all_away(race_faults(), "reset_session (race)");
    expect_all_away(session_state(now_before, generation_before), "reset_session (session)");
    reset_session();
    expect_all_default(car_state(cleared), "reset_session (car)");
    expect_all_default(race_faults(), "reset_session (race)");
    expect_all_default(session_state(now_before, generation_before), "reset_session (session)");
    checked += session_state(now_before, generation_before).size();

    // ---- reset_car_state keeps the timelines it is told to keep (a flashback, a damage rewind) ----
    dirty_car_state();
    reset_car_state(false, true, false);
    assert(lock_dirty(flashback_tracker) && lock_clean(damage_flashback_tracker));
    dirty_car_state();
    reset_car_state(false, false, true);
    assert(lock_clean(flashback_tracker) && lock_dirty(damage_flashback_tracker));
    dirty_car_state();
    reset_car_state(false, true, true);
    assert(lock_dirty(flashback_tracker) && lock_dirty(damage_flashback_tracker));
    assert(!player_broken && !car_dead && terminal_mask == 0);  // the rest of the car is reset all the same

    // ---- a retired car keeps its silence and its drive lock through the reset (preserve_retirement_audio) ----
    for (bool sound : {true, false}) {
        engine_off_sound_on = sound;
        dirty_car_state();
        player_vehicle = 7;
        reset_car_state(true);
        assert(confirmed_retired_vehicle == 7 && retired_drive_lock_vehicle == 7 && pit_clearance_hold_vehicle == 7);
        assert(retired_engine_off_vehicle == 7 && engine_kill_vehicle == 7);
        assert(engine_off_vehicle == (sound ? 7 : -1));  // the engine audio follows the engine_off_sound option
        assert(throttle_hook.cap == 0.0f);                // the retired car is the player's: no drive at all
        assert(!player_broken && !car_dead && terminal_mask == 0);
        // The same through a new session; the player's id goes at its end.
        dirty_car_state();
        dirty_session();
        player_vehicle = 7;
        reset_session(true);
        assert(confirmed_retired_vehicle == 7 && retired_drive_lock_vehicle == 7 && pit_clearance_hold_vehicle == 7);
        assert(retired_engine_off_vehicle == 7 && engine_kill_vehicle == 7);
        assert(engine_off_vehicle == (sound ? 7 : -1));
        assert(throttle_hook.cap == 0.0f);
        assert(player_vehicle == -1 && !player_broken);
    }
    engine_off_sound_on = true;
    // A retired car that is not the player's leaves the player's throttle alone.
    dirty_car_state();
    player_vehicle = 3;
    reset_car_state(true);
    assert(throttle_hook.cap == 1.0f);
    player_vehicle = -1;
    printf("PASS: reset_car_state, reset_race_faults and reset_session clear all %zu values they own\n", checked);
}
