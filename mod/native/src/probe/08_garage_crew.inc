// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// The player's garage crew: spawn and removal.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// GarageCrewModule::onSessionLoaded (0x14038f360) spawns the garage crews for practice, qualifying and time trial,
// but for the race session types (15..17) only when feature flag byte 7 bit 3 is set, and destroys them otherwise.
// The site is that check; turning its `jne` (75 70 at +0x1f) into `jmp` spawns them in the race too.
constexpr const char *sig_garage_crew_spawn =
    "48 8B 46 20 48 8B B8 A8 01 00 00 8B 47 04 8D 48 F1 83 F9 02 77 0E E8 ?? ?? ?? ?? F6 40 07 08 75 70 8B 47 04 83 "
    "F8 12 74 68 FF C8 83 F8 0D 76 61 48";

// Defined in 16_detour.inc (declared here for the fragments before it).
bool detour(const char *name, const char *sig, const uint8_t *prologue, size_t len, void *handler, void **orig);
bool detour_at(const char *name, uintptr_t at, const uint8_t *prologue, size_t len, void *handler, void **orig);
// onSessionLoaded spawns every crew via SpawnGarageCrew(module, vehicle) only
// outside races; enabling that for the race parks all 20 cars in their garages.
// Instead, spawn only the player's crew once the car is broken, from on_frame.
void **crew_module;
std::atomic<bool> crew_spawn_wanted{false};
using destroy_crew_fn = void (*)(void *);
destroy_crew_fn destroy_crew;
uintptr_t owned_crew_module=0, owned_crew_instance=0;
int owned_crew_vehicle=-1;
void (*orig_damage_session_restart)(void *);
void *(*hud_visibility_manager)();
void (*update_hud_visibility)(void *);

// Garage report3 adds reason2; report4 normally clears it. A restart during
// our push skips report4. Only release that bit for the offline primary HUD.
void release_push_hud_hide(bool owned_push)
{
    if (!owned_push || !hud_visibility_manager || !update_hud_visibility) return;
    void *manager=hud_visibility_manager();
    uint32_t before=0;
    if (!manager || !guarded_read(reinterpret_cast<uintptr_t>(manager),before)) return;
    if (!(before & 4)) {
        swofty::log("[td-probe] restart HUD: own push ended, hide mask=0x%x; no push reason to clear",before);
        return;
    }
    InterlockedAnd(reinterpret_cast<volatile LONG *>(manager),~LONG(4));
    update_hud_visibility(manager);
    uint32_t after=0;guarded_read(reinterpret_cast<uintptr_t>(manager),after);
    swofty::log("[td-probe] restart HUD: released garage push reason2, primary hide mask 0x%x -> 0x%x; other reasons retained",before,after);
}
void find_hud_restart_cleanup()
{
    uintptr_t getter=0,update=0;
    const char *getter_site_sig="48 8B 8E 78 02 00 00 0F B6 57 10 E8 ?? ?? ?? ?? 48 89 C3 E8 ?? ?? ?? ?? 48 85 DB";
    const char *update_sig="56 57 53 48 83 EC 30 48 89 CE 83 39 00 0F 94 44 24 2F 48 8B 79 68 48 85 FF";
    uintptr_t site=0;
    if (swofty::api->scan(".xtls",getter_site_sig,&site)!=1 || !site ||
        swofty::api->scan(".xtls",update_sig,&update)!=1 || !update) {
        swofty::log("[td-probe] restart HUD: visibility API signatures unavailable; no override");return;
    }
    int32_t rel=0;memcpy(&rel,reinterpret_cast<void *>(site+20),4);
    getter=site+24+int64_t(rel);
    constexpr uint8_t prefix[]={0x48,0x83,0xec,0x28,0x8b,0x05};
    if (memcmp(reinterpret_cast<void *>(getter),prefix,sizeof prefix)) return;
    hud_visibility_manager=reinterpret_cast<void *(*)()>(getter);
    update_hud_visibility=reinterpret_cast<void (*)(void *)>(update);
    swofty::log("[td-probe] restart HUD: resolved visibility manager=%p update=%p",reinterpret_cast<void *>(getter),reinterpret_cast<void *>(update));
}




