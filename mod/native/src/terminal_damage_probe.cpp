// SPDX-License-Identifier: MIT
//
// Terminal Damage Probe: logs what the game does when the player's car suffers terminal damage
// (engine blow-up, crash) so we can see which call takes control away and where the pit / garage
// states go. Optional experiment: keep_control drops the TERMINAL_DAMAGE hand-over to the AI.
//
// The two targets are L1 entries of the Swofty knowledge base (static only, not in bindings.json),
// so the mixin system cannot hook them. Both are 5-byte `jmp rel32` thunks in .xtls, which we
// redirect ourselves: the thunk's rel32 is pointed at a near relay that jumps to our handler, and
// the handler calls the thunk's original target.

#include <swofty/events.hpp>
#include <swofty/game/core.hpp>
#include <swofty/mem.hpp>
#include <swofty/mixin.hpp>
#include <swofty/telemetry.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>
#include <winsock2.h>
#include <windows.h>

// The SDK header casts GetProcAddress to the table getter.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#include <swofty/hud.hpp>
#pragma clang diagnostic pop

SWOFTY_SETTING(bool, keep_control, true);
SWOFTY_SETTING(bool, engine_off_sound, true);
SWOFTY_SETTING(bool, pause_menu_retire, true);
SWOFTY_SETTING(bool, log_telemetry, false);
SWOFTY_SETTING(bool, debug_failure_hotkey, false);
SWOFTY_SETTING(std::string_view, debug_failure_key, "F10");
SWOFTY_SETTING(std::string_view, debug_failure_part, "0");
SWOFTY_SETTING(std::string_view, limp_level, "off");
SWOFTY_SETTING(std::string_view, damaged_power, "0.3");
SWOFTY_SETTING(bool, retire_in_pit_box, true);
SWOFTY_SETTING(bool, garage_push, true);
SWOFTY_SETTING(bool, garage_crew_in_race, true);
SWOFTY_SETTING(bool, retire_hotkey, true);
SWOFTY_SETTING(std::string_view, retire_key, "F11");
SWOFTY_SETTING(bool, crew_asset_diagnostics, false);
SWOFTY_SETTING(std::string_view, udp_port_choice, "20777");
SWOFTY_SETTING(bool, garage_assets_in_race, true);

namespace {

// Steam build d873a1c8d2044cef, re/db/signatures.json
constexpr const char *sig_set_controller = "E9 ?? ?? ?? ?? 28 E9 92 0F 9D 01 00 5A D0";
constexpr const char *sig_pit_state_changed = "E9 ?? ?? ?? ?? F4 49 5A 39 87 21 7D F8 4B";

// Found in F1_25.exe d873a1c8d2044cef, not in the knowledge base yet:
// VehicleTerminalDamageSystem::updateVehicle(system, u8 vehicle) at 0x142f16490 runs every frame per car. Its
// records are system+0x50+vehicle*0x70 (count at +0xc20); record+8 -> +0x58 is the car, car+0x530 the engine.
// The AI failure planner (0x1409b2940) breaks a power-unit part by writing its failure value into the engine:
// ICE +0x254c, MGU-K +0x255c, MGU-H +0x2558, ES +0x2554, TC +0x2550, CE +0x2560 (gearbox: car+0x538, +0x674).
// The terminal check takes the max of those, so 1.0 there is a failed part.
constexpr const char *sig_update_vehicle =
    "41 57 41 56 41 54 56 57 55 53 48 83 EC 60 C5 F9 29 74 24 50 0F B6 DA 48 39 99 20 0C 00 00 0F 86 4E 02 00 00 "
    "48 89 CE 4C 6B F3 70 4A 8D 3C 31 48 83 C7 50 48 89 FA E8 ?? ?? ?? ??";
constexpr uint8_t update_vehicle_prologue[14] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56,
                                                 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x60};
constexpr uint32_t part_offsets[6] = {0x254c, 0x255c, 0x2558, 0x2554, 0x2550, 0x2560};

// VehicleTerminalDamageSystem's send site at 0x142f16878: `lea rcx, [rip+vtable]` at +0x09 is the vtable of
// MessageVehicleTerminallyDamaged {vtable, 0, u8 vehicle @+0x10, u32 mask of parts at damage state 5 @+0x14}, which
// it publishes through Ne_Post_BusMessage.
constexpr const char *sig_terminal_send_site =
    "48 C7 44 24 28 00 00 00 00 48 8D 0D ?? ?? ?? ?? 48 89 4C 24 20 48 8B 4C 24 48 48 89 4C 24 30 48 8B 48 08 "
    "48 8D 54 24 20 E8 ?? ?? ?? ??";
std::atomic<const void *> terminal_vtable{nullptr};

uint64_t (*orig_update_vehicle)(void *, uint8_t);
// The frame callback queues values only; vehicle pointers are resolved in updateVehicle.
// Packed request: expiry in milliseconds << 16 | vehicle << 8 | (part + 1).
std::atomic<uint64_t> pending_failure{0};
std::atomic<bool> session_ended{false};
std::atomic<uint64_t> telemetry_not_before{0};
std::atomic<bool> garage_return_wanted{false};
std::atomic<float> test_cap{-1.0f};
void apply_pending_failure(void *system, uint8_t vehicle);
// The damage-state tables of VehicleTerminalDamageSystem: per category k (0 engine, 1 power unit, 2 gearbox) a count
// at +0xc60+k*0x40 and entries {u32 state, f32 threshold} from +0xc30+k*0x40. State 5 is terminal.
constexpr uintptr_t damage_table = 0xc30, damage_table_count = 0xc60, damage_table_stride = 0x40;
constexpr uint32_t damage_terminal = 5;

// Safety car and red flag come from deployment points (rules.safety_car.logic.conditions.deployment_points).
// game::rules::logic::conditions::DeploymentPointsAccumulatorStoppedCars::update (0x143641bd0, vtable slot
// 0x145854ab0) walks the cars in [[this+0x40]+0x88, +0x90); a car whose byte +0x31 has bit 3 set is skipped and its
// stopped timer reset. The points it sends are u32 at this+0x80. DeploymentPointsAccumulatorDebris::update
// (0x143640f10, slot 0x145854820) keeps its points at this+0x4c.
constexpr const char *sig_stopped_cars_update =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC A8 00 00 00 C5 78 29 84 24 90 00 00 00 C5 F8 29 BC 24 80 00 00 00 "
    "C5 F8 29 74 24 70 48 89 CE 48 8B 79 38 48 8B 81 80 00 00 00 48 89 44 24 40 80 BF 98 00 00 00 00 74 7C";
constexpr const char *sig_debris_update =
    "41 57 41 56 41 54 56 57 55 53 48 83 EC 70 C5 78 29 44 24 60 C5 F8 29 7C 24 50 C5 F8 29 74 24 40 83 79 18 01 "
    "0F 8F ?? ?? ?? ?? 48 89 CE 8B 69 4C C5 FA 10 71 50 80 79 48 00 C5 F8 28 CE 0F 84 ?? ?? ?? ?? 48 8B 4E 38";
void (*orig_stopped_cars_update)(void *);
void (*orig_debris_update)(void *);
// The player's car has terminal damage the game was not told about, so they drive it themselves.
std::atomic<bool> player_broken{false};
std::atomic<bool> retire_requested{false}; // broken only by a retire request (hotkey/MFD), no part failed
// Practice/qualifying: the broken car is back in its garage and stays there for the rest of the session.
std::atomic<bool> car_dead{false};
bool garage_part_failed; // the dead car in its garage was seen with a failed part (game thread)
// The dead car may leave its garage; 300 ms later (tick below) it is sent back unless a part swap unlocked it. A
// career part swap shows only a few ms after the OnTrack (0.32.0 test: refused at .828, ICE 0 at .836). Refusing
// the OnTrack left the camera in the box (0.32.2 test); setting OnTrack later from the garage got the car pushed
// back in (0.32.1).
std::atomic<uint64_t> exit_check_at{0};
// Parts mask of the player's last dropped MessageVehicleTerminallyDamaged (bits 3/4 = crash/body damage).
std::atomic<uint32_t> terminal_mask{0};
bool practice_like();
// Player's last pit status (1 OnTrack) and how long the broken car has stood still on track.
std::atomic<int> player_pit{1};
// Read by the throttle stub (game thread): when the car being updated is `car`, its throttle is capped at `cap`.
// `car` is set from updateVehicle on the game thread and only compared, never dereferenced by the stub.
struct alignas(16) throttle_hook_data {
    volatile uintptr_t car;
    volatile float cap;
};
throttle_hook_data throttle_hook{0, 1.0f};
// Vehicle whose engine sound plays as switched off (failed ICE), or -1. Read by the engine audio stub.
alignas(4) volatile int32_t engine_off_vehicle = -1;
std::atomic<bool> engine_off_standing{false}; // broken car pulled over and standing still for 6 s
double still_for;
bool limp_logged;

// Game::setLocalPlayersStatuses (0x142cea230), which the pause menu's Retire reaches through
// Game::retireLocalPlayer, posts MessageVehicleStatusRequest {vtable, 0, u32 vehicle | status << 8 | 2 << 16 |
// 1 << 24} with status 7 (retired) to the bus *(*(0x14618b3b8) + 8). The site: `mov rcx, [rip+holder]` at +0x00,
// `lea rdx, [rip+vtable]` at +0x13.
constexpr const char *sig_status_request_site =
    "48 8B 0D ?? ?? ?? ?? 48 C7 84 24 90 00 00 00 00 00 00 00 48 8D 15 ?? ?? ?? ?? 48 89 94 24 88 00 00 00 89 84 24 "
    "98 00 00 00 48 8B 49 08 4C 89 F2 E8 ?? ?? ?? ??";
constexpr uint32_t status_retired = 7;
const void *status_request_vtable;
uintptr_t bus_holder;
// Seconds until the player's car retires in its pit box, or < 0.
std::atomic<double> retire_in{-1.0};

// MessageTriggerGarageCrewEvent {vtable 0x145624020, 0, u8 vehicle @+0x10, u32 request @+0x14} tells a garage crew
// what to do. In practice and qualifying (PitLaneSystem+0x300 set) PitLaneSystem::vehicleEnteringPitLaneInternal
// sends ERequest_PrepareToPushIntoGarage (6) for every car entering the pit lane; the crew then waits in its box
// and, once the car stands there, reports GarageCrewMsg_PushingIntoGarage, which PitLaneSystem::onMessageFromGarageCrew
// (0x1434cebe0) turns into pit state BeingPushedIntoGarage. In a race the flag is clear and nobody asks. The site
// below is that send: `lea rdx, [rip+vtable]` at +0x1d.
constexpr const char *sig_crew_event_site =
    "48 B8 00 00 00 00 06 00 00 00 48 09 F0 48 8B 0D ?? ?? ?? ?? 48 C7 44 24 28 00 00 00 00 48 8D 15 ?? ?? ?? ?? "
    "48 89 54 24 20 48 89 44 24 30 48 8B 49 08 48 8D 54 24 20 E8 ?? ?? ?? ??";
constexpr uint32_t crew_prepare_push = 6, crew_push = 7;
const void *crew_event_vtable;
// Crew request to send from on_frame (0: none) and its delay.
std::atomic<uint32_t> crew_request{0};
std::atomic<double> crew_request_in{-1.0};
// The player's broken car is on its way into the garage: retire it there instead of in the box.
std::atomic<bool> pushing{false};
std::atomic<bool> pushed{false};
// PitLaneSystem, seen in setVehicleStatus. +0x48 is the PitLaneMovementManager, whose PittingVehicle for car v is
// at +0x938 + v*16; its byte +0x104 is crewReady, which the car waits for before pulling into its box.
std::atomic<uintptr_t> pit_lane_system{0};

