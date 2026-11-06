// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// The exported player state API and the bus message wrap.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, at file scope.
// It is not a standalone header; it uses what the fragments before it define.

extern "C" SWOFTY_EXPORT const TdApi *TerminalDamageGetApi(uint32_t version)
{
    return version==TERMINAL_DAMAGE_API_VERSION ? &td_state::api : nullptr;
}
static_assert(std::is_same_v<decltype(&TerminalDamageGetApi), TerminalDamageGetApiFn>,
              "the export must match the function type terminal_damage_api.h documents");

SWOFTY_WRAP(Ne_Post_BusMessage, uint64_t, void *bus, void *msg)
{
    if (status_request_vtable && msg && *static_cast<const void *const *>(msg) == status_request_vtable) {
        uint32_t packed;
        memcpy(&packed, static_cast<const uint8_t *>(msg) + 0x10, 4);
        if (((packed >> 8) & 0xff) == 7 && int(packed & 0xff) == player_vehicle.load())
            status7_at = GetTickCount64();
        swofty::log("[td-probe] MessageVehicleStatusRequest vehicle=%u status=%u field=%u flag=%u", packed & 0xff,
                    (packed >> 8) & 0xff, (packed >> 16) & 0xff, packed >> 24);
    }
    if (crew_event_vtable && msg && *static_cast<const void *const *>(msg) == crew_event_vtable) {
        auto *b = static_cast<const uint8_t *>(msg);
        uint32_t request;
        memcpy(&request, b + 0x14, 4);
        if (int(b[0x10]) == player_vehicle.load()) {
            swofty::log("[td-probe] MessageTriggerGarageCrewEvent vehicle=%u request=%u", b[0x10], request);
            // A dead car is not sent out again by request 0 SendCarToTrack. Request 2 SendCarToTrack_Inst goes
            // through: its crew moves the car to the pit exit, and the OnTrack check sends it back unless a part
            // was replaced (0.32.3: without the crew the car stayed at the garage and drove into the pit lane).
            if (car_dead && request == 0) {
                swofty::log("[td-probe] practice/qualifying: your car is broken, it stays in the garage");
                return 0;
            }
        }
    }
    if (crew_msg_vtable && msg && *static_cast<const void *const *>(msg) == crew_msg_vtable) {
        auto *b = static_cast<const uint8_t *>(msg);
        uint32_t code;
        memcpy(&code, b + 0x14, 4);
        if (int(b[0x10]) == player_vehicle.load())
            swofty::log("[td-probe] crew report vehicle=%u code=%u", b[0x10], code);
    }
    if (pit_entry_vtable && crew_event_vtable && msg && *static_cast<const void *const *>(msg) == pit_entry_vtable) {
        uint8_t vehicle = static_cast<const uint8_t *>(msg)[0x10];
        if (int(vehicle) == player_vehicle.load() && keep_control && retire_in_pit_box && garage_push &&
            player_broken && retire_in.load() > -2.0 && restart_cleanup_available) {
            // Like practice: no box stop, the crew waits to push the car into the garage.
            struct {
                const void *vtable;
                uint64_t zero;
                uint8_t vehicle;
                uint8_t pad[3];
                uint32_t request;
                uint8_t tail[0x28];
            } crew{};
            crew.vtable = crew_event_vtable;
            crew.vehicle = vehicle;
            crew.request = crew_prepare_push;
            if (spawn_crew) {
                swofty::log("[td-probe] garage: dropped MessageCarEnteredPitLane of your broken car, the crew gets "
                            "ready once the car reaches its box");
                pushing = true;
                prepare_at_box = true;
                return 0;
            }
            swofty::log("[td-probe] garage: dropped MessageCarEnteredPitLane of your broken car, asking the crew to "
                        "get ready to push it in instead");
            pushing = true;
            orig(bus, &crew);
            return 0;
        }
    }
    const void *vt = terminal_vtable.load(std::memory_order_relaxed);
    if (vt && msg && *static_cast<const void *const *>(msg) == vt) {
        auto *bytes = static_cast<const uint8_t *>(msg);
        uint8_t vehicle = bytes[0x10];
        uint32_t mask;
        memcpy(&mask, bytes + 0x14, 4);
        bool mine = int(vehicle) == player_vehicle.load();
        swofty::log("[td-probe] MessageVehicleTerminallyDamaged vehicle=%u parts=0x%x player=%d", vehicle, mask, mine);
        if (!mine && vehicle < 32) {
            ai_watch_since[vehicle] = GetTickCount64();
            ai_watch_still_since[vehicle] = 0;
            ai_watch_asked.fetch_and(~(1u << vehicle));
            ai_watch_mask.fetch_or(1u << vehicle);
            susp::ai_terminal(vehicle);
        }
        if (mine) {
            if (keep_control) {
                swofty::log("[td-probe] keep_control: dropped terminal damage message of vehicle %u", vehicle);
                terminal_mask.fetch_or(mask);
                flashback_tracker.mark(GetTickCount64());
                damage_flashback_tracker.record_damage(mask, GetTickCount64());
                // The first damage of a whole car and only aero or body parts: it still drives, so it is called in.
                if (!player_broken.exchange(true) && !(mask & 0x1f))
                    heavy_damage_call(mask);
                return 0;
            }
        }
    }
    return orig(bus, msg);
}
