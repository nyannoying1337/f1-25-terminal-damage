#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include "test_util.hpp"

// Flashback without UDP (MessageFlashbackUsed + the game's terminal record) and the pit lane standstill retirement.
namespace {
int watch(const char *, SwoftySettingFn, void *) { return 0; }
int setting(const char *id, uint32_t type, SwoftySettingValue *out)
{
    if (strcmp(id, "keep_control") || type != SWOFTY_SETTING_TOGGLE)
        return 0;
    out->type = type;
    out->toggle = true;
    return 1;
}
using tu::put;
std::vector<uint8_t> car(0x600), engine(0x2600), gearbox(0x700);
uintptr_t c = 0;
void broken(uint32_t mask, float ice)
{
    reset_session();
    player_vehicle = 9;
    player_broken = true;
    terminal_mask = mask;
    put(engine.data(), part_offsets[0], ice);
    live_terminal_state = mask ? 5 : 1;
    live_terminal_mask = mask;
}
void flashback() { flashback_used_at = GetTickCount64(); }
}

int main()
{
    SwoftyApi api{}; api.version = 4; api.log = tu::quiet_log; api.setting = setting; api.watch_setting = watch;
    swofty::api = &api;
    put(car.data(), 0x530, reinterpret_cast<uintptr_t>(engine.data()));
    put(car.data(), 0x538, reinterpret_cast<uintptr_t>(gearbox.data()));
    c = reinterpret_cast<uintptr_t>(car.data());

    // Nothing happens without a flashback.
    broken(1, 1.0f);
    flashback_check(c);
    assert(player_broken && terminal_mask == 1);

    // Flashback before an ICE failure: record normal and parts whole -> the car is yours again.
    broken(1, 1.0f);
    retire_in = -1.0; engine_off_standing = true; engine_kill_vehicle = 9;
    flashback();
    flashback_check(c);  // record still terminal: wait
    assert(player_broken);
    live_terminal_state = 1; live_terminal_mask = 0;
    flashback_check(c);  // parts not restored yet: wait
    assert(player_broken);
    put(engine.data(), part_offsets[0], 0.01f);
    flashback_check(c);
    assert(!player_broken && !terminal_mask && throttle_hook.cap == 1.0f && engine_kill_vehicle.load() == -1 &&
           !engine_off_standing);

    // Voluntary retirement request (no damage) is cancelled by a flashback.
    broken(0, 0.01f);
    retire_requested = true; still_for = 7; crew_msg_in = 3;
    flashback();
    flashback_check(c);
    assert(!player_broken && !retire_requested && still_for == 0 && crew_msg_in == -1);

    // The game lets you flash back after a retirement: back before the damage undoes the retirement too.
    broken(8, 0.01f);
    confirmed_retired_vehicle = 9; retired_drive_lock_vehicle = 9; player_controller_flags = 1u << 4;
    live_terminal_state = 1; live_terminal_mask = 0;
    flashback();
    flashback_check(c);
    assert(!player_broken && !terminal_mask && confirmed_retired_vehicle.load() == -1 &&
           retired_drive_lock_vehicle.load() == -1 && !(player_controller_flags.load() & (1u << 4)));
    // A retired car still broken after the flashback stays retired.
    broken(8, 0.01f);
    confirmed_retired_vehicle = 9;
    flashback();
    flashback_check(c);
    Sleep(2100);
    flashback_check(c);
    assert(player_broken && confirmed_retired_vehicle.load() == 9);

    // The game's own failure: the failing part reads 0.99 before it (record state 4). A flashback there clears the
    // lock and hands the part back, so the game's next RETIRED hand-over is dropped and the part breaks again.
    broken(0x3, 1.0f);
    game_failure_part = 2; game_planned_part = -1; game_failure_deadline = GetTickCount64() + 60000;
    put(engine.data(), part_offsets[0], 0.99f);
    live_terminal_state = 4; live_terminal_mask = 0;
    flashback();
    flashback_check(c);
    assert(!player_broken && game_planned_part.load() == 2 && game_failure_part.load() == -1 &&
           game_failure_deadline.load() == 0);
    put(engine.data(), part_offsets[0], 0.01f);

    // The same right after the failure, before the game's terminal message arrived (no mask yet, no retire request):
    // not a cancelled retirement request; the part goes back to the game and the pending DSQ timer and DSQ stop.
    broken(0, 0.01f);
    game_failure_part = 2; game_planned_part = -1; game_failure_deadline = GetTickCount64() + 60000;
    dsq_pending = true;
    put(engine.data(), part_offsets[2], 1.0f);  // the part the addon broke (MGU-H)
    live_terminal_state = 4; live_terminal_mask = 0;
    flashback();
    flashback_check(c);  // part still failed: wait
    assert(player_broken && game_failure_part.load() == 2 && game_failure_deadline.load() != 0);
    put(engine.data(), part_offsets[2], 0.99f);
    flashback_check(c);
    assert(!player_broken && game_planned_part.load() == 2 && game_failure_part.load() == -1 &&
           game_failure_deadline.load() == 0 && !dsq_pending);
    // Still failed after the window: the failure stays and the retirement starts over, the request path is not taken.
    broken(0, 0.01f);
    put(engine.data(), part_offsets[2], 1.0f);
    live_terminal_state = 5; live_terminal_mask = 0x3;
    still_for = 8;
    flashback();
    flashback_check(c);
    Sleep(2100);
    flashback_check(c);
    assert(player_broken && still_for == 0 && retire_in == -1.0);
    put(engine.data(), part_offsets[2], 0.01f);

    // The pit state is read back from the pit lane system after the rewind (here: back in the pit lane entry).
    {
        std::vector<uint8_t> pit_sys(0x400), entry(0x10);
        put(entry.data(), 4, uint32_t(2));
        put(pit_sys.data(), 0x68 + 9 * 16, reinterpret_cast<uintptr_t>(entry.data()));
        pit_lane_system = reinterpret_cast<uintptr_t>(pit_sys.data());
        broken(8, 0.01f);
        live_terminal_state = 1; live_terminal_mask = 0;
        flashback();
        flashback_check(c);
        assert(!player_broken && player_pit.load() == 2);
        pit_lane_system = 0;
    }

    // Red flag (SAFETY_CAR flag) and a withdrawn car in the garage are left alone.
    broken(8, 0.01f);
    player_controller_flags = 1u << 11;
    live_terminal_state = 1; live_terminal_mask = 0;
    flashback();
    flashback_check(c);
    assert(player_broken);
    broken(8, 0.01f);
    car_dead = true;
    live_terminal_state = 1; live_terminal_mask = 0;
    flashback();
    flashback_check(c);
    assert(player_broken);

    // Flashback after the damage: the damage stays, the retirement work starts over; damage the rewind removed goes.
    broken(0x9, 0.01f);
    live_terminal_mask = 0x8;
    limp_state = 0; still_for = 8; engine_off_standing = true; garage_return_wanted = true; crew_msg_in = 5;
    flashback();
    flashback_check(c);
    assert(player_broken && terminal_mask == 0x9);  // within the 2 s window nothing changes yet
    Sleep(2100);
    flashback_check(c);
    assert(player_broken && terminal_mask == 0x8 && limp_state == 0 && still_for == 0 && !engine_off_standing &&
           !garage_return_wanted && crew_msg_in == -1 && retire_in == -1.0);

    // Without the hook, the record turning normal on track starts the same check.
    broken(8, 0.01f);
    flashback_hooked = false;
    {
        std::vector<uint8_t> sys(0xd00);
        put(sys.data(), 0xc20, uint64_t(20));
        // record of vehicle 9: parts at +0x10, overall at +0x68 (all 1 = normal)
        uint8_t *rec = sys.data() + 0x50 + 9 * 0x70;
        for (int i = 0; i < 23; ++i) put(rec, 0x10 + i * 4, uint32_t(1));
        read_terminal_record(reinterpret_cast<uintptr_t>(sys.data()), 9);
        flashback_check(c);
        assert(!player_broken && !terminal_mask);
    }

    // Standstill retirement: on track and stranded in the pit lane, not in the box.
    for (int pit : {1, 2, 14, 15, 4, 8}) {
        broken(8, 0.01f);
        player_pit = pit;
        telemetry_not_before = 0;
        game_speed = 0;
        for (int i = 0; i < 12; ++i) {
            game_speed_ms = GetTickCount64();
            standstill_retire(1.0);
        }
        bool retiring = retire_in.load() == 0.0;
        assert(retiring == (pit == 1 || pit == 2 || pit == 14 || pit == 15));
    }
    return 0;
}