// In a race, PitLaneSystem::vehicleEnterPitLaneLimitZoneInternal (0x1434d0340) posts MessageCarEnteredPitLane
// {vtable, 0, u8 vehicle @+0x10} ("prep garage crew to perform a box stop"); the crew then does a tyre stop and
// ignores a later push request. The site: log call, then `lea rcx, [rip+vtable]` at +0x26.
constexpr const char *sig_pit_entry_site =
    "48 8D 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 41 89 F0 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 C7 44 24 30 00 00 00 "
    "00 48 8D 0D ?? ?? ?? ?? 48 89 4C 24 28 88 5C 24 38 48 8B 48 08 48 8D 54 24 28 E8 ?? ?? ?? ??";
const void *pit_entry_vtable;

// The crew's own report to PitLaneSystem::onMessageFromGarageCrew: {vtable, 0, u8 vehicle @+0x10, u32 code @+0x14}.
// Code 3 (PushingIntoGarage) turns InPitBox into BeingPushedIntoGarage, code 4 (CarInGarage) ends it in the garage.
// The site is GarageCrew StateWaitPushIntoGarage sending code 3: `lea rcx, [rip+vtable]` at +0x14.
constexpr const char *sig_crew_msg_site =
    "48 8B 46 70 0F B6 80 20 01 00 00 48 C7 44 24 30 00 00 00 00 48 8D 0D ?? ?? ?? ?? 48 89 4C 24 28 88 44 24 38 "
    "C7 44 24 3C 03 00 00 00";
constexpr uint32_t crew_msg_pushing = 3, crew_msg_in_garage = 4;
const void *crew_msg_vtable;
// Crew report to fake from on_frame (0: none) and its delay.
std::atomic<uint32_t> crew_msg{0};
std::atomic<double> crew_msg_in{-1.0};

// GarageCrewInstanceLogic states, processEvent(state, logic), called every update. logic+0xa0..+0xa8 queues
// 0x18-byte copies of MessageTriggerGarageCrewEvent, logic+0x70 -> +0x120 is the crew's car, logic+0x80 the next
// state. StateSendCarToTrack_P3 takes request 6 into StateWaitPushIntoGarage only when [[logic+0x78]+0x88]->vfunc(+0x30)
// is false; that one takes 7 and reports PushingIntoGarage. The eight states are static 0x60-byte objects in .xtext,
// processEvent at +0x28 and the name inline at +0x40, starting with StateCarIsReadyForTrack (0x145622cc0). Most of
// the functions are obfuscated thunks, so we patch the slots.
constexpr int crew_state_count = 8;
constexpr size_t crew_state_size = 0x60, crew_state_slot = 0x28, crew_state_name = 0x40;
struct CrewState {
    const char *name;
    uint64_t (*orig)(void *, void *);
};
CrewState crew_states[crew_state_count];
std::atomic<int> crew_state_now{-1};
// Crew states by index in the table above.
constexpr int crew_ready_for_track = 0, crew_wait_push = 6;
// Crew request to post from on_frame (-1: none), for requests including 0, and how far the send-off has got
// (0 not started, 1 SendCarToTrack sent, 2 LaneClear sent).
std::atomic<int> pending_crew{-1};
std::atomic<int> sendoff_stage{0};

// VehicleControllerSystem::setController flag reasons (table 0x1457d7b10)
const char *const reasons[] = {"PIT_LANE",
                               "ROLLING_START",
                               "DISQUALIFIED",
                               "NOT_IN_SESSION",
                               "RETIRED",
                               "PAUSE",
                               "DEV",
                               "BENCHMARK",
                               "CUTSCENE",
                               "TERMINAL_DAMAGE",
                               "SESSION_NOT_STARTED",
                               "SAFETY_CAR",
                               "DROP_IN",
                               "FORMATION_LAP_REFORM_GRID",
                               "ROBODRIVER",
                               "RESET_TO_TRACK",
                               "CINEMATIC_SAFETY_CAR",
                               "PDS"};
constexpr int32_t reason_terminal_damage = 9;

// MessagePitStateChanged states (table 0x145845b90)
const char *const pit_states[] = {"InactiveInGarage",
                                  "OnTrack",
                                  "EnteringPitLane",
                                  "EnteringPitBox",
                                  "InPitBox",
                                  "InPitBox_CrewCompleted",
                                  "InPitBoxPerformedRelease",
                                  "LeavingPitBox",
                                  "BeingPushedIntoGarage",
                                  "InGarage",
                                  "InGarage_VehicleWantsToLeave",
                                  "InGarage_PreparedToLeave",
                                  "InGarage_SafeToLeave",
                                  "LeavingGarage",
                                  "LeavingPitLane",
                                  "InPitLane_DrivingThrough",
                                  "Invalid"};

template <size_t N> const char *label(const char *const (&names)[N], int64_t i)
{
    return i >= 0 && size_t(i) < N ? names[i] : "?";
}

constexpr int max_vehicles = 32;

// Last (ai, reason) logged per vehicle, so a call the game repeats every frame logs once.
int32_t last_ai[max_vehicles], last_reason[max_vehicles];
// Vehicles the game has handed to a human at least once this session.
bool human[max_vehicles];
// Seconds of extra telemetry logging left after an interesting event.
double focus_left;
// The vehicle the game last handed to a human: the player in an offline session.
std::atomic<int> player_vehicle{-1};

uint64_t (*orig_set_controller)(void *, uint64_t, uint32_t, int32_t);
uint64_t (*orig_pit_state_changed)(void *, void *);

int32_t player_index()
{
    swofty::telemetry::SessionState s{};
    if (swofty::telemetry::session(&s) != swofty::telemetry::Result::ok)
        return -1;
    return int32_t(s.player_index);
}

bool is_player(uint64_t vehicle)
{
    return (vehicle < max_vehicles && human[vehicle]) || int64_t(vehicle) == player_index();
}

std::atomic<void *> controller_system{nullptr};

uint64_t on_set_controller(void *system, uint64_t vehicle, uint32_t ai, int32_t reason)
{
    controller_system = system;
    uint8_t id = uint8_t(vehicle);
    if (id < max_vehicles) {
        if (!ai) {
            human[id] = true;
            player_vehicle = id;
        }
        if (last_ai[id] != int32_t(ai) || last_reason[id] != reason) {
            last_ai[id] = int32_t(ai);
            last_reason[id] = reason;
            swofty::log("[td-probe] setController vehicle=%u player_index=%d known_human=%d -> %s reason=%d %s", id,
                        player_index(), human[id], ai ? "AI" : "HUMAN", reason, label(reasons, reason));
        }
    }
    if (reason == reason_terminal_damage && ai && is_player(id)) {
        focus_left = 60.0;
        if (keep_control) {
            swofty::log("[td-probe] keep_control: dropped TERMINAL_DAMAGE hand-over of vehicle %u", id);
            return 0;
        }
    }
    return orig_set_controller(system, vehicle, ai, reason);
}

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
    if (swofty::mem::read(base + 0x10, id) && swofty::mem::read(base + 0x18, state) && is_player(id)) {
        swofty::log("[td-probe] pit state vehicle=%u -> %u %s", id, state, label(pit_states, state));
        player_pit = int(state);
        focus_left = focus_left > 30.0 ? focus_left : 30.0;
        if (state == 9 && player_broken && keep_control && practice_like()) {
            swofty::log("[td-probe] practice/qualifying: your broken car is in its garage and stays there");
            car_dead = true;
            garage_part_failed = false;
        }
        bool broken = keep_control && retire_in_pit_box && player_broken && retire_in.load() > -2.0;
        bool push = broken && garage_push && crew_event_vtable;
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
                // In a race there is no garage crew (the game unloads it for the race and loads the pit stop
                // crews instead), so nobody pushes and we put the car in the garage after a short stop.
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

// Redirect the `jmp rel32` thunk at `at` to handler; returns the thunk's old target via orig.
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
        player_broken && retire_in.load() > -2.0) {
        if (!pit_stop_refused.exchange(true)) {
            swofty::log("[td-probe] garage: no tyre stop for your broken car, the pit crew stays in the garage");
            // Without the job nobody tells the car its crew is ready, and it waits in front of the box.
            uintptr_t mm = 0, pitting = 0;
            if (uintptr_t sys = pit_lane_system.load(); sys && swofty::mem::read(sys + 0x48, mm) && mm &&
                swofty::mem::read(mm + 0x938 + uintptr_t(*vehicle) * 16, pitting) && pitting) {
                *reinterpret_cast<volatile uint8_t *>(pitting + 0x104) = 1;
                swofty::log("[td-probe] garage: told the pit lane your crew is ready, so the car pulls into its box");
            } else {
                swofty::log("[td-probe] garage: pit lane system not known yet, the car may wait in front of the box");
            }
        }
        return 0;
    }
    return orig_pit_stop_job(logic, vehicle);
}

// GarageCrewModule::onSessionLoaded (0x14038f360) spawns the garage crews for practice, qualifying and time trial,
// but for the race session types (15..17) only when feature flag byte 7 bit 3 is set, and destroys them otherwise.
// The site is that check; turning its `jne` (75 70 at +0x1f) into `jmp` spawns them in the race too.
constexpr const char *sig_garage_crew_spawn =
    "48 8B 46 20 48 8B B8 A8 01 00 00 8B 47 04 8D 48 F1 83 F9 02 77 0E E8 ?? ?? ?? ?? F6 40 07 08 75 70 8B 47 04 83 "
    "F8 12 74 68 FF C8 83 F8 0D 76 61 48";

// onSessionLoaded spawns every crew via SpawnGarageCrew(module, vehicle) only
// outside races; enabling that for the race parks all 20 cars in their garages.
// Instead, spawn only the player's crew once the car is broken, from on_frame.
extern std::atomic<int> garage_of[0x20];
extern spawn_crew_fn orig_spawn_crew;
void on_spawn_crew(void *module, uint32_t slot);
bool detour_at(const char *name, uintptr_t at, const uint8_t *prologue, size_t len, void *handler, void **orig);
extern const uint8_t spawn_crew_prologue[19];
void **crew_module;
std::atomic<bool> crew_spawn_wanted{false};
using destroy_crew_fn = void (*)(void *);
destroy_crew_fn destroy_crew;


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
    for (auto &g : garage_of)
        g = -1;
    if (!detour_at("GarageCrewModule::spawnGarageCrew", reinterpret_cast<uintptr_t>(spawn_crew), spawn_crew_prologue,
                   sizeof spawn_crew_prologue, reinterpret_cast<void *>(&on_spawn_crew),
                   reinterpret_cast<void **>(&orig_spawn_crew))) {
        spawn_crew = nullptr;
        return;
    }

    swofty::log("[td-probe] garage: your crew spawns in the race once your car is broken (module %p, spawn %p)",
                crew_module, reinterpret_cast<void *>(spawn_crew));
}

// Garage slot of each vehicle, learned whenever the game spawns crews (training):
// SpawnGarageCrew(slot) creates the instances module+0x68+v*8 of that garage's cars.
std::atomic<int> garage_of[0x20];
constexpr uint8_t spawn_crew_prologue[19] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
                                           0x55, 0x53, 0x48, 0x81, 0xEC, 0xF8, 0x03, 0x00, 0x00};
spawn_crew_fn orig_spawn_crew;

void on_spawn_crew(void *module, uint32_t slot)
{
    auto m = reinterpret_cast<uintptr_t>(module);
    uintptr_t before[0x20] = {};
    for (int v = 0; v < 0x20; v++)
        swofty::mem::read(m + 0x68 + uintptr_t(v) * 8, before[v]);
    orig_spawn_crew(module, slot);
    for (int v = 0; v < 0x20; v++) {
        uintptr_t now = 0;
        swofty::mem::read(m + 0x68 + uintptr_t(v) * 8, now);
        if (now && now != before[v] && garage_of[v].exchange(int(slot)) != int(slot))
            swofty::log("[td-probe] garage: vehicle %d belongs to garage slot %u", v, slot);
    }
}