void find_garage_crew_spawn()
{
    if (!garage_crew_in_race)
        return;
    if (!garage_assets_in_race) {
        swofty::log("[td-probe] garage: crew in the race needs \"Garage crew assets in the race\", off");
        return;
    }
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_garage_crew_spawn, &site);
    auto *code = reinterpret_cast<const uint8_t *>(site);
    if (n != 1 || code[0xb3] != 0x48 || code[0xb4] != 0x8B || code[0xb5] != 0x35 || code[0x154] != 0xE8 || code[0x7e] != 0xE8) {
        swofty::log("[td-probe] garage: crew spawn site has %d hits, no garage crew in the race", n);
        return;
    }
    int32_t rel;
    memcpy(&rel, code + 0xb6, 4);
    crew_module = reinterpret_cast<void **>(site + 0xba + int64_t(rel));
    memcpy(&rel, code + 0x7f, 4);
    destroy_crew = reinterpret_cast<destroy_crew_fn>(site + 0x83 + int64_t(rel));
    memcpy(&rel, code + 0x155, 4);
    spawn_crew = reinterpret_cast<spawn_crew_fn>(site + 0x159 + int64_t(rel));
    // SpawnGarageCrew is called, not hooked: check that the call target is the function this build has there.
    constexpr uint8_t spawn_crew_prologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
                                               0x55, 0x53, 0x48, 0x81, 0xEC, 0xF8, 0x03, 0x00, 0x00};
    auto spawn_at = reinterpret_cast<uintptr_t>(spawn_crew);
    if (!swofty::mem::readable(spawn_at, sizeof spawn_crew_prologue) ||
        memcmp(reinterpret_cast<const void *>(spawn_at), spawn_crew_prologue, sizeof spawn_crew_prologue)) {
        swofty::log("[td-probe] garage: SpawnGarageCrew does not match, no garage crew in the race");
        spawn_crew = nullptr;
        return;
    }

    swofty::log("[td-probe] garage: your crew spawns in the race once your car is broken (module %p, spawn %p)",
                crew_module, reinterpret_cast<void *>(spawn_crew));
}

// Pit box slot of each vehicle, learned from the live race pitstop (pit_retirement_clearance.hpp).
std::atomic<int> pit_slot_of[0x20];

