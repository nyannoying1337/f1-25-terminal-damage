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
    // Rewinding before crash damage clears its non-wear mask too.
    packet(150, 300, true, 50); packet(50, 301); power_loss(c);
    assert(!player_broken && !terminal_mask && throttle_hook.cap == 1);
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