void spawn_player_crew()
{
    int vehicle = player_vehicle.load();
    void *module = crew_module ? *crew_module : nullptr;
    if (!spawn_crew || !module || vehicle < 0 || vehicle >= 0x20)
        return;
    uintptr_t instance = 0;
    if (!swofty::mem::read(reinterpret_cast<uintptr_t>(module) + 0x68 + uintptr_t(vehicle) * 8, instance))
        return;
    if (instance) {
        swofty::log("[td-probe] garage: your crew already exists (%p), no spawn", reinterpret_cast<void *>(instance));
        return;
    }
    // Spawn slots are the 10 garages, not vehicles; the map comes from training.
    int learned = garage_of[vehicle].load();
    if (learned < 0) {
        swofty::log("[td-probe] garage: garage slot of vehicle %d unknown (load a practice session first), no spawn",
                    vehicle);
        return;
    }
    uint32_t slot = uint32_t(learned);
    uintptr_t table = 0, entry = 0;
    uint32_t entry_slot = 0xffffffff;
    swofty::mem::read(reinterpret_cast<uintptr_t>(module) + 0x60, table);
    if (table)
        swofty::mem::read(table + 0x838 + uintptr_t(slot) * 8, entry);
    if (entry)
        swofty::mem::read(entry + 0x18, entry_slot);
    if (!entry || entry_slot != slot) {
        swofty::log("[td-probe] garage: garage slot %u for vehicle %d missing (entry %p, slot %u), no spawn", slot,
                    vehicle, reinterpret_cast<void *>(entry), entry_slot);
        return;
    }
    swofty::log("[td-probe] garage: spawning garage slot %u for vehicle %d", slot, vehicle);
    on_spawn_crew(module, slot);
    // The garage spawns a crew for both its cars, and their send-off moves both cars
    // into the pit lane. Remove the other car's crew the way onSessionLoaded does in races:
    // Destroy(instance), then free it through [module+0x18]'s vtable +0x38.
    auto m = reinterpret_cast<uintptr_t>(module);
    for (int v = 0; v < 0x20; v++) {
        uintptr_t other = 0, alloc = 0, vt = 0, free_fn = 0;
        if (v == vehicle || garage_of[v].load() != int(slot) || !swofty::mem::read(m + 0x68 + uintptr_t(v) * 8, other) ||
            !other)
            continue;
        if (!destroy_crew || !swofty::mem::read(m + 0x18, alloc) || !alloc || !swofty::mem::read(alloc, vt) ||
            !swofty::mem::read(vt + 0x38, free_fn) || !free_fn) {
            swofty::log("[td-probe] garage: cannot remove the crew of vehicle %d", v);
            continue;
        }
        destroy_crew(reinterpret_cast<void *>(other));
        reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(free_fn)(alloc, other);
        *reinterpret_cast<volatile uintptr_t *>(m + 0x68 + uintptr_t(v) * 8) = 0;
        swofty::log("[td-probe] garage: removed the crew of teammate vehicle %d (%p)", v, reinterpret_cast<void *>(other));
    }
    swofty::mem::read(reinterpret_cast<uintptr_t>(module) + 0x68 + uintptr_t(vehicle) * 8, instance);
    swofty::log("[td-probe] garage: crew spawn returned, instance %p", reinterpret_cast<void *>(instance));
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

void reset_session()
{
    throttle_hook.car = 0;
    throttle_hook.cap = 1.0f;
    engine_off_vehicle = -1;
    engine_off_standing = false;
    pending_failure = 0;
    garage_return_wanted = false;
    test_cap = -1.0f;
    telemetry_not_before = GetTickCount64();
    swofty::mem::invalidate();
    car_dead = false;
    garage_part_failed = false;
    exit_check_at = 0;
    terminal_mask = 0;
    for (int i = 0; i < max_vehicles; i++) {
        last_ai[i] = -1;
        last_reason[i] = -1;
        human[i] = false;
    }
    focus_left = 0;
    player_vehicle = -1;
    player_broken = false;
    retire_requested = false;
    crew_spawn_wanted = false;
    crew_spawn_now = false;
    player_pit = 1;
    still_for = 0;
    prepare_at_box = false;
    crew_seq_t = -1.0;
    limp_logged = false;
    retire_in = -1.0;
    crew_request = 0;
    crew_request_in = -1.0;
    crew_msg = 0;
    crew_msg_in = -1.0;
    crew_state_now = -1;
    pit_stop_refused = false;
    pending_crew = -1;
    sendoff_stage = 0;
    pushing = false;
    pushed = false;
}

void find_terminal_message()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_terminal_send_site, &site);
    if (n != 1) {
        swofty::log("[td-probe] terminal message: signature has %d hits, keep_control only drops the hand-over", n);
        return;
    }
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void *>(site + 0x0c), 4);
    terminal_vtable = reinterpret_cast<const void *>(site + 0x10 + int64_t(rel));
    swofty::log("[td-probe] terminal message vtable %p", terminal_vtable.load());
}

// The player's car as VehicleTerminalDamageSystem sees it, or 0.
uintptr_t car_of(uintptr_t sys, int vehicle)
{
    uint64_t count = 0;
    uintptr_t rec = 0, car = 0;
    if (vehicle < 0 || !sys || !swofty::mem::read(sys + 0xc20, count) || uint64_t(vehicle) >= count ||
        !swofty::mem::read(sys + 0x50 + uint64_t(vehicle) * 0x70 + 8, rec) || !rec ||
        !swofty::mem::read(rec + 0x58, car))
        return 0;
    return car;
}

float fraction(std::string_view v)
{
    if (v.size() < 3 || v[0] != '0' || v[1] != '.')
        return 0;
    float f = 0, scale = 0.1f;
    for (char ch : v.substr(2)) {
        if (ch < '0' || ch > '9')
            return 0;
        f += float(ch - '0') * scale;
        scale *= 0.1f;
    }
    return f;
}

// A failed part sits at 1.0, and at 1.0 the car has no drive. After the terminal message is dropped, hold the
// player's broken parts just below it, so the engine runs badly and they can limp to the pits or pull over.
void limp(uintptr_t sys, uint8_t vehicle)
{
    float level = fraction(limp_level);
    if (!keep_control || !player_broken || level <= 0 || int(vehicle) != player_vehicle)
        return;
    uintptr_t car = car_of(sys, vehicle), engine = 0, gearbox = 0;
    if (!car)
        return;
    bool clamped = false;
    if (swofty::mem::read(car + 0x530, engine) && engine) {
        for (uint32_t off : part_offsets) {
            float v = 0;
            if (swofty::mem::read(engine + off, v) && v > level) {
                *reinterpret_cast<volatile float *>(engine + off) = level;
                clamped = true;
            }
        }
    }
    float g = 0;
    if (swofty::mem::read(car + 0x538, gearbox) && gearbox && swofty::mem::read(gearbox + 0x674, g) && g > level) {
        *reinterpret_cast<volatile float *>(gearbox + 0x674) = level;
        clamped = true;
    }
    if (clamped && !limp_logged) {
        limp_logged = true;
        swofty::log("[td-probe] limp: holding the broken parts of vehicle %u at %.2f", vehicle, level);
    }
}

void return_to_garage();

// Throttle cap for the player's broken car (game thread, inside updateVehicle): a failed ICE or gearbox gives no
// drive (cap 0, the car rolls out), a failed MGU-K, MGU-H, ES, turbo or CE leaves `damaged_power` of the throttle.
// Ctrl+Shift+F9 (test) overrides it.

void power_loss(uintptr_t car)
{
    static int logged = -1;
    static uintptr_t last_car;
    if (car != last_car) {
        // 0.25.2 crash log: after the crash car +0x170.. read all zero while UDP throttle was 1, so the car object
        // may change on terminal crash damage. Log every change to see it.
        float thr = -1;
        if (car)
            swofty::mem::read(car + 0x188, thr);
        swofty::log("[td-probe] power: player car object %p -> %p (throttle %.2f)", reinterpret_cast<void *>(last_car),
                    reinterpret_cast<void *>(car), double(thr));
        last_car = car;
    }
    // Repair research: while the dead car sits in its practice garage, log its part wear (ICE, MGU-K, MGU-H, ES,
    // TC, CE, gearbox) whenever it changes, to see what a part swap or the game's repair does to it.
    if (car_dead && car) {
        static int last_parts[7] = {-1, -1, -1, -1, -1, -1, -1};
        int parts[7] = {-1, -1, -1, -1, -1, -1, -1};
        uintptr_t eng = 0, gb = 0;
        float v = 0;
        if (swofty::mem::read(car + 0x530, eng) && eng)
            for (int i = 0; i < 6; i++)
                if (swofty::mem::read(eng + part_offsets[i], v))
                    parts[i] = int(v * 1000.0f);
        if (swofty::mem::read(car + 0x538, gb) && gb && swofty::mem::read(gb + 0x674, v))
            parts[6] = int(v * 1000.0f);
        if (memcmp(parts, last_parts, sizeof parts)) {
            memcpy(last_parts, parts, sizeof parts);
            swofty::log("[td-probe] repair: garage car %p parts ICE=%d MGU-K=%d MGU-H=%d ES=%d TC=%d CE=%d gearbox=%d "
                        "(per mille)",
                        reinterpret_cast<void *>(car), parts[0], parts[1], parts[2], parts[3], parts[4], parts[5],
                        parts[6]);
        }
        // Career part swap (0.31.5 tests): a fitted new engine sets the failed part back near 0 on the next send-out
        // attempt; without a swap it stays at 1000, or 999 once the car is really out (0.32.3 test). Once no part
        // is above 900 per mille, the car may leave again.
        // Crash damage and a plain retirement (no failed part) keep the car in the garage.
        bool readable = true, failed = false;
        for (int p : parts) {
            readable = readable && p >= 0;
            failed = failed || p >= 900;
        }
        if (failed)
            garage_part_failed = true;
        if (readable && !failed && garage_part_failed && !(terminal_mask.load() & 0x18)) {
            swofty::log("[td-probe] repair: the failed part was replaced, your car may leave the garage again");
            car_dead = false;
            garage_part_failed = false;
            terminal_mask = 0;
            // Engine state flags at engine+0x2530 (UDP snapshot writer 0x142d00a63): bit 0 blown, bit 1 seized. The
            // swap resets the wear but keeps them, and a seized engine made the out-lap AI stop (0.32.1 test).
            uint8_t flags = 0;
            if (eng && swofty::mem::read(eng + 0x2530, flags) && (flags & 3)) {
                *reinterpret_cast<volatile uint8_t *>(eng + 0x2530) = uint8_t(flags & ~3);
                swofty::log("[td-probe] repair: cleared engine flags 0x%02x (blown/seized)", flags);
            }
            if (swofty::hud::available())
                swofty::hud::toast("PART REPLACED", "Your car is ready again", 4);
        }
    }
    float cap = 1.0f;
    int why = 0;
    uintptr_t engine = 0, gearbox = 0;
    if (keep_control && player_broken && car) {
        float v = 0, pu = fraction(damaged_power);
        // Terminal crash damage (broken suspension): the game makes the car near undrivable, so it only rolls out.
        if (terminal_mask.load() & 0x18)
            why = 8;
        if (!why && swofty::mem::read(car + 0x530, engine) && engine) {
            if (swofty::mem::read(engine + part_offsets[0], v) && v >= 1.0f)
                why = 1;
            for (int i = 1; i < 6 && !why; i++)
                if (swofty::mem::read(engine + part_offsets[i], v) && v >= 1.0f)
                    why = 2 + i;
        }
        if (!why && swofty::mem::read(car + 0x538, gearbox) && gearbox && swofty::mem::read(gearbox + 0x674, v) &&
            v >= 1.0f)
            why = 2;
        // The game's own (random) failures end in the same terminal message. If no part reads as failed, its mask
        // decides (bit 0 engine, 1 power unit, 2 gearbox, 3/4 body); any other terminal damage rolls out.
        uint32_t mask = terminal_mask.load();
        if (!why && mask)
            why = mask & 1 ? 1 : mask & 4 ? 2 : mask & 2 ? 9 : 10;
        cap = why == 1 || why == 2 || why == 8 || why == 10 ? 0.0f : why ? pu : 1.0f;
    }
    // Terminal crash damage (suspension): the engine physics keeps idling, so the engine sound stops after a random
    // 3-6 s, as a stalled engine would.
    static uint64_t crash_off_at;
    uint64_t now = GetTickCount64();
    if (why != 8)
        crash_off_at = 0;
    else if (!crash_off_at)
        crash_off_at = now + 3000 + now % 3001;
    bool off = why == 1 || (why == 8 && now >= crash_off_at) || (player_broken && engine_off_standing);
    engine_off_vehicle = off && engine_off_sound ? player_vehicle.load() : -1;
    float t = test_cap.load();
    throttle_hook.cap = t >= 0 ? t : cap;
    if (why != logged) {
        static const char *const names[] = {"none", "ICE", "gearbox", "MGU-K", "MGU-H", "ES", "turbo", "CE", "crash damage",
                                             "power unit (mask)", "other terminal damage (mask)"};
        swofty::log("[td-probe] power: failed part %s, throttle cap %.2f", names[why], double(cap));
        logged = why;
    }
}

