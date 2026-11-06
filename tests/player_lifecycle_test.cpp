// Standalone Windows test. No game hooks, sockets or patch installation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include "test_util.hpp"

namespace {
unsigned updates;
float *observed_failure;
bool expect_failed;
uint64_t update(void *, uint8_t vehicle)
{
    ++updates;
    if (vehicle == 3)
        assert((*observed_failure >= 1.0f) == expect_failed);
    return 42;
}
using tu::put;
}

int main()
{
    SwoftyApi api{};
    api.log = tu::quiet_log;
    swofty::api = &api;
    orig_update_vehicle = update;
    std::vector<uint8_t> system(0xd00), record(0x70), car(0x600), engine(0x2600);
    put(system, 0xc20, uint64_t(4));
    put(system, 0x50 + 3 * 0x70 + 8, reinterpret_cast<uintptr_t>(record.data()));
    put(record, 0x58, reinterpret_cast<uintptr_t>(car.data()));
    put(car, 0x530, reinterpret_cast<uintptr_t>(engine.data()));
    observed_failure = reinterpret_cast<float *>(engine.data() + part_offsets[0]);
    reset_session();
    player_vehicle = 3;

    // The hotkey must never touch game memory. Mutation waits for this player's
    // update and must precede the original damage check; AI updates do not consume it.
    trigger_failure();
    assert(pending_failure && *observed_failure == 0 && !player_broken);
    assert(on_update_vehicle(system.data(), 1) == 42);
    assert(pending_failure && *observed_failure == 0 && !player_broken);
    expect_failed = true;
    assert(on_update_vehicle(system.data(), 3) == 42);
    assert(!pending_failure && *observed_failure == 1 && player_broken);
    assert(throttle_hook.cap == 0 && updates == 2);

    *observed_failure = 0;
    expect_failed = false;
    player_broken = false;
    trigger_failure();
    player_vehicle = 2;
    apply_pending_failure(system.data(), 3);
    assert(!pending_failure && *observed_failure == 0 && !player_broken);
    player_vehicle = 3;
    pending_failure = ((GetTickCount64() - 1) << 16) | (3u << 8) | 1;
    apply_pending_failure(system.data(), 3);
    assert(!pending_failure && *observed_failure == 0);

    // End/start must discard requests and cap overrides. Ended-session callbacks cannot execute work.
    pit_slot_of[3] = 5;
    trigger_failure();
    garage_return_wanted = true;
    test_cap = 0;
    retire_in = 0;
    pit_lane_system = 1;
    controller_system = reinterpret_cast<void *>(1);
    // The race's lap count: Swofty's telemetry (none here), the session context, then the game's race settings.
    {
        const char *src = nullptr;
        session_laps = 0;
        game_race_laps = -1;
        assert(race_total_laps(&src) == 0);
        game_race_laps = 5;
        assert(race_total_laps(&src) == 5 && !strcmp(src, "the game's race settings"));
        session_laps = 12;
        assert(race_total_laps(&src) == 12 && !strcmp(src, "session context"));
        session_laps = 0;
        game_race_laps = 500;
        assert(race_total_laps() == 0);
        game_race_laps = 5;
        game_race_length = 3;
    }
    SwoftyEvent ev{};
    ev.name = SWOFTY_EVENT_SESSION_END;
    on_event(&ev, nullptr);
    assert(session_ended && !pending_failure && !garage_return_wanted);
    assert(game_race_laps == -1 && game_race_length == -1);  // the next session reads its own
    assert(test_cap == -1 && throttle_hook.car == 0 && throttle_hook.cap == 1);
    assert(!pit_lane_system && !controller_system && retire_in == -1);
    assert(pit_slot_of[3] == -1);
    udp_player = 3;
    udp_speed[3] = 0;
    udp_last_ms = GetTickCount64();
    assert(player_speed() == -1);
    trigger_failure();
    on_frame(10, nullptr);
    assert(!pending_failure && !garage_return_wanted);
    assert(on_update_vehicle(system.data(), 3) == 42 && *observed_failure == 0);
    // A packet cached before the session start must not count as fresh after it: the start stamps
    // telemetry_not_before. An old stamp first, so that only the start's own stamp can make the packet stale.
    telemetry_not_before = 1;
    udp_last_ms = GetTickCount64();
    ev.name = SWOFTY_EVENT_SESSION_START;
    on_event(&ev, nullptr);
    assert(!session_ended && player_vehicle == -1);
    assert(player_speed() == -1); // A cached packet from before reset is not fresh.
    player_vehicle = 3;
    trigger_failure();
    expect_failed = true;
    on_update_vehicle(system.data(), 3);
    assert(*observed_failure == 1 && !pending_failure);

    // DNF controller/session cleanup clears gameplay requests but must not
    // restart the retired car's engine audio. Fresh session preparation,
    // session.start or, once the race is over (NOT_IN_SESSION), a
    // SESSION_NOT_STARTED (the replay from the results screen) releases it.
    reset_session();
    player_vehicle = 3;
    *observed_failure = 0;
    player_broken = true;
    terminal_mask = 8;
    latch_retirement_drive(3);
    retired_engine_off_vehicle = 3;
    engine_off_vehicle = 3;
    orig_set_controller = [](void *, uint64_t, uint32_t, int32_t) -> uint64_t { return 77; };
    assert(on_set_controller(nullptr, 3, 1, 3) == 77);
    assert(!player_broken && !car_dead && !terminal_mask && retire_in == -1);
    assert(retired_engine_off_vehicle == 3 && engine_off_vehicle == 3);
    power_loss(reinterpret_cast<uintptr_t>(car.data()));
    assert(throttle_hook.cap == 0 && engine_off_vehicle == 3 && retired_drive_lock_vehicle==3);
    assert(on_set_controller(nullptr, 3, 1, 3) == 77);
    assert(engine_off_vehicle == 3);
    // 0.51.125: the replay after the race resets every car to SESSION_NOT_STARTED; your car's engine sound comes
    // back (a test report: the replays after a retirement had your car muted).
    assert(on_set_controller(nullptr, 3, 1, 10) == 77);
    assert(retired_engine_off_vehicle == -1 && engine_off_vehicle == -1 && engine_kill_vehicle == -1);
    retired_engine_off_vehicle = 3;
    engine_off_vehicle = 3;
    reset_for_session_restart(nullptr);
    assert(retired_engine_off_vehicle == -1 && engine_off_vehicle == -1 && retired_drive_lock_vehicle==-1 && throttle_hook.cap==1);
    retired_engine_off_vehicle = 3;
    engine_off_vehicle = 3;
    ev.name = SWOFTY_EVENT_SESSION_END;
    on_event(&ev, nullptr);
    assert(session_ended && engine_off_vehicle == 3 && retired_engine_off_vehicle == 3);
    ev.name = SWOFTY_EVENT_SESSION_START;
    on_event(&ev, nullptr);
    assert(!session_ended && engine_off_vehicle == -1 && retired_engine_off_vehicle == -1);
    // Drive retention is independent of audio and cannot be overridden by a
    // diagnostic cap. Other vehicle IDs are not restricted by this latch.
    player_vehicle=3;latch_retirement_drive(3);test_cap=1;
    retired_engine_off_vehicle=-1;engine_off_standing=false;
    reset_car_state(true);power_loss(reinterpret_cast<uintptr_t>(car.data()));
    assert(retired_drive_lock_vehicle==3 && throttle_hook.cap==0 && engine_off_vehicle==-1);
    player_vehicle=2;power_loss(reinterpret_cast<uintptr_t>(car.data()));assert(throttle_hook.cap==1);
    player_vehicle=3;reset_for_session_restart(nullptr);assert(retired_drive_lock_vehicle==-1 && throttle_hook.cap==1);

    // 0.75.4: the failure clocks run while the game updates your car, also when nothing reads its speed (suspension
    // damage off, the car healthy). 0.75.3 counted only fresh speed reads, so no realistic failure ever came.
    *observed_failure = 0;
    expect_failed = false;
    player_broken = false;
    telemetry_not_before = 0;
    game_speed_ms = 0;
    udp_last_ms = 0;  // the game's UDP telemetry off (the default)
    player_update_ms = 0;
    assert(on_update_vehicle(system.data(), 1) == 42);  // an AI car's update does not count for yours
    assert(drive_seconds(0.016) == 0.0);
    assert(on_update_vehicle(system.data(), 3) == 42);
    assert(game_speed_ms.load() == 0 && drive_seconds(0.016) == 0.016);
    puts("PASS: queued player failure, ordering, AI isolation, expiry and session cleanup");
    puts("PASS: DNF audio survives gameplay/session cleanup; restart/start releases shutdown");
}
