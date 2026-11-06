// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Part lock, garage menu and exit hooks, crew state handlers.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Issue #5: a car with a failed power unit part or gearbox may leave its garage only once a new part is chosen. The
// Career allocation (EngineManagementComponent, re/career-part-fitted-state.md) keeps one 0xa60-byte record per
// driver in mgmt+0x2a0..+0x2a8 ([rec+0x80]+0x58 = car) with a holder per component type at [rec+0x18+t*0x10]: units
// +0x08..+0x10 (16 bytes each, wear float at +4), pending unit +0x20, pending state +0x28 (0 none, 1 new unit,
// 2 existing unit), fitted unit +0x30. Confirming a part in the Power Unit menu only sets the pending fit (the car's
// wear resets on the send-out), and the menu can clear it again, so this polls. A type counts as replaced with a new
// unit pending, an existing unit pending whose wear is below 0.9, or a fitted unit that changed. Without a Career
// record (other modes, lookup failed) the old behaviour stays: the swap shows on a send-out attempt.
// on_frame (the SDK's career component lookup must run on the game's flow thread).
void part_lock_tick()
{
    static uintptr_t rec = 0, rec_car = 0;
    static bool missing_logged = false;
    static uint32_t seen = 0;
    static uint64_t fitted_at_fail[7];
    static uint64_t last_key[7];
    static uint64_t retry_at = 0;  // without a record: when the Career lookup may run again
    uintptr_t car = player_car();
    uint32_t types = garage_failed_types.load();
    if (!car_dead || !garage_part_failed || !types || !car || session_ended || !keep_control) {
        part_lock = false;
        rec = 0;
        seen = 0;
        missing_logged = false;
        retry_at = 0;
        return;
    }
    if (!rec || rec_car != car) {
        // The Career lookup is expensive in the core (0.51.123 log: 254 us a frame in a Career practice while it
        // found no record and ran again every frame; the core's flow tick grew too): at most every 2 s.
        uint64_t now = GetTickCount64();
        if (rec_car == car && now < retry_at) {
            part_lock = false;
            return;
        }
        retry_at = now + 2000;
        rec = 0;
        rec_car = car;
        swofty::career::State st{};
        uintptr_t mgmt = 0, begin = 0, end = 0;
        if (swofty::career::state(&st) == swofty::career::Result::ok && st.loaded)
            mgmt = reinterpret_cast<uintptr_t>(swofty::career::component("EngineManagementComponent"));
        if (mgmt && guarded_read(mgmt + 0x2a0, begin) && guarded_read(mgmt + 0x2a8, end) && begin && end > begin &&
            (end - begin) % 0xa60 == 0 && (end - begin) / 0xa60 <= 64)
            for (uintptr_t r = begin; r < end && !rec; r += 0xa60) {
                uintptr_t veh = 0, c = 0;
                if (guarded_read(r + 0x80, veh) && veh && guarded_read(veh + 0x58, c) && c == car)
                    rec = r;
            }
        if (!rec) {
            if (!missing_logged)
                swofty::log("[td-probe] garage: no Career power unit record for your car (career %d, component %p), "
                            "a part swap shows on the send-out", int(st.loaded), reinterpret_cast<void *>(mgmt));
            missing_logged = true;
            part_lock = false;
            return;
        }
        swofty::log("[td-probe] garage: Career power unit record %p, failed parts 0x%x", reinterpret_cast<void *>(rec),
                    types);
    }
    static const char *names[7] = {"ICE", "MGU-K", "MGU-H", "ES", "TC", "CE", "gearbox"};
    bool ready = true;
    for (int t = 0; t < 7; ++t) {
        if (!(types & (1u << t)))
            continue;
        uintptr_t h = 0, ub = 0, ue = 0;
        uint64_t idx = 0, fitted = 0;
        uint32_t state = 0;
        if (!guarded_read(rec + 0x18 + t * 0x10, h) || !h || !guarded_read(h + 0x08, ub) || !guarded_read(h + 0x10, ue) ||
            !guarded_read(h + 0x20, idx) || !guarded_read(h + 0x28, state) || !guarded_read(h + 0x30, fitted) ||
            ue < ub || (ue - ub) / 16 > 256) {
            ready = false;
            continue;
        }
        uint64_t count = (ue - ub) / 16;
        if (!(seen & (1u << t))) {
            seen |= 1u << t;
            fitted_at_fail[t] = fitted;
        }
        float wear = -1;
        if (idx < count)
            guarded_read(ub + idx * 16 + 4, wear);
        bool ok = state == 1 || (state == 2 && idx < count && idx != fitted && wear >= 0 && wear < 0.9f) ||
                  fitted != fitted_at_fail[t];
        uint64_t key = (uint64_t(state) << 56) ^ (idx << 32) ^ (fitted << 8) ^ count;
        if (key != last_key[t]) {
            last_key[t] = key;
            swofty::log("[td-probe] garage: %s pending state %u unit %llu (wear %.3f), fitted %llu (at failure %llu), "
                        "%llu units -> %s",
                        names[t], state, (unsigned long long)idx, double(wear), (unsigned long long)fitted,
                        (unsigned long long)fitted_at_fail[t], (unsigned long long)count,
                        ok ? "replaced" : "not replaced");
        }
        ready = ready && ok;
    }
    bool was = part_lock.exchange(!ready);
    if (was != !ready) {
        swofty::log("[td-probe] garage: %s", ready ? "a new part is fitted, your car may leave the garage"
                                                   : "fit a new part first: drive out is locked");
        if (ready && swofty::hud::available())
            swofty::hud::toast("PART FITTED", "Your car may leave the garage once the work is done", 4);
    }
}
// The garage menu (view model update 0x141bc0940: rcx view model, xmm1 dt, r8d) binds its buttons to bool nodes
// whose handles it keeps at vm+0x390 goto_track_visible, +0x3d0 drive_out_enabled, +0x3f0 flying_lap_enabled,
// +0x450 practice_programmes_enabled, +0x470 pp_flying_lap_enabled, +0x490 pp_drive_out_enabled (created in 0x141bc2f9f), and sets them every update
// with Vm_NodeSet_Bool(node, &value, 0, 0) (0x140973e20, called at +0x4db) after the checks at +0x4f1 (node not the
// empty node, +0x26 bit 0 clear, vtable +0x20 true). It already hides "go to track" while the RepairTimes record
// has jobs (vm+0x33b). For a locked car the addon sets these nodes to false after each update, so the menu offers
// no way out of the garage and its camera never leaves.
using garage_vm_update_fn = uint64_t (*)(void *, float, uint64_t, uint64_t);
garage_vm_update_fn orig_garage_vm_update;
using vm_node_set_bool_fn = void *(*)(void *, uint8_t *, uint64_t, uint64_t);
vm_node_set_bool_fn vm_node_set_bool;
uintptr_t vm_empty_node = 0;  // address of the global holding the empty node
constexpr uint8_t garage_vm_update_prologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
                                                 0x55, 0x53, 0x48, 0x81, 0xEC, 0x58, 0x01, 0x00, 0x00};