uint64_t on_update_vehicle(void *system, uint8_t vehicle)
{
    if (session_ended)
        return orig_update_vehicle(system, vehicle);
    apply_pending_failure(system, vehicle);
    uint64_t r = orig_update_vehicle(system, vehicle);
    if (int(vehicle) == player_vehicle.load()) {
        uintptr_t car = car_of(reinterpret_cast<uintptr_t>(system), vehicle);
        throttle_hook.car = car;
        power_loss(car);
        return_to_garage();
    }
    limp(reinterpret_cast<uintptr_t>(system), vehicle);
    return r;
}

// The 14-byte prologue is plain pushes and `sub rsp, imm8`, so it runs unchanged in a trampoline; the entry gets
// `jmp [rip+0]` to the handler.
bool hook_update_vehicle()
{
    uintptr_t at = 0;
    int n = swofty::api->scan(".xtls", sig_update_vehicle, &at);
    if (n != 1 || memcmp(reinterpret_cast<void *>(at), update_vehicle_prologue, 14)) {
        swofty::log("[td-probe] debug failure: updateVehicle signature has %d hits, hotkey disabled", n);
        return false;
    }
    auto *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp)
        return false;
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t back = at + 14;
    memcpy(tramp, update_vehicle_prologue, 14);
    memcpy(tramp + 14, jmp, 6);
    memcpy(tramp + 20, &back, 8);
    DWORD old;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 28);
    orig_update_vehicle = reinterpret_cast<decltype(orig_update_vehicle)>(tramp);
    uint8_t patch[14];
    void *handler = reinterpret_cast<void *>(&on_update_vehicle);
    memcpy(patch, jmp, 6);
    memcpy(patch + 6, &handler, 8);
    auto *code = reinterpret_cast<uint8_t *>(at);
    if (!VirtualProtect(code, 14, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(code, patch, 14);
    VirtualProtect(code, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 14);
    swofty::log("[td-probe] hooked VehicleTerminalDamageSystem::updateVehicle at %p", code);
    return true;
}

void on_stopped_cars_update(void *self)
{
    auto base = reinterpret_cast<uintptr_t>(self);
    uintptr_t cars = 0, first = 0, last = 0, mine = 0;
    uint8_t flags = 0;
    int vehicle = player_vehicle;
    if (keep_control && player_broken && vehicle >= 0 && swofty::mem::read(base + 0x40, cars) && cars &&
        swofty::mem::read(cars + 0x88, first) && swofty::mem::read(cars + 0x90, last) && last - first < 0x800) {
        for (uintptr_t it = first; it < last; it += 8) {
            uintptr_t car = 0;
            uint8_t id = 0xff;
            if (swofty::mem::read(it, car) && car && swofty::mem::read(car + 0x38, id) && id == vehicle &&
                swofty::mem::read(car + 0x31, flags)) {
                mine = car;
                break;
            }
        }
    }
    if (mine && !(flags & 8))
        *reinterpret_cast<volatile uint8_t *>(mine + 0x31) = uint8_t(flags | 8);
    orig_stopped_cars_update(self);
    if (mine && !(flags & 8))
        *reinterpret_cast<volatile uint8_t *>(mine + 0x31) = flags;

    static uint32_t last_points = 0;
    static bool excluded_logged = false;
    uint32_t points = 0;
    if (swofty::mem::read(base + 0x80, points) && points != last_points) {
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
    if (swofty::mem::read(reinterpret_cast<uintptr_t>(self) + 0x4c, points) && points != last_points) {
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
    auto logic = reinterpret_cast<uintptr_t>(logic_ptr);
    uintptr_t first = 0, last = 0, car = 0, rules = 0, obj = 0, vt = 0;
    uint8_t vehicle = 0xff, armed = 0;
    if (!swofty::mem::read(logic + 0x70, car) || !car || !swofty::mem::read(car + 0x120, vehicle) ||
        int(vehicle) != player_vehicle.load())
        return;
    uint32_t request = 0, next = 0;
    swofty::mem::read(logic + 0x80, next);
    swofty::mem::read(logic + 0x90, armed);
    if (crew_state_now.exchange(which) != which)
        swofty::log("[td-probe] crew: your crew is now in %s (next %u, armed %u)", crew_states[which].name, next, armed);
    // In a race the crew starts in StateCarIsReadyForTrack although the car is on the grid, and only
    // StateSendCarToTrack_P3 takes PrepareToPushIntoGarage. So we run the send-off once while the car is on track,
    // as the game does when you leave the garage in practice: SendCarToTrack, then LaneClear once it is in P2.
    // The crew's own SendToTrack report changes nothing while the car is OnTrack.
    uintptr_t sys = pit_lane_system.load(), entry = 0;
    uint32_t pit = 0xffffffff;
    if (garage_crew_in_race && keep_control && sys &&
        swofty::mem::read(sys + 0x68 + uintptr_t(vehicle) * 16, entry) && entry && swofty::mem::read(entry + 4, pit) &&
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
    if (!swofty::mem::read(logic + 0xa0, first) || !swofty::mem::read(logic + 0xa8, last) || first == last)
        return;
    swofty::mem::read(first + 0x14, request);
    int blocked = -1, other = -1;
    if (swofty::mem::read(logic + 0x78, rules) && rules && swofty::mem::read(rules + 0x88, obj) && obj &&
        swofty::mem::read(obj, vt) && vt) {
        uintptr_t f30 = 0, f38 = 0;
        if (swofty::mem::read(vt + 0x30, f30) && swofty::mem::read(vt + 0x38, f38) && f30 && f38) {
            blocked = reinterpret_cast<bool (*)(uintptr_t)>(f30)(obj);
            other = reinterpret_cast<bool (*)(uintptr_t)>(f38)(obj);
            static uint64_t last_key;
            uint64_t key = uint64_t(which) | uint64_t(request) << 8 | uint64_t(blocked & 1) << 40 |
                           uint64_t(other & 1) << 41 | uint64_t(armed) << 48 | 1ull << 63;
            if (key == last_key)
                return;
            last_key = key;
            swofty::log("[td-probe] crew: %s gets request %u (queue %llu, armed %u, next %u); check +0x30 %p = %d, "
                        "+0x38 %p = %d, object vtable %p",
                        crew_states[which].name, request, (unsigned long long)((last - first) / 0x18), armed, next,
                        reinterpret_cast<void *>(f30), blocked, reinterpret_cast<void *>(f38), other,
                        reinterpret_cast<void *>(vt));
            return;
        }
    }
    swofty::log("[td-probe] crew: %s gets request %u (queue %llu, armed %u, next %u), no check object",
                crew_states[which].name, request, (unsigned long long)((last - first) / 0x18), armed, next);
}

template <int I> uint64_t on_crew_state(void *state, void *logic)
{
    log_crew_state(I, logic);
    return crew_states[I].orig(state, logic);
}

// Detour a function whose first len (>= 14) bytes are whole, position-independent instructions.
bool detour_at(const char *name, uintptr_t at, const uint8_t *prologue, size_t len, void *handler, void **orig)
{
    if (!at || len < 14 || len > 32 || !swofty::mem::readable(at, len) ||
        memcmp(reinterpret_cast<void *>(at), prologue, len)) {
        swofty::log("[td-probe] %s: prologue does not match, not hooked", name);
        return false;
    }
    auto *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp)
        return false;
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t back = at + len;
    memcpy(tramp, prologue, len);
    memcpy(tramp + len, jmp, 6);
    memcpy(tramp + len + 6, &back, 8);
    DWORD old;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 14);
    *orig = tramp;
    uint8_t patch[32];
    memset(patch, 0xCC, sizeof patch);
    memcpy(patch, jmp, 6);
    memcpy(patch + 6, &handler, 8);
    auto *code = reinterpret_cast<uint8_t *>(at);
    if (!VirtualProtect(code, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(code, patch, len);
    VirtualProtect(code, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, len);
    swofty::log("[td-probe] hooked %s at %p", name, code);
    return true;
}

bool detour(const char *name, const char *sig, const uint8_t *prologue, size_t len, void *handler, void **orig)
{
    uintptr_t at = 0;
    int n = swofty::api->scan(".xtls", sig, &at);
    if (n != 1) {
        swofty::log("[td-probe] %s: signature has %d hits, not hooked", name, n);
        return false;
    }
    return detour_at(name, at, prologue, len, handler, orig);
}

template <int... I> void crew_handlers(std::integer_sequence<int, I...>, void **out)
{
    ((out[I] = reinterpret_cast<void *>(&on_crew_state<I>)), ...);
}

// Read-only probes of the normal asset selection and its completion callback.
// The six arguments of ChangeResidentAssets are forwarded unchanged. Its first
// 16 bytes and the callback's first 15 bytes are whole, RIP-independent instructions.
constexpr const char *sig_change_resident_assets =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 78 44 89 CF 48 89 D3 48 89 CE 80 3D ?? ?? ?? ?? 01 "
    "4C 8D 0D ?? ?? ?? ?? 45 0F B6 F0 0F 85 90 00 00 00 B8 C1 00 00 00";
constexpr uint8_t change_assets_prologue[] = {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xec, 0x78};
// Start/finish callbacks have matching entry signatures. Resolve the finish
// callback from its unique registration site instead (first lea rdx).
constexpr const char *sig_assets_finished_site =
    "48 8D 15 ?? ?? ?? ?? 48 89 54 24 48 48 89 74 24 50 48 8D 15 ?? ?? ?? ?? "
    "48 89 54 24 68 48 89 74 24 70 31 D2 48 85 C0";
constexpr uint8_t assets_finished_prologue[] = {
    0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xec, 0x28, 0x44, 0x89, 0xc6, 0x48, 0x89, 0xd7};
using change_assets_fn = void (*)(void *, const void *, uint32_t, uint32_t, const void *, const void *);
using assets_finished_fn = void (*)(void *, void *, uint32_t);
change_assets_fn orig_change_assets;
assets_finished_fn orig_assets_finished;
std::atomic<uint32_t> asset_request_serial{0};

const char *asset_selection(uint32_t selection)
{
    if (!(selection & 0xff00))
        return "none";
    return (selection & 0xff) == 0 ? "Garages" : (selection & 0xff) == 1 ? "PitStops" : "unknown";
}

std::string asset_path(uintptr_t record, uintptr_t offset)
{
    uintptr_t ptr = 0;
    std::string value;
    if (!swofty::mem::read(record + offset, ptr))
        return "<unreadable pointer>";
    if (!ptr)
        return "<null>";
    char first = 0;
    if (swofty::mem::read(ptr, first) && !first)
        return "<empty>";
    if (!swofty::mem::c_string(ptr, value, 512))
        return "<unreadable or overlong string>";
    // Keep one path per log line even if an unexpected string contains controls.
    for (char &c : value)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    return value.empty() ? "<empty>" : value;
}

void on_change_assets(void *manager, const void *records, uint32_t type, uint32_t selection,
                      const void *on_start, const void *on_finish)
{
    if (crew_asset_diagnostics) {
        uint32_t request = asset_request_serial.fetch_add(1) + 1;
        uint8_t kind = uint8_t(type);
        swofty::log("[td-probe] assets: request=%u manager=%p type=%u %s selection=0x%04x %s caller=%p",
                    request, manager, unsigned(kind), kind == 0 ? "Session" : kind == 1 ? "Frontend" : "unknown",
                    unsigned(selection & 0xffff), asset_selection(selection), __builtin_return_address(0));
        constexpr uintptr_t stride = 0x6780;
        uintptr_t first = 0, last = 0;
        uintptr_t vec = reinterpret_cast<uintptr_t>(records);
        if (!vec || !swofty::mem::read(vec + 8, first) || !swofty::mem::read(vec + 0x10, last) ||
            last < first || (last - first) % stride || (last - first) / stride > 64 || (!first && last)) {
            swofty::log("[td-probe] assets: request=%u record vector unreadable or unexpected; original call unchanged",
                        request);
        } else {
            unsigned count = unsigned((last - first) / stride);
            swofty::log("[td-probe] assets: request=%u records=%u (record indices are not vehicle IDs)", request, count);
            for (unsigned i = 0; i < count; i++) {
                uintptr_t record = first + uintptr_t(i) * stride;
                auto pit = asset_path(record, 0x60b8);
                auto garage = asset_path(record, 0x62e8);
                swofty::log("[td-probe] assets: request=%u record=%u PitStops path=%s", request, i, pit.c_str());
                swofty::log("[td-probe] assets: request=%u record=%u Garages path=%s", request, i, garage.c_str());
            }
        }
    }
    orig_change_assets(manager, records, type, selection, on_start, on_finish);
}

void on_assets_finished(void *manager, void *result, uint32_t code)
{
    if (crew_asset_diagnostics) {
        uint32_t state = 0xffffffff;
        uint16_t selection = 0;
        auto at = reinterpret_cast<uintptr_t>(manager);
        swofty::mem::read(at, state);
        swofty::mem::read(at + 0x60, selection);
        // The callback payload/code are logged raw: their semantics are not yet
        // established. Callback invocation alone does not prove a usable scene.
        swofty::log("[td-probe] assets: load-finished callback manager=%p state=%u selection=0x%04x %s "
                    "payload=%p code=%u (raw; scene availability unverified)",
                    manager, state, unsigned(selection), asset_selection(selection), result, code);
    }
    orig_assets_finished(manager, result, code);
}

void hook_asset_diagnostics()
{
    if (!crew_asset_diagnostics)
        return;
    bool change = detour("VehicleAssetManager::changeResidentAssets", sig_change_resident_assets,
                         change_assets_prologue, sizeof change_assets_prologue,
                         reinterpret_cast<void *>(&on_change_assets), reinterpret_cast<void **>(&orig_change_assets));
    uintptr_t site = 0;
    int hits = swofty::api->scan(".xtls", sig_assets_finished_site, &site);
    bool finish = false;
    if (hits == 1 && site) {
        int32_t rel = 0;
        if (swofty::mem::read(site + 3, rel))
            finish = detour_at("VehicleAssetManager::onLoadFinished", site + 7 + int64_t(rel),
                               assets_finished_prologue, sizeof assets_finished_prologue,
                               reinterpret_cast<void *>(&on_assets_finished),
                               reinterpret_cast<void **>(&orig_assets_finished));
    } else {
        swofty::log("[td-probe] assets: finish callback registration has %d hits, completion not hooked", hits);
    }
    swofty::log("[td-probe] assets: diagnostic hooks selection=%d completion=%d; asset requests remain unchanged",
                change, finish);
}

// In ChangeResidentAssets the session selection loads either a record's Garages
// group (+0x62e8) or its PitStops group (+0x60b8); the race picks PitStops. The
// patch sends the PitStops branch (site+0x22) to a stub that adds both groups
// with the game's own three helper calls, then rejoins at site+0x73.
constexpr const char *sig_asset_selection_site =
    "80 7E 61 00 74 6D 0F B6 46 60 83 F8 01 74 13 85 C0 75 60 49 8B 94 24 E8 62 00 00 48 85 D2 75 0F EB 51 "
    "49 8B 94 24 B8 60 00 00 48 85 D2 74 44 80 3A 00 74 3F 48 8B 5C 24 40 48 89 F9 E8 ?? ?? ?? ?? "
    "48 8B 0D ?? ?? ?? ?? 48 8B 54 24 48 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 89 D9 48 89 C2 E8";

uintptr_t rel_target(uintptr_t field, uintptr_t next)
{
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void *>(field), sizeof rel);
    return next + int64_t(rel);
}

struct emitter {
    uint8_t *p;
    void bytes(std::initializer_list<uint8_t> b) { for (auto x : b) *p++ = x; }
    void u32(uint32_t v) { memcpy(p, &v, 4); p += 4; }
    void u64(uint64_t v) { memcpy(p, &v, 8); p += 8; }
    void call(uintptr_t fn) { bytes({0x48, 0xB8}); u64(fn); bytes({0xFF, 0xD0}); }
    void jmp(uintptr_t to) { bytes({0xFF, 0x25, 0, 0, 0, 0}); u64(to); }
};

// A page within 2 GB of near_to, so a `jmp rel32` there and back is possible.
uint8_t *alloc_near(uintptr_t near_to)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t step = si.dwAllocationGranularity;
    for (uintptr_t at = (near_to & ~(step - 1)) - step; at > near_to - 0x7ff00000; at -= step)
        if (auto *p = static_cast<uint8_t *>(
                VirtualAlloc(reinterpret_cast<void *>(at), step, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
            return p;
    return nullptr;
}

// Seals the stub [stub, end) and overwrites `len` bytes at `at` with `jmp stub` plus nops.
bool jump_to_stub(uintptr_t at, size_t len, uint8_t *stub, uint8_t *end)
{
    DWORD old;
    VirtualProtect(stub, end - stub, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, end - stub);
    int64_t rel = int64_t(reinterpret_cast<uintptr_t>(stub)) - int64_t(at + 5);
    if (rel < INT32_MIN || rel > INT32_MAX || len < 5 || len > 32)
        return false;
    uint8_t patch[32];
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    int32_t rel32 = int32_t(rel);
    memcpy(patch + 1, &rel32, 4);
    auto *code = reinterpret_cast<uint8_t *>(at);
    if (!VirtualProtect(code, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(code, patch, len);
    VirtualProtect(code, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, len);
    return true;
}

void patch_garage_assets_in_race()
{
    if (!garage_assets_in_race)
        return;
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_asset_selection_site, &site);
    if (n != 1 || !site) {
        swofty::log("[td-probe] assets: selection site has %d hits, race keeps PitStops only", n);
        return;
    }
    uintptr_t make_name = rel_target(site + 0x3d, site + 0x41);
    uintptr_t registry = rel_target(site + 0x44, site + 0x48);
    uintptr_t find_group = rel_target(site + 0x4e, site + 0x52);
    uintptr_t fail = rel_target(site + 0x57, site + 0x5b);
    uintptr_t add_group = rel_target(site + 0x62, site + 0x66);
    uintptr_t patch_at = site + 0x22, resume = site + 0x73;

    uint8_t *stub = alloc_near(patch_at);
    if (!stub) {
        swofty::log("[td-probe] assets: no memory within 2 GB for the stub, race keeps PitStops only");
        return;
    }
    emitter e{stub};
    for (uint32_t field : {0x60b8u, 0x62e8u}) { // PitStops first, as the game does, then Garages
        e.bytes({0x49, 0x8B, 0x94, 0x24}); // mov rdx, [r12+field]
        e.u32(field);
        e.bytes({0x48, 0x85, 0xD2, 0x74, 0}); // test rdx, rdx; jz next
        uint8_t *jz1 = e.p - 1;
        e.bytes({0x80, 0x3A, 0x00, 0x74, 0}); // cmp byte [rdx], 0; jz next
        uint8_t *jz2 = e.p - 1;
        e.bytes({0x48, 0x8B, 0x5C, 0x24, 0x40, 0x48, 0x89, 0xF9}); // mov rbx, [rsp+0x40]; mov rcx, rdi
        e.call(make_name);
        e.bytes({0x48, 0xB8}); // mov rax, registry; mov rcx, [rax]; mov rdx, [rsp+0x48]
        e.u64(registry);
        e.bytes({0x48, 0x8B, 0x08, 0x48, 0x8B, 0x54, 0x24, 0x48});
        e.call(find_group);
        e.bytes({0x48, 0x85, 0xC0, 0x75, 14}); // test rax, rax; jnz over the fail jump
        e.jmp(fail);
        e.bytes({0x48, 0x89, 0xD9, 0x48, 0x89, 0xC2}); // mov rcx, rbx; mov rdx, rax
        e.call(add_group);
        *jz1 = uint8_t(e.p - (jz1 + 1));
        *jz2 = uint8_t(e.p - (jz2 + 1));
    }
    e.jmp(resume);
    if (!jump_to_stub(patch_at, 13, stub, e.p))
        return;
    auto *code = reinterpret_cast<uint8_t *>(patch_at);
    swofty::log("[td-probe] assets: race loads Garages next to PitStops (patched %p, stub %p, %d bytes)", code, stub,
                int(e.p - stub));
}

// GarageCrewInstance setup reads `[scene+0xe0]` before its own null check of the
// spawned scene (instance+0xc8), the 0.11.0 race crash. The stub checks first:
// null takes the game's existing no-scene path (site+0xe6) with esi = 0.
constexpr const char *sig_crew_scene_read =
    "48 8B BD C8 00 00 00 0F B6 B7 E0 00 00 00 48 85 FF 0F 84 CF 00 00 00 4C 8B B5 D0 00 00 00";

void guard_crew_scene_read()
{
    if (!garage_crew_in_race || !garage_assets_in_race)
        return;
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_crew_scene_read, &site);
    uint8_t *stub = n == 1 && site ? alloc_near(site) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] garage: crew scene read has %d hits or no stub memory, not guarded", n);
        return;
    }
    emitter e{stub};
    e.bytes({0x31, 0xF6, 0x48, 0x85, 0xFF, 0x75, 14}); // xor esi, esi; test rdi, rdi; jnz over
    e.jmp(site + 0x17 + 0xcf);                           // the game's null path
    e.bytes({0x0F, 0xB6, 0xB7, 0xE0, 0x00, 0x00, 0x00}); // movzx esi, byte [rdi+0xe0]
    e.jmp(site + 0x17);
    if (jump_to_stub(site + 7, 16, stub, e.p))
        swofty::log("[td-probe] garage: crew scene read null-guarded at %p", reinterpret_cast<void *>(site + 7));
}

void hook_crew_states()
{
    static const char first_name[] = "StateCarIsReadyForTrack";
    char sig[sizeof first_name * 3 + 1] = {};
    for (size_t i = 0; i < sizeof first_name; i++)
        snprintf(sig + i * 3, 4, "%02X ", unsigned(uint8_t(first_name[i])));
    sig[sizeof first_name * 3 - 1] = 0;
    uintptr_t name = 0;
    int n = swofty::api->scan(".xtext", sig, &name);
    if (n != 1 || !name) {
        swofty::log("[td-probe] crew states: name has %d hits, not hooked", n);
        return;
    }
    uintptr_t base = name - crew_state_name;
    for (int i = 0; i < crew_state_count; i++) {
        auto *nm = reinterpret_cast<const char *>(base + i * crew_state_size + crew_state_name);
        uintptr_t fn = 0;
        if (strncmp(nm, "State", 5) || !swofty::mem::read(base + i * crew_state_size + crew_state_slot, fn) || !fn) {
            swofty::log("[td-probe] crew states: entry %d does not look like a state, not hooked", i);
            return;
        }
    }
    void *handlers[crew_state_count];
    crew_handlers(std::make_integer_sequence<int, crew_state_count>{}, handlers);
    for (int i = 0; i < crew_state_count; i++) {
        uintptr_t slot = base + i * crew_state_size + crew_state_slot;
        crew_states[i].name = reinterpret_cast<const char *>(base + i * crew_state_size + crew_state_name);
        memcpy(&crew_states[i].orig, reinterpret_cast<void *>(slot), 8);
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void *>(slot), 8, PAGE_READWRITE, &old))
            continue;
        memcpy(reinterpret_cast<void *>(slot), &handlers[i], 8);
        VirtualProtect(reinterpret_cast<void *>(slot), 8, old, &old);
        swofty::log("[td-probe] hooked crew %s (slot %p)", crew_states[i].name, reinterpret_cast<void *>(slot));
    }
}

// PitLaneSystem::setVehicleStatus (0x1434bc540, rcx system, dl vehicle, r8d status); the current status is
// [system + vehicle*16 + 0x68] -> +4. In a race the box stop logic finishes its tyre stop (InPitBox_CrewCompleted)
// even while the car is BeingPushedIntoGarage and then releases it, so for the broken player's car we refuse
// every change from BeingPushedIntoGarage back to the box stop states.
constexpr const char *sig_set_vehicle_status =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC E8 00 00 00 C5 79 29 84 24 D0 00 00 00 C5 F8 29 BC 24 C0 00 00 00 "
    "C5 F8 29 B4 24 B0 00 00 00 80 FA 1A 0F 87 FD 08 00 00 89 D3 48 89 CE 0F B6 FA 49 89 FC 49 C1 E4 04 4A 8B 44 21 "
    "68 8B 68 04";
constexpr uint8_t set_vehicle_status_prologue[19] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
                                                     0x55, 0x53, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00};
