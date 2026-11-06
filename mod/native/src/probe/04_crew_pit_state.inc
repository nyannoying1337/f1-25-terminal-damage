// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Crew and pit-entry state, controller reasons, pit state names.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

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
// Seconds left on the player's retire timer (pit box, garage push, standstill, game failure), counted down in on_frame:
// -1 idle, -2 the timer ran out and the retirement (practice and qualifying: the return to the garage) was started.
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
// (0 not started, 2 the instant send-off, request 2, is queued; log_crew_state claims it with 1 and sets 2 at once).
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
// The vehicle the game last handed to a human: the player in an offline session.
std::atomic<int> player_vehicle{-1};
// The game handed the player's car to the AI with RETIRED. Kept through the DNF reset (NOT_IN_SESSION); a flashback
// to before the damage clears it (flashback_check).
std::atomic<int> confirmed_retired_vehicle{-1};
// Encoded pit state + 1; zero means not observed this session.
std::atomic<unsigned> observed_pit_state[max_vehicles]{};

uint64_t (*orig_set_controller)(void *, uint64_t, uint32_t, int32_t);
uint64_t (*orig_pit_state_changed)(void *, void *);

bool is_player(uint64_t vehicle)
{
    return vehicle < max_vehicles && human[vehicle];
}