void vm_bool_off(uintptr_t vm, uint32_t offset)
{
    uintptr_t node = 0, empty = 0, vtable = 0, valid = 0;
    uint8_t flags = 0;
    if (!guarded_read(vm + offset, node) || !node || !guarded_read(vm_empty_node, empty) || node == empty ||
        !guarded_read(node + 0x26, flags) || (flags & 1) || !guarded_read(node, vtable) || !vtable ||
        !guarded_read(vtable + 0x20, valid) || !valid || !reinterpret_cast<bool (*)(void *)>(valid)(
                                                                  reinterpret_cast<void *>(node)))
        return;
    uint8_t off = 0;
    vm_node_set_bool(reinterpret_cast<void *>(node), &off, 0, 0);
}
uint64_t on_garage_vm_update(void *vm, float dt, uint64_t r8, uint64_t r9)
{
    uint64_t r = orig_garage_vm_update(vm, dt, r8, r9);
    static bool logged = false;
    if (!garage_exit_locked()) {
        logged = false;
    } else {
        if (!logged) {
            logged = true;
            swofty::log("[td-probe] garage: drive out, flying lap and practice programme buttons switched off in the garage menu");
        }
        for (uint32_t offset : {0x390u, 0x3d0u, 0x3f0u, 0x450u, 0x470u, 0x490u})
            vm_bool_off(reinterpret_cast<uintptr_t>(vm), offset);
    }
    return r;
}
void hook_garage_menu()
{
    uintptr_t at = 0;
    int n = swofty::api->scan(".xtls",
                              "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 58 01 00 00 C5 F8 29 B4 24 40 01 00 00 44 "
                              "89 C3 C5 F8 28 F1 48 89 CE 4C 8B",
                              &at);
    uint8_t call = 0, cmp[3] = {};
    int32_t rel = 0, empty_rel = 0;
    bool ok = n == 1 && guarded_read(at + 0x4db, call) && call == 0xE8 && guarded_read(at + 0x4dc, rel) &&
              swofty::mem::readable(at + 0x4f1, 7) && guarded_read(at + 0x4f4, empty_rel);
    if (ok)
        memcpy(cmp, reinterpret_cast<void *>(at + 0x4f1), 3);
    if (!ok || cmp[0] != 0x48 || cmp[1] != 0x3B || cmp[2] != 0x3D) {
        swofty::log("[td-probe] garage menu: update not found (%d hits), drive out stays possible", n);
        return;
    }
    vm_node_set_bool = reinterpret_cast<vm_node_set_bool_fn>(at + 0x4e0 + int64_t(rel));
    vm_empty_node = at + 0x4f8 + int64_t(empty_rel);
    detour_at("garage menu update", at, garage_vm_update_prologue, sizeof garage_vm_update_prologue,
              reinterpret_cast<void *>(&on_garage_vm_update), reinterpret_cast<void **>(&orig_garage_vm_update));
}