using set_vehicle_status_fn = uint64_t (*)(void *, uint64_t, uint64_t);
set_vehicle_status_fn orig_set_vehicle_status;

uint64_t on_set_vehicle_status(void *system, uint64_t vehicle, uint64_t status)
{
    pit_lane_system = reinterpret_cast<uintptr_t>(system);
    uint8_t id = uint8_t(vehicle);
    uint32_t to = uint32_t(status), from = 0xffffffff;
    uintptr_t entry = 0;
    // Practice/qualifying: a dead car stays InGarage (0.25.1 dropped the crew send-off, but 4 s later the game
    // still set the car OnTrack, and it counted as stopped on track).
    if (car_dead && id < 0x1b && is_player(id) &&
        swofty::mem::read(reinterpret_cast<uintptr_t>(system) + 0x68 + uintptr_t(id) * 16, entry) && entry &&
        swofty::mem::read(entry + 4, from) && from == 9 && to != 9) {
        if (to == 1) {
            swofty::log("[td-probe] practice/qualifying: your broken car leaves its garage, back in 300 ms unless a "
                        "part was replaced");
            exit_check_at = GetTickCount64() + 300;
            return orig_set_vehicle_status(system, vehicle, status);
        }
        swofty::log("[td-probe] practice/qualifying: your car is broken, refused pit state %u %s", to,
                    label(pit_states, to));
        return 0;
    }
    if (!pushing || id >= 0x1b || !is_player(id) ||
        !swofty::mem::read(reinterpret_cast<uintptr_t>(system) + 0x68 + uintptr_t(id) * 16, entry) || !entry ||
        !swofty::mem::read(entry + 4, from))
        return orig_set_vehicle_status(system, vehicle, status);
    if (from == 8 && (to == 5 || to == 6 || to == 7 || to == 14)) {
        swofty::log("[td-probe] garage: kept your car BeingPushedIntoGarage, refused pit state %u %s (from %p)", to,
                    label(pit_states, to), __builtin_return_address(0));
        return 0;
    }
    return orig_set_vehicle_status(system, vehicle, status);
}


