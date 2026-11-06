// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Game build signatures and the first hook pointers.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Steam build d873a1c8d2044cef. The signatures come from the disassembly notes in re/ (re/README.md) and from the
// Swofty knowledge base.
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
std::atomic<bool> restart_cleanup_available{true};
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
