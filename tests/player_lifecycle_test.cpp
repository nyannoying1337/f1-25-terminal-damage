// Standalone Windows test. No game hooks, sockets or patch installation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
void discard_log(const char *, ...) {}
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
template <class T> void put(std::vector<uint8_t> &data, size_t offset, T value)
{
    assert(offset + sizeof value <= data.size());
    memcpy(data.data() + offset, &value, sizeof value);
}
}

int main()
{
    SwoftyApi api{};
    api.log = discard_log;
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

    // End/start must discard requests and cap overrides, preserving the garage
    // mapping learned in practice. Ended-session callbacks cannot execute work.
    garage_of[3] = 7;
    trigger_failure();
    garage_return_wanted = true;
    test_cap = 0;
    retire_in = 0;
    pit_lane_system = 1;
    controller_system = reinterpret_cast<void *>(1);
    SwoftyEvent ev{};
    ev.name = SWOFTY_EVENT_SESSION_END;
    on_event(&ev, nullptr);
    assert(session_ended && !pending_failure && !garage_return_wanted);
    assert(test_cap == -1 && throttle_hook.car == 0 && throttle_hook.cap == 1);
    assert(!pit_lane_system && !controller_system && retire_in == -1);
    assert(garage_of[3] == 7);
    udp_player = 3;
    udp_speed[3] = 0;
    udp_last_ms = GetTickCount64();
    assert(player_speed() == -1);
    trigger_failure();
    on_frame(10, nullptr);
    assert(!pending_failure && !garage_return_wanted);
    assert(on_update_vehicle(system.data(), 3) == 42 && *observed_failure == 0);
    ev.name = SWOFTY_EVENT_SESSION_START;
    on_event(&ev, nullptr);
    assert(!session_ended && player_vehicle == -1 && garage_of[3] == 7);
    udp_last_ms = telemetry_not_before.load();
    assert(player_speed() == -1); // A cached packet from before reset is not fresh.
    player_vehicle = 3;
    trigger_failure();
    expect_failed = true;
    on_update_vehicle(system.data(), 3);
    assert(*observed_failure == 1 && !pending_failure);

    // DNF controller/session cleanup clears gameplay requests but must not
    // restart the retired car's engine audio. Only fresh session preparation
    // or session.start releases the separate audio latch.
    reset_session();
    player_vehicle = 3;
    *observed_failure = 0;
    player_broken = true;
    terminal_mask = 8;
    retired_engine_off_vehicle = 3;
    engine_off_vehicle = 3;
    orig_set_controller = [](void *, uint64_t, uint32_t, int32_t) -> uint64_t { return 77; };
    assert(on_set_controller(nullptr, 3, 1, 3) == 77);
    assert(!player_broken && !car_dead && !terminal_mask && retire_in == -1);
    assert(retired_engine_off_vehicle == 3 && engine_off_vehicle == 3);
    power_loss(reinterpret_cast<uintptr_t>(car.data()));
    assert(throttle_hook.cap == 1 && engine_off_vehicle == 3);
    assert(on_set_controller(nullptr, 3, 1, 3) == 77);
    assert(engine_off_vehicle == 3);
    assert(on_set_controller(nullptr, 3, 1, 10) == 77);
    assert(retired_engine_off_vehicle == -1 && engine_off_vehicle == -1);
    retired_engine_off_vehicle = 3;
    engine_off_vehicle = 3;
    ev.name = SWOFTY_EVENT_SESSION_END;
    on_event(&ev, nullptr);
    assert(session_ended && engine_off_vehicle == 3 && retired_engine_off_vehicle == 3);
    ev.name = SWOFTY_EVENT_SESSION_START;
    on_event(&ev, nullptr);
    assert(!session_ended && engine_off_vehicle == -1 && retired_engine_off_vehicle == -1);
    puts("PASS: queued player failure, ordering, AI isolation, expiry and session cleanup");
    puts("PASS: DNF audio survives gameplay/session cleanup; restart/start releases shutdown");
}
