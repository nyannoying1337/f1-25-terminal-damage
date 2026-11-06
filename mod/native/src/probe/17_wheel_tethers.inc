// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Wheel detach hook and the wheel tether options.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Optional workaround: use the stock missing-tether-node path ONLY during
// creation of a fresh detached wheel in the selected player/AI scope. Never destroy a body/constraint
// or alter an already connected wheel. The node is restored before returning.
uintptr_t wheel_part_vtable = 0;
bool wheel_tether_test_active = false; // immutable after hook installation
bool ai_wheel_tethers_active = false; // independent opt-in, immutable after installation
bool tether_collision_active = false;
uintptr_t wheel_joint_vtable = 0;
std::atomic<unsigned> tether_collision_failures{0};
void (*orig_wheel_detach)(void *, uint8_t) = nullptr;

// Fresh live joint only. The stock pair rejection predicate follows body+350
// joints and rejects endpoint pairs when +78==0 and +79!=0. Verify BOTH exact
// endpoints so no road/barrier/other-car pair is excluded by this change.
bool exclude_fresh_wheel_chassis_pair(uintptr_t car,uintptr_t body,uintptr_t connection)
{
    uintptr_t native=0,native_again=0,vt=0,wheel=0,physics=0,chassis_wrapper=0,chassis=0,a=0,b=0;
    uint8_t wrapper[0x28],joint[0x80];
    if (!car || !body || !connection || !wheel_joint_vtable ||
        !guarded_copy(connection,wrapper,sizeof wrapper)) return false;
    memcpy(&native,wrapper+0x18,8);memcpy(&native_again,wrapper+0x20,8);
    if (!native || native_again!=native || !guarded_copy(native,joint,sizeof joint)) return false;
    memcpy(&vt,joint,8);memcpy(&a,joint+8,8);memcpy(&b,joint+0x10,8);
    if (vt!=wheel_joint_vtable || joint[0x30] || joint[0x78] || joint[0x79] ||
        !guarded_read(body+0x18,wheel) || !wheel || a!=wheel ||
        !guarded_read(car+0x558,physics) || !physics ||
        !guarded_read(physics+0x340,chassis_wrapper) || !chassis_wrapper ||
        !guarded_read(chassis_wrapper+0x18,chassis) || !chassis ||
        wheel==chassis || b!=chassis) return false;
    // Same freshly-created writable native joint the original just wrote.
    *reinterpret_cast<volatile uint8_t *>(native+0x79)=1;
    return true;
}

void on_wheel_detach(void *self, uint8_t notify)
{
    td_perf::Scope before(td_perf::wheel_before);
    uintptr_t part=reinterpret_cast<uintptr_t>(self), vt=0, car=0, body=0, connection=0;
    uint8_t vehicle=255, active=1;
    int32_t node=-1;
    int player=player_vehicle.load();
    uint8_t snapshot[0x110];
    bool eligible=(wheel_tether_test_active || ai_wheel_tethers_active || tether_collision_active) && part && wheel_part_vtable &&
        player>=0 && player<max_vehicles && !session_ended.load() &&
        guarded_copy(part,snapshot,sizeof snapshot);
    if (eligible) {
        memcpy(&vt,snapshot,sizeof vt);
        memcpy(&car,snapshot+0x10,sizeof car);
        memcpy(&body,snapshot+0x40,sizeof body);
        memcpy(&node,snapshot+0xe0,sizeof node);
        memcpy(&connection,snapshot+0x100,sizeof connection);
        active=snapshot[0x10c];
    }
    eligible=eligible && vt==wheel_part_vtable && car && !body && !connection && !active && node>=0 &&
        guarded_read(car+0xe8,vehicle) && vehicle<max_vehicles &&
        (int(vehicle)==player ? ((wheel_tether_test_active || tether_collision_active) && car==throttle_hook.car) :
                               (ai_wheel_tethers_active && car!=throttle_hook.car));
    bool remove=eligible && (int(vehicle)!=player || wheel_tether_test_active);
    // This is the game's live, writable self object, validated above, not a
    // cached pointer. The original function also writes it and never frees self.
    // Avoid WriteProcessMemory's protection/bookkeeping overhead in this hot
    // replay path. Only this established field is written.
    auto *node_field=reinterpret_cast<volatile int32_t *>(part+0xe0);
    if (remove) *node_field=-1;
    before.finish();
    {
        td_perf::Scope original(td_perf::wheel_original);
        orig_wheel_detach(self,notify);
    }
    if (!eligible) return;
    td_perf::Scope after(int(vehicle)==player ? td_perf::wheel_player_after : td_perf::wheel_ai_after);
    if (remove) *node_field=node;
    bool restored=*node_field==node;
    uintptr_t live_body=0, live_connection=0;
    uint8_t live_active=1;
    memcpy(&live_body,reinterpret_cast<const void *>(part+0x40),sizeof live_body);
    memcpy(&live_connection,reinterpret_cast<const void *>(part+0x100),sizeof live_connection);
    live_active=*reinterpret_cast<const volatile uint8_t *>(part+0x10c);
    if (!remove) {
        bool applied=live_active && int(vehicle)==player_vehicle.load() && car==throttle_hook.car &&
            !session_ended.load() && exclude_fresh_wheel_chassis_pair(car,live_body,live_connection);
        if (log_telemetry || (!applied && tether_collision_failures.fetch_add(1)<4))
            swofty::log("[td-probe] tether collision experiment: vehicle=%u own_chassis_excluded=%d body=%d joint=%d active=%u",
                unsigned(vehicle),int(applied),int(live_body!=0),int(live_connection!=0),unsigned(live_active));
        return;
    }
    if (!restored || live_connection || live_active || log_telemetry)
        swofty::log("[td-probe] wheel tethers: vehicle=%u node_restored=%d inspected=%d body=%d connection=%d active=%u",
            unsigned(vehicle),int(restored),1,int(live_body!=0),int(live_connection!=0),unsigned(live_active));
}

