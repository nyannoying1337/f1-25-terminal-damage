// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Pit state hook, relays, thunk hooks, pit stop job and crew signatures.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

using spawn_crew_fn = void (*)(void *, uint32_t);
spawn_crew_fn spawn_crew;
std::atomic<bool> crew_spawn_now{false};
// With the race spawn, PrepareToPushIntoGarage waits for the box: at pit entry it made the crew
// ready at once and the pit lane jumped the car straight into its box.
std::atomic<bool> prepare_at_box{false};
// Seconds since the race crew was spawned in the box (-1: not running).
std::atomic<double> crew_seq_t{-1.0};

uint64_t on_pit_state_changed(void *ai, void *msg)
{
    uint8_t id = 0xff;
    uint32_t state = 0xffffffff;
    auto base = reinterpret_cast<uintptr_t>(msg);
    if (guarded_read(base + 0x10, id) && id < max_vehicles &&
        guarded_read(base + 0x18, state) && state < 16)
        observed_pit_state[id] = state + 1;
    if (guarded_read(base + 0x10, id) && guarded_read(base + 0x18, state) && is_player(id)) {
        swofty::log("[td-probe] pit state vehicle=%u -> %u %s", id, state, label(pit_states, state));
        player_pit = int(state);
        // The DNF camera is over and the game has put your car away for the results.
        if (state == 0 || state == 9)
            release_retired_engine_sound(id, "car put away after the race");
        if (state == 9 && player_broken && keep_control && practice_like()) {
            // It came in under its own power on one broken corner: the team rebuilds the corner.
            bool rebuild = !garage_teleport && limp_state.load() == 1 && crash_corners.load() &&
                           !(terminal_mask.load() & 0x7) && crash_repair_available() && repair_update_hooked;
            if (rebuild) {
                garage_repair = 1;
                repair_label(!has_detached_corner(crash_corners.load()));
            }
            car_dead = true;
            garage_part_failed = false;
            if (rebuild) {
                swofty::log("[td-probe] garage repair: your car limped into its garage (corners 0x%03x), the team "
                            "rebuilds the broken corner", crash_corners.load());
            } else {
                swofty::log("[td-probe] practice/qualifying: your broken car is in its garage and stays there");
            }
        }
        bool broken = keep_control && retire_in_pit_box && player_broken && retire_in.load() > -2.0;
        bool push = broken && garage_push && crew_event_vtable && restart_cleanup_available;
        // Any crew send-off (also the instant one) makes the pit lane put the car into its box, so the
        // race crew is spawned only once your broken car stands in the box. on_frame then walks it
        // through SendCarToTrack_Inst, PrepareToPushIntoGarage and PushIntoGarage.
        if (broken && state == 4 && push && spawn_crew) {
            swofty::log("[td-probe] garage: car in the box, spawning your crew to push it in (retire in 15 s if not)");
            crew_spawn_now = true;
            if (crew_msg_vtable) {
                crew_msg = crew_msg_pushing;
                crew_msg_in = 1.5;
            }
            pushing = true;
            retire_in = 15.0;
        } else if (broken && state == 4) {
            if (push) {
                swofty::log("[td-probe] garage: car in the box, asking the crew to push it in (retire in 15 s if not)");
                crew_request = crew_push;
                crew_request_in = 0.5;
                // A garage crew waiting to push reports PushingIntoGarage itself once it has request 7.
                if (crew_msg_vtable && crew_state_now.load() != crew_wait_push) {
                    crew_msg = crew_msg_pushing;
                    crew_msg_in = 1.0;
                }
                pushing = true;
                retire_in = 15.0;
            } else if (retire_in.load() < 0) {
                swofty::log("[td-probe] retire: your broken car is in its pit box, retiring in 2 s");
                retire_in = 2.0;
            }
        } else if (broken && pushing && !pushed && (state == 5 || state == 6 || state == 7)) {
            crew_msg_in = -1.0;
            swofty::log("[td-probe] garage: the crew did a pit stop instead of pushing, retiring in the box now");
            retire_in = 0.0;
        } else if (broken && pushing && state == 8) {
            swofty::log("[td-probe] garage: the crew is pushing your car in");
            pushed = true;
            retire_in = 45.0;
            // In the race the game hands the car back to you in the box, so nothing moves it into the garage and
            // it just rolls. Give it to the pit lane AI, as for driving through the pit lane.
            if (void *sys = controller_system.load()) {
                swofty::log("[td-probe] garage: handing your car to the pit lane AI for the push");
                orig_set_controller(sys, id, 1, 0);
            }
            if (crew_msg_vtable) {
                // If the crew never reports the car as in the garage, report it after 35 s (in practice the
                // push takes about 27 s).
                crew_msg = crew_msg_in_garage;
                // In a race the game unloads the garage crews and loads the pit stop crews instead, so the player
                // has a garage crew only once spawn_player_crew has made one (crew_state_now is then >= 0). Without
                // one nobody pushes, and we put the car in the garage after a short stop.
                crew_msg_in = crew_state_now.load() >= 0 ? 35.0 : 3.0;
            }
        } else if (broken && pushing && state == 9) {
            swofty::log("[td-probe] garage: car is in the garage, retiring in 1 s");
            crew_msg_in = -1.0;
            retire_in = 1.0;
        }
    }
    return orig_pit_state_changed(ai, msg);
}

