// Regression coverage for the six audited lifecycle conflicts.
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
int main() {
    SwoftyApi api{}; api.log=discard_log; swofty::api=&api;
    std::vector<uint8_t> car(0x600), engine(0x2600), gearbox(0x700);
    put(car.data(),0x530,reinterpret_cast<uintptr_t>(engine.data()));
    put(car.data(),0x538,reinterpret_cast<uintptr_t>(gearbox.data()));
    auto c=reinterpret_cast<uintptr_t>(car.data());
    orig_set_controller=+[](void*,uint64_t,uint32_t,int32_t)->uint64_t{return 0;};
    // A clear of NOT_IN_SESSION must not clear gameplay damage.
    reset_session();player_vehicle=9;player_broken=true;terminal_mask=0x18;
    on_set_controller(nullptr,9,0,3);
    assert(player_broken && terminal_mask==0x18);
    puts("PASS: clearing NOT_IN_SESSION preserves real damage");
    // Crash damage does not need failed engine wear. The wear-only restart gate
    // therefore accepts a crash car without an actual reset of its crash state.
    reset_session();player_vehicle=9;player_broken=true;terminal_mask=0x18;
    on_set_controller(nullptr,9,1,10);power_loss(c);
    assert(player_broken && terminal_mask==0x18);
    puts("PASS: controller preparation alone preserves crash state");
    // Rewind between two failures removes only the later failure.
    reset_session();player_vehicle=9;player_pit=1;
    packet(100,1000);flashback_tracker.mark(GetTickCount64());damage_flashback_tracker.record_damage(2,GetTickCount64());
    player_broken=true;terminal_mask=2;
    packet(150,1050);damage_flashback_tracker.record_damage(0x18,GetTickCount64());terminal_mask=0x1a;
    packet(190,1090,true,120);packet(120,1091);power_loss(c);
    assert(terminal_mask==2 && throttle_hook.cap>0);
    puts("PASS: rewind before second damage restores earlier mask");
    // An earlier failed ICE remains failed while the later crash is removed.
    reset_session();player_vehicle=9;player_pit=1;
    packet(100,1100);flashback_tracker.mark(GetTickCount64());damage_flashback_tracker.record_damage(1,GetTickCount64());
    player_broken=true;terminal_mask=1;put(engine.data(),part_offsets[0],1.0f);
    packet(150,1150);damage_flashback_tracker.record_damage(0x18,GetTickCount64());terminal_mask=0x19;
    packet(190,1190,true,120);packet(120,1191);power_loss(c);
    assert(terminal_mask==1 && player_broken && throttle_hook.cap==0);
    put(engine.data(),part_offsets[0],0.0f);
    // Discard future crew/standstill/audio state on retained approach rewind.
    reset_session();player_vehicle=9;player_pit=2;
    packet(100,1200);flashback_tracker.mark(GetTickCount64());player_broken=true;retire_requested=true;
    sendoff_stage=2;crew_state_now=4;still_for=7;engine_off_standing=true;
    retired_engine_off_vehicle=9;
    packet(180,1280,true,120);packet(120,1281);power_loss(c);
    assert(sendoff_stage==0 && crew_state_now==-1 && still_for==0 && !engine_off_standing && engine_off_vehicle==-1);
    puts("PASS: approach rearm cancels future crew/audio state");
    // Garage return cancels future queued crew work.
    reset_session();player_vehicle=9;player_pit=9;player_broken=true;
    uint8_t system[0x310]{};pit_lane_system=reinterpret_cast<uintptr_t>(system);
    orig_set_vehicle_status=+[](void*,uint64_t,uint64_t)->uint64_t{return 0;};
    garage_return_wanted=true;crew_request=7;crew_request_in=0.5;crew_msg=4;crew_msg_in=1;
    return_to_garage();
    assert(car_dead && !player_broken && crew_request_in==-1 && crew_msg_in==-1);
    puts("PASS: garage return cancels queued crew events");
    // Part replacement invalidates both trackers before the next failure.
    reset_session();player_vehicle=9;player_pit=9;car_dead=true;garage_part_failed=true;
    packet(100,1300);damage_flashback_tracker.record_damage(1,GetTickCount64());power_loss(c);
    { FlashbackTracker::Guard guard(damage_flashback_tracker.mutex);assert(!damage_flashback_tracker.lock_known); }
    packet(150,1350);damage_flashback_tracker.record_damage(1,GetTickCount64());
    { FlashbackTracker::Guard guard(damage_flashback_tracker.mutex);assert(damage_flashback_tracker.lock_frame==150); }
    puts("PASS: part replacement clears old damage timeline");
    // The tracker uses a global snapshot with no player ID binding.
    reset_session();packet(100,1400);flashback_tracker.mark(GetTickCount64());
    { FlashbackTracker::Guard guard(flashback_tracker.mutex); assert(flashback_tracker.lock_frame==100); }
    puts("PASS: six audited lifecycle conflicts prevented; no game hooks installed");
}