void post_crew_event(uint32_t request)
{
    int vehicle = player_vehicle;
    uintptr_t holder = 0, bus = 0;
    if (!crew_event_vtable || vehicle < 0 || !swofty::mem::read(bus_holder, holder) || !holder ||
        !swofty::mem::read(holder + 8, bus) || !bus)
        return;
    struct {
        const void *vtable;
        uint64_t zero;
        uint8_t vehicle;
        uint8_t pad[3];
        uint32_t request;
        uint8_t tail[0x28];
    } msg{};
    msg.vtable = crew_event_vtable;
    msg.vehicle = uint8_t(vehicle);
    msg.request = request;
    swofty::log("[td-probe] garage: posting MessageTriggerGarageCrewEvent vehicle %d request %u", vehicle, request);
    swofty::game::Ne_Post_BusMessage(reinterpret_cast<void *>(bus), &msg);
}

void post_crew_msg(uint32_t code)
{
    int vehicle = player_vehicle;
    uintptr_t holder = 0, bus = 0;
    if (!crew_msg_vtable || vehicle < 0 || !swofty::mem::read(bus_holder, holder) || !holder ||
        !swofty::mem::read(holder + 8, bus) || !bus)
        return;
    struct {
        const void *vtable;
        uint64_t zero;
        uint8_t vehicle;
        uint8_t pad[3];
        uint32_t code;
        uint8_t tail[0x28];
    } msg{};
    msg.vtable = crew_msg_vtable;
    msg.vehicle = uint8_t(vehicle);
    msg.code = code;
    swofty::log("[td-probe] garage: posting crew report vehicle %d code %u", vehicle, code);
    swofty::game::Ne_Post_BusMessage(reinterpret_cast<void *>(bus), &msg);
}

// What the pause menu's Retire does, for the player's car.
void retire_player()
{
    int vehicle = player_vehicle;
    uintptr_t holder = 0, bus = 0;
    if (!status_request_vtable || vehicle < 0 || !swofty::mem::read(bus_holder, holder) || !holder ||
        !swofty::mem::read(holder + 8, bus) || !bus) {
        swofty::log("[td-probe] retire: not possible (vehicle %d)", vehicle);
        return;
    }
    struct {
        const void *vtable;
        uint64_t zero;
        uint32_t packed;
        uint8_t pad[0x2c];
    } msg{};
    msg.vtable = status_request_vtable;
    msg.packed = uint32_t(vehicle) | status_retired << 8 | 2u << 16 | 1u << 24;
    swofty::log("[td-probe] retire: posting MessageVehicleStatusRequest 0x%08x", msg.packed);
    swofty::game::Ne_Post_BusMessage(reinterpret_cast<void *>(bus), &msg);
    focus_left = 60.0;
}

// Practice and qualifying set PitLaneSystem+0x300 (return to garage on pit entry). There the pause menu's Retire
// ends the whole session (every car NOT_IN_SESSION, the AI parks your car, final classification), so a broken car
// is sent back to its garage instead, as the game's own teleportToGarage does: pit state InGarage + crew request 5.

bool practice_like()
{
    uint8_t flag = 0;
    uintptr_t sys = pit_lane_system.load();
    return sys && swofty::mem::read(sys + 0x300, flag) && flag;
}

// On the game thread (updateVehicle of the player's car).
void return_to_garage()
{
    uint64_t check = exit_check_at.load();
    if (check && GetTickCount64() >= check) {
        exit_check_at = 0;
        if (car_dead) {
            swofty::log("[td-probe] practice/qualifying: no part was replaced, your car goes back to its garage");
            garage_return_wanted = true;
            if (swofty::hud::available())
                swofty::hud::toast("CAR BROKEN", "Fit a new part in the garage first", 4);
        }
    }
    if (!garage_return_wanted.exchange(false))
        return;
    uintptr_t sys = pit_lane_system.load();
    int vehicle = player_vehicle.load();
    if (!sys || vehicle < 0 || !orig_set_vehicle_status)
        return;
    swofty::log("[td-probe] practice/qualifying: sending your broken car back to its garage (pit state was %d)",
                player_pit.load());
    if (player_pit.load() != 9) {
        orig_set_vehicle_status(reinterpret_cast<void *>(sys), uint64_t(vehicle), 9);
        post_crew_event(5);
    }
    player_broken = false;
    retire_requested = false;
    pushing = false;
    retire_in = -1.0;
    car_dead = true;
    garage_part_failed = false;
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

void trigger_failure()
{
    int vehicle = player_vehicle.load();
    int part = choice_int(debug_failure_part, 0);
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
    if (vehicle < 0 || !sys || !swofty::mem::read(sys + 0xc20, count) || uint64_t(vehicle) >= count ||
        !swofty::mem::read(sys + 0x50 + uint64_t(vehicle) * 0x70 + 8, rec) || !rec ||
        !swofty::mem::read(rec + 0x58, car) || !car) {
        swofty::log("[td-probe] debug failure: player car not found (vehicle %d, system %p, count %llu)", vehicle,
                    reinterpret_cast<void *>(sys), (unsigned long long)count);
        return;
    }
    uintptr_t slot;
    if (part == 6) {
        if (!swofty::mem::read(car + 0x538, engine) || !engine)
            return;
        slot = engine + 0x674;
    } else {
        if (part < 0 || part > 5 || !swofty::mem::read(car + 0x530, engine) || !engine)
            return;
        slot = engine + part_offsets[part];
    }
    float before = 0;
    if (!swofty::mem::read(slot, before))
        return;
    // The lowest value the damage tables call terminal (1.0 in this build).
    float value = 1.0f;
    for (int k = part == 6 ? 2 : 0; k <= (part == 6 ? 2 : 1); k++) {
        uint64_t n = 0;
        uintptr_t table = sys + damage_table + k * damage_table_stride;
        if (!swofty::mem::read(sys + damage_table_count + k * damage_table_stride, n) || n > 6)
            continue;
        for (uint64_t i = 0; i < n; i++) {
            uint32_t state = 0;
            float threshold = 0;
            if (!swofty::mem::read(table + i * 8, state) || !swofty::mem::read(table + i * 8 + 4, threshold))
                break;
            swofty::log("[td-probe] damage table %d entry %llu: state %u from %.3f", k, (unsigned long long)i, state,
                        threshold);
            if (state == damage_terminal && threshold > 0 && threshold < value)
                value = threshold;
        }
    }
    player_broken = true;
    *reinterpret_cast<volatile float *>(slot) = value;
    swofty::log("[td-probe] debug failure: vehicle %d part %d at %p: %.3f -> %.3f", vehicle, part,
                reinterpret_cast<void *>(slot), before, value);
    focus_left = 60.0;
}

// Ctrl+Shift+<debug_failure_key>, once per press.
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

int player_speed();

// Throttle: car +0x188, written each frame by the controller update at the site below
// (0.22.4 hardware watch: 605 writes in 10 s, all from there): clamp(input, 0, 1) -> vmovss [r12+0x188], xmm0
// with r12 = car. The stub caps xmm0 at throttle_hook.cap when r12 is the player's car.
constexpr const char *sig_throttle_store =
    "C4 C1 7A 10 8C 94 00 02 00 00 C5 E8 57 D2 C5 EA 5F D1 C5 FA C2 C9 01 C4 E3 69 4A 0D ?? ?? ?? ?? 10 C5 FA 5D C1 "
    "C4 C1 7A 11 84 24 88 01 00 00 44 0F B6 10 41 C0 EA 06 41 80 E2 02 41 0F B6 94 24 B8 01 00 00";
constexpr uintptr_t throttle_store_at = 0x25;
std::atomic<bool> throttle_hooked{false};

void hook_throttle()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_throttle_store, &site);
    uint8_t *stub = n == 1 && site ? alloc_near(site) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] throttle: store has %d hits or no stub memory, not hooked", n);
        return;
    }
    uintptr_t at = site + throttle_store_at;
    emitter e{stub};
    e.bytes({0x50, 0x48, 0xB8});                       // push rax; mov rax, &throttle_hook
    e.u64(reinterpret_cast<uintptr_t>(&throttle_hook));
    e.bytes({0x4C, 0x3B, 0x20, 0x75, 0x05});           // cmp r12, [rax]; jne +5
    e.bytes({0xC5, 0xFA, 0x5D, 0x40, 0x08});           // vminss xmm0, xmm0, [rax+8]
    e.bytes({0x58});                                   // pop rax
    e.bytes({0xC4, 0xC1, 0x7A, 0x11, 0x84, 0x24, 0x88, 0x01, 0x00, 0x00}); // the game's vmovss [r12+0x188], xmm0
    e.jmp(at + 10);
    throttle_hooked = jump_to_stub(at, 10, stub, e.p);
    swofty::log("[td-probe] throttle: %s store at %p", throttle_hooked ? "hooked" : "could not hook", reinterpret_cast<void *>(at));
}

