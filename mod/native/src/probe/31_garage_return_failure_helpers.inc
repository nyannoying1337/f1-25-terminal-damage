// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Return to garage, scripted failure and hotkey helpers.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Practice and qualifying set PitLaneSystem+0x300 (return to garage on pit entry). There the pause menu's Retire
// ends the whole session (every car NOT_IN_SESSION, the AI parks your car, final classification), so a broken car
// is sent back to its garage instead, as the game's own teleportToGarage does: pit state InGarage + crew request 5.

// Every frame after a retirement (susp::tick) and in the failure tick: a guarded copy, not swofty::mem::read, whose
// VirtualQuery cost 260-700 us a frame once the car was out (0.51.119 log 16:32-16:38).
bool practice_like()
{
    uint8_t flag = 0;
    uintptr_t sys = pit_lane_system.load();
    return sys && guarded_read(sys + 0x300, flag) && flag;
}

// Issue #10: the game's own removal of a retired AI car (its virtual session, 0x142f73486): the pit lane system puts
// the car InGarage. Game thread. Never the player's car.
bool send_ai_car_to_garage(uint8_t vehicle)
{
    uintptr_t sys = pit_lane_system.load();
    if (!sys || !orig_set_vehicle_status || vehicle >= 32 || int(vehicle) == player_vehicle.load())
        return false;
    orig_set_vehicle_status(reinterpret_cast<void *>(sys), uint64_t(vehicle), 9);
    return true;
}

// On the game thread (updateVehicle of the player's car).
void return_to_garage()
{
    uint64_t check = exit_check_at.load();
    if (check && GetTickCount64() >= check) {
        exit_check_at = 0;
        if (car_dead) {
            int reason = garage_lock_reason();
            swofty::log("[td-probe] practice/qualifying: your car may not leave (reason %d), it goes back to its "
                        "garage", reason);
            garage_return_wanted = true;
            if (swofty::hud::available()) {
                if (reason == 0) {
                    swofty::hud::toast("CAR WITHDRAWN", "A car that stopped on track is withdrawn from the "
                                       "session. It is ready for the next one.", 5);
                } else if (reason == 1) {
                    swofty::hud::toast("CAR BROKEN", "Fit a new part in the Power Unit menu first", 4);
                } else if (reason == 2 || reason == 4) {
                    char text[160];
                    int minutes = int(garage_repair_left.load() / 60.0f + 0.99f);
                    const char *what = reason == 4 ? "repairing the suspension" : "rebuilding the broken corner";
                    if (minutes > 1)
                        snprintf(text, sizeof text, "The team is still %s: %d minutes left.", what, minutes);
                    else
                        snprintf(text, sizeof text, "The team is still %s: about a minute left.", what);
                    swofty::hud::toast("REPAIR IN PROGRESS", text, 4);
                } else {
                    swofty::hud::toast("CAR WITHDRAWN", "Your car cannot be repaired in this session. It is "
                                       "ready for the next session.", 5);
                }
            }
        }
    }
    if (!garage_return_wanted.exchange(false))
        return;
    uintptr_t sys = pit_lane_system.load();
    int vehicle = player_vehicle.load();
    if (!sys || vehicle < 0 || !orig_set_vehicle_status)
        return;
    swofty::log("[td-probe] practice/qualifying: sending your broken car back to its garage (pit state was %d)%s",
                player_pit.load(), garage_withdrawn.load() ? ", it stopped on track: withdrawn for this session (FIA "
                                                             "B1.8.9), a new part does not bring it back" : "");
    if (player_pit.load() != 9) {
        garage_teleport = true;  // the InGarage message arrives inside this call
        orig_set_vehicle_status(reinterpret_cast<void *>(sys), uint64_t(vehicle), 9);
        garage_teleport = false;
        post_crew_event(5);
    }
    cancel_crew_work();
    player_broken = false;
    retire_requested = false;
    pushing = false;
    retire_in = -1.0;
    car_dead = true;
    garage_part_failed = false;
    garage_failed_types = 0;
    part_lock = false;
}

int choice_int(std::string_view v, int fallback)
{
    int n = 0;
    if (v.empty())
        return fallback;
    for (char ch : v) {
        if (ch < '0' || ch > '9')
            return fallback;
        n = n * 10 + (ch - '0');
    }
    return n;
}

