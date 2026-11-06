// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Stopped cars and debris hooks, vtable hooks, crew state logging.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

void on_stopped_cars_update(void *self)
{
    td_perf::Scope before(td_perf::stopped_before);
    auto base = reinterpret_cast<uintptr_t>(self);
    uintptr_t mine = 0;
    uint8_t flags = 0;
    int vehicle = player_vehicle;
    if (keep_control && player_broken && vehicle >= 0) {
        static thread_local size_t hint = 0;
        // Kernel-guarded copies avoid expensive VirtualQuery walks of the game's
        // huge heap regions. Failed/partial reads never produce a usable pointer.
        mine = find_stopped_player(base, vehicle, flags, hint, guarded_copy);
    }
    if (mine && !(flags & 8))
        *reinterpret_cast<volatile uint8_t *>(mine + 0x31) = uint8_t(flags | 8);
    before.finish();
    orig_stopped_cars_update(self);
    td_perf::Scope after(td_perf::stopped_after);
    if (mine && !(flags & 8))
        *reinterpret_cast<volatile uint8_t *>(mine + 0x31) = flags;

    if (!log_telemetry)
        return;

    static uint32_t last_points = 0;
    static bool excluded_logged = false;
    uint32_t points = 0;
    if (guarded_read(base + 0x80, points) && points != last_points) {
        swofty::log("[td-probe] safety car points from stopped cars: %u -> %u", last_points, points);
        last_points = points;
    }
    if (bool(mine) != excluded_logged) {
        excluded_logged = bool(mine);
        swofty::log("[td-probe] keep_control: your car %s the stopped-car count for safety car and red flag",
                    mine ? "is left out of" : "is back in");
    }
}

void on_debris_update(void *self)
{
    orig_debris_update(self);
    static uint32_t last_points = 0;
    uint32_t points = 0;
    if (guarded_read(reinterpret_cast<uintptr_t>(self) + 0x4c, points) && points != last_points) {
        swofty::log("[td-probe] safety car points from debris: %u -> %u", last_points, points);
        last_points = points;
    }
}

// Point the vtable slot (in .xtext) that holds the function found by sig at handler.
bool hook_vslot(const char *name, const char *sig, void *handler, void **orig)
{
    uintptr_t fn = 0, slot = 0;
    if (swofty::api->scan(".xtls", sig, &fn) != 1 || !fn) {
        swofty::log("[td-probe] %s: signature not found, not hooked", name);
        return false;
    }
    char ptr_sig[8 * 3 + 1];
    for (int i = 0; i < 8; i++)
        snprintf(ptr_sig + i * 3, 4, "%02X ", unsigned(uint8_t(fn >> (8 * i))));
    ptr_sig[8 * 3 - 1] = 0;
    int n = swofty::api->scan(".xtext", ptr_sig, &slot);
    if (n != 1 || !slot) {
        swofty::log("[td-probe] %s: %d vtable slots hold %p, not hooked", name, n, reinterpret_cast<void *>(fn));
        return false;
    }
    *orig = reinterpret_cast<void *>(fn);
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void *>(slot), 8, PAGE_READWRITE, &old))
        return false;
    memcpy(reinterpret_cast<void *>(slot), &handler, 8);
    VirtualProtect(reinterpret_cast<void *>(slot), 8, old, &old);
    swofty::log("[td-probe] hooked %s (vtable slot %p)", name, reinterpret_cast<void *>(slot));
    return true;
}

void find_status_request()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_status_request_site, &site);
    if (n != 1) {
        swofty::log("[td-probe] retire: status request signature has %d hits, no retiring in the pit box", n);
        return;
    }
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void *>(site + 3), 4);
    bus_holder = site + 7 + int64_t(rel);
    memcpy(&rel, reinterpret_cast<void *>(site + 0x16), 4);
    status_request_vtable = reinterpret_cast<const void *>(site + 0x1a + int64_t(rel));
    swofty::log("[td-probe] retire: status request vtable %p, bus holder %p", status_request_vtable,
                reinterpret_cast<void *>(bus_holder));
}

void find_crew_event()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_crew_event_site, &site);
    if (n != 1) {
        swofty::log("[td-probe] garage: crew event signature has %d hits, no pushing into the garage", n);
        return;
    }
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void *>(site + 0x20), 4);
    crew_event_vtable = reinterpret_cast<const void *>(site + 0x24 + int64_t(rel));
    swofty::log("[td-probe] garage: crew event vtable %p", crew_event_vtable);
}

void find_crew_msg()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_crew_msg_site, &site);
    if (n != 1) {
        swofty::log("[td-probe] garage: crew report signature has %d hits, cannot start the push myself", n);
        return;
    }
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void *>(site + 0x17), 4);
    crew_msg_vtable = reinterpret_cast<const void *>(site + 0x1b + int64_t(rel));
    swofty::log("[td-probe] garage: crew report vtable %p", crew_msg_vtable);
}

