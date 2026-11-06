// Standalone Windows test. No game hooks, sockets or patch installation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include <cstdarg>
#include "test_util.hpp"

namespace {
unsigned destroyed, freed, hud_updates;
uint32_t hud_masks[2]{};
uintptr_t expected, expected_allocator;
void destroy(void *p)
{
    assert(reinterpret_cast<uintptr_t>(p) == expected);
    ++destroyed;
}
void release(uintptr_t a, uintptr_t p)
{
    assert(a == expected_allocator && p == expected);
    ++freed;
}
using tu::put;
// gp_failures for the mode change test and debug_limp_roll for the limp test (nullptr: the code default); every
// other setting keeps its default.
const char *fake_gp_failures = nullptr;
const char *fake_limp_roll = nullptr;
int fake_setting(const char *id, uint32_t type, SwoftySettingValue *out)
{
    const char *value = !strcmp(id, "gp_failures")     ? fake_gp_failures
                        : !strcmp(id, "debug_limp_roll") ? fake_limp_roll
                                                         : nullptr;
    if (!value || type != SWOFTY_SETTING_CHOICE)
        return 0;
    out->type = type;
    out->choice = value;
    return 1;
}
int fake_watch(const char *, SwoftySettingFn, void *) { return 0; }
// A log function that keeps the lines, for the tests that assert on what the addon says.
std::vector<std::string> captured_lines;
void capture_log(const char *fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    captured_lines.emplace_back(text);
}
unsigned captured_count(const char *needle)
{
    unsigned n = 0;
    for (const std::string &line : captured_lines)
        n += line.find(needle) != std::string::npos;
    return n;
}
}  // namespace