using exit_garage_fn = uint64_t (*)(void *, void *);
exit_garage_fn orig_exit_garage;
constexpr uint8_t exit_garage_prologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
                                            0x55, 0x53, 0x48, 0x81, 0xEC, 0x08, 0x01, 0x00, 0x00};
// Why a broken car may not leave its practice/qualifying garage (log and toast when it tries): 0 it stopped on
// track (withdrawn), 1 a failed part is not replaced yet, 2 the corner is still being rebuilt, 3 damage the garage
// cannot repair in this session, 4 the damaged suspension is still being repaired (the garage service).
int garage_lock_reason()
{
    if (garage_withdrawn.load())
        return 0;
    if (part_lock.load() || garage_part_failed || (terminal_mask.load() & 0x7))
        return 1;
    int state = garage_repair.load();
    if (state >= 1 && state <= 3)
        return garage_service_corners.load() ? 4 : 2;
    return 3;
}

// GarageComponent's drive out request MessageRequestVehicleExitGarage (handler 0x142a41120, rcx component, rdx
// message: +0x10 vehicle, +0x14 type). Type 0 is the crew's drive out (InGarage -> InGarage_VehicleWantsToLeave ->
// LeavingGarage), type 1 the instant one (0x142a3a560: InGarage -> OnTrack, ERequest_SendCarToTrack_Inst; a
// practice programme start). The game zooms out of the monitor before it sends the request, so swallowing it left
// the camera out of the monitor with no menu: a softlock (0.51.73; 0.51.125 practice programme start in a withdrawn
// car). A locked car's request goes on as the instant one: on_set_vehicle_status lets the car out to OnTrack and
// return_to_garage sends it back 300 ms later (ReturnToGarage_Inst, the menu comes back) with a toast that says
// why (the same path as a send-out without a new part, 0.51.124).
uint64_t on_exit_garage(void *component, void *msg)
{
    uint8_t vehicle = 0xff;
    uint32_t type = 0;
    uintptr_t m = reinterpret_cast<uintptr_t>(msg);
    if (!garage_exit_locked() || !guarded_read(m + 0x10, vehicle) || int(vehicle) != player_vehicle.load() ||
        !guarded_read(m + 0x14, type))
        return orig_exit_garage(component, msg);
    static const char *const reasons[] = {"your car stopped on track, withdrawn for this session",
                                          "no new part fitted yet", "the corner is still being rebuilt",
                                          "your car is out for this session",
                                          "the suspension is still being repaired"};
    swofty::log("[td-probe] garage: drive out request (type %u) for your locked car (%s): passed on%s, the car "
                "comes straight back", type, reasons[garage_lock_reason()], type == 0 ? " as the instant one" : "");
    if (type == 0)
        *reinterpret_cast<volatile uint32_t *>(m + 0x14) = 1;
    return orig_exit_garage(component, msg);
}
void hook_exit_garage()
{
    detour("GarageComponent drive out request",
           "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 08 01 00 00 48 89 D6 48 89 CF 48 8B 89 F8 01 00 00 4C 8B 87 "
           "00 02 00 00",
           exit_garage_prologue, sizeof exit_garage_prologue, reinterpret_cast<void *>(&on_exit_garage),
           reinterpret_cast<void **>(&orig_exit_garage));
}

template <int... I> void crew_handlers(std::integer_sequence<int, I...>, void **out)
{
    ((out[I] = reinterpret_cast<void *>(&on_crew_state<I>)), ...);
}
