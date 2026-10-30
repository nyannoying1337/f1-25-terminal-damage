// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
unsigned calls;
int32_t expected_node;
bool create_body;
bool check_timing;
void discard_log(const char *, ...) {}
void detach(void *self,uint8_t notify)
{
    ++calls;
    if (check_timing) {
        assert(td_perf::counters[td_perf::wheel_before].samples==1);
        assert(td_perf::counters[td_perf::wheel_original].samples==0);
        assert(td_perf::counters[td_perf::wheel_ai_after].samples==0);
    }
    assert(notify==7);
    int32_t node=0;
    assert(guarded_read(reinterpret_cast<uintptr_t>(self)+0xe0,node));
    assert(node==expected_node);
    if (create_body) {
        uintptr_t body=0x9876;
        memcpy(static_cast<uint8_t *>(self)+0x40,&body,sizeof body);
    }
}
template<class T> void put(std::vector<uint8_t>& b,size_t o,T x) {memcpy(b.data()+o,&x,sizeof x);}
}

int main()
{
    SwoftyApi api{};api.log=discard_log;swofty::api=&api;
    // Default-off must return before scanning or installing any wheel hook.
    assert(!bool(wheel_tether_test));
    assert(!bool(ai_wheel_tethers));
    hook_wheel_tether_test();
    assert(!orig_wheel_detach && !wheel_tether_test_active);
    orig_wheel_detach=detach;
    std::vector<uint8_t> car(0x200),part(0x120);
    uintptr_t cp=reinterpret_cast<uintptr_t>(car.data());
    wheel_part_vtable=0x1234;wheel_tether_test_active=true;
    player_vehicle=3;throttle_hook.car=cp;session_ended=false;
    put(car,0xe8,uint8_t(3));
    put(part,0,uintptr_t(0x1234));put(part,0x10,cp);put(part,0xe0,int32_t(12));
    auto before=part;
    expected_node=-1;create_body=true;
    on_wheel_detach(part.data(),7);
    assert(calls==1);
    uintptr_t body=0;int32_t node=0;
    assert(guarded_read(reinterpret_cast<uintptr_t>(part.data())+0x40,body)&&body==0x9876);
    assert(guarded_read(reinterpret_cast<uintptr_t>(part.data())+0xe0,node)&&node==12);
    // Only the mock game's new body differs. The temporary node, attachment
    // metadata, damage state and every other part byte are preserved.
    put(before,0x40,uintptr_t(0x9876));assert(part==before);
    create_body=false;expected_node=12;
    on_wheel_detach(part.data(),7); // existing body: never change a live joint
    assert(part==before);
    put(part,0x40,uintptr_t(0));before=part;
    wheel_tether_test_active=false;on_wheel_detach(part.data(),7);assert(part==before);
    wheel_tether_test_active=true;
    put(car,0xe8,uint8_t(4));on_wheel_detach(part.data(),7);assert(part==before);
    put(car,0xe8,uint8_t(3));throttle_hook.car=cp+8;on_wheel_detach(part.data(),7);assert(part==before);
    throttle_hook.car=cp;session_ended=true;on_wheel_detach(part.data(),7);assert(part==before);
    session_ended=false;wheel_part_vtable=0x5555;on_wheel_detach(part.data(),7);assert(part==before);
    wheel_part_vtable=0x1234;put(part,0x100,uintptr_t(0x777));before=part;
    on_wheel_detach(part.data(),7);assert(part==before);
    put(part,0x100,uintptr_t(0));put(part,0x10c,uint8_t(1));before=part;
    on_wheel_detach(part.data(),7);assert(part==before);
    // A recreated wheel after rewind/restart can use the same path, without
    // retaining object addresses or needing a separate reset registry.
    put(part,0x10c,uint8_t(0));expected_node=-1;create_body=true;
    on_wheel_detach(part.data(),7);
    assert(calls==10);
    assert(guarded_read(reinterpret_cast<uintptr_t>(part.data())+0xe0,node)&&node==12);
    // All four toggle combinations must select player and AI independently.
    std::vector<uint8_t> ai_car(0x200);
    uintptr_t acp=reinterpret_cast<uintptr_t>(ai_car.data());
    put(ai_car,0xe8,uint8_t(4));
    for (bool players : {false,true}) for (bool ai : {false,true}) {
        wheel_tether_test_active=players;ai_wheel_tethers_active=ai;
        for (bool mine : {false,true}) {
            put(part,0x10,mine?cp:acp);put(part,0x40,uintptr_t(0));
            expected_node=(mine?players:ai)?-1:12;
            on_wheel_detach(part.data(),7);
            assert(guarded_read(reinterpret_cast<uintptr_t>(part.data())+0xe0,node)&&node==12);
        }
    }
    // Unknown player identity or contradictory pointer/ID cannot opt the player
    // into the AI switch. Invalid vehicle IDs must also retain stock behavior.
    wheel_tether_test_active=false;ai_wheel_tethers_active=true;expected_node=12;
    put(part,0x40,uintptr_t(0));put(part,0x10,cp);
    on_wheel_detach(part.data(),7);
    put(part,0x40,uintptr_t(0));put(car,0xe8,uint8_t(4));on_wheel_detach(part.data(),7);
    put(car,0xe8,uint8_t(3));player_vehicle=-1;put(part,0x10,acp);
    put(part,0x40,uintptr_t(0));on_wheel_detach(part.data(),7);
    // Replay diagnostics measure original separately and classify only the
    // selected wheel's post-work; other private-hook timing stays disabled.
    for (auto &c:td_perf::counters) {c.samples=0;c.ticks=0;c.worst=0;}
    td_perf::enabled=false;td_perf::wheel_capture=true;
    player_vehicle=3;
    put(ai_car,0xe8,uint8_t(4));put(part,0x40,uintptr_t(0));
    expected_node=-1;check_timing=true;on_wheel_detach(part.data(),7);check_timing=false;
    assert(td_perf::counters[td_perf::wheel_original].samples==1);
    assert(td_perf::counters[td_perf::wheel_ai_after].samples==1);
    assert(td_perf::counters[td_perf::wheel_player_after].samples==0);
    assert(td_perf::counters[td_perf::vehicle_before].samples==0);
    td_perf::wheel_capture=false;
    expected_node=12;player_vehicle=3;put(ai_car,0xe8,uint8_t(255));
    put(part,0x40,uintptr_t(0));on_wheel_detach(part.data(),7);
    put(part,0x40,uintptr_t(0));put(part,0x10,uintptr_t(1));
    on_wheel_detach(part.data(),7); // unreadable owner: original path only
    put(ai_car,0xe8,uint8_t(4));put(part,0x10,acp);
    put(part,0x40,uintptr_t(0));create_body=false;expected_node=-1;
    before=part;
    for (unsigned i=0;i<1000;++i) on_wheel_detach(part.data(),7);
    assert(part==before); // repeated replay calls never leak the missing node
    assert(td_perf::counters[td_perf::wheel_original].samples==1); // capture disabled
    puts("PASS: independent player/AI toggles, both defaults off, unknown identity fails closed");
    puts("PASS: fresh player wheel uses missing-node path; body retained, node restored, unselected cars/live joints untouched");
}