// Engine sound off: a failed ICE already has 0 rpm, but the car's engine audio keeps idling. The audio update
// (0x1409fac38) reads the vehicle's pit state (r14 = vehicle index) and, on a change to 8 (BeingPushedIntoGarage),
// plays play_engine_stop and silences the engine loop, as when a car is pushed into its garage. The stub makes
// the audio see state 8 for engine_off_vehicle; gameplay keeps the real pit state.
constexpr const char *sig_engine_audio_pit_state =
    "41 80 FE 1A 77 29 49 8B 8D 08 0E 00 00 4C 89 F2 48 C1 E2 04 48 8B 4C 11 68 8B 49 04 83 F9 10 77 0E BA FF E3 01 "
    "00 0F A3 CA";
constexpr uintptr_t engine_audio_read_at = 0x14;

void hook_engine_audio()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_engine_audio_pit_state, &site);
    uint8_t *stub = n == 1 && site ? alloc_near(site) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] engine sound: audio site has %d hits or no stub memory, engine keeps idling", n);
        return;
    }
    uintptr_t at = site + engine_audio_read_at;
    emitter e{stub};
    e.bytes({0x48, 0x8B, 0x4C, 0x11, 0x68});           // the game's mov rcx, [rcx+rdx+0x68]
    e.bytes({0x8B, 0x49, 0x04});                       // the game's mov ecx, [rcx+4] (pit state)
    e.bytes({0x50, 0x48, 0xB8});                       // push rax; mov rax, &engine_off_vehicle
    e.u64(reinterpret_cast<uintptr_t>(&engine_off_vehicle));
    e.bytes({0x44, 0x3B, 0x30, 0x75, 0x05});           // cmp r14d, [rax]; jne +5
    e.bytes({0xB9, 0x08, 0x00, 0x00, 0x00});           // mov ecx, 8
    e.bytes({0x58});                                   // pop rax
    e.jmp(at + 8);
    bool ok = jump_to_stub(at, 8, stub, e.p);
    swofty::log("[td-probe] engine sound: %s audio pit state read at %p", ok ? "hooked" : "could not hook",
                reinterpret_cast<void *>(at));
}

uintptr_t player_car()
{
    return throttle_hook.car;
}

int player_speed();


// Retire request (hotkey, later the MFD): marks the car as retiring, nothing breaks and power stays. You pull over
// and stand still (retire after 10 s) or drive into your box (crew pushes it in). A second request cancels it
// while you are still on track and no real failure or retirement is under way.
void request_retire(const char *source)
{
    if (player_vehicle.load() < 0) {
        swofty::log("[td-probe] %s: your car is not known yet", source);
    } else if (retire_requested && player_broken && player_pit.load() == 1 && retire_in.load() == -1.0 &&
               !terminal_mask.load() && throttle_hook.cap >= 1.0f) {
        retire_requested = false;
        player_broken = false;
        swofty::log("[td-probe] %s: retirement cancelled", source);
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT CANCELLED", {}, 3);
    } else if (!player_broken && retire_in.load() > -2.0 && player_pit.load() == 1) {
        retire_requested = true;
        player_broken = true;
        swofty::log("[td-probe] %s: pull over (retire after 10 s standing) or drive into your box", source);
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT", "Pull over and stop, or box", 4);
    } else {
        swofty::log("[td-probe] %s: ignored (broken %d, pit state %d)", source, int(player_broken.load()),
                    player_pit.load());
    }
}

// Pause menu Retire: Game::retireLocalPlayer (0x142cea5b0, `push rsi; push rdi; sub rsp, 0x28; mov edi, edx`, then
// setLocalPlayersStatuses(7)). On track it becomes a retire request instead: nothing breaks, you pull over or drive
// into your box. Choosing Retire again changes nothing.
constexpr const char *sig_retire_local_player =
    "56 57 48 83 EC 28 89 D7 48 89 CE 80 3D ?? ?? ?? ?? 01 75 ?? B8 5F 01 00 00";

// The pause menu's thread. True swallows the game's retirement.
extern "C" bool on_retire_local_player(void *, uint32_t reason)
{
    if (!keep_control || !pause_menu_retire || player_vehicle.load() < 0 || player_pit.load() != 1 ||
        retire_in.load() != -1.0) {
        swofty::log("[td-probe] pause menu retire (reason %u): the game retires your car", reason);
        return false;
    }
    if (player_broken) {
        // Already retiring: no shortcut, you still pull over or drive into your box.
        swofty::log("[td-probe] pause menu retire again: still pull over or box");
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT", "Pull over and stop, or box", 4);
        return true;
    }
    request_retire("pause menu retire");
    return true;
}

void hook_retire_local_player()
{
    uintptr_t at = 0;
    int n = swofty::api->scan(".xtls", sig_retire_local_player, &at);
    uint8_t *stub = n == 1 && at ? alloc_near(at) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] pause menu retire: %d hits or no stub memory, Retire stays the game's", n);
        return;
    }
    emitter e{stub};
    e.bytes({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});    // push rcx; push rdx; push r8; push r9
    e.bytes({0x48, 0x83, 0xEC, 0x28});                // sub rsp, 0x28
    e.call(reinterpret_cast<uintptr_t>(&on_retire_local_player));
    e.bytes({0x48, 0x83, 0xC4, 0x28});                // add rsp, 0x28
    e.bytes({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});    // pop r9; pop r8; pop rdx; pop rcx
    e.bytes({0x84, 0xC0, 0x74, 0x01, 0xC3});          // test al, al; jz +1; ret (swallowed)
    e.bytes({0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x89, 0xD7}); // the game's prologue
    e.jmp(at + 8);
    bool ok = jump_to_stub(at, 8, stub, e.p);
    swofty::log("[td-probe] pause menu retire: %s Game::retireLocalPlayer at %p", ok ? "hooked" : "could not hook",
                reinterpret_cast<void *>(at));
}

void poll_hotkey()
{
    // Debug: Ctrl+Shift+F9 cycles the player's throttle cap: 0.0 (engine off) -> 0.3 -> automatic.
    static bool cut_was_down;
    bool cut = debug_failure_hotkey && ctrl_shift_down(VK_F9);
    if (cut && !cut_was_down) {
        float t = test_cap.load();
        test_cap = t < 0 ? 0.0f : t < 0.1f ? 0.3f : -1.0f;
        swofty::log("[td-probe] throttle test: cap %.1f (car %p, hooked %d)", double(test_cap.load()),
                    reinterpret_cast<void *>(player_car()), int(throttle_hooked.load()));
    }
    cut_was_down = cut;
    static bool failure_was_down, retire_was_down;
    bool down = ctrl_shift_down(function_key(debug_failure_key, 10));
    if (down && !failure_was_down && debug_failure_hotkey)
        trigger_failure();
    failure_was_down = down;
    down = ctrl_shift_down(function_key(retire_key, 11));
    if (down && !retire_was_down && retire_hotkey)
        request_retire("retire hotkey");
    retire_was_down = down;
}

void on_event(const SwoftyEvent *ev, void *)
{
    if (!strcmp(ev->name, SWOFTY_EVENT_SESSION_START)) {
        reset_session();
        session_ended = false;
    } else if (!strcmp(ev->name, SWOFTY_EVENT_SESSION_END)) {
        session_ended = true;
        reset_session();
        pit_lane_system = 0;
        controller_system = nullptr;
    }
    swofty::log("[td-probe] event %s a=%lld b=%lld player_index=%d", ev->name, (long long)ev->a, (long long)ev->b,
                player_index());
}

// SpawnGarageCrew(module, slot) uses slot entry [[module+0x60]+0x838+slot*8];
// the crew instance is per vehicle (module+0x68+v*8). The slot-to-vehicle map
// is unknown, so log the non-null slots (and their first bytes) when they change.
void probe_crew_entry(double dt)
{
    static double since;
    static uintptr_t last[33];
    since += dt;
    if (!crew_module || !crew_asset_diagnostics || since < 2.0)
        return;
    since = 0;
    auto module = reinterpret_cast<uintptr_t>(*crew_module);
    if (!module)
        return;
    uintptr_t now[33] = {};
    swofty::mem::read(module + 0x60, now[32]);
    for (int i = 0; i < 32 && now[32]; i++)
        swofty::mem::read(now[32] + 0x838 + uintptr_t(i) * 8, now[i]);
    if (!memcmp(now, last, sizeof now))
        return;
    memcpy(last, now, sizeof now);
    int v = player_vehicle.load();
    uintptr_t instance = 0;
    if (v >= 0 && v < 0x20)
        swofty::mem::read(module + 0x68 + uintptr_t(v) * 8, instance);
    swofty::log("[td-probe] garage: crew slots: table %p, player vehicle %d instance %p",
                reinterpret_cast<void *>(now[32]), v, reinterpret_cast<void *>(instance));
    for (int i = 0; i < 32; i++) {
        if (!now[i])
            continue;
        uint64_t q[12] = {};
        for (int k = 0; k < 12; k++)
            swofty::mem::read(now[i] + k * 8, q[k]);
        swofty::log("[td-probe] garage: crew slot %d entry %p: %016llx %016llx %016llx %016llx %016llx %016llx "
                    "%016llx %016llx %016llx %016llx %016llx %016llx", i, reinterpret_cast<void *>(now[i]),
                    q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
    }
}

// Speed source: the game's own UDP telemetry (Settings > Telemetry: UDP on, 127.0.0.1:20777, format 2026).
// Swofty's telemetry API does not fill car fields yet. Header 29 bytes (packetId @6, playerCarIndex @27);
// Format 2026 has 24 car slots everywhere. CarTelemetry (id 6) = 29 + 24 x 59 + 3 bytes (the 2025 60-byte car
// minus the DRS byte): u16 speed km/h @0, f32 throttle @2, i8 gear @15, u16 rpm @16.
uint16_t udp_port()
{
    int n = choice_int(udp_port_choice, 20777);
    return uint16_t(n >= 1024 && n <= 65535 ? n : 20777);
}
constexpr int udp_cars = 24, udp_car_size = 59;
std::atomic<int> udp_speed[udp_cars], udp_rpm[udp_cars], udp_gear[udp_cars];
std::atomic<float> udp_throttle[udp_cars];
std::atomic<int> udp_player{-1};
std::atomic<uint64_t> udp_packets{0}, udp_last_ms{0};
// CarDamage (id 10) = 29 + 24 x 46 bytes (2025 car layout): u8 gearbox @36, engine damage % @37, ICE wear @41,
// engine blown @44, engine seized @45. Packed as gearbox | engine << 8 | ice << 16 | blown << 24 | seized << 25.
constexpr int udp_damage_size = 46;
std::atomic<uint32_t> udp_engine_state{0xffffffff};

DWORD WINAPI udp_thread(void *)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        return 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    // No SO_REUSEADDR: if another telemetry app already has the port, fail instead of taking its packets.
    uint16_t port = udp_port();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr)) {
        swofty::log("[td-probe] udp: cannot listen on port %u (err %d): another app may use it. Pick another port in "
                    "the addon settings and set it in the game (or forward to it from your telemetry app).",
                    port, WSAGetLastError());
        return 0;
    }
    swofty::log("[td-probe] udp: listening on port %u for the game's telemetry", port);
    static char buf[2048];
    for (;;) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n < 29)
            continue;
        auto *b = reinterpret_cast<const uint8_t *>(buf);
        uint16_t format;
        memcpy(&format, b, 2);
        udp_packets.fetch_add(1);
        static bool seen_id[32];
        if (b[6] < 32 && !seen_id[b[6]]) {
            seen_id[b[6]] = true;
            swofty::log("[td-probe] udp: first packet id %u, format %u, %d bytes, player index %u", b[6], format, n, b[27]);
        }
        // Layout checked with format 2026 (F1 25 setting); other formats are ignored.
        if (format != 2026)
            continue;
        udp_player = b[27];
        if (b[6] == 10 && n == 29 + udp_cars * udp_damage_size && b[27] < udp_cars) {
            const uint8_t *car = b + 29 + b[27] * udp_damage_size;
            udp_engine_state = car[36] | car[37] << 8 | car[41] << 16 | (car[44] & 1) << 24 | (car[45] & 1) << 25;
            continue;
        }
        if (b[6] != 6)
            continue;
        if (n != 29 + udp_cars * udp_car_size + 3) {
            static bool warned;
            if (!warned)
                swofty::log("[td-probe] udp: car telemetry has %d bytes, expected %d; speed source off", n,
                            29 + udp_cars * udp_car_size + 3);
            warned = true;
            continue;
        }
        for (int c = 0; c < udp_cars; c++) {
            const uint8_t *car = b + 29 + c * udp_car_size;
            uint16_t speed, rpm;
            float thr;
            memcpy(&speed, car, 2);
            memcpy(&thr, car + 2, 4);
            memcpy(&rpm, car + 16, 2);
            udp_speed[c] = speed;
            udp_rpm[c] = rpm;
            udp_gear[c] = int8_t(car[15]);
            udp_throttle[c] = thr;
        }
        udp_last_ms = GetTickCount64();
    }
}

