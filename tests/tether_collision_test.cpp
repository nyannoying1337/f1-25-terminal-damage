// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
uintptr_t body_wrapper,connection_wrapper;
unsigned calls;
void discard(const char *,...) {}
void create(void *self,uint8_t)
{
    ++calls;
    auto *part=static_cast<uint8_t *>(self);
    int32_t node;memcpy(&node,part+0xe0,4);assert(node==12);
    memcpy(part+0x40,&body_wrapper,8);memcpy(part+0x100,&connection_wrapper,8);part[0x10c]=1;
}
template<class T,size_t N> void put(uint8_t (&v)[N],size_t offset,T value){memcpy(v+offset,&value,sizeof value);}
}
int main()
{
    SwoftyApi api{};api.log=discard;swofty::api=&api;
    assert(!bool(tether_collision_experiment));
    alignas(16) uint8_t car[0x600]{},physics[0x350]{},bw[0x30]{},cw[0x30]{},jw[0x30]{},
        joint[0x3c0]{},wheel[0x500]{},chassis[0x500]{},part[0x120]{};
    auto addr=[](auto &v){return reinterpret_cast<uintptr_t>(v);};
    wheel_joint_vtable=0x1234;wheel_part_vtable=0x5678;
    put(car,0x558,addr(physics));put(physics,0x340,addr(cw));
    put(bw,0x18,addr(wheel));put(cw,0x18,addr(chassis));
    put(jw,0x18,addr(joint));put(jw,0x20,addr(joint));
    put(joint,0,uintptr_t(0x1234));put(joint,8,addr(wheel));put(joint,0x10,addr(chassis));
    uint8_t before[sizeof joint];memcpy(before,joint,sizeof joint);
    assert(exclude_fresh_wheel_chassis_pair(addr(car),addr(bw),addr(jw)));
    assert(joint[0x79]==1);before[0x79]=1;assert(!memcmp(before,joint,sizeof joint));
    assert(!exclude_fresh_wheel_chassis_pair(addr(car),addr(bw),addr(jw))); // already excluded
    joint[0x79]=0;joint[0x78]=1;
    assert(!exclude_fresh_wheel_chassis_pair(addr(car),addr(bw),addr(jw)));joint[0x78]=0;
    put(joint,0x10,addr(wheel)); // wrong endpoint / other body
    assert(!exclude_fresh_wheel_chassis_pair(addr(car),addr(bw),addr(jw)));put(joint,0x10,addr(chassis));
    joint[0x30]=1;assert(!exclude_fresh_wheel_chassis_pair(addr(car),addr(bw),addr(jw)));joint[0x30]=0;
    put(joint,0,uintptr_t(9));assert(!exclude_fresh_wheel_chassis_pair(addr(car),addr(bw),addr(jw)));put(joint,0,uintptr_t(0x1234));
    assert(!exclude_fresh_wheel_chassis_pair(addr(car),0,addr(jw)));
    assert(!exclude_fresh_wheel_chassis_pair(addr(car),addr(bw),1));
    put(part,0,uintptr_t(0x5678));put(part,0x10,addr(car));put(part,0xe0,int32_t(12));
    car[0xe8]=3;player_vehicle=3;throttle_hook.car=addr(car);session_ended=false;
    tether_collision_active=true;wheel_tether_test_active=false;ai_wheel_tethers_active=false;
    body_wrapper=addr(bw);connection_wrapper=addr(jw);orig_wheel_detach=create;
    on_wheel_detach(part,0);assert(calls==1 && joint[0x79]==1);
    // Existing joints aren't touched, including after a replay re-entry.
    joint[0x79]=0;on_wheel_detach(part,0);assert(calls==2 && joint[0x79]==0);
    auto clear=[&](){put(part,0x40,uintptr_t(0));put(part,0x100,uintptr_t(0));part[0x10c]=0;};
    clear();car[0xe8]=4;on_wheel_detach(part,0);assert(joint[0x79]==0); // AI identity
    clear();car[0xe8]=3;session_ended=true;on_wheel_detach(part,0);assert(joint[0x79]==0);
    clear();session_ended=false;player_vehicle=-1;on_wheel_detach(part,0);assert(joint[0x79]==0);
    clear();player_vehicle=3;on_wheel_detach(part,0);assert(joint[0x79]==1); // fresh recreation
    LARGE_INTEGER frequency,start,finish;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    for (unsigned i=0;i<2000;++i) {
        clear();joint[0x79]=0;on_wheel_detach(part,0);assert(joint[0x79]==1);
    }
    QueryPerformanceCounter(&finish);
    printf("fresh-joint wrapper: %.3f us/call in synthetic replay recreation (2000 calls)\n",
        double(finish.QuadPart-start.QuadPart)*1e6/double(frequency.QuadPart)/2000.0);
    assert(tether_collision_failures.load()==0); // routine success has no atomic log counter
    puts("PASS: fresh player tether exact endpoints only; no other fields/body contacts changed");
}