void trigger_failure(int forced_part)
{
    int vehicle = player_vehicle.load();
    int part = forced_part >= 0 ? forced_part : choice_int(debug_failure_part, 0);
    if (session_ended || !orig_update_vehicle || vehicle < 0 || vehicle >= max_vehicles || part < 0 || part > 6) {
        swofty::log("[td-probe] debug failure: no active player update, request ignored");
        return;
    }
    uint64_t request = ((GetTickCount64() + 5000) << 16) | (uint64_t(vehicle) << 8) | uint64_t(part + 1);
    uint64_t empty = 0;
    if (pending_failure.compare_exchange_strong(empty, request))
        swofty::log("[td-probe] debug failure: queued player vehicle %d part %d", vehicle, part);
}

void apply_pending_failure(void *system, uint8_t update_vehicle)
{
    uint64_t request = pending_failure.load();
    if (!request)
        return;
    int vehicle = int((request >> 8) & 0xff);
    if (session_ended || GetTickCount64() >= (request >> 16) || vehicle != player_vehicle.load()) {
        if (pending_failure.compare_exchange_strong(request, 0))
            swofty::log("[td-probe] debug failure: stale player request discarded");
        return;
    }
    if (int(update_vehicle) != vehicle || !pending_failure.compare_exchange_strong(request, 0))
        return;
    int part = int(request & 0xff) - 1;
    uintptr_t sys = reinterpret_cast<uintptr_t>(system);
    uint64_t count = 0;
    uintptr_t rec = 0, car = 0, engine = 0;
    if (vehicle < 0 || !sys || !guarded_read(sys + 0xc20, count) || uint64_t(vehicle) >= count ||
        !guarded_read(sys + 0x50 + uint64_t(vehicle) * 0x70 + 8, rec) || !rec ||
        !guarded_read(rec + 0x58, car) || !car) {
        swofty::log("[td-probe] debug failure: player car not found (vehicle %d, system %p, count %llu)", vehicle,
                    reinterpret_cast<void *>(sys), (unsigned long long)count);
        return;
    }
    uintptr_t slot;
    if (part == 6) {
        if (!guarded_read(car + 0x538, engine) || !engine)
            return;
        slot = engine + 0x674;
    } else {
        if (part < 0 || part > 5 || !guarded_read(car + 0x530, engine) || !engine)
            return;
        slot = engine + part_offsets[part];
    }
    float before = 0;
    if (!guarded_read(slot, before))
        return;
    // The lowest value the damage tables call terminal (1.0 in this build).
    float value = 1.0f;
    for (int k = part == 6 ? 2 : 0; k <= (part == 6 ? 2 : 1); k++) {
        uint64_t n = 0;
        uintptr_t table = sys + damage_table + k * damage_table_stride;
        if (!guarded_read(sys + damage_table_count + k * damage_table_stride, n) || n > 6)
            continue;
        for (uint64_t i = 0; i < n; i++) {
            uint32_t state = 0;
            float threshold = 0;
            if (!guarded_read(table + i * 8, state) || !guarded_read(table + i * 8 + 4, threshold))
                break;
            swofty::log("[td-probe] damage table %d entry %llu: state %u from %.3f", k, (unsigned long long)i, state,
                        threshold);
            if (state == damage_terminal && threshold > 0 && threshold < value)
                value = threshold;
        }
    }
    flashback_tracker.mark(GetTickCount64());
    damage_flashback_tracker.record_damage(part==0 ? 1u : part==6 ? 4u : 2u, GetTickCount64());
    player_broken = true;
    *reinterpret_cast<volatile float *>(slot) = value;
    swofty::log("[td-probe] debug failure: vehicle %d part %d at %p: %.3f -> %.3f", vehicle, part,
                reinterpret_cast<void *>(slot), before, value);
}

// The virtual key of a setting such as "F10": F1 to F12, the fallback key when the text names none.
int function_key(std::string_view k, int fallback)
{
    int n = k.size() > 1 && (k[0] == 'F' || k[0] == 'f') ? choice_int(k.substr(1), fallback) : fallback;
    return n >= 1 && n <= 12 ? VK_F1 + n - 1 : VK_F1 + fallback - 1;
}

bool ctrl_shift_down(int key)
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
           (GetAsyncKeyState(key) & 0x8000);
}