// Player speed from UDP in km/h, or -1 if there is no fresh packet (paused, UDP off).
int player_speed()
{
    int p = udp_player.load();
    uint64_t last = udp_last_ms.load();
    if (session_ended || p < 0 || p >= udp_cars || !last || last <= telemetry_not_before.load() ||
        GetTickCount64() - last > 500)
        return -1;
    return udp_speed[p].load();
}

// A broken car that stands still on track for 10 s retires, like the game's own retirement.
void standstill_retire(double dt)
{
    int speed = player_speed();
    if (!player_broken || retire_in.load() != -1.0 || player_pit.load() != 1 || speed < 0) {
        still_for = 0;
        // Once retiring the engine stays off; it only comes back for a car that is no longer broken.
        if (!player_broken)
            engine_off_standing = false;
        return;
    }
    still_for = speed <= 2 ? still_for + dt : 0;
    // Pulled over: the driver switches the engine off after 6 s, before the retirement at 10 s.
    if (still_for >= 6.0 && !engine_off_standing)
        swofty::log("[td-probe] engine: standing still for 6 s, engine off");
    engine_off_standing = still_for >= 6.0;
    if (still_for >= 10.0) {
        engine_off_standing = true;
        swofty::log("[td-probe] retire: your broken car stood still for 10 s, retiring");
        still_for = 0;
        retire_in = 0.0;
    }
}

// Warns once per session when no UDP car telemetry has arrived at all (UDP off, wrong format or port); with
// log_telemetry also logs the player's UDP values every 3 s.
void probe_speed(double dt)
{
    static double since, waited;
    static int warned_vehicle = -1;
    int p = udp_player.load(), v = player_vehicle.load();
    bool fresh = p >= 0 && p < udp_cars && udp_last_ms.load() && GetTickCount64() - udp_last_ms.load() < 1000;
    waited = v >= 0 && !fresh && player_pit.load() == 1 ? waited + dt : 0;
    if (waited > 15 && warned_vehicle != v && !udp_last_ms.load()) {
        warned_vehicle = v;
        swofty::log("[td-probe] udp: no car telemetry from the game (packets %llu). Set Telemetry Settings: UDP on, "
                    "127.0.0.1, port %u, format 2026. Without it, standing still does not retire a broken car (use the "
                    "retire hotkey).",
                    (unsigned long long)udp_packets.load(), unsigned(udp_port()));
    }
    since += dt;
    if (!log_telemetry || since < 3.0 || !fresh)
        return;
    since = 0;
    swofty::log("[td-probe] udp: player %d (vehicle %d) speed=%d km/h rpm=%d gear=%d throttle=%.2f cap=%.2f", p, v,
                udp_speed[p].load(), udp_rpm[p].load(), udp_gear[p].load(), udp_throttle[p].load(),
                double(throttle_hook.cap));
}

// Engine-off research: crash damage already shows rpm 0 in UDP while a failed ICE idles at ~3900 rpm. Logs the
// player's UDP engine damage flags when they change, and rpm/gear/speed every second for 20 s after the car breaks.
void probe_engine(double dt)
{
    static uint32_t last_state = 0xffffffff;
    static double broken_for = -1, since;
    int p = udp_player.load();
    if (p < 0 || p >= udp_cars)
        return;
    uint32_t state = udp_engine_state.load();
    if (state != last_state && state != 0xffffffff) {
        last_state = state;
        swofty::log("[td-probe] engine: udp damage gearbox=%u%% engine=%u%% ice_wear=%u%% blown=%u seized=%u "
                    "(rpm=%d gear=%d speed=%d mask=0x%x)",
                    state & 0xff, state >> 8 & 0xff, state >> 16 & 0xff, state >> 24 & 1, state >> 25 & 1,
                    udp_rpm[p].load(), udp_gear[p].load(), udp_speed[p].load(), terminal_mask.load());
    }
    if (!player_broken) {
        broken_for = -1;
        return;
    }
    if (broken_for < 0)
        broken_for = since = 0;
    broken_for += dt;
    since += dt;
    if (broken_for > 20.0 || since < 1.0)
        return;
    since = 0;
    swofty::log("[td-probe] engine: broken %.0f s rpm=%d gear=%d speed=%d throttle=%.2f cap=%.2f mask=0x%x",
                broken_for, udp_rpm[p].load(), udp_gear[p].load(), udp_speed[p].load(), udp_throttle[p].load(),
                double(throttle_hook.cap), terminal_mask.load());
}

void on_frame(double dt, void *)
{
    static double since;
    if (session_ended)
        return;
    uint64_t request = pending_failure.load();
    if (request && GetTickCount64() >= (request >> 16) && pending_failure.compare_exchange_strong(request, 0))
        swofty::log("[td-probe] debug failure: request timed out without a player update");
    poll_hotkey();
    probe_crew_entry(dt);
    probe_speed(dt);
    probe_engine(dt);
    double crew = crew_request_in.load();
    if (crew >= 0) {
        crew -= dt;
        crew_request_in = crew < 0 ? -1.0 : crew;
        if (crew < 0)
            post_crew_event(crew_request.exchange(0));
    }
    if (int request = pending_crew.exchange(-1); request >= 0)
        post_crew_event(uint32_t(request));
    double report = crew_msg_in.load();
    if (report >= 0) {
        report -= dt;
        crew_msg_in = report < 0 ? -1.0 : report;
        if (report < 0)
            post_crew_msg(crew_msg.exchange(0));
    }
    if (spawn_crew && crew_spawn_now.exchange(false) && !crew_spawn_wanted.exchange(true)) {
        spawn_player_crew();
        crew_seq_t = 0.0;
    }
    if (double t = crew_seq_t.load(); t >= 0) {
        double n = t + dt;
        if (t < 0.3 && n >= 0.3)
            post_crew_event(crew_prepare_push);
        if (t < 0.8 && n >= 0.8)
            post_crew_event(crew_push);
        crew_seq_t = n >= 0.8 ? -1.0 : n;
    }
    standstill_retire(dt);
    double r = retire_in.load();
    if (r >= 0) {
        r -= dt;
        retire_in = r < 0 ? -2.0 : r; // -2: done for this session
        if (r < 0 && practice_like())
            garage_return_wanted = true;
        else if (r < 0)
            retire_player();
    }
    if (!log_telemetry)
        return;
    focus_left = focus_left > dt ? focus_left - dt : 0;
    double period = 0.5;
    if (focus_left > 0)
        period = 0.1;
    since += dt;
    if (since < period)
        return;
    since = 0;
    int32_t p = player_index();
    swofty::telemetry::CarState c{};
    if (p < 0 || swofty::telemetry::car(uint32_t(p), &c) != swofty::telemetry::Result::ok)
        return;
    swofty::log("[td-probe] car%s lap=%u dist=%.1f speed=%.1f rpm=%.0f gear=%d thr=%.2f brk=%.2f steer=%.2f "
                "pos=%.1f,%.1f,%.1f",
                focus_left > 0 ? "*" : "", c.lap, c.lap_distance, c.speed, c.rpm, c.gear, c.throttle, c.brake,
                c.steer, c.world_pos[0], c.world_pos[1], c.world_pos[2]);
}

}

SWOFTY_WRAP(Ne_Post_BusMessage, uint64_t, void *bus, void *msg)
{
    if (status_request_vtable && msg && *static_cast<const void *const *>(msg) == status_request_vtable) {
        uint32_t packed;
        memcpy(&packed, static_cast<const uint8_t *>(msg) + 0x10, 4);
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
            player_broken && retire_in.load() > -2.0) {
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
        if (mine) {
            focus_left = 60.0;
            if (keep_control) {
                swofty::log("[td-probe] keep_control: dropped terminal damage message of vehicle %u", vehicle);
                terminal_mask = terminal_mask.load() | mask;
                player_broken = true;
                return 0;
            }
        }
    }
    return orig(bus, msg);
}

SWOFTY_ON_CHANGE(keep_control, bool on)
{
    swofty::log("[td-probe] keep_control %s", on ? "on" : "off");
}

SWOFTY_ON_LOAD
{
    CloseHandle(CreateThread(nullptr, 0, udp_thread, nullptr, 0, nullptr));
    reset_session();
    hook_asset_diagnostics();
    patch_garage_assets_in_race();
    guard_crew_scene_read();
    hook_throttle();
    hook_engine_audio();
    hook_retire_local_player();
    hook_thunk("VehicleControllerSystem::setController", sig_set_controller,
               reinterpret_cast<void *>(&on_set_controller), reinterpret_cast<void **>(&orig_set_controller));
    hook_thunk("Ai MessagePitStateChanged handler", sig_pit_state_changed,
               reinterpret_cast<void *>(&on_pit_state_changed), reinterpret_cast<void **>(&orig_pit_state_changed));
    hook_update_vehicle();
    hook_vslot("DeploymentPointsAccumulatorStoppedCars::update", sig_stopped_cars_update,
               reinterpret_cast<void *>(&on_stopped_cars_update), reinterpret_cast<void **>(&orig_stopped_cars_update));
    hook_vslot("DeploymentPointsAccumulatorDebris::update", sig_debris_update,
               reinterpret_cast<void *>(&on_debris_update), reinterpret_cast<void **>(&orig_debris_update));
    find_terminal_message();
    find_status_request();
    find_crew_event();
    find_pit_entry();
    find_crew_msg();
    hook_pit_stop_job();
    find_garage_crew_spawn();
    hook_crew_states();
    detour("PitLaneSystem::setVehicleStatus", sig_set_vehicle_status, set_vehicle_status_prologue,
           sizeof set_vehicle_status_prologue, reinterpret_cast<void *>(&on_set_vehicle_status),
           reinterpret_cast<void **>(&orig_set_vehicle_status));
    for (const char *e : {SWOFTY_EVENT_SESSION_START, SWOFTY_EVENT_SESSION_END, SWOFTY_EVENT_RACE_START,
                          SWOFTY_EVENT_STATE, SWOFTY_EVENT_FLAG, SWOFTY_EVENT_RESULTS})
        swofty::events::subscribe(e, on_event);
    swofty::events::on_frame(on_frame);
    swofty::log("[td-probe] loaded version=0.32.4, keep_control=%d asset_diagnostics=%d garage_assets_in_race=%d crew_in_race=%d",
                bool(keep_control), bool(crew_asset_diagnostics), bool(garage_assets_in_race),
                bool(garage_crew_in_race));
}

SWOFTY_ADDON()
