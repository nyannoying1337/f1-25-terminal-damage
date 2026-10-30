// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

static unsigned scans;
static int no_scan(const char *,const char *,uintptr_t *) {++scans;return 0;}
static void discard(const char *,...) {}

// Execute the exact emitted mid-function stub with the game's live RSI/RBX
// convention. Tiny return targets identify its selected continuation.
static int run(void *stub,void *part,void *car,unsigned lhs=2,unsigned rhs=1)
{
    int result;
    asm volatile("cmp %[rhs], %[lhs]\n\tcall *%[fn]"
        : "=a"(result)
        : [fn]"r"(stub),"S"(part),"b"(car),[lhs]"r"(lhs),[rhs]"r"(rhs)
        : "cc","memory");
    return result;
}

int main()
{
    SwoftyApi api{};api.log=discard;api.scan=no_scan;swofty::api=&api;
    configure_player_wheels("stock");
    assert(!wheel_tether_test && !tether_plane_experiment && !tether_distance_experiment && !tether_collision_experiment);
    configure_player_wheels("reduced");
    assert(!wheel_tether_test && tether_plane_experiment && tether_distance_experiment && tether_collision_experiment);
    configure_player_wheels("loose");
    assert(wheel_tether_test && !tether_plane_experiment && !tether_distance_experiment && !tether_collision_experiment);
    configure_player_wheels("invalid");
    assert(!wheel_tether_test && !tether_plane_experiment && !tether_distance_experiment && !tether_collision_experiment);
    assert(!bool(tether_plane_experiment));
    hook_tether_plane_experiment(); assert(scans==0);
    assert(!bool(tether_distance_experiment));
    hook_tether_distance_experiment();assert(scans==0);
    auto *page=static_cast<uint8_t *>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    assert(page);
    const uint8_t stock[]={0xb8,11,0,0,0,0xc3}, skip[]={0xb8,22,0,0,0,0xc3};
    memcpy(page+1024,stock,sizeof stock);memcpy(page+1040,skip,sizeof skip);
    constexpr uintptr_t vt=0x12345678;
    emitter e{page};emit_tether_plane_stub(e,reinterpret_cast<uintptr_t>(page+1024),reinterpret_cast<uintptr_t>(page+1040),vt);
    FlushInstructionCache(GetCurrentProcess(),page,4096);
    alignas(16) uint8_t part[0x120]{}, car[0x200]{}, other[0x200]{};
    auto put=[&](size_t offset,uintptr_t value){memcpy(part+offset,&value,sizeof value);};
    put(0,vt);put(0x10,reinterpret_cast<uintptr_t>(car));car[0xe8]=3;
    player_vehicle=3;session_ended=false;throttle_hook.car=reinterpret_cast<uintptr_t>(car);
    uint8_t before[sizeof part];memcpy(before,part,sizeof part);
    assert(run(page,part,car)==22); // selected player: bypass correction
    assert(run(page,part,car,1,2)==22); // original jbe remains taken
    assert(run(page,part,car,2,2)==22);
    assert(run(page,part,other)==11); // contradictory owner
    throttle_hook.car=reinterpret_cast<uintptr_t>(other);
    assert(run(page,part,car)==11); // AI/nonplayer
    throttle_hook.car=reinterpret_cast<uintptr_t>(car);
    session_ended=true;assert(run(page,part,car)==11);session_ended=false;
    player_vehicle=-1;assert(run(page,part,car)==11);
    player_vehicle=32;assert(run(page,part,car)==11);
    player_vehicle=4;assert(run(page,part,car)==11);
    player_vehicle=3;put(0,vt+8);assert(run(page,part,car)==11);put(0,vt);
    assert(memcmp(before,part,sizeof part)==0); // no field changes
    // Distance variant: original under-limit continuation is DIFFERENT from
    // the experimental over-limit continuation, and RBX is not an owner here.
    const uint8_t under_limit[]={0xb8,33,0,0,0,0xc3};
    memcpy(page+1056,under_limit,sizeof under_limit);
    emitter radial{page+512};
    emit_tether_plane_stub(radial,reinterpret_cast<uintptr_t>(page+1024),
        reinterpret_cast<uintptr_t>(page+1040),vt,reinterpret_cast<uintptr_t>(page+1056),true);
    FlushInstructionCache(GetCurrentProcess(),page,4096);
    assert(run(page+512,part,other)==22); // reads live owner; preserves unrelated RBX
    assert(run(page+512,part,other,1,2)==33);
    assert(run(page+512,part,other,2,2)==33);
    throttle_hook.car=reinterpret_cast<uintptr_t>(other);
    assert(run(page+512,part,other)==11);
    throttle_hook.car=reinterpret_cast<uintptr_t>(car);
    session_ended=true;assert(run(page+512,part,other)==11);session_ended=false;
    player_vehicle=-1;assert(run(page+512,part,other)==11);player_vehicle=3;
    assert(memcmp(before,part,sizeof part)==0);
    VirtualFree(page,0,MEM_RELEASE);
}