bool spawn_player_crew()
{
    int vehicle = player_vehicle.load();
    void *module = crew_module ? *crew_module : nullptr;
    if (!spawn_crew || !module || vehicle < 0 || vehicle >= 0x20)
        return false;
    uintptr_t instance = 0;
    if (!guarded_read(reinterpret_cast<uintptr_t>(module) + 0x68 + uintptr_t(vehicle) * 8, instance))
        return false;
    if (instance) {
        swofty::log("[td-probe] garage: your crew already exists (%p), no spawn", reinterpret_cast<void *>(instance));
        return true;
    }
    // Resolve CURRENT garage spawn assignment, not the pitstop visibility slot
    // or a vehicle-ID cache learned in a previous session.
    auto assignment = current_garage_assignment(reinterpret_cast<uintptr_t>(module), vehicle, guarded_copy);
    if (assignment.slot < 0) {
        swofty::log("[td-probe] garage: no validated current spawn assignment for vehicle %d; no spawn", vehicle);
        return false;
    }
    uint32_t slot = uint32_t(assignment.slot);
    uintptr_t table = 0, entry = 0;
    uint32_t entry_slot = 0xffffffff;
    guarded_read(reinterpret_cast<uintptr_t>(module) + 0x60, table);
    if (table)
        guarded_read(table + 0x838 + uintptr_t(slot) * 8, entry);
    if (entry)
        guarded_read(entry + 0x18, entry_slot);
    if (!entry || entry_slot != slot) {
        swofty::log("[td-probe] garage: garage slot %u for vehicle %d missing (entry %p, slot %u), no spawn", slot,
                    vehicle, reinterpret_cast<void *>(entry), entry_slot);
        return false;
    }
    swofty::log("[td-probe] garage: spawning garage slot %u for vehicle %d", slot, vehicle);
    spawn_crew(module, slot);
    // The garage spawns a crew for both its cars, and their send-off moves both cars
    // into the pit lane. Remove the other car's crew the way onSessionLoaded does in races:
    // Destroy(instance), then free it through [module+0x18]'s vtable +0x38.
    auto m = reinterpret_cast<uintptr_t>(module);
    for (int v = 0; v < 0x20; v++) {
        uintptr_t other = 0, alloc = 0, vt = 0, free_fn = 0;
        if (v != assignment.teammate || !guarded_read(m + 0x68 + uintptr_t(v) * 8, other) ||
            !other)
            continue;
        if (!destroy_crew || !guarded_read(m + 0x18, alloc) || !alloc || !guarded_read(alloc, vt) ||
            !guarded_read(vt + 0x38, free_fn) || !free_fn) {
            swofty::log("[td-probe] garage: cannot remove the crew of vehicle %d", v);
            continue;
        }
        destroy_crew(reinterpret_cast<void *>(other));
        reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(free_fn)(alloc, other);
        *reinterpret_cast<volatile uintptr_t *>(m + 0x68 + uintptr_t(v) * 8) = 0;
        swofty::log("[td-probe] garage: removed the crew of teammate vehicle %d (%p)", v, reinterpret_cast<void *>(other));
    }
    guarded_read(reinterpret_cast<uintptr_t>(module) + 0x68 + uintptr_t(vehicle) * 8, instance);
    swofty::log("[td-probe] garage: crew spawn returned, instance %p", reinterpret_cast<void *>(instance));
    if (instance) {
        owned_crew_module=reinterpret_cast<uintptr_t>(module);
        owned_crew_instance=instance; owned_crew_vehicle=vehicle;
    }
    return instance != 0;
}