void find_pit_entry()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_pit_entry_site, &site);
    if (n != 1) {
        swofty::log("[td-probe] garage: pit entry signature has %d hits, the crew will do a normal stop", n);
        return;
    }
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void *>(site + 0x29), 4);
    pit_entry_vtable = reinterpret_cast<const void *>(site + 0x2d + int64_t(rel));
    swofty::log("[td-probe] garage: MessageCarEnteredPitLane vtable %p", pit_entry_vtable);
}

void log_crew_state(int which, void *logic_ptr)
{
    bool diagnostics = crew_diagnostics_active.load(std::memory_order_relaxed);
    // Stock crews need no inspection during healthy driving. Keep gameplay
    // tracking for an active broken-car push, independently of diagnostics.
    if (!diagnostics && !player_broken && !pushing)
        return;
    auto logic = reinterpret_cast<uintptr_t>(logic_ptr);
    uintptr_t first = 0, last = 0, car = 0, rules = 0, obj = 0, vt = 0;
    uint8_t vehicle = 0xff, armed = 0;
    if (!guarded_read(logic + 0x70, car) || !car || !guarded_read(car + 0x120, vehicle) ||
        int(vehicle) != player_vehicle.load())
        return;
    uint32_t request = 0, next = 0;
    bool changed = crew_state_now.exchange(which) != which;
    if (diagnostics) {
        guarded_read(logic + 0x80, next);
        guarded_read(logic + 0x90, armed);
    }
    if (diagnostics && changed)
        swofty::log("[td-probe] crew: your crew is now in %s (next %u, armed %u)", crew_states[which].name, next, armed);
    // In a race the crew starts in StateCarIsReadyForTrack although the car is on the grid, and only
    // StateSendCarToTrack_P3 takes PrepareToPushIntoGarage. So we run the send-off once while the car is on track,
    // as the game does when you leave the garage in practice: with the instant request (SendCarToTrack_Inst), which
    // goes straight to the end state. The crew's own SendToTrack report changes nothing while the car is OnTrack.
    uintptr_t sys = pit_lane_system.load(), entry = 0;
    uint32_t pit = 0xffffffff;
    if (!session_ended && player_broken && pushing && crew_spawn_wanted &&
        garage_crew_in_race && keep_control && sys &&
        guarded_read(sys + 0x68 + uintptr_t(vehicle) * 16, entry) && entry && guarded_read(entry + 4, pit) &&
        (pit == 1 || pit == 2 || pit == 4)) {
        int expected = 0;
        if (which == crew_ready_for_track && sendoff_stage.compare_exchange_strong(expected, 1)) {
            // ERequest_SendCarToTrack_Inst (2) goes straight to StateSendCarToTrack_P3 without the
            // send-off sequence; ERequest_SendCarToTrack (0) played it and moved the car into the box.
            swofty::log("[td-probe] crew: your car is out, putting your crew straight into its send-off end state");
            pending_crew = 2;
            sendoff_stage = 2;
        }
    }
    if (!diagnostics || !guarded_read(logic + 0xa0, first) || !guarded_read(logic + 0xa8, last) || first == last)
        return;
    guarded_read(first + 0x14, request);
    if (guarded_read(logic + 0x78, rules) && rules && guarded_read(rules + 0x88, obj) && obj &&
        guarded_read(obj, vt) && vt) {
        uintptr_t f30 = 0, f38 = 0;
        if (guarded_read(vt + 0x30, f30) && guarded_read(vt + 0x38, f38) && f30 && f38) {
            // These are predicates with additional game-owned arguments, not bool(this).
            // Calling +0x30 from this diagnostic crashed on a qualifying garage return:
            // 0x14043e2f0 reads its second argument (rdx+0x90). Log addresses only.
            static uint64_t last_key;
            uint64_t key = uint64_t(which) | uint64_t(request) << 8 | uint64_t(armed) << 40 |
                           uint64_t(next & 0x7fff) << 48 | 1ull << 63;
            if (key == last_key)
                return;
            last_key = key;
            swofty::log("[td-probe] crew: %s gets request %u (queue %llu, armed %u, next %u); check +0x30 %p, "
                        "+0x38 %p, object vtable %p (addresses only)",
                        crew_states[which].name, request, (unsigned long long)((last - first) / 0x18), armed, next,
                        reinterpret_cast<void *>(f30), reinterpret_cast<void *>(f38),
                        reinterpret_cast<void *>(vt));
            return;
        }
    }
    swofty::log("[td-probe] crew: %s gets request %u (queue %llu, armed %u, next %u), no check object",
                crew_states[which].name, request, (unsigned long long)((last - first) / 0x18), armed, next);
}

template <int I> uint64_t on_crew_state(void *state, void *logic)
{
    {
        td_perf::Scope sample(td_perf::crew);
        log_crew_state(I, logic);
    }
    return crew_states[I].orig(state, logic);
}