void *near_relay(uintptr_t near_to, void *handler)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t step = si.dwAllocationGranularity;
    uintptr_t lo = near_to > 0x7ff00000 ? near_to - 0x7ff00000 : step;
    for (uintptr_t at = (near_to & ~(step - 1)); at > lo; at -= step) {
        auto *p = static_cast<uint8_t *>(
            VirtualAlloc(reinterpret_cast<void *>(at), step, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!p)
            continue;
        const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
        memcpy(p, jmp, sizeof jmp);
        memcpy(p + 6, &handler, sizeof handler);
        DWORD old;
        VirtualProtect(p, step, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 14);
        return p;
    }
    return nullptr;
}

// Redirect the `jmp rel32` thunk at `at` to handler; returns the thunk's old target via orig. The thunk's rel32 is
// pointed at a near relay that jumps to the handler, and the handler calls the thunk's original target. The two
// targets found by signature (setController, the pit state handler) are L1 entries of the Swofty knowledge base,
// static only and not in bindings.json, so the mixin system cannot hook them.
bool hook_thunk_at(const char *name, uintptr_t at, void *handler, void **orig)
{
    auto *code = reinterpret_cast<uint8_t *>(at);
    if (code[0] != 0xE9) {
        swofty::log("[td-probe] %s at %p is not a jmp thunk, not hooked", name, code);
        return false;
    }
    int32_t rel;
    memcpy(&rel, code + 1, sizeof rel);
    uintptr_t target = at + 5 + int64_t(rel);
    void *relay = near_relay(at, handler);
    if (!relay) {
        swofty::log("[td-probe] %s: no memory within 2 GB for the relay, not hooked", name);
        return false;
    }
    int64_t to_relay = int64_t(reinterpret_cast<uintptr_t>(relay)) - int64_t(at + 5);
    if (to_relay < INT32_MIN || to_relay > INT32_MAX)
        return false;
    *orig = reinterpret_cast<void *>(target);
    int32_t new_rel = int32_t(to_relay);
    DWORD old;
    if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(code + 1, &new_rel, sizeof new_rel);
    VirtualProtect(code, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
    swofty::log("[td-probe] hooked %s at %p (original target %p)", name, code, *orig);
    return true;
}

// The same for a thunk found by sig.
bool hook_thunk(const char *name, const char *sig, void *handler, void **orig)
{
    uintptr_t at = 0;
    int hits = swofty::api->scan(".xtls", sig, &at);
    if (hits != 1 || !at) {
        swofty::log("[td-probe] %s: signature has %d hits, not hooked (other game build?)", name, hits);
        return false;
    }
    return hook_thunk_at(name, at, handler, orig);
}

// In a race the tyre stop comes from PitStopLogic, not from the garage crew. Its StateInGarage (0x1403ba320)
// watches the team's two cars (logic+0x150/+0x151) and, as soon as one is EnteringPitBox or InPitBox, hands it to
// the job start (thunk 0x1403b9ae0, rcx logic, rdx -> u8 vehicle), which jacks the car up and changes tyres. For
// the broken player's car we refuse that call, so the crew stays in the garage and the car is free to be pushed.
// The site: the status check of the second car, then `call` at +0x2d.
constexpr const char *sig_pit_stop_job_site =
    "88 4C 24 2F EB 1F 3C 1A 77 3B 0F B6 C8 48 C1 E1 04 48 8B 4C 0A 68 8B 49 04 83 C1 FD 83 F9 01 77 24 88 44 24 "
    "2F 48 8D 54 24 2F 48 89 F9 E8 ?? ?? ?? ??";
uint64_t (*orig_pit_stop_job)(void *, const uint8_t *);
std::atomic<bool> pit_stop_refused{false};

uint64_t on_pit_stop_job(void *logic, const uint8_t *vehicle)
{
    if (vehicle && int(*vehicle) == player_vehicle.load() && keep_control && retire_in_pit_box && garage_push &&
        player_broken && retire_in.load() > -2.0 && restart_cleanup_available) {
        if (!pit_stop_refused.exchange(true)) {
            swofty::log("[td-probe] garage: no tyre stop for your broken car, the pit crew stays in the garage");
            release_retirement_box_entry();
        }
        return 0;
    }
    return orig_pit_stop_job(logic, vehicle);
}