void configure_player_wheels(std::string_view mode)
{
    wheel_tether_test=mode=="loose";
    bool reduced=mode=="reduced";
    tether_plane_experiment=reduced;
    tether_distance_experiment=reduced;
    tether_collision_experiment=reduced;
}

void hook_wheel_tether_test()
{
    if (!wheel_tether_test && !ai_wheel_tethers && !tether_collision_experiment) return;
    constexpr const char *sig=
        "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 38 03 00 00 C5 78 29 BC 24 20 03 00 00 C5 78 29 B4 24 10 03 00 00 C5 78 29 AC 24 00 03 00 00 C5 78 29 A4 24 F0 02 00 00 C5 78 29 9C 24 E0 02 00 00 C5 78 29 94 24 D0 02 00 00 C5 78 29 8C 24 C0 02 00 00 C5 78 29 84 24 B0 02 00 00 C5 F8 29 BC 24 A0 02 00 00 C5 F8 29 B4 24 90 02 00 00 48 83 79 40 00 74 6E";
    constexpr uint8_t prologue[]={0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53,
                                  0x48,0x81,0xec,0x38,0x03,0x00,0x00};
    uintptr_t at=0;
    if (swofty::api->scan(".xtls",sig,&at)!=1) {
        swofty::log("[td-probe] tether test: detach signature unavailable; stock behavior retained");
        return;
    }
    uintptr_t vt=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x5bf9420, detach=0, update=0;
    uintptr_t expected_update=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x4664080;
    if (!guarded_read(vt+0xa0,detach) || detach!=at || !guarded_read(vt+0x68,update) || update!=expected_update) {
        swofty::log("[td-probe] tether test: wheel vtable mismatch; stock behavior retained");
        return;
    }
    wheel_part_vtable=vt;
    wheel_tether_test_active=bool(wheel_tether_test);
    ai_wheel_tethers_active=bool(ai_wheel_tethers);
    tether_collision_active=bool(tether_collision_experiment) && !wheel_tether_test_active;
    wheel_joint_vtable=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x5a5a890;
    if (!detour_at("wheel tether options",at,prologue,sizeof prologue,
                   reinterpret_cast<void *>(&on_wheel_detach),reinterpret_cast<void **>(&orig_wheel_detach))) {
        wheel_tether_test_active=false;
        ai_wheel_tethers_active=false;
        tether_collision_active=false;
        wheel_part_vtable=0;
    } else {
        swofty::log("[td-probe] wheel tethers: player=%d AI=%d; restart required to change",
                    int(wheel_tether_test_active),int(ai_wheel_tethers_active));
        swofty::log("[td-probe] tether collision experiment: enabled=%d (player removal takes precedence)",int(tether_collision_active));
    }
}