int main()
{
    SwoftyApi api{};
    api.log = tu::quiet_log;
    swofty::api = &api;

    // ---- Issue #8: a flashback removes the race garage crew the addon spawned ----
    uint8_t module[0x180]{}, allocator[8]{}, vt[0x40]{}, crew[8]{};
    put(module, 0x18, reinterpret_cast<uintptr_t>(allocator));
    put(allocator, 0, reinterpret_cast<uintptr_t>(vt));
    put(vt, 0x38, reinterpret_cast<uintptr_t>(&release));
    auto base = reinterpret_cast<uintptr_t>(module);
    expected = reinterpret_cast<uintptr_t>(crew);
    expected_allocator = reinterpret_cast<uintptr_t>(allocator);
    void *module_ptr = module;
    crew_module = &module_ptr;
    destroy_crew = destroy;
    hud_visibility_manager = +[]() -> void * { return hud_masks; };
    update_hud_visibility = +[](void *p) {
        assert(p == hud_masks);
        ++hud_updates;
    };
    reset_session();
    player_vehicle = 3;
    // Practice (no own crew): nothing happens.
    remove_owned_crew_after_flashback();
    assert(destroyed == 0);
    // Race, crew out for the push, flashback rewinds to before the box.
    put(module, 0x68 + 3 * 8, expected);
    owned_crew_module = base;
    owned_crew_instance = expected;
    owned_crew_vehicle = 3;
    hud_masks[0] = 4;
    player_pit = 1;
    flashback_count.fetch_add(1);
    on_frame(0.016, nullptr);
    uintptr_t slot = 1;
    guarded_read(base + 0x68 + 3 * 8, slot);
    assert(destroyed == 1 && freed == 1 && slot == 0 && !owned_crew_instance);
    assert(hud_masks[0] == 0 && hud_updates == 1);
    // No second removal on the next frame.
    on_frame(0.016, nullptr);
    assert(destroyed == 1);
    // A replaced slot (the game made its own crew) is left alone.
    uint8_t other[8]{};
    put(module, 0x68 + 3 * 8, reinterpret_cast<uintptr_t>(other));
    owned_crew_module = base;
    owned_crew_instance = expected;
    owned_crew_vehicle = 3;
    flashback_count.fetch_add(1);
    on_frame(0.016, nullptr);
    guarded_read(base + 0x68 + 3 * 8, slot);
    assert(destroyed == 1 && slot == reinterpret_cast<uintptr_t>(other) && !owned_crew_instance);

    // ---- Issue #9: no addon retirement while the game holds the car (red flag hand-over) ----
    reset_session();
    player_vehicle = 3;
    player_broken = true;
    retire_in = 1.0;
    player_controller_flags = 1u << 11;
    on_frame(3.0, nullptr);
    assert(retire_in.load() == 1.0);
    player_controller_flags = 1u << 10;
    on_frame(3.0, nullptr);
    assert(retire_in.load() == 1.0);
    player_controller_flags = 0;
    on_frame(3.0, nullptr);
    assert(retire_in.load() == -2.0);  // expired: retire_player ran (and logged "not possible" without the game)

    // ---- Issue #5: garage lock with a failed part ----
    reset_session();
    car_dead = true;
    // Plain retirement or crash damage: locked, as before.
    assert(garage_exit_locked());
    // A failed part without a Career record: the old path, the send-out shows the swap.
    garage_part_failed = true;
    garage_failed_types = 1;
    part_lock_tick();  // no player car: never locks
    assert(!part_lock && !garage_exit_locked());
    // With the Career allocation showing no replacement: locked.
    part_lock = true;
    assert(garage_exit_locked());
    terminal_mask = 0x1;
    assert(garage_exit_locked());
    part_lock = false;
    assert(!garage_exit_locked());
    part_lock = true;  // not the default, so the reset has something to clear
    reset_car_state();
    assert(!garage_failed_types && !part_lock);

    // ---- 0.51.124: without a Career record the lookup runs at most every 2 s (0.51.123 log: 254 us a frame in a
    // Career practice, retried every frame); a new failure looks again at once ----
    {
        static int lookups = 0;
        static SwoftyCareer cr{};
        cr.state = [](SwoftyCareerState *out) {
            *out = {};
            out->loaded = 1;
            return int(SWOFTY_ASSET_OK);
        };
        cr.component = [](const char *) -> void * {
            ++lookups;
            return nullptr;
        };
        uint32_t version = api.version;
        api.version = 7;
        api.career = &cr;
        auto car_before = throttle_hook.car;
        throttle_hook.car = 0x1000;
        car_dead = true;
        garage_part_failed = true;
        garage_failed_types = 1;
        for (int i = 0; i < 5; ++i)
            part_lock_tick();
        assert(lookups == 1 && !part_lock);
        reset_car_state();
        part_lock_tick();  // nothing failed: the next failure looks again at once
        car_dead = true;
        garage_part_failed = true;
        garage_failed_types = 1;
        part_lock_tick();
        assert(lookups == 2 && !part_lock);
        reset_car_state();
        part_lock_tick();
        throttle_hook.car = car_before;
        api.career = nullptr;
        api.version = version;
    }

    // ---- 0.51.124: Ctrl+Shift+F7 replays the game's planner too, so the addon keeps the car as after the game's own
    // scripted failure (0.51.123 log: without the planned part the AI took it) ----
    {
        reset_session();
        session_ended = false;
        player_vehicle = 3;
        player_pit = 1;
        player_broken = false;
        retire_requested = false;
        retire_in = -1.0;
        own_retire_until = 0;
        status7_at = 0;
        controller_system = reinterpret_cast<void *>(0x1000);
        player_controller_flags = 0;
        confirmed_retired_vehicle = -1;
        game_planned_part = -1;
        game_failure_part = -1;
        game_failure_deadline = 0;
        scripted_part_at = 0;
        trigger_scripted_failure();
        assert(game_failure_part.load() == 0 && game_planned_part.load() == -1);  // Debug: failing part, 0 ICE
        assert(game_failure_deadline.load() != 0 && scripted_part_at.load() == 0);
        assert(confirmed_retired_vehicle.load() == -1 && !(player_controller_flags.load() & (1u << 4)));
        controller_system = nullptr;
        game_failure_deadline = 0;
        game_failure_part = -1;
        pending_failure = 0;
        reset_session();
    }

    // ---- FIA 26.18 / B1.8.9 (0.51.118): a broken car taken off the track in practice/qualifying is withdrawn ----
    {
        static uint8_t pit_lane[0x400]{};
        pit_lane[0x300] = 1;  // practice_like()
        reset_session();
        pit_lane_system = reinterpret_cast<uintptr_t>(pit_lane);
        assert(practice_like());
        player_vehicle = 3;
        player_broken = true;
        player_pit = 1;  // stopped on track
        retire_in = 1.0;
        on_frame(3.0, nullptr);
        assert(garage_withdrawn && garage_return_wanted && retire_in.load() == -2.0);
        // In the garage: a failed part that was replaced does not unlock it.
        car_dead = true;
        terminal_mask = 0x1;
        garage_part_failed = true;
        assert(garage_exit_locked());
        part_lock = false;
        assert(garage_exit_locked());
        reset_car_state();
        assert(!garage_withdrawn && !car_dead);
        // The same in the pit lane (it drove in): not withdrawn, the part swap path stays.
        garage_return_wanted = false;
        player_vehicle = 3;
        player_broken = true;
        player_pit = 2;
        retire_in = 1.0;
        on_frame(3.0, nullptr);
        assert(!garage_withdrawn && garage_return_wanted);
        car_dead = true;
        terminal_mask = 0x1;
        garage_part_failed = true;
        assert(!garage_exit_locked());
        pit_lane_system = 0;
        reset_session();
        garage_return_wanted = false;
    }

    // ---- Black flag with an orange disc (0.51.118): pit by the end of the NEXT lap ----
    assert(!pit_window_over(4, 4) && !pit_window_over(4, 5) && pit_window_over(4, 6));
    assert(!pit_window_over(-1, 9) && !pit_window_over(4, -1));
    {
        // Without a lap number: 10 minutes of driving, and the pause menu (no update of the car) does not count. No
        // speed read is needed (0.75.4): with suspension damage off nothing reads it.
        reset_session();
        player_vehicle = 3;
        player_pit = 1;
        dsq_pending = false;
        game_failure_lap = -1;
        game_failure_deadline = GetTickCount64() | 1;
        telemetry_not_before = 1;
        game_speed_ms = 0;
        player_update_ms = 0;  // paused: the game does not update the car
        gp_failure_tick(700.0);
        assert(game_failure_deadline.load() != 0 && !dsq_pending);
        player_update_ms = GetTickCount64();
        gp_failure_tick(500.0);
        assert(game_failure_deadline.load() != 0 && !dsq_pending);
        player_update_ms = GetTickCount64();
        gp_failure_tick(101.0);
        assert(game_failure_deadline.load() == 0 && dsq_pending);
        dsq_pending = false;
        // A new window starts its own count.
        game_failure_deadline = (GetTickCount64() + 2) | 1;
        player_update_ms = GetTickCount64();
        gp_failure_tick(500.0);
        assert(game_failure_deadline.load() != 0 && !dsq_pending);
        // Reaching the pit lane closes it without a penalty.
        player_pit = 2;
        gp_failure_tick(0.1);
        assert(game_failure_deadline.load() == 0 && !dsq_pending);
        reset_session();
    }

    // ---- Red flag (0.51.118): a broken corner that could not be driven, on the circuit -> abandoned (B1.8.9) ----
    {
        orig_red_flag_strategy_enter = +[](void *) {};
        uint8_t state[0x1000]{};
        reset_session();
        player_vehicle = 3;
        player_broken = true;
        terminal_mask = 0x10;  // suspension, no limp
        pit_at_suspension = 1;
        red_flag_screen_open = false;
        red_flag_verdict = 0;
        on_red_flag_strategy_enter(state);
        assert(red_flag_verdict.load() == 3 && red_flag_screen_open.load() && !red_flag_repair_wait.load());
        // 0.51.126: aero or body damage only keeps its drive, so on track at the suspension it is accident
        // damage the team may repair, not an abandoned car.
        Sleep(1100);  // the hook ignores a second enter within a second
        red_flag_screen_open = false;
        red_flag_toast_at = 0;
        reset_session();
        player_vehicle = 3;
        player_broken = true;
        terminal_mask = 0x40;
        pit_at_suspension = 1;
        red_flag_verdict = 0;
        on_red_flag_strategy_enter(state);
        assert(red_flag_verdict.load() == 0 && !red_flag_screen_open.load() && red_flag_repair_wait.load() &&
               red_flag_fix_pending.load());
        // In the pit lane at the suspension: the team may repair it.
        Sleep(1100);
        reset_session();
        player_vehicle = 3;
        player_broken = true;
        terminal_mask = 0x40;
        pit_at_suspension = 2;
        red_flag_screen_open = false;
        red_flag_verdict = 0;
        on_red_flag_strategy_enter(state);
        assert(red_flag_verdict.load() == 0 && !red_flag_screen_open.load() && red_flag_repair_wait.load() &&
               red_flag_fix_pending.load());
        // A failed power unit retires whatever the place.
        Sleep(1100);
        reset_session();
        player_vehicle = 3;
        player_broken = true;
        terminal_mask = 0x2;
        pit_at_suspension = 2;
        red_flag_screen_open = false;
        red_flag_verdict = 0;
        on_red_flag_strategy_enter(state);
        assert(red_flag_verdict.load() == 1 && red_flag_screen_open.load());
        red_flag_screen_open = false;
        red_flag_toast_at = 0;
        red_flag_state = 0;
        reset_session();
        orig_red_flag_strategy_enter = nullptr;
    }

    // ---- A race restart (0.51.119): no session end or start, the last race's faults and damage must go ----
    {
        reset_session();
        player_vehicle = 3;
        player_pit = 1;
        power_fault.kind = pl::mguk_loss;
        power_fault_test = true;
        gp_clock = 300;
        gp_at = 900;
        gp_fault_at = 400;
        rear_brake_loss_target = -0.8f;
        brake_fade = 0.3f;
        gearbox_decline.path = fm::decline_hydraulics;
        decline_shift_scale = 4.0f;
        game_failure_deadline = GetTickCount64() | 1;
        game_failure_lap = 7;
        radio_pending = 1u << radio_mgu;
        ai_failures_rolled = true;
        susp::enabled = true;
        susp::model.c[2].bend = 0.02f;
        susp::model.c[2].crack_at = 1;
        reset_for_session_restart(nullptr);
        assert(race_restart_pending.load() && test_run_restarted.load());
        // 0.75: Realistic is the default and plans the new race's failure on the first frame; this checks that the
        // old race's plan goes, so the failures are off here.
        uint32_t version = api.version;
        api.version = 4;
        api.setting = fake_setting;
        api.watch_setting = fake_watch;
        fake_gp_failures = "off";
        on_frame(0.016, nullptr);
        fake_gp_failures = nullptr;
        api.setting = nullptr;
        api.watch_setting = nullptr;
        api.version = version;
        assert(!race_restart_pending.load());
        assert(power_fault.kind == pl::none && !power_fault_test && gp_clock < 1 && gp_at == -1 && gp_fault_at == -1);
        assert(rear_brake_loss_target.load() == 0.0f && brake_fade.load() == 0.0f && gearbox_decline.path == -1);
        assert(decline_shift_scale.load() == 0.0f && game_failure_deadline.load() == 0 && game_failure_lap.load() == -1);
        assert(radio_pending.load() == 0 && !ai_failures_rolled);
        assert(susp::model.c[2].bend == 0 && susp::model.c[2].crack_at == 0);  // the suspension model too
        susp::enabled = false;
        test_run_restarted = false;
        reset_session();
    }

    // ---- The failure mode changed mid-session (0.51.119: one click with a preset) ----
    {
        api.version = 4;
        api.setting = fake_setting;
        api.watch_setting = fake_watch;
        reset_session();
        player_vehicle = 3;
        player_pit = 1;
        // Realistic with warning signs running, then Off: the signs end at once (their power loss froze before).
        fake_gp_failures = "realistic";
        gp_failure_tick(0.016);
        gp_at = gp_clock + 100;
        gp_warn.outcome = fm::brakes;
        gp_warn.warn_at = 0;
        power_warn.kind = pl::leak;
        brake_fade = 0.3f;
        gp_fault_at = 500;
        fake_gp_failures = "off";
        gp_failure_tick(0.016);
        assert(power_warn.kind == pl::none && brake_fade.load() == 0.0f && gp_at == -1 && gp_fault_at == -1 &&
               gp_warn.outcome == -1);
        // A percentage that rolled "no failure", then Realistic: Realistic plans its own.
        fake_gp_failures = "25";
        gp_failure_tick(0.016);
        gp_clock = 30;
        gp_at = 1e18;
        fake_gp_failures = "realistic";
        gp_failure_tick(0.016);
        assert(gp_at > 30 && gp_at < 1e17);
        // Realistic's plan, then 25 %: dropped, 25 % rolls its own (fails or not, never the realistic time).
        fake_gp_failures = "25";
        double realistic_at = gp_at;
        gp_failure_tick(0.016);
        assert(gp_at != realistic_at);
        fake_gp_failures = nullptr;
        api.setting = nullptr;
        api.watch_setting = nullptr;
        api.version = 0;
        gp_failure_tick(0.016);  // back to the default (off)
        reset_session();
    }

    // ---- The failure machinery stops once the game ends the session (0.75.9) ----
    // The 0.75.8 Career log: you retired into the garage, the game ended the race for everybody (NOT_IN_SESSION) and
    // Swofty's session.end came only when the results screen was left, 8 minutes later. Resetting the retired car at
    // NOT_IN_SESSION put player_broken, retire_requested and player_pit back, so the failure clock ran on and an AI
    // car's planned failure was armed 37 s into the results screen.
    {
        api.version = 4;
        api.setting = fake_setting;
        api.watch_setting = fake_watch;
        fake_gp_failures = "realistic";
        // The game's failure manager with objects for vehicles 0, 4 and 8 (as in realistic_failures_test).
        alignas(16) uint8_t fmgr[0x200]{}, fobjs[3][0x100]{};
        alignas(16) uint64_t fentries[6]{};
        for (int i = 0; i < 3; ++i) {
            fobjs[i][0x78] = uint8_t(i * 4);
            fobjs[i][0xa0] = 7;
            fentries[i * 2 + 1] = reinterpret_cast<uint64_t>(fobjs[i]);
        }
        uint64_t fbegin = reinterpret_cast<uint64_t>(fentries), fend = fbegin + sizeof fentries;
        memcpy(fmgr + 0x198, &fbegin, 8);
        memcpy(fmgr + 0x1a0, &fend, 8);
        reset_session();
        player_vehicle = 0;
        player_pit = 1;
        gp_failure_tick(0.0);  // reads the mode once: a change of it would drop the plans below
        // One race: you are vehicle 0, AI car 4 is due after 3 s of driving, nothing is planned for you.
        auto race = [&] {
            reset_session();
            note_controller_switch(0, 0, 0);  // HUMAN: vehicle 0 is yours
            player_pit = 1;
            failure_mgr = reinterpret_cast<uintptr_t>(fmgr);
            gp_clock = 30;
            gp_at = 1e18;
            gp_fault_at = 1e18;
            gp_ai_failure_tick();  // rolls the AI plan
            assert(ai_failures_rolled);
            for (double &t : ai_fail_at)
                t = 1e18;
            ai_fail_at[4] = gp_clock + 3;
            player_plan_epoch = fault_plan_epoch = team_epoch.load();  // no new plan for the late team ids
        };
        // In a running race the clock runs and car 4 breaks after 3 s.
        race();
        for (int i = 0; i < 4; ++i)
            gp_failure_tick(1.0);
        assert(gp_clock == 34 && breakdown_request.load() == (1u << 4));
        // You retire into the garage, the game ends the session: the clock and the plans stand, however often it ticks.
        race();
        player_broken = true;
        player_pit = 9;  // in the garage
        note_controller_switch(0, 1, 3);  // NOT_IN_SESSION; the reset of a broken car puts the clock's guards back
        assert(!player_broken.load() && player_pit.load() == 1);
        for (int i = 0; i < 10; ++i)
            gp_failure_tick(1.0);
        assert(gp_clock == 30 && breakdown_request.load() == 0);
        // The same when the car was not broken (a finish: nothing is reset, the flag alone holds).
        race();
        note_controller_switch(0, 1, 3);
        for (int i = 0; i < 10; ++i)
            gp_failure_tick(1.0);
        assert(gp_clock == 30 && breakdown_request.load() == 0);
        // The pit window of the game's failure does not run out on the results screen (700 s would disqualify the car).
        race();
        dsq_pending = false;
        game_failure_lap = -1;
        game_failure_deadline = GetTickCount64() | 1;
        telemetry_not_before = 1;
        player_update_ms = GetTickCount64();
        note_controller_switch(0, 1, 3);
        gp_failure_tick(700.0);
        assert(game_failure_deadline.load() != 0 && !dsq_pending);
        // A race restart from the results screen has no session.end or session.start: it clears the flag itself.
        race();
        note_controller_switch(0, 1, 3);
        gp_failure_tick(1.0);
        assert(gp_clock == 30);
        reset_for_session_restart(nullptr);
        assert(!(player_controller_flags.load() & (1u << 3)));
        race_restart_pending = false;
        test_run_restarted = false;
        gp_failure_tick(1.0);
        assert(gp_clock == 31);
        // The next session (session.start) does not inherit the flag either.
        race();
        note_controller_switch(0, 1, 3);
        reset_session();
        note_controller_switch(0, 0, 0);
        player_pit = 1;
        gp_failure_tick(1.0);
        assert(gp_clock == 1);
        failure_mgr = 0;
        fake_gp_failures = nullptr;
        api.setting = nullptr;
        api.watch_setting = nullptr;
        api.version = 0;
        reset_session();
    }

    // ---- The test run's gearbox decline asks the game for seconds, not the rest of the race (0.51.119) ----
    {
        reset_session();
        player_vehicle = 3;
        player_pit = 1;
        start_gearbox_decline(40.0, fm::decline_hydraulics);
        gearbox_decline.test = true;
        gearbox_decline_tick(0.1);
        uint64_t r = fault_request.load();
        assert(r && (r & 0xff) == uint64_t(fm::f_slow_shift) && ((r >> 40) & 1) == 1 && ((r >> 8) & 0xffffffff) == 1200);
        test_run_end_effects("test");
        assert(fault_request.load() == 0 && gearbox_decline.path == -1);
        // A real decline (warning signs of a gearbox failure): 999 laps.
        start_gearbox_decline(40.0, fm::decline_hydraulics);
        gearbox_decline_tick(0.1);
        r = fault_request.load();
        assert(((r >> 40) & 1) == 0 && ((r >> 8) & 0xffffffff) == 9990);
        reset_session();
    }

    // ---- Player update cost (0.51.119): the failure object is cached, slow-shift records read 4x a second ----
    {
        reset_session();
        alignas(16) static uint8_t mgr[0x200]{}, obj[0x100]{}, rcar[0x600]{}, gearbox[0x700]{};
        alignas(16) static uint64_t entries[2]{};
        alignas(16) static uint8_t recs[0x34]{};
        uintptr_t o = reinterpret_cast<uintptr_t>(obj), rc = reinterpret_cast<uintptr_t>(rcar);
        entries[1] = o;
        uint64_t b = reinterpret_cast<uint64_t>(entries), e = b + sizeof entries;
        put(mgr, 0x198, b);
        put(mgr, 0x1a0, e);
        obj[0x78] = 3;
        put(obj, 0x70, rc);
        failure_mgr = reinterpret_cast<uintptr_t>(mgr);
        player_vehicle = 3;
        assert(player_failure_obj(rc) == o && player_failure_obj(rc) == o);
        // The object no longer belongs to this car: looked up again, none.
        put(obj, 0x70, uintptr_t(1));
        assert(player_failure_obj(rc) == 0);
        put(obj, 0x70, rc);
        assert(player_failure_obj(rc) == o && player_failure_obj(0) == 0);
        // Slow shifts: an active record of x10 is seen, ends at once when it finishes, a new one within 250 ms.
        uint64_t rb = reinterpret_cast<uint64_t>(recs), re = rb + sizeof recs;
        put(obj, 0xc8, rb);
        put(obj, 0xd0, re);
        put(recs, 0, uint32_t(fm::f_slow_shift));
        put(recs, 0x2c, 100.0f);
        recs[0x18] = 1;
        put(rcar, 0x538, reinterpret_cast<uintptr_t>(gearbox));
        put(gearbox, 0x664, 4);
        Sleep(300);
        slow_shift_tick(rc);
        assert(shown_shift_scale.load() == 10.0f && shown_missing_gear.load() == 4);
        recs[0x19] = 1;  // finished
        slow_shift_tick(rc);
        assert(shown_shift_scale.load() == 0.0f);
        recs[0x19] = 0;  // a new one: read at the next scan, not on every update
        slow_shift_tick(rc);
        assert(shown_shift_scale.load() == 0.0f);
        Sleep(300);
        slow_shift_tick(rc);
        assert(shown_shift_scale.load() == 10.0f);
        recs[0x18] = 0;
        slow_shift_tick(rc);
        failure_mgr = 0;
        // The game's speed: a guarded copy of body+0x220 (no VirtualQuery on the game thread).
        alignas(16) static uint8_t physics[0x400]{}, rigid[0x40]{}, body[0x240]{};
        put(rcar, 0x558, reinterpret_cast<uintptr_t>(physics));
        put(physics, 0x340, reinterpret_cast<uintptr_t>(rigid));
        put(rigid, 0x18, reinterpret_cast<uintptr_t>(body));
        put(body, 0x220, 30.0f);
        put(body, 0x224, 40.0f);
        read_game_speed(rc);
        assert(std::abs(game_speed.load() - 180) <= 1);
        reset_session();
    }

    // ---- Frame work is timed in parts even without the diagnostics option (0.51.119) ----
    {
        uint64_t before = td_perf::counters[td_perf::frame_total].samples.load();
        on_frame(0.016, nullptr);
        assert(!td_perf::enabled.load() && td_perf::counters[td_perf::frame_total].samples.load() >= std::max<uint64_t>(before, 1) &&
               td_perf::counters[td_perf::frame_state].samples.load() > 0);
    }

    // ---- Race: a car crawling on a broken corner must stop; 20 s of driving, then it is retired ----
    reset_session();
    player_vehicle = 3;
    player_broken = true;
    limp_state = 1;
    race_stop_left = race_stop_grace;
    game_speed = 80;
    telemetry_not_before = 1;
    game_speed_ms = GetTickCount64();
    race_stop_tick(10);
    assert(race_stop_left.load() == 10 && retire_in.load() == -1.0);
    player_controller_flags = 1u << 11;  // red flag hand-over: held
    race_stop_tick(30);
    assert(race_stop_left.load() == 10 && retire_in.load() == -1.0);
    player_controller_flags = 0;
    player_pit = 2;  // in the pit lane: the box flow decides
    race_stop_tick(30);
    assert(race_stop_left.load() == 10);
    player_pit = 1;
    game_speed = 0;  // stopped: the standstill retirement takes over
    race_stop_tick(30);
    assert(race_stop_left.load() == 10 && retire_in.load() == -1.0);
    game_speed = 60;
    race_stop_tick(11);
    assert(race_stop_left.load() == 0 && retire_in.load() == 0.0);
    race_stop_left = 5;  // not the default, so the reset has something to clear
    reset_car_state();
    assert(race_stop_left.load() == 0);

    // ---- Heavy damage (0.51.126): aero or body damage only keeps the drive and calls the car in ----
    {
        reset_session();
        player_vehicle = 3;
        player_pit = 1;
        player_broken = true;
        terminal_mask = 0x40;  // the Shanghai test race: a side hit, the front wing / nose part at terminal
        std::vector<uint8_t> hcar(0x1000);
        power_loss(reinterpret_cast<uintptr_t>(hcar.data()));
        assert(throttle_hook.cap == 1.0f);
        // The call opens the window once; without the game's lap the line crossings count.
        player_crossings = 7;
        gp_failure_tick(0.1);
        uint64_t opened = game_failure_deadline.load();
        assert(opened && pit_call_reason.load() == 1 && game_failure_crossing.load() == 7 &&
               game_failure_lap.load() == -1);
        gp_failure_tick(0.1);
        assert(game_failure_deadline.load() == opened);
        player_crossings = 8;  // the next lap starts: still open
        gp_failure_tick(0.1);
        assert(game_failure_deadline.load() == opened && !dsq_pending);
        player_crossings = 9;  // the next lap ends: disqualified (the default), and no new call after it
        gp_failure_tick(0.1);
        assert(!game_failure_deadline.load() && dsq_pending && pit_call_ended.load());
        dsq_pending = false;
        gp_failure_tick(0.1);
        assert(!game_failure_deadline.load());
        // Reaching the pit lane closes the window; back on track (drove through the box) it opens again.
        reset_session();
        player_vehicle = 3;
        player_broken = true;
        terminal_mask = 0x40;
        player_pit = 1;
        gp_failure_tick(0.1);
        assert(game_failure_deadline.load());
        player_pit = 2;
        gp_failure_tick(0.1);
        assert(!game_failure_deadline.load() && !pit_call_ended.load());
        gp_failure_tick(0.1);
        assert(!game_failure_deadline.load());
        player_pit = 1;
        gp_failure_tick(0.1);
        assert(game_failure_deadline.load());
        // Retiring, a red flag hold, a corner part or another part: no call.
        for (int k = 0; k < 4; ++k) {
            reset_session();
            player_vehicle = 3;
            player_broken = true;
            player_pit = 1;
            terminal_mask = k == 2 ? 0x50u : k == 3 ? 0x41u : 0x40u;
            if (k == 0)
                retire_in = 0.0;
            if (k == 1)
                player_controller_flags = 1u << 11;
            gp_failure_tick(0.1);
            assert(!game_failure_deadline.load());
            player_controller_flags = 0;
        }
        // The game's failure window keeps its own name and count.
        reset_session();
        assert(pit_call_reason.load() == 0 && game_failure_crossing.load() == -1 && !pit_call_ended.load());
        reset_session();
    }

    // ---- 0.51.127: no limp once the wheel is gone; a wheel that comes off while limping stops the car ----
    {
        static uint32_t corners = 0;
        alignas(16) static uint8_t limp_car[0x600]{}, limp_owner[0x40]{};
        put(limp_car, 0x5d0, reinterpret_cast<uintptr_t>(limp_owner));
        put(limp_owner, 8, reinterpret_cast<uintptr_t>(limp_car));
        auto lc = reinterpret_cast<uintptr_t>(limp_car);
        wheel_state_get = +[](void *) -> uint32_t { return corners; };
        api.version = 4;
        api.setting = fake_setting;
        api.watch_setting = fake_watch;
        fake_limp_roll = "success";
        // The forced success roll: a broken suspension (FL state 2) and a front wheel coming loose (FR state 3) limp, a
        // wheel the game tears off itself (FR state 4: its terminal crash damage stops the engine, 0.51.133) or one
        // that is gone (FR or FL state 5) does not. 0.51.132: a rear corner with its suspension collapsed (state 2, RL
        // or RR) limps too; a rear wheel coming loose, torn off or gone does not.
        // A wheel the game only damaged (RL state 2, a flat tyre) is no broken corner next to another: the FR alone limps.
        // Two wheels the game damaged (RR and FL state 2) count as two broken corners: no limp.
        const uint32_t cases[][3] = {{2u << 6, 1, 2}, {4u << 9, 0, 0}, {3u << 9, 1, 3}, {5u << 9, 0, 0}, {5u << 6, 0, 0},
                                     {4u << 6, 0, 0}, {2u, 1, 0}, {2u << 3, 1, 1}, {4u << 3, 0, 0}, {3u, 0, 0},
                                     {5u << 3, 0, 0}, {2u | (3u << 9), 1, 3}, {2u | (4u << 9), 0, 0},
                                     {(2u << 3) | (2u << 6), 0, 0}};
        for (auto &c : cases) {
            reset_session();
            corners = c[0];
            decide_limp(lc);
            assert(limp_state.load() == int(c[1]));
            assert(limp_corner.load() == (c[1] ? int(c[2]) : -1));
        }
        // The chances: collapsed 85 % front, 60 % rear; wheel coming loose 35 % front, none at the rear; torn off by
        // the game (its engine stops) or gone none.
        assert(limp_chance(2, 2) == 85 && limp_chance(2, 3) == 85 && limp_chance(2, 0) == 60 && limp_chance(2, 1) == 60);
        assert(limp_chance(3, 2) == 35 && limp_chance(3, 3) == 35 && limp_chance(3, 0) == 0 && limp_chance(3, 1) == 0);
        assert(limp_chance(4, 2) == 0 && limp_chance(4, 3) == 0 && limp_chance(4, 0) == 0);
        assert(limp_chance(5, 2) == 0 && limp_chance(5, 0) == 0);
        // Limping on a collapsed rear corner: the wheel there comes off (state 4), the limp ends.
        reset_session();
        susp::model.c[1].collapsed = true;
        corners = 2u << 3;
        decide_limp(lc);
        assert(limp_state.load() == 1 && limp_corner.load() == 1);
        corners = 4u << 3;
        for (int i = 0; i < 30 && limp_state.load() == 1; ++i) {
            decide_limp(lc);
            if (limp_state.load() == 1)
                Sleep(50);
        }
        assert(limp_state.load() == 0 && limp_corner.load() == -1 && crash_corners.load() == (4u << 3));
        susp::model.c[1].collapsed = false;
        // Limping on a rear wheel the game damaged (RL state 2 alone): the model then collapses the FL, a second
        // broken corner, the limp ends (the RL no longer counts next to it, the limping corner still does).
        reset_session();
        corners = 2u;
        decide_limp(lc);
        assert(limp_state.load() == 1 && limp_corner.load() == 0);
        susp::model.c[2].collapsed = true;
        corners = 2u | (2u << 6);
        for (int i = 0; i < 30 && limp_state.load() == 1; ++i) {
            decide_limp(lc);
            if (limp_state.load() == 1)
                Sleep(50);
        }
        assert(limp_state.load() == 0);
        susp::model.c[2].collapsed = false;
        // Limping on a loose FR wheel: the wheel comes off, the limp ends (re-checked every 500 ms).
        reset_session();
        corners = 3u << 9;
        decide_limp(lc);
        assert(limp_state.load() == 1);
        corners = 5u << 9;
        for (int i = 0; i < 30 && limp_state.load() == 1; ++i) {
            decide_limp(lc);
            if (limp_state.load() == 1)
                Sleep(50);
        }
        assert(limp_state.load() == 0 && crash_corners.load() == (5u << 9));
        // The decision holds the minor suspension messages back, so a bent corner cannot hide it.
        assert(susp::minor_toast_until.load() > GetTickCount64());
        // 0.51.128 test 20:41:10: the game broke the FR (state 4 then, a loose FR state 3 here: a torn-off wheel no
        // longer limps) and only damaged the RL wheel (state 2). The car limps on the FR; the damaged RL does not stop
        // it, the addon's model collapsing the RL does.
        reset_session();
        corners = 2u | (3u << 9);
        decide_limp(lc);
        assert(limp_state.load() == 1);
        for (int i = 0; i < 12; ++i) {
            decide_limp(lc);
            Sleep(50);
        }
        assert(limp_state.load() == 1);
        susp::model.c[0].collapsed = true;
        for (int i = 0; i < 30 && limp_state.load() == 1; ++i) {
            decide_limp(lc);
            if (limp_state.load() == 1)
                Sleep(50);
        }
        assert(limp_state.load() == 0);
        // The RL collapsed by the model before the decision: two broken corners, no limp.
        reset_session();
        susp::model.c[0].collapsed = true;
        decide_limp(lc);
        assert(limp_state.load() == 0);
        susp::model.c[0].collapsed = false;
        // 0.51.133: the model collapses a corner the game had already damaged (FL state 2, a flat tyre; FR state 3, a
        // wheel coming loose). The state stays, the car counts as broken all the same and the limp roll runs.
        static uint32_t applied_states = 0;
        static int applies = 0;
        wheel_state_apply = +[](void *, uint32_t s) {
            applied_states = corners = s;
            ++applies;
        };
        for (unsigned k = 0; k < 3; ++k) {
            reset_session();
            int at = k == 2 ? 0 : 2 + int(k);
            corners = k == 0 ? 2u << 6 : k == 1 ? 3u << 9 : 0u;
            applies = 0;
            susp::model.c[at].collapsed = true;
            susp::collapse_request = 1u << at;
            susp::game_thread(lc);
            assert(susp::collapse_request.load() == 0);
            assert((terminal_mask.load() & 0x10) && player_broken.load());
            assert(limp_state.load() == 1 && limp_corner.load() == at);
            // Only an intact corner gets the state 2 written (the RL here); the damaged ones keep theirs.
            assert(applies == (k == 2 ? 1 : 0) && (k != 2 || applied_states == 2u));
            susp::model.c[at].collapsed = false;
        }
        wheel_state_apply = nullptr;
        fake_limp_roll = nullptr;
        api.setting = nullptr;
        api.watch_setting = nullptr;
        api.version = 0;
        wheel_state_get = nullptr;
        reset_session();
    }

    // ---- 0.51.133: the game's wheel detach limits x2 while the suspension model runs; 0.75.1: and the game's
    // front and rear damage scales back to stock (another addon's raised scales would add up with the model) ----
    {
        struct Made {
            uint32_t index, field, op;
            float value;
            int32_t priority;
        };
        static std::vector<Made> made;
        static std::vector<uint32_t> removed;
        static uint32_t next_token = 0, refuse_field = ~0u;
        static SwoftyCarOps ops{};
        ops.version = 2;
        ops.available = +[]() { return 1; };
        ops.field = +[](const char *, uint32_t *) { return int(SWOFTY_ASSET_INVALID); };
        ops.layer = +[](const char *, uint32_t index, uint32_t field, uint32_t op, float value, int32_t priority,
                        uint32_t *token) {
            made.push_back({index, field, op, value, priority});
            if (field == refuse_field)
                return int(SWOFTY_ASSET_INVALID);
            *token = ++next_token;
            return int(SWOFTY_ASSET_OK);
        };
        ops.unlayer = +[](uint32_t token) {
            removed.push_back(token);
            return int(SWOFTY_ASSET_OK);
        };
        auto layers_on = [](size_t from, uint32_t index) {
            assert(made.size() == from + 4);
            assert(made[from].index == index && made[from].field == SWOFTY_CAR_SUSP_WHEEL_DETACH_SCALE &&
                   made[from].op == SWOFTY_CAR_OP_SCALE && made[from].value == 2.0f && made[from].priority == 50);
            assert(made[from + 1].index == index && made[from + 1].field == SWOFTY_CAR_SUSP_PARTS_DETACH_SCALE &&
                   made[from + 1].op == SWOFTY_CAR_OP_SCALE && made[from + 1].value == 2.0f);
            assert(made[from + 2].index == index && made[from + 2].field == SWOFTY_CAR_SUSP_FRONT_DAMAGE_SCALE &&
                   made[from + 2].op == SWOFTY_CAR_OP_SET && made[from + 2].value == 0.001f &&
                   made[from + 2].priority == 50);
            assert(made[from + 3].index == index && made[from + 3].field == SWOFTY_CAR_SUSP_REAR_DAMAGE_SCALE &&
                   made[from + 3].op == SWOFTY_CAR_OP_SET && made[from + 3].value == 0.0001f);
        };
        swofty::detail::set_car_ops_table(&ops);
        bool was = susp::enabled, was_ai = susp::ai::enabled;
        int was_player = player_vehicle.load();
        susp::enabled = false;  // the model off: the game's (or another addon's) values stay
        player_vehicle = 3;
        apply_game_suspension_layers();
        assert(made.empty());
        susp::enabled = true;
        susp::ai::enabled = false;
        player_vehicle = -1;
        apply_game_suspension_layers();
        assert(made.empty());  // no player car yet
        player_vehicle = 3;
        apply_game_suspension_layers();
        layers_on(0, 3);
        assert(removed.empty());
        apply_game_suspension_layers();
        assert(made.size() == 4);
        player_vehicle = -1;  // between sessions: the layers stay
        apply_game_suspension_layers();
        assert(made.size() == 4 && removed.empty());
        player_vehicle = 5;  // a restart in another car: the layers move with it
        apply_game_suspension_layers();
        assert(removed.size() == 4);
        layers_on(4, 5);
        susp::ai::enabled = true;  // the AI model runs too: every car, the same layers
        apply_game_suspension_layers();
        assert(removed.size() == 8);
        layers_on(8, SWOFTY_CAR_ALL);
        player_vehicle = 7;
        apply_game_suspension_layers();
        assert(made.size() == 12 && removed.size() == 8);
        // A core that refuses one field: that value stays the game's, the other three still apply, and the refused
        // field is not asked for again.
        refuse_field = SWOFTY_CAR_SUSP_FRONT_DAMAGE_SCALE;
        susp::ai::enabled = false;
        apply_game_suspension_layers();
        assert(removed.size() == 12);
        layers_on(12, 7);
        refuse_field = ~0u;
        player_vehicle = 9;
        apply_game_suspension_layers();
        assert(removed.size() == 15 && made.size() == 19);
        for (size_t i = 16; i < made.size(); ++i)
            assert(made[i].index == 9 && made[i].field != SWOFTY_CAR_SUSP_FRONT_DAMAGE_SCALE);
        swofty::detail::set_car_ops_table(nullptr);
        susp::enabled = was;
        susp::ai::enabled = was_ai;
        player_vehicle = was_player;
    }

    // ---- 0.51.134: the corner repair counts down (the game's job keeps its length at +4, 0.51.133 test) ----
    {
        alignas(16) static uint8_t manager[0x60]{}, record[0x58]{}, jobs[16]{};
        put(manager, 0x48, reinterpret_cast<uintptr_t>(record));
        put(manager, 0x50, reinterpret_cast<uintptr_t>(record) + 0x58);
        record[0x10] = 1;
        record[0x16] = 7;
        put(record, 0x20, reinterpret_cast<uintptr_t>(jobs));
        put(record, 0x28, reinterpret_cast<uintptr_t>(jobs) + 8);
        put(jobs, 0, uint32_t(16));
        put(jobs, 4, 60.0f);
        int was_player = player_vehicle.load();
        player_vehicle = 7;
        session_ended = false;
        orig_add_repair = +[](void *, uint32_t) -> uint64_t { return 0; };
        orig_repair_update = +[](void *, float) {};
        garage_repair = 2;
        garage_repair_left = 60.0f;
        for (int i = 0; i < 100; ++i)
            on_repair_update(manager, 0.25f);
        assert(std::fabs(garage_repair_left.load() - 35.0f) < 0.01f);
        // A bad step (a pause, a hitch) does not jump the countdown.
        on_repair_update(manager, std::nanf(""));
        on_repair_update(manager, 5.0f);
        on_repair_update(manager, -1.0f);
        assert(std::fabs(garage_repair_left.load() - 35.0f) < 0.01f);
        float length = 0;
        memcpy(&length, jobs + 4, 4);
        assert(length == 60.0f);  // the game's job is left alone
        for (int i = 0; i < 400; ++i)
            on_repair_update(manager, 0.25f);
        assert(garage_repair_left.load() == 0.0f);
        garage_repair = 0;
        garage_repair_left = 0.0f;
        orig_add_repair = nullptr;
        orig_repair_update = nullptr;
        player_vehicle = was_player;
    }

    // ---- 0.51.135: a damaged corner that holds is repaired in the garage, and that takes time ----
    {
        float half[4] = {0.5f, 0.5f, 0.5f, 0.5f}, zero[4] = {};
        susp::Model m;
        susp::ServicePlan sp = susp::service_plan(m, half);
        assert(!sp.corners && sp.seconds == 0.0f && !sp.crack);
        m.c[0].health = 0.95f;  // kerb wear alone: nothing to repair
        assert(!susp::service_plan(m, half).corners);
        m.c[2].bend = 0.5f * susp::deg;  // FL, a light bend: 4-8 minutes
        m.c[2].health = 0.9f;
        sp = susp::service_plan(m, half);
        assert(sp.corners == 1u << 2 && std::fabs(sp.seconds - 360.0f) < 0.5f && !sp.crack);
        m.c[3].health = 0.3f;  // FR, close to breaking: 14-20 minutes
        m.c[3].bend = -1.0f * susp::deg;
        sp = susp::service_plan(m, zero);
        assert(sp.corners == 0xcu && std::fabs(sp.seconds - (14.0f + 2.0f) * 60.0f) < 0.5f);  // longest + half
        m.c[1].health = 0.85f;  // RR with a crack: 8-14 minutes
        m.c[1].bend = 0.2f * susp::deg;
        m.c[1].crack_at = 5000;
        sp = susp::service_plan(m, zero);
        assert(sp.corners == 0xeu && sp.crack && std::fabs(sp.seconds - (14.0f + 2.0f + 4.0f) * 60.0f) < 0.5f);
        m.c[1].collapsed = true;  // a broken corner is the crash repair's
        sp = susp::service_plan(m, zero);
        assert(sp.corners == 0xcu && !sp.crack);
        assert(susp::service_minutes(m.c[2], 1.0f) == 8.0f && susp::service_minutes(m.c[3], 1.0f) == 20.0f);
        susp::Corner weak;
        weak.health = 0.7f;
        assert(susp::service_minutes(weak, 0.0f) == 8.0f);

        char where[64];
        service_corners_text(1u << 2, where, sizeof where);
        assert(!strcmp(where, "front-left suspension"));
        service_corners_text((1u << 1) | (1u << 3), where, sizeof where);
        assert(!strcmp(where, "front-right and rear-right suspension"));
        service_corners_text(0x7u, where, sizeof where);
        assert(!strcmp(where, "suspension on three corners"));
        service_corners_text(0xfu, where, sizeof where);
        assert(!strcmp(where, "suspension on all four corners"));

        // The car drives into its practice garage with a bent, weakened front-left corner.
        alignas(16) static uint8_t lane[0x310]{}, manager[0x60]{}, record[0x58]{}, jobs[16]{};
        lane[0x300] = 1;
        put(manager, 0x48, reinterpret_cast<uintptr_t>(record));
        put(manager, 0x50, reinterpret_cast<uintptr_t>(record) + 0x58);
        record[0x16] = 7;
        put(record, 0x20, reinterpret_cast<uintptr_t>(jobs));
        put(record, 0x28, reinterpret_cast<uintptr_t>(jobs) + 8);
        put(jobs, 0, uint32_t(16));
        put(jobs, 4, 30.0f);
        bool was_enabled = susp::enabled, was_hooked = repair_update_hooked;
        int was_player = player_vehicle.load();
        reset_session();
        susp::enabled = true;
        susp::tick("off", susp::flashback_seen);  // takes the new session's reset
        pit_lane_system = reinterpret_cast<uintptr_t>(lane);
        repair_update_hooked = true;
        orig_add_repair = +[](void *, uint32_t) -> uint64_t { return 0; };
        orig_repair_update = +[](void *, float) {};
        orig_repair_done = +[](void *, uint32_t) {};
        session_ended = false;
        player_vehicle = 7;
        susp::model.c[2].bend = 0.6f * susp::deg;
        susp::model.c[2].health = 0.7f;
        player_pit = 9;
        susp::tick("off", susp::flashback_seen);
        assert(garage_service_corners.load() == 1u << 2 && garage_repair.load() == 1 && car_dead.load());
        float planned = garage_service_seconds.load();
        assert(planned >= 8.0f * 60.0f && planned <= 14.0f * 60.0f && !garage_service_crack.load());
        assert(susp::model.c[2].bend != 0.0f);  // the damage stays until the job is done
        assert(garage_exit_locked() && garage_lock_reason() == 4);
        assert(garage_repair_time(0) == planned);
        assert(!start_garage_service(600.0f, 1u, false));  // one at a time
        // A drive out attempt: 300 ms out, back in. Nothing new starts, nothing is repaired.
        player_pit = 1;
        susp::tick("off", susp::flashback_seen);
        player_pit = 9;
        susp::tick("off", susp::flashback_seen);
        assert(susp::model.c[2].bend != 0.0f && garage_service_corners.load() == 1u << 2);
        // The game's repair job carries it: the addon's time, the countdown, then the job ends.
        on_repair_update(manager, 0.016f);
        float length = 0;
        memcpy(&length, jobs + 4, 4);
        assert(garage_repair.load() == 2 && length == planned && garage_repair_left.load() == planned);
        assert(record[0x10] == 1);
        on_repair_update(manager, 0.5f);
        assert(std::fabs(garage_repair_left.load() - (planned - 0.5f)) < 0.01f);
        on_repair_done(record, 16);
        assert(garage_repair.load() == 3 && garage_exit_locked());
        garage_repair_finish(0x1000);
        assert(!car_dead.load() && garage_repair.load() == 0 && !garage_service_corners.load() && !garage_exit_locked());
        susp::tick("off", susp::flashback_seen);  // the model forgets the damage
        assert(susp::model.c[2].bend == 0.0f && susp::model.c[2].health == 1.0f);
        // Not serviced (repaired at once, as before): a car that limped in on a broken corner, time trial, or no
        // repair hooks.
        player_broken = true;
        assert(!start_garage_service(600.0f, 1u, false));
        player_broken = false;
        static uint32_t kind = 18;
        repair_session_kind = reinterpret_cast<uintptr_t>(&kind);
        assert(!start_garage_service(600.0f, 1u, false));
        kind = 3;  // practice 3
        assert(start_garage_service(600.0f, 1u, true) && garage_service_crack.load());
        reset_car_state();  // the session ends during the service
        assert(!garage_service_corners.load() && !car_dead.load() && garage_repair.load() == 0);
        repair_session_kind = 0;
        repair_update_hooked = false;
        player_pit = 1;
        susp::tick("off", susp::flashback_seen);
        susp::model.c[0].bend = 1.0f * susp::deg;
        player_pit = 9;
        susp::tick("off", susp::flashback_seen);
        assert(!garage_service_corners.load() && susp::model.c[0].bend == 0.0f);
        pit_lane_system = 0;
        orig_add_repair = nullptr;
        orig_repair_update = nullptr;
        orig_repair_done = nullptr;
        repair_update_hooked = was_hooked;
        susp::enabled = was_enabled;
        player_vehicle = was_player;
        player_pit = 1;
        reset_session();
    }

    // ---- 0.51.128: the message for an undrivable car names the broken corner by the game's state ----
    {
        char text[120];
        const char *title = undrivable_message(4u << 9, 0, text, sizeof text);
        assert(title && !strcmp(title, "SUSPENSION FAILURE") &&
               !strcmp(text, "Front-right wheel coming off. The car cannot be driven."));
        // State 2: the addon's model collapsed the corner, or the game only damaged the wheel (a flat tyre), which
        // counts only when no other corner broke.
        title = undrivable_message(2u << 3, 1u << 1, text, sizeof text);
        assert(title && !strcmp(title, "SUSPENSION FAILURE") &&
               !strcmp(text, "Rear-right suspension collapsed. The car cannot be driven."));
        title = undrivable_message(2u << 3, 0, text, sizeof text);
        assert(title && !strcmp(title, "WHEEL DAMAGE") &&
               !strcmp(text, "Rear-right wheel damaged. The car cannot be driven."));
        title = undrivable_message(5u << 9, 0, text, sizeof text);
        assert(title && !strcmp(title, "WHEEL LOST") && !strcmp(text, "Front-right wheel lost. The car cannot be driven."));
        title = undrivable_message((2u << 6) | (3u << 9), 1u << 2, text, sizeof text);
        assert(title && !strcmp(title, "SUSPENSION FAILURE") &&
               !strcmp(text, "Front-right wheel coming off, front-left suspension collapsed. The car cannot be driven."));
        // 0.51.128 test 20:41:10: the game broke the FR wheel and only damaged the RL wheel: the FR alone is named.
        title = undrivable_message(2u | (4u << 9), 0, text, sizeof text);
        assert(!strcmp(title, "SUSPENSION FAILURE") && !strcmp(text, "Front-right wheel coming off. The car cannot be driven."));
        title = undrivable_message((5u << 3) | (2u << 9), 0, text, sizeof text);
        assert(!strcmp(title, "WHEEL LOST") && !strcmp(text, "Rear-right wheel lost. The car cannot be driven."));
        title = undrivable_message(2u | (2u << 3) | (4u << 9), 3u, text, sizeof text);
        assert(!strcmp(text, "3 corners broken. The car cannot be driven."));
        title = undrivable_message(2u | (2u << 3) | (4u << 9), 0, text, sizeof text);
        assert(!strcmp(text, "Front-right wheel coming off. The car cannot be driven."));
        title = undrivable_message(2u | (2u << 6), 0, text, sizeof text);
        assert(!strcmp(title, "WHEEL DAMAGE") &&
               !strcmp(text, "Front-left wheel damaged, rear-left wheel damaged. The car cannot be driven."));
        assert(!undrivable_message(1u | (1u << 3), 0, text, sizeof text));  // punctures only
        assert(broken_corners(2u | (4u << 9), 0) == 8u && broken_corners(2u | (4u << 9), 1u) == 9u &&
               broken_corners(2u << 3, 0) == 2u && broken_corners(1u | (1u << 3), 0) == 0u);
    }

    // ---- 0.51.127: a locked car's drive out request goes on as the instant one, the car comes straight back ----
    {
        static uint32_t seen_type = 99;
        static int calls = 0, sent_home = 0;
        orig_exit_garage = +[](void *, void *msg) -> uint64_t {
            ++calls;
            memcpy(&seen_type, static_cast<uint8_t *>(msg) + 0x14, sizeof seen_type);
            return 7;
        };
        alignas(8) uint8_t msg[0x20]{};
        reset_session();
        session_ended = false;
        player_vehicle = 3;
        msg[0x10] = 3;
        // Not locked: untouched.
        assert(on_exit_garage(nullptr, msg) == 7 && calls == 1 && seen_type == 0);
        // Withdrawn: the crew's drive out (type 0) becomes the instant one.
        car_dead = true;
        garage_withdrawn = true;
        assert(garage_exit_locked() && garage_lock_reason() == 0);
        assert(on_exit_garage(nullptr, msg) == 7 && calls == 2 && seen_type == 1);
        // Type 1 (practice programme) stays; another car's request is not touched.
        assert(on_exit_garage(nullptr, msg) == 7 && calls == 3 && seen_type == 1);
        put(msg, 0x14, uint32_t(0));
        msg[0x10] = 4;
        assert(on_exit_garage(nullptr, msg) == 7 && calls == 4 && seen_type == 0);
        // The reason the toast names.
        garage_withdrawn = false;
        assert(garage_lock_reason() == 3);
        garage_repair = 1;
        assert(garage_lock_reason() == 2);
        part_lock = true;
        assert(garage_lock_reason() == 1);
        // 300 ms after the car left: back to its garage, still locked.
        static uint8_t lane[0x310]{};
        pit_lane_system = reinterpret_cast<uintptr_t>(lane);
        orig_set_vehicle_status = +[](void *, uint64_t, uint64_t status) -> uint64_t {
            sent_home += status == 9;
            return 0;
        };
        player_pit = 1;
        exit_check_at = 1;
        return_to_garage();
        assert(sent_home == 1 && car_dead && !exit_check_at.load() && !garage_return_wanted.load());
        orig_set_vehicle_status = nullptr;
        orig_exit_garage = nullptr;
        pit_lane_system = 0;
        reset_session();
    }

    // ---- 0.75.10: the pit status hook is the core's wrap, so another addon's wrap cannot hide it ----
    // A friend's log (2026-11-06 20:34): the Spectator HUD had wrapped Game_Pitlane_SetVehicleStatus before this addon
    // loaded, the byte signature found 0 hits, pit_lane_system stayed 0 and practice_like() was false in every
    // practice and qualifying session: a bent suspension was never serviced in the garage.
    {
        alignas(16) static uint8_t lane[0x310]{}, entry[16]{};
        static unsigned forwarded, last_vehicle, last_status;
        static void *last_system;
        forwarded = last_vehicle = last_status = 0;
        last_system = nullptr;
        pit_status_next = +[](void *system, uint8_t vehicle, uint32_t status) -> uint64_t {
            ++forwarded;
            last_system = system;
            last_vehicle = vehicle;
            last_status = status;
            return 0x77;
        };
        // The registration the core reads: one wrap on the binding, normal priority, our handler and our orig slot.
        unsigned registered = 0;
        for (auto *n = swofty::detail::mixins; n; n = n->next)
            if (n->m.target && !strcmp(n->m.target, "Game_Pitlane_SetVehicleStatus")) {  // an on_load entry has no target
                ++registered;
                assert(n->m.kind == SWOFTY_MIXIN_WRAP && n->m.priority == 0);
                assert(n->m.handler == reinterpret_cast<void *>(&pit_status_wrap));
                assert(n->m.orig == reinterpret_cast<void **>(&pit_status_next));
            }
        assert(registered == 1);
        orig_set_vehicle_status = pit_status_chain;
        pit_lane_system = 0;
        lane[0x300] = 1;  // practice and qualifying
        assert(!practice_like());
        captured_lines.clear();
        api.log = capture_log;
        uint32_t calls = pit_status_calls.load();
        assert(pit_status_wrap(lane, 7, 9) == 0x77);  // the first call of the session hands the system over
        assert(pit_lane_system.load() == reinterpret_cast<uintptr_t>(lane) && practice_like());
        assert(pit_status_calls.load() == calls + 1);
        assert(forwarded == 1 && last_system == lane && last_vehicle == 7 && last_status == 9);
        assert(captured_count("pit lane system ") == 1 && captured_count("practice/qualifying flag 1") == 1);
        pit_status_wrap(lane, 7, 9);  // only the first call says it
        assert(captured_count("pit lane system ") == 1 && forwarded == 2);
        forwarded = 1;
        // The session ends: the system is forgotten and the next session's first change finds it again.
        pit_lane_system = 0;
        assert(!practice_like());
        pit_status_wrap(lane, 3, 1);
        assert(practice_like() && forwarded == 2 && last_vehicle == 3 && last_status == 1);
        // A broken car in its practice or qualifying garage stays there: every change but OnTrack is refused, and
        // OnTrack is let through to be undone 300 ms later.
        human[7] = true;
        car_dead = true;
        put(entry, 4, uint32_t(9));
        put(lane, 0x68 + 7 * 16, reinterpret_cast<uintptr_t>(entry));
        assert(pit_status_wrap(lane, 7, 3) == 0);  // refused: the game is not called, the caller gets 0
        assert(forwarded == 2);
        exit_check_at = 0;
        assert(pit_status_wrap(lane, 7, 1) == 0x77);
        assert(forwarded == 3 && last_vehicle == 7 && last_status == 1 && exit_check_at.load() != 0);
        pit_status_wrap(lane, 4, 3);  // another car is not touched
        assert(forwarded == 4 && last_vehicle == 4 && last_status == 3);
        // The old signature in, the next function out: no next function (the wrap is not installed) forwards nothing.
        pit_status_next = nullptr;
        assert(pit_status_chain(lane, 7, 9) == 0 && forwarded == 4);
        // The watch: 30 s into a session with the player known and no pit lane system.
        pit_lane_system = 0;
        session_ended = false;
        player_vehicle = 7;
        captured_lines.clear();
        pit_lane_watch_tick(1000);
        pit_lane_watch_tick(1000 + 29000);
        assert(captured_count("still unknown") == 0);
        pit_lane_watch_tick(1000 + 31000);
        assert(captured_count("still unknown") == 1 && captured_count("hook not installed") == 1);
        pit_lane_watch_tick(1000 + 40000);  // once per session
        assert(captured_count("still unknown") == 1);
        session_ended = true;
        pit_lane_watch_tick(1000 + 50000);  // the session is over: the watch starts again
        session_ended = false;
        pit_lane_watch_tick(1000 + 60000);
        pit_lane_watch_tick(1000 + 95000);
        assert(captured_count("still unknown") == 2);
        pit_lane_system = reinterpret_cast<uintptr_t>(lane);  // the hook fired: nothing more to say
        pit_lane_watch_tick(1000 + 200000);
        assert(captured_count("still unknown") == 2);
        api.log = tu::quiet_log;
        human[7] = false;
        car_dead = false;
        exit_check_at = 0;
        pit_status_next = nullptr;
        orig_set_vehicle_status = nullptr;
        pit_lane_system = 0;
        reset_session();
    }

    std::puts("road to 1.0 ok");
}
