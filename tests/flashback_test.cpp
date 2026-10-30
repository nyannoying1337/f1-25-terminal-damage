#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
void discard_log(const char *, ...) {}
template<class T> void put(uint8_t *p, size_t at, T value) { memcpy(p + at, &value, sizeof value); }
void packet(uint32_t frame, uint32_t overall, bool flashback = false, uint32_t target = 0, uint64_t uid = 123)
{
    uint8_t bytes[1448]{};
    put(bytes, 0, uint16_t(2026)); bytes[6] = flashback ? 3 : 6; bytes[27] = 2;
    put(bytes, 7, uid); put(bytes, 19, frame); put(bytes, 23, overall);
    if (flashback) { memcpy(bytes + 29, "FLBK", 4); put(bytes, 33, target); }
    flashback_tracker.packet(bytes, flashback ? 45 : sizeof bytes, GetTickCount64());
    damage_flashback_tracker.packet(bytes, flashback ? 45 : sizeof bytes, GetTickCount64());
}
}
int main()
{
    SwoftyApi api{}; api.log = discard_log; swofty::api = &api;
    std::vector<uint8_t> car(0x600), engine(0x2600), gearbox(0x700);
    put(car.data(), 0x530, reinterpret_cast<uintptr_t>(engine.data()));
    put(car.data(), 0x538, reinterpret_cast<uintptr_t>(gearbox.data()));
    uintptr_t c = reinterpret_cast<uintptr_t>(car.data());
    reset_session(); player_vehicle = 9;
    packet(100, 100); flashback_tracker.mark(GetTickCount64());
    player_broken = true; terminal_mask = 1;
    put(engine.data(), part_offsets[0], 1.0f);
    packet(180, 180, true, 50);
    assert(!flashback_tracker.ready(GetTickCount64())); // event alone cannot clear gameplay
    packet(181, 181); assert(!flashback_tracker.ready(GetTickCount64())); // stale timeline
    packet(50, 182); assert(flashback_tracker.ready(GetTickCount64()));
    power_loss(c); assert(player_broken && throttle_hook.cap == 0); // parts not restored yet
    put(engine.data(), part_offsets[0], 0.003f);
    garage_return_wanted = true; crew_msg = 4; crew_msg_in = 2; engine_off_standing = true;
    retired_engine_off_vehicle = 9; engine_off_vehicle = 9; test_cap = 0;
    power_loss(c);
    assert(!player_broken && !terminal_mask && !garage_return_wanted && crew_msg == 0 && crew_msg_in == -1);
    assert(throttle_hook.cap == 1 && test_cap == -1 && engine_off_vehicle == -1 && retired_engine_off_vehicle == -1);
    // Rewinding to a point after the failure must keep its lock.
    packet(100, 200); flashback_tracker.mark(GetTickCount64()); player_broken = true; terminal_mask = 0x18;
    packet(180, 280, true, 120); packet(120, 281); power_loss(c);
    assert(player_broken && terminal_mask == 0x18 && throttle_hook.cap == 0);
    // Replay deserialization clears SESSION_NOT_STARTED; healthy parts during
    // a plain retirement must not make that look like a fresh session restart.
    reset_session(); player_vehicle = 9; packet(100, 290); flashback_tracker.mark(GetTickCount64());
    player_broken = true; retire_requested = true; player_pit = 3;
    pit_clearance_hold_vehicle = 9; retired_engine_off_vehicle = 9;
    orig_set_controller = +[](void *,uint64_t,uint32_t,int32_t)->uint64_t{return 17;};
    assert(on_set_controller(nullptr,9,0,10)==17);
    assert(!restart_check_until && pit_clearance_hold_vehicle==9 && retired_engine_off_vehicle==9);
    power_loss(c);assert(player_broken && retire_requested && player_pit==3);
    retire_in=-2; crew_msg=4; crew_msg_in=2; pushing=false; prepare_at_box=false;
    packet(180, 380, true, 120); packet(120, 381); power_loss(c);
    assert(retire_in==-1 && pushing && prepare_at_box && crew_msg==0 && crew_msg_in==-1);
    assert(player_broken && retire_requested); // post-request rewind remains retired
    packet(150, 390, true, 50);packet(50, 391);power_loss(c);
    assert(!player_broken && !retire_requested && pit_clearance_hold_vehicle==-1); // pre-request unlock
    // Controller preparation alone is not proof of restart. Actual callback resets.
    player_broken=true;retire_requested=true;pit_clearance_hold_vehicle=9;retired_engine_off_vehicle=9;
    assert(on_set_controller(nullptr,9,1,10)==17);
    assert(!restart_check_until && pit_clearance_hold_vehicle==9 && retired_engine_off_vehicle==9);
    power_loss(c);assert(player_broken && retire_requested);
    reset_for_session_restart(nullptr);
    assert(!player_broken && !retire_requested && pit_clearance_hold_vehicle==-1 && retired_engine_off_vehicle==-1);
    // Recreate a crash lock for the pre-crash rewind check below.
    packet(100, 400);flashback_tracker.mark(GetTickCount64());player_broken=true;terminal_mask=0x18;
    // Rewinding before crash damage clears its non-wear mask too.
    packet(150, 300, true, 50); packet(50, 301); power_loss(c);
    assert(!player_broken && !terminal_mask && throttle_hook.cap == 1);
    // Retire at100, crash at150, rewind to120: request survives, crash does not.
    reset_session(); player_vehicle=9; player_pit=1;
    packet(100,600); flashback_tracker.mark(GetTickCount64());
    retire_requested=true; player_broken=true;
    packet(150,650); damage_flashback_tracker.record_damage(0x48,GetTickCount64());
    terminal_mask=0x48; engine_off_vehicle=9; retired_engine_off_vehicle=9;
    packet(190,690,true,120); packet(120,691); power_loss(c);
    assert(player_broken && retire_requested && terminal_mask==0 && throttle_hook.cap==1);
    assert(engine_off_vehicle==-1 && retired_engine_off_vehicle==-1);
    // A later rewind before the original request still cancels that request.
    packet(140,710,true,50); packet(50,711); power_loss(c);
    assert(!player_broken && !retire_requested);
    // Session change invalidates the timeline proof; stale/expired requests fail closed.
    packet(100, 400); flashback_tracker.mark(GetTickCount64()); player_broken = true; terminal_mask = 1;
    packet(180, 480, true, 50); packet(50, 481, false, 0, 456); power_loss(c);
    assert(player_broken);
    reset_session(); packet(100, 500); flashback_tracker.mark(GetTickCount64());
    packet(180, 580, true, 50); packet(50, 581);
    assert(!flashback_tracker.ready(GetTickCount64() + 6000));
    reset_session(); assert(!flashback_tracker.ready(GetTickCount64()));
    puts("PASS: flashback pre-damage resume, live repair gate, crash mask, after-damage retention and cleanup");
}