// This callback is part of the game's real restart path, not replay flag restore.
// Its original loops the live module+68 vehicle table and restarts every crew.
// Remove ONLY the instance we created before that loop can put the car in a bay.
bool remove_owned_race_crew(void *module, const char *why = "restart")
{
    uintptr_t base=reinterpret_cast<uintptr_t>(module), live=0, alloc=0, vt=0, free_fn=0;
    if (!owned_crew_instance || owned_crew_module!=base || owned_crew_vehicle<0 || owned_crew_vehicle>=32) return true;
    uintptr_t slot=base+0x68+uintptr_t(owned_crew_vehicle)*8;
    if (!guarded_read(slot,live)) return false;
    if (live!=owned_crew_instance) {
        owned_crew_instance=owned_crew_module=0;owned_crew_vehicle=-1;return true;
    }
    if (!destroy_crew || !guarded_read(base+0x18,alloc) || !alloc ||
        !guarded_read(alloc,vt) || !vt || !guarded_read(vt+0x38,free_fn) || !free_fn) return false;
    *reinterpret_cast<uintptr_t *>(slot)=0;
    destroy_crew(reinterpret_cast<void *>(live));
    reinterpret_cast<void (*)(uintptr_t,uintptr_t)>(free_fn)(alloc,live);
    owned_crew_instance=owned_crew_module=0;owned_crew_vehicle=-1;
    swofty::log("[td-probe] %s: removed only our spawned race garage crew", why);
    return true;
}
void reset_for_session_restart(void *module)
{
    if (!remove_owned_race_crew(module))
        swofty::log("[td-probe] restart: owned crew cleanup failed validation; stock restart forwarded unchanged");
    reset_car_state();
    player_controller_flags=0;
    flashback_tracker.invalidate_session();damage_flashback_tracker.invalidate_session();
    for (auto &slot:pit_slot_of) slot=-1;
    for (auto &state:observed_pit_state) state=0;
    telemetry_not_before=GetTickCount64();
    test_run_restarted=true;  // 0.51.114 log: the run went on into the restarted race
    // A restart has no session end or start (onSessionRestart, then race.start): the last race's power and brake
    // faults, failure plans and suspension damage went on into the new race (0.51.117 review). The suspension request
    // is safe from any thread; the rest is frame-thread state, reset on the next frame.
    susp::request_reset("restart");
    race_restart_pending = true;
    swofty::log("[td-probe] restart lifecycle: cleared retirement and future work");
}
// Engine subsystem lifecycle callback, vtable1457f5a08. Unlike the garage-only
// helper, this is dispatched through the general session restart lifecycle.
void on_damage_session_restart(void *self)
{
    uintptr_t module=0;
    if (crew_module) guarded_read(reinterpret_cast<uintptr_t>(crew_module),module);
    bool owned_push=module && owned_crew_module==module && owned_crew_instance && (pushing || pushed);
    reset_for_session_restart(reinterpret_cast<void *>(module));
    swofty::log("[td-probe] restart lifecycle: CarbonDamage onSessionRestart, module=%p owned_remaining=%p; cleared future work before original",
        reinterpret_cast<void *>(module),reinterpret_cast<void *>(owned_crew_instance));
    orig_damage_session_restart(self);
    release_push_hud_hide(owned_push);
}
bool hook_damage_session_restart()
{
    constexpr uint8_t prologue[]={0x56,0x57,0x53,0x48,0x83,0xec,0x20,0x48,0x89,0xce,0x48,0x8b,0x41,0x60};
    return detour("CarbonDamageSystem::onSessionRestart",
        "56 57 53 48 83 EC 20 48 89 CE 48 8B 41 60 48 8B 80 88 00 00 00 48 85 C0 74 5B",
        prologue,sizeof prologue,reinterpret_cast<void *>(&on_damage_session_restart),reinterpret_cast<void **>(&orig_damage_session_restart));
}

void hook_pit_stop_job()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_pit_stop_job_site, &site);
    if (n != 1) {
        swofty::log("[td-probe] garage: pit stop job signature has %d hits, the crew will change tyres", n);
        return;
    }
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void *>(site + 0x2e), 4);
    hook_thunk_at("PitStopLogic job start", site + 0x32 + int64_t(rel), reinterpret_cast<void *>(&on_pit_stop_job),
                  reinterpret_cast<void **>(&orig_pit_stop_job));
}

void cancel_crew_work()
{
    crew_spawn_now=false; crew_spawn_wanted=false;
    crew_seq_t=-1; crew_request=0; crew_request_in=-1;
    crew_msg=0; crew_msg_in=-1; pending_crew=-1;
    crew_state_now=-1; sendoff_stage=0;
    pushing=false; pushed=false; prepare_at_box=false; pit_stop_refused=false;
}

// A flashback rewinds the car, its pit state and the damage record, but not the race garage crew the addon spawned
// for the push (in a race the box has no garage crew of its own): it stayed standing in the box (issue #8), and a
// later box arrival reused it ("already exists") although it no longer takes the push requests. Remove it from
// on_frame, the thread that spawned it; the next box arrival spawns a fresh one. Practice never owns a crew.
void remove_owned_crew_after_flashback()
{
    if (!owned_crew_instance)
        return;
    uintptr_t module = 0;
    if (crew_module)
        guarded_read(reinterpret_cast<uintptr_t>(crew_module), module);
    bool push_hud = owned_crew_module == module;
    if (!module || !remove_owned_race_crew(reinterpret_cast<void *>(module), "flashback")) {
        swofty::log("[td-probe] flashback: our race garage crew failed validation, left in place");
        return;
    }
    // The crew's push message 3 hides the HUD (reason bit 4) until message 4, which never comes after the rewind.
    if (push_hud && player_pit.load() != 8)
        release_push_hud_hide(true);
}
