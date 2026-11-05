// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Terminal Damage: preserve player control after failure, apply power loss,
// and retire through a roadside stop or the player's garage crew.
//
// The two targets are L1 entries of the Swofty knowledge base (static only, not in bindings.json),
// so the mixin system cannot hook them. Both are 5-byte `jmp rel32` thunks in .xtls, which we
// redirect ourselves: the thunk's rel32 is pointed at a near relay that jumps to our handler, and
// the handler calls the thunk's original target.

// winsock2.h has to come before windows.h, which the SDK headers include.
#include <winsock2.h>

// The SDK headers cast GetProcAddress to their table getters.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#include <swofty/events.hpp>
#include <swofty/game/core.hpp>
#include <swofty/mem.hpp>
#include <swofty/mixin.hpp>
#include <swofty/systems/cars.hpp>
#include <swofty/systems/sessions.hpp>
#include <swofty/systems/spectator.hpp>
#pragma clang diagnostic pop

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <random>
#include <utility>
#include <vector>
#include <windows.h>
#include "performance_probe.hpp"
#include "guarded_copy.hpp"
#include "garage_assignment.hpp"
#include "player_state_service.hpp"
#include "presets.hpp"

// The SDK header casts GetProcAddress to the table getter.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#include <swofty/hub.hpp>
#include <swofty/hud.hpp>
#include <swofty/lang.hpp>
#include <swofty/overlay.hpp>
#include <swofty/storage.hpp>
#include <swofty/telemetry.hpp>
#include <swofty/popup.hpp>
#include <swofty/assets.hpp>
#include <swofty/career.hpp>
#pragma clang diagnostic pop

SWOFTY_SETTING(bool, keep_control, true);
SWOFTY_SETTING(bool, engine_off_sound, true);
SWOFTY_SETTING(bool, pause_menu_retire, true);
SWOFTY_SETTING(bool, radio_command_probe, true);
SWOFTY_SETTING(bool, performance_diagnostics, false);
SWOFTY_SETTING(std::string_view, player_wheel_behavior, "stock");
bool wheel_tether_test=false;
SWOFTY_SETTING(bool, ai_wheel_tethers, false);
bool tether_plane_experiment=false;
bool tether_distance_experiment=false;
bool tether_collision_experiment=false;
SWOFTY_SETTING(bool, log_telemetry, false);
SWOFTY_SETTING(bool, debug_failure_hotkey, false);  // test keys: on in the test builds 0.51.122-0.51.135, off in a release
SWOFTY_SETTING(std::string_view, debug_failure_key, "F10");
SWOFTY_SETTING(std::string_view, debug_failure_part, "0");
SWOFTY_SETTING(std::string_view, damaged_power, "0.3");
SWOFTY_SETTING(bool, retire_in_pit_box, true);
SWOFTY_SETTING(bool, red_flag_retire, true);
SWOFTY_SETTING(bool, keep_game_failure, true);
SWOFTY_SETTING(std::string_view, game_failure_timeout, "dsq");
// Debug: force the crash dice rolls (random, success, fail) instead of playing many races.
SWOFTY_SETTING(std::string_view, debug_limp_roll, "random");
SWOFTY_SETTING(std::string_view, debug_repair_roll, "random");
SWOFTY_SETTING(std::string_view, debug_garage_repair, "normal");
// Debug: front-left suspension geometry/damage test (suspension_test.hpp), off, toe, camber or damage.
SWOFTY_SETTING(std::string_view, debug_suspension_test, "off");
SWOFTY_SETTING(std::string_view, gp_failures, "realistic");  // the Realistic preset (0.75)
SWOFTY_SETTING(std::string_view, gp_fault_type, "random");
SWOFTY_SETTING(std::string_view, failure_era, "2025");
SWOFTY_SETTING(bool, engineer_radio, true);
SWOFTY_SETTING(std::string_view, damage_panel, "auto");
SWOFTY_SETTING(bool, garage_push, true);
SWOFTY_SETTING(bool, pit_retire_clearance, true);
SWOFTY_SETTING(bool, garage_crew_in_race, true);
SWOFTY_SETTING(bool, retire_hotkey, true);
SWOFTY_SETTING(std::string_view, retire_key, "F11");
SWOFTY_SETTING(bool, crew_asset_diagnostics, false);
SWOFTY_SETTING(std::string_view, udp_port_choice, "off");
SWOFTY_SETTING(bool, garage_assets_in_race, true);
SWOFTY_SETTING(bool, suspension_damage, false);  // experimental and opt-in since 0.75 (Heavier Damage users)
SWOFTY_SETTING(bool, suspension_damage_ai, false);
SWOFTY_SETTING(std::string_view, preset, "realistic");  // the defaults are the Realistic preset (0.75)
std::atomic<bool> crew_diagnostics_active{false}; // cached for the frequently called crew callbacks

void report_pit_clearance();
bool release_retirement_box_entry();

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
// The player's car has terminal damage the game was not told about, so they drive it themselves.
#include "flashback_tracker.hpp"
#include "failure_model.hpp"
#include "damage_icons.hpp"
#include "brake_telemetry.hpp"

std::atomic<bool> player_broken{false};
std::atomic<uint64_t> status7_at{0}; // last MessageVehicleStatusRequest status 7 (retire) for the player
std::atomic<uint64_t> own_retire_until{0}; // the player or the addon is retiring the car: a RETIRED hand-over is wanted
std::atomic<bool> retire_requested{false}; // broken only by a retire request (hotkey/MFD), no part failed
std::atomic<uint64_t> pending_radio_command{0}; // marker bit + command id, never a game pointer
// Practice/qualifying: the broken car is back in its garage and stays there for the rest of the session.
std::atomic<bool> car_dead{false};
// Practice/qualifying: the broken car stopped on track and was taken to its garage. A car abandoned on the circuit is
// withdrawn from the session (FIA 26.18 / B1.8.9; qualifying 39.6 / B4.3.2), so a new part does not bring it back.
std::atomic<bool> garage_withdrawn{false};
bool garage_part_failed; // the dead car in its garage was seen with a failed part (game thread)
// Component types (0 ICE, 1 MGU-K, 2 MGU-H, 3 ES, 4 TC, 5 CE, 6 gearbox) seen failed in the garage, and whether the
// Career power unit allocation shows no replacement for them yet (part_lock_tick, issue #5).
std::atomic<uint32_t> garage_failed_types{0};
std::atomic<bool> part_lock{false};
// The dead car may leave its garage; 300 ms later (tick below) it is sent back unless a part swap unlocked it. A
// career part swap shows only a few ms after the OnTrack (0.32.0 test: refused at .828, ICE 0 at .836). Refusing
// the OnTrack left the camera in the box (0.32.2 test); setting OnTrack later from the garage got the car pushed
// back in (0.32.1).
std::atomic<uint64_t> exit_check_at{0};
// Parts mask of the player's last dropped MessageVehicleTerminallyDamaged (bits 3/4 = crash/body damage).
std::atomic<uint32_t> terminal_mask{0};
// The game's terminal damage record of the player's car (VehicleTerminalDamageSystem, record sys+0x50+v*0x70), read
// after its update while the car is broken: overall state (+0x68, 5 = terminal) and the parts at state 5 (+0x10 + i*4,
// same bits as the terminal message: 0 engine, 1 PU, 2 gearbox, 3 wheel lost, 4 suspension, 5 tyres, 6-10 aero/body).
std::atomic<int> live_terminal_state{-1};
std::atomic<uint32_t> live_terminal_mask{0};
// Pit state of the player's car when the latest red flag or safety car took control (-1 unknown).
std::atomic<int> pit_at_suspension{-1};
// Red flag with repairable accident damage: waiting for the game's repairs.
std::atomic<bool> red_flag_repair_wait{false};
// Tick of the game's MessageFlashbackUsed (the player resumed from a flashback), 0 = none pending. Read by
// flashback_check on the game thread. Without the hook, a terminal record going back to normal on track stands in.
std::atomic<uint64_t> flashback_used_at{0};
std::atomic<uint32_t> flashback_count{0};  // every flashback, for the suspension damage undo
// Issue #10 logging: AI cars after their terminal damage message, until they reach their garage.
std::atomic<uint32_t> ai_watch_mask{0};
std::atomic<uint64_t> ai_watch_since[32];
namespace susp {
void game_thread(uintptr_t car);
bool game_damage(float d[4]);
bool speed_wanted();
void request_reset(const char *why);
bool ai_wanted();
void ai_game_thread(int v, uintptr_t car);
void ai_forget(int v);
void ai_terminal(int v);
inline const char *corner_name(int i);
void hold_minor_toasts(uint32_t ms);
uint32_t collapsed_corners();
}
bool post_vehicle_status(int vehicle, uint32_t status, uint32_t reason);
bool flashback_hooked = false;
void flashback_check(uintptr_t car);
// Crash damage on one corner: the car may still be driven (limping) back to the pits. -1 undecided, 0 cannot drive,
// 1 limping; limp_corner: the corner it limps on (RL RR FL FR = 0-3), -1 none. crash_corners: the packed corner
// states (3 bits each) when it was decided.
std::atomic<int> limp_state{-1};
std::atomic<int> limp_corner{-1};
std::atomic<double> limp_watch_left{0}; // seconds of limp watch lines left after a crash decision (frame thread)
// Race: a car that can still crawl on a broken corner must stop as soon as it is safe (an unsafe car may not
// continue). Seconds of driving left before it is retired, 0 = no deadline. Practice and qualifying crawl to the garage.
constexpr double race_stop_grace = 20.0;
std::atomic<double> race_stop_left{0};
std::atomic<uint32_t> crash_corners{0};
// Practice/qualifying garage repair: 0 none, 1 wanted, 2 job running, 3 finished. It rebuilds the corner a car limped
// in on (crash_corners) or, since 0.51.135, services the corners that took damage but hold (garage_service_corners,
// started by susp::tick when the car reaches its garage).
std::atomic<int> garage_repair{0};
std::atomic<uint32_t> garage_service_corners{0};  // bit i = corner i; 0: no service
std::atomic<float> garage_service_seconds{0};
std::atomic<bool> garage_service_crack{false};
bool start_garage_service(float seconds, uint32_t corners, bool crack);
void repair_label(bool suspension);
// Set while the addon teleports the abandoned car into its garage: no repair for it (FIA B1.8.9).
std::atomic<bool> garage_teleport{false};
std::atomic<uint64_t> corner_toast_at{0};
std::atomic<int> corner_toast_minutes{0};
bool garage_exit_locked();
std::atomic<float> garage_repair_left{0};  // seconds left of the job
constexpr float limp_power = 0.35f;
uint32_t corner_states(uintptr_t car, bool &ok);
unsigned roll_percent();
unsigned forced_roll(std::string_view mode, unsigned chance, unsigned roll);
std::atomic<bool> red_flag_repaired_toast{false};
bool practice_like();
uintptr_t player_car();
bool crash_repair_available();
bool has_detached_corner(uint32_t packed);
void garage_repair_finish(uintptr_t car);
extern bool repair_update_hooked;
// Player's last pit status (1 OnTrack) and how long the broken car has stood still on track.
std::atomic<int> player_pit{1};
double gp_clock = 0, gp_at = -1;
// The fixed-chance modes' failure by laps (fm::race_point): its lap, the lap seen last and when it began (gp_clock),
// the last full lap's time.
fm::RacePoint gp_point;
int gp_lap_seen = -1;
double gp_lap_started = 0, gp_lap_time = 0;
// Realistic Grand Prix failures (gp_failures "realistic", issue #11): the outcome the session rolled
// (fm::Outcome, -1 none) and the drive clock at which its warning signs start (1e18 = a sudden failure).
struct gp_warning_state {
    int outcome = -1;
    double warn_at = 1e18;
    bool announced = false;
    double next_cut = 0, cut_end = 0;
};
gp_warning_state gp_warn;
// Gearbox decline (a gearbox failure with warning signs, or the debug F6 cycle): fm::DeclinePath (-1 none), its own
// drive clock and length, the stage reached, the lost gear (gearbox index) and the shift time scale it asks for
// (read by slow_shift_tick on the game thread, 0 = none).
struct gearbox_decline_state {
    int path = -1;
    double clock = 0, length = 0;
    int stage = -1;
    int gear = 0xff;
    bool test = false;  // started by the test run, which ends it when it stops
};
gearbox_decline_state gearbox_decline;
std::atomic<float> decline_shift_scale{0.0f};
void start_gearbox_decline(double length, int path);
// Debug test run (Ctrl+Shift+F4): the game's AI drives your car (setController reason ROBODRIVER, BENCHMARK as the
// fallback) while the addon runs every partial failure in turn and logs what the gearbox did. test_run_on gates the
// game-thread shift measurement; the shift stats are filled on the game thread and read and reset per step.
// Damage panel (issue #15): values the game and frame threads publish for the render thread.
std::atomic<float> shown_shift_scale{0.0f};  // slow shifts in force (times the shift time), 0 none
std::atomic<int> shown_missing_gear{11};     // gearbox +0x664, 11 = none
std::atomic<int> shown_warning{-1};          // fm::Outcome whose warning signs run, -1 none
std::atomic<int> shown_decline_stage{-1};    // gearbox decline stage, -1 none

// Engineer radio (issue #12): the game's own voice lines. The failure planner plays its radio through the player's
// failure object: [[obj+0x98]+8] is the speech interface, vtable+0x98 (self, group name, 0, 0) with a group name of
// audio/2025/speechEvents.bin (the planner reads its names from gamedb keys like
// canned_events.player_data.failure_data.track_vo.suspected via 0x1409b2120, which stores the decoded string).
enum RadioLine : int {
    radio_car_issue,
    radio_ers_derating,
    radio_brakes,
    radio_electrical,
    radio_cooling,
    radio_lubricant,
    radio_gearbox,
    radio_gearbox_jam,
    radio_engine_a,
    radio_engine_b,
    radio_mgu,
    radio_turbo,
    radio_control_electronics,
    radio_energy_store,
    radio_mech_a,
    radio_mech_b,
    radio_mech_c,
    radio_mech_dnf,
    radio_box,
    radio_fault_fixed,
    radio_damage_strategy,
    n_radio_lines
};
constexpr const char *radio_names[n_radio_lines] = {
    "eng_carissue",          "eng_ers_derating",       "eng_randomfault_brakes",
    "eng_randomfault_electrical", "eng_randomfault_cooling", "eng_randomfault_lubricant",
    "eng_pu_fault_gb",       "eng_gearbox_jam",        "eng_enginefault_parta",
    "eng_enginefault_partb", "eng_pu_fault_mgu",       "eng_pu_fault_turbo",
    "eng_pu_fault_controlelectronics", "eng_pu_fault_energystore_risk", "eng_mechfailure_parta",
    "eng_mechfailure_partb", "eng_mechfailure_partc",  "eng_mechfailure_dnf",
    "eng_vc_fl_box",         "eng_faultfixed",         "eng_newstrat_damage",
};
// Lines waiting for the game thread (bit per line), played one at a time at least 6 s apart.
std::atomic<uint32_t> radio_pending{0};
void radio_say(int line, bool forced = false)
{
    if ((engineer_radio || forced) && line >= 0 && line < n_radio_lines)
        radio_pending.fetch_or(1u << line);
}

// Rear brake failure (fm::f_rear_brakes): the change of the rear braking (> 0 the share lost, < 0 the share gained:
// the rear grabs; 0 none) and the tick it ends at (0 = the rest of the session). Set on the frame thread; the game
// thread turns it (with brake_fade, the fade of failing brakes, 0..1) into rear_brake_factor, which the brake step
// hook (on_brake_step, physics thread) applies to the player's car brake_car.
std::atomic<float> rear_brake_loss_target{0.0f};
std::atomic<uint64_t> rear_brake_until{0};
std::atomic<float> brake_fade{0.0f};
std::atomic<float> rear_brake_factor{1.0f};
std::atomic<uintptr_t> brake_car{0};
std::atomic<uint32_t> brake_steps_scaled{0};
void start_rear_brake_fault(double loss, double seconds)
{
    rear_brake_until = seconds > 0 ? GetTickCount64() + uint64_t(seconds * 1000.0) : 0;
    rear_brake_loss_target = float(loss);
    bool grabs = loss < 0;
    swofty::log("[td-probe] fault: rear brake failure, rear braking %s %.0f %%%s", grabs ? "up" : "down",
                std::fabs(loss) * 100.0, seconds > 0 ? " (timed)" : " for the rest of the race");
    // Brake-by-wire from 2014 (the Failure season's era); before it a plain rear brake problem.
    bool by_wire = fm::season_of(failure_era).era >= 1;
    radio_say(radio_brakes);
    if (swofty::hud::available())
        swofty::hud::toast("ENGINEER",
                           grabs ? (by_wire ? "Brake-by-wire fault, the rear brakes are grabbing. Bias forward, the "
                                              "rear will lock on entry."
                                            : "Rear brakes are grabbing. Bias forward, the rear will lock on entry.")
                           : by_wire ? "We lost brake-by-wire, the rear is on the backup. Bias forward, careful into "
                                       "the braking zones."
                                     : "Rear brake problem. Bias forward, careful into the braking zones.",
                           6);
}
std::atomic<bool> test_run_on{false};
std::atomic<bool> test_run_restarted{false};  // the race restarted: the run stops on the next frame
// The race restarted (no session end or start comes): on_frame resets the race's failures and faults
// (reset_race_faults) on the next frame.
std::atomic<bool> race_restart_pending{false};
void test_run_reset();
void test_run_start(bool ai);
int test_key_action(bool edge, uint64_t now, bool running);
void test_run_stop(const char *why);
void test_run_stop_by_you();
void preset_tick();
void welcome_tick(uint64_t now);
bool welcome_menu_ready(uint64_t since_first_ms);
struct shift_stats {
    std::atomic<uint32_t> count[2]{};     // 0 up, 1 down
    std::atomic<uint64_t> total_us[2]{};
    std::atomic<uint32_t> max_us[2]{};
    std::atomic<uint32_t> gear_seen[12]{};  // shifts that ended in this gearbox index
    std::atomic<bool> reset{false};
};
shift_stats test_shifts;
// Partial failure (one per session): drive clock when it starts (-1 not rolled, 1e18 none) and its fault type.
double gp_fault_at = -1;
int gp_fault_type_rolled = -1;
// The game's failure manager ([scheduler+0xf0], seen at the session's scheduling) and a fault the frame thread wants
// registered for the player's car on the game thread: type | duration*10 << 8 | seconds << 40 | gear << 48, 0 = none.
std::atomic<uintptr_t> failure_mgr{0};
std::atomic<uint64_t> fault_request{0};
// Breakdowns to arm on the game thread (the game's own failure sequence): vehicle bits and the part per vehicle.
std::atomic<uint32_t> breakdown_request{0};
// The player's armed breakdown (game thread): if the game has not started it (state +0xa1 still 0) 15 s later, the
// addon breaks the part itself (0.51.107 log: a gearbox breakdown armed in a Grand Prix never started).
struct breakdown_watch_state {
    uintptr_t obj = 0;
    uint8_t part = 0xff;
    uint64_t at = 0;
};
breakdown_watch_state breakdown_watch;
std::atomic<uint8_t> breakdown_part[32];
// Realistic mode, AI cars: drive clock of each car's failure (1e18 none, -1 not rolled) and its part.
double ai_fail_at[32];
int ai_fail_part[32];
bool ai_failures_rolled = false;
// Engine power over time (power_loss.hpp, 0.51.114): the warning signs of the rolled failure (gp_warn) and a partial
// power fault each run a course; their product is the player's engine power, applied on the frame thread as a Swofty
// engine.power_scale layer (the drive torque times the share). warning_cap: the same as a throttle cap when the core
// has no car layers, 1 = none.
std::atomic<float> warning_cap{1.0f};
pl::State power_warn, power_fault;  // frame thread
double power_fault_brake_at = -1;   // MGU-K lost: the fault's drive time when the rear brakes overheat, -1 none
bool power_fault_pitted = false;    // the car went through the pits since a misfire began (a power cycle fixes it)
bool power_fault_test = false;      // started by the test run or the debug key (ends with the test run)
std::atomic<float> shown_power_loss{0.0f};   // panel: the steady loss in force (0..1)
std::atomic<int> shown_power_kind{0};        // panel: pl::Kind with the bigger loss
std::atomic<bool> shown_power_cuts{false};   // panel: the engine cuts out
std::atomic<bool> power_speed_wanted{false}; // the battery derate needs the speed from the game thread
// Share of the last minute the player ran at full throttle (game thread): lift and coast lowers it.
std::atomic<float> full_throttle_share{0.65f};
// Read by the throttle stub (game thread): when the car being updated is `car`, its throttle is capped at `cap`.
// `car` is set from updateVehicle on the game thread and only compared, never dereferenced by the stub.
struct alignas(16) throttle_hook_data {
    volatile uintptr_t car;
    volatile float cap;
};
throttle_hook_data throttle_hook{0, 1.0f};
// Vehicle whose engine sound plays as switched off (failed ICE), or -1. Read by the engine audio stub.
alignas(4) volatile int32_t engine_off_vehicle = -1;
alignas(4) volatile int32_t replay_terminal_vehicle = -1;
std::atomic<int> pit_clearance_hold_vehicle{-1}; // keep tyre crew hidden after the completed garage push
std::atomic<bool> engine_off_standing{false}; // broken car pulled over and standing still for 6 s
std::atomic<int> retired_drive_lock_vehicle{-1}; // independent of the optional audio shutdown
std::atomic<int> retired_engine_off_vehicle{-1}; // keep retirement audio off through the DNF camera
// Vehicle whose engine gives no drive torque at all (engine switched off or stalled), or -1. Applied on the frame
// thread as a Swofty car layer engine.power_scale = 0: the game's torque at site_Car_EngineDriveTorque becomes 0, so
// the idle controller and the gear change blip can no longer move the car (they bypass the throttle cap).
std::atomic<int> engine_kill_vehicle{-1};
std::atomic<double> still_for{0}; // frame thread counts it, the game thread resets it (flashback, garage)

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

std::atomic<void *> controller_system{nullptr};
void reset_car_state(bool preserve_retirement_audio = false, bool preserve_request_timeline = false, bool preserve_damage_timeline = false);
std::atomic<uint32_t> player_controller_flags{0};

// One controller switch (ai: 1 hands the car to the AI, reason: the controller flag). Called from the
// setController hook and from the session.player.control event (Swofty 1.7); both see the same switch,
// so everything here is idempotent. Returns true to drop the switch.
void trigger_failure(int forced_part = -1);
// Part the game's own failure planner picked for the player's car (0 ICE .. 5 CE, 6 gearbox), -1 if none yet.
std::atomic<int> game_planned_part{-1};
// Part the addon broke for the game's own failure (after dropping its RETIRED hand-over), -1 none. A flashback to
// before it hands it back to game_planned_part: the game replays the failure and its hand-over, without the planner.
std::atomic<int> game_failure_part{-1};
// After the game's own failure or heavy damage: the pit window, open since this tick (0 = closed). It ends with the
// next lap (the game's lap, else your line crossings), or after 10 minutes of driving (gp_failure_tick).
std::atomic<uint64_t> game_failure_deadline{0};
std::atomic<int> game_failure_lap{-1};          // game lap (0-based) the failure was announced in, -1 unknown
std::atomic<int> game_failure_crossing{-1};     // player_crossings when the window opened, -1 unknown
std::atomic<int> pit_call_reason{0};            // what opened the window: 0 the game's failure, 1 heavy damage
std::atomic<bool> pit_call_ended{false};        // the window ran out (disqualified or retired): no new call
std::atomic<uintptr_t> failure_planner_obj{0};  // the player's failure object, seen by the planner
// Your car's line crossings this session (session.lap), logged at the session end to check the game's lap count.
std::atomic<int> player_crossings{0};
int game_lap();
int game_lap_of(uintptr_t obj);
const char *pit_call_name() { return pit_call_reason.load() ? "heavy damage" : "game failure"; }

// Aero or body damage the game calls terminal (mask bits 5-10 only: tyres?, front wing / nose, sidepods, floor,
// rear wing, diffuser; no power unit, gearbox or corner part): the power unit still drives the car. Until 0.51.125
// it lost all drive with no word why (test, Shanghai race: a side hit gave 0x40 and only the steering was left).
// Race: like a black flag with an orange disc (FIA Appendix H 2.5.4.1 e), pit by the end of the next lap or retire;
// in the pit lane the crew pushes the car into the garage. Practice and qualifying: back to the garage, where crash
// damage keeps the car out for the session (garage_exit_locked). In the pit lane of a race the call waits until
// the car is on track again (heavy_damage_tick).
void heavy_damage_call(uint32_t mask)
{
    bool race = !practice_like();
    bool window = race && player_pit.load() == 1 && !session_ended;
    if (race && !window) {
        swofty::log("[td-probe] heavy damage 0x%x (aero or body only): the car keeps its drive; not on track, the "
                    "pit call comes once it is", mask);
        return;
    }
    if (window) {
        pit_call_reason = 1;
        game_failure_lap = game_lap();
        game_failure_crossing = player_crossings.load();
        game_failure_deadline = GetTickCount64() | 1;
    }
    swofty::log("[td-probe] heavy damage 0x%x (aero or body only, no power unit, gearbox or corner part): the car "
                "keeps its drive; %s", mask, window ? "pit by the end of the next lap or retire" :
                "back to the garage, out for this session");
    if (swofty::hud::available())
        swofty::hud::toast("HEAVY DAMAGE", window ? "Pit by the end of the next lap or retire"
                                                  : "Bring the car back to the garage. No repairs this session.", 6);
    radio_say(radio_damage_strategy);
}

bool note_controller_switch(uint8_t id, uint32_t ai, int32_t reason)
{
    if (id < max_vehicles) {
        if (!ai) {
            human[id] = true;
            player_vehicle = id;
        }
        if (last_ai[id] != int32_t(ai) || last_reason[id] != reason) {
            last_ai[id] = int32_t(ai);
            last_reason[id] = reason;
            swofty::log("[td-probe] setController vehicle=%u known_human=%d -> %s reason=%d %s", id,
                        human[id], ai ? "AI" : "HUMAN", reason, label(reasons, reason));
        }
    }
    // Controller flags are not restart evidence. The actual garage restart
    // callback resets lifecycle state before stock crews can move grid cars.
    bool mine = id < max_vehicles && int(id) == player_vehicle.load();
    // A red flag starts as SAFETY_CAR for every car (Sess_ScRedFlagDeploy_Update): remember where the car was then.
    if (mine && ai && reason == 11 && !(player_controller_flags.load() & (1u << 11))) {
        pit_at_suspension = player_pit.load();
        swofty::log("[td-probe] suspension/safety car: your car was in pit state %d", pit_at_suspension.load());
    }
    if (mine && reason>=0 && reason<32) {
        if (ai) player_controller_flags.fetch_or(1u<<reason);
        else player_controller_flags.fetch_and(~(1u<<reason));
    }
    if (mine && reason == 3 && ai && (player_broken || car_dead || terminal_mask.load())) {
        swofty::log("[td-probe] session end (NOT_IN_SESSION set): clearing gameplay, retaining DNF audio");
        reset_car_state(true);
    }
    // After the race (NOT_IN_SESSION) the game puts your retired car back to SESSION_NOT_STARTED when the replay
    // from the results screen starts (0.51.124 logs 18:47:38 and 19:14:09: every car reset as at a start) or a
    // restart follows. The DNF camera is over by then; the kept silence muted your car in the replay (test report).
    if (mine && ai && reason == 10 && (player_controller_flags.load() & (1u << 3)) &&
        retired_engine_off_vehicle.load() == int(id)) {
        retired_engine_off_vehicle = -1;
        engine_off_vehicle = -1;
        engine_kill_vehicle = -1;
        swofty::log("[td-probe] engine: sound back for vehicle %u (the race is over: replay or restart)", id);
    }
    // The game's own scripted mechanical failure (career, My Team) pulls the car over and hands it to the AI with
    // RETIRED, with no terminal-damage message first. Keep the car yours unless you or the addon retire it.
    // Only while racing on track with a failure the game planned for this car (planner hook): a red flag, safety car or
    // garage hand-over must pass through, or the game's flow can wait for a car that never retires.
    constexpr uint32_t not_racing = (1u << 3) | (1u << 10) | (1u << 11);
    if (mine && reason == 4 && ai && keep_control && keep_game_failure && !session_ended &&
        game_planned_part.load() >= 0 && player_pit.load() == 1 && !(player_controller_flags.load() & not_racing) &&
        GetTickCount64() >= own_retire_until.load() && GetTickCount64() >= status7_at.load() + 3000 && !player_broken && !retire_requested &&
        retire_in.load() == -1.0 && confirmed_retired_vehicle.load() < 0) {
        swofty::log("[td-probe] keep_control: dropped the game's RETIRED hand-over of vehicle %u (not requested by you)", id);
        // The flag was recorded above, but the hand-over never happens; a stale RETIRED bit blocks the standstill retire.
        player_controller_flags.fetch_and(~(1u << 4));
        // The game already broke a part (planner). Run the addon's own broken-car flow for that part: throttle cap,
        // retire after standing still, pause menu / hotkey retire.
        int part = game_planned_part.exchange(-1);
        if (part > 6)
            part = 0;
        game_failure_part = part;
        swofty::log("[td-probe] game failure: breaking part %d for the addon's broken-car handling", part);
        trigger_failure(part);
        // Like a black flag with an orange disc (FIA Appendix H 2.5.4.1 e): shown at the line, the driver must stop at
        // his pit on the next lap. So: pit by the end of the next lap, or retire.
        pit_call_reason = 0;
        game_failure_lap = game_lap();
        game_failure_crossing = player_crossings.load();
        game_failure_deadline = player_pit.load() == 1 ? GetTickCount64() | 1 : 0;
        if (game_failure_deadline.load() && swofty::hud::available())
            swofty::hud::toast("MECHANICAL FAILURE", "Pit by the end of the next lap or retire", 6);
        return true;
    }
    if (reason == reason_terminal_damage && ai && is_player(id)) {
        if (keep_control) {
            swofty::log("[td-probe] keep_control: dropped TERMINAL_DAMAGE hand-over of vehicle %u", id);
            return true;
        }
    }
    return false;
}

uint64_t on_set_controller(void *system, uint64_t vehicle, uint32_t ai, int32_t reason)
{
    controller_system = system;
    uint8_t id = uint8_t(vehicle);
    if (note_controller_switch(id, ai, reason))
        return 0;
    uint64_t result=orig_set_controller(system, vehicle, ai, reason);
    if (id < max_vehicles && int(id) == player_vehicle.load() && reason==4 && ai) confirmed_retired_vehicle=int(id);
    return result;
}

// Swofty 1.7 hook points. They run next to the setController hook above (this build only adds them; the hook
// stays until a test log shows the event fires): the log line says whether it does.
std::atomic<unsigned> control_events{0}, rebuild_events{0};
// The session's laps from the event context (Swofty's telemetry is off on this core): read at the first player
// control change after session.start; 0 outside a race or sprint.
std::atomic<int> session_laps{0};
std::atomic<bool> session_laps_read{false};

void attach_session_events()
{
    namespace sess = swofty::sessions;
    namespace cars = swofty::cars;
    auto control = swofty::events::on<sess::events::PlayerControl>().replace(
        [](sess::events::PlayerControl &c, const swofty::events::Context &ctx) {
            if (!session_laps_read.exchange(true)) {
                if (const auto &v = ctx.session()) {
                    bool race = v->type == swofty::events::SessionType::race ||
                                v->type == swofty::events::SessionType::sprint;
                    session_laps = race && v->laps > 0 && v->laps < 200 ? v->laps : 0;
                    std::string_view track = v->track.id();
                    swofty::log("[td-probe] session: type %d '%s', track '%.*s', %d laps (session context)",
                                int(v->type), v->session.c_str(), int(track.size()), track.data(), v->laps);
                } else {
                    swofty::log("[td-probe] session: no session context");
                }
            }
            if (control_events.fetch_add(1) == 0)
                swofty::log("[td-probe] session.player.control fires (first: vehicle %d %d -> %d reason %d)",
                            int(c.car.index()), int(c.from), int(c.to), int(c.reason));
            return note_controller_switch(uint8_t(c.car.index()), c.to == sess::Control::ai ? 1u : 0u, c.reason);
        });
    auto lap = swofty::events::on<sess::events::Lap>().after(
        [](const sess::events::Lap &l, const swofty::events::Context &) {
            if (int(l.car.index()) == player_vehicle.load())
                player_crossings.fetch_add(1);
        });
    auto rebuilt = swofty::events::on<cars::events::ControllersRebuilt>().after(
        [](const cars::events::ControllersRebuilt &r, const swofty::events::Context &) {
            if (int(r.car.index()) == player_vehicle.load() || rebuild_events.fetch_add(1) == 0)
                swofty::log("[td-probe] car.controllers.rebuilt: vehicle %d (flashback or return to garage)",
                            int(r.car.index()));
        });
    // Red flag camera reports: log when the spectator director's HUD comes and goes while your car is broken, with
    // the controller flags, so a test log shows what switched the camera.
    auto spectator = swofty::events::on<swofty::spectator::events::HudVisible>().after(
        [](const swofty::spectator::events::HudVisible &h, const swofty::events::Context &) {
            if (player_broken || confirmed_retired_vehicle.load() >= 0)
                swofty::log("[td-probe] spectator HUD %s (following vehicle %d; your car %d broken %d retired %d pit "
                            "state %d controller flags 0x%x)",
                            h.visible ? "on" : "off", int(h.following.index()), player_vehicle.load(),
                            int(player_broken.load()), confirmed_retired_vehicle.load() >= 0, player_pit.load(),
                            player_controller_flags.load());
        });
    swofty::log("[td-probe] session events: player.control %s, lap %s, controllers.rebuilt %s, spectator HUD %s",
                control == swofty::events::Result::ok ? "attached" : "unavailable",
                lap == swofty::events::Result::ok ? "attached" : "unavailable",
                rebuilt == swofty::events::Result::ok ? "attached" : "unavailable",
                spectator == swofty::events::Result::ok ? "attached" : "unavailable");
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
    if (guarded_read(base + 0x10, id) && id < max_vehicles &&
        guarded_read(base + 0x18, state) && state < 16)
        observed_pit_state[id] = state + 1;
    if (guarded_read(base + 0x10, id) && guarded_read(base + 0x18, state) && is_player(id)) {
        swofty::log("[td-probe] pit state vehicle=%u -> %u %s", id, state, label(pit_states, state));
        player_pit = int(state);
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
        player_broken && retire_in.load() > -2.0 && restart_cleanup_available) {
        if (!pit_stop_refused.exchange(true)) {
            swofty::log("[td-probe] garage: no tyre stop for your broken car, the pit crew stays in the garage");
            release_retirement_box_entry();
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
bool detour(const char *name, const char *sig, const uint8_t *prologue, size_t len, void *handler, void **orig);
bool detour_at(const char *name, uintptr_t at, const uint8_t *prologue, size_t len, void *handler, void **orig);
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

// Everything about the player's broken car; the player's vehicle id stays known.
void reset_car_state(bool preserve_retirement_audio, bool preserve_request_timeline, bool preserve_damage_timeline)
{
    if (!preserve_request_timeline) flashback_tracker.clear_lock();
    if (!preserve_damage_timeline) damage_flashback_tracker.clear_lock();
    replay_terminal_vehicle = -1;
    if (!preserve_retirement_audio) confirmed_retired_vehicle=-1;
    if (!preserve_retirement_audio) retired_drive_lock_vehicle = -1;
    if (!preserve_retirement_audio) pit_clearance_hold_vehicle = -1;
    int silent_vehicle = preserve_retirement_audio ? retired_engine_off_vehicle.load() : -1;
    retired_engine_off_vehicle = silent_vehicle;
    pending_radio_command = 0;
    throttle_hook.cap = retired_drive_lock_vehicle.load() == player_vehicle.load() && player_vehicle.load()>=0 ? 0.0f : 1.0f;
    engine_off_vehicle = engine_off_sound ? silent_vehicle : -1;
    engine_kill_vehicle = silent_vehicle;
    engine_off_standing = false;
    pending_failure = 0;
    garage_return_wanted = false;
    test_cap = -1.0f;
    car_dead = false;
    garage_withdrawn = false;
    garage_part_failed = false;
    garage_failed_types = 0;
    part_lock = false;
    exit_check_at = 0;
    terminal_mask = 0;
    pit_call_ended = false;
    limp_state = -1;
    limp_corner = -1;
    race_stop_left = 0;
    crash_corners = 0;
    garage_repair = 0;
    garage_service_corners = 0;
    garage_service_seconds = 0;
    garage_service_crack = false;
    repair_label(false);
    player_broken = false;
    retire_requested = false;
    player_pit = 1;
    still_for = 0;
    retire_in = -1.0;
    cancel_crew_work();
}

// The race's failures and faults: the plans, warning signs, power and brake faults, the gearbox decline, the pit
// window after the game's failure, and what the panel shows of them. A new session and a race restart (frame thread).
// Values are cleared, not ended: ending a fault plays its "fixed" radio line. The test run has its own restart path.
void reset_race_faults()
{
    game_failure_part = -1;
    game_failure_deadline = 0;
    game_failure_lap = -1;
    game_failure_crossing = -1;
    pit_call_reason = 0;
    pit_call_ended = false;
    pit_at_suspension = -1;
    red_flag_repair_wait = false;
    live_terminal_state = -1;
    live_terminal_mask = 0;
    gp_clock = 0; gp_at = -1;
    gp_point = {};
    gp_lap_seen = -1;
    gp_lap_started = gp_lap_time = 0;
    gp_warn = {};
    warning_cap = 1.0f;
    power_warn = {};
    power_fault = {};
    power_fault_test = false;
    power_fault_brake_at = -1;
    power_fault_pitted = false;
    shown_power_loss = 0.0f;
    shown_power_kind = 0;
    shown_power_cuts = false;
    power_speed_wanted = false;
    gearbox_decline = {};
    decline_shift_scale = 0.0f;
    rear_brake_loss_target = 0.0f;
    rear_brake_until = 0;
    brake_fade = 0.0f;
    rear_brake_factor = 1.0f;
    radio_pending = 0;
    shown_shift_scale = 0.0f;
    shown_missing_gear = 11;
    shown_warning = -1;
    shown_decline_stage = -1;
    gp_fault_at = -1;
    gp_fault_type_rolled = -1;
    fault_request = 0;
    breakdown_request = 0;
    breakdown_watch = {};
    ai_failures_rolled = false;
}

void reset_session(bool preserve_retirement_audio = false)
{
    susp::request_reset("new session");
    ai_watch_mask = 0;
    reset_car_state(preserve_retirement_audio);
    reset_race_faults();
    test_run_reset();
    flashback_tracker.invalidate_session();damage_flashback_tracker.invalidate_session();
    player_controller_flags=0;
    owned_crew_instance=owned_crew_module=0;owned_crew_vehicle=-1;
    throttle_hook.car = 0;
    telemetry_not_before = GetTickCount64();
    swofty::mem::invalidate();
    for (int i = 0; i < max_vehicles; i++) {
        last_ai[i] = -1;
        last_reason[i] = -1;
        human[i] = false;
        observed_pit_state[i] = 0;
        pit_slot_of[i] = -1;
    }
    player_vehicle = -1;
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
    if (vehicle < 0 || !sys || !guarded_read(sys + 0xc20, count) || uint64_t(vehicle) >= count ||
        !guarded_read(sys + 0x50 + uint64_t(vehicle) * 0x70 + 8, rec) || !rec ||
        !guarded_read(rec + 0x58, car))
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

void return_to_garage();
void game_failure_dsq_now();

// Throttle cap for the player's broken car (game thread, inside updateVehicle): a failed ICE or gearbox gives no
// drive (cap 0, the car rolls out), a failed MGU-K, MGU-H, ES, turbo or CE leaves `damaged_power` of the throttle.
// Ctrl+Shift+F9 (test) overrides it.

// The corners a crash broke, bit i = corner i (RL RR FL FR): the game's state 3-4 (wheel coming off) and 5 (wheel
// lost), and state 2 where the addon's model collapsed the corner (`collapsed`, bit i). State 2 is wheel+0x20 bit 0
// (getter 0x1446568e0; bit 1 alone is state 1, a puncture), and the setter 0x144656a80 adds a puncture for it
// (Car_Puncture_Add): the corner shows a flat tyre. The game sets it itself too, without suspension damage, and
// drives on with it: it sends no terminal message for a state 2 corner (0.51.89: not for the addon's collapse until
// more broke; 0.51.127 test 20:27:46: FL state 2 at 0.00 suspension damage, the car drove on). 0.51.128 test
// 20:41:10: the game broke the FR (state 4) and set the RL to state 2 (0.02 damage; in the test 'a puncture at least');
// the RL counted as a broken rear corner, so the car could not limp and the panel showed FR RL. A wheel the game
// only damaged counts only when no other corner broke.
uint32_t broken_corners(uint32_t packed, uint32_t collapsed)
{
    uint32_t mask = 0, damaged = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned s = (packed >> (3 * i)) & 7;
        if (s >= 3 || (s == 2 && ((collapsed >> i) & 1)))
            mask |= 1u << i;
        else if (s == 2)
            damaged |= 1u << i;
    }
    return mask ? mask : damaged;
}

// What broke at corner i, by the game's corner state (state 2: the addon's collapse, or a wheel the game damaged).
const char *corner_damage_name(uint32_t packed, uint32_t collapsed, int i)
{
    unsigned s = (packed >> (3 * i)) & 7;
    if (s >= 5)
        return "wheel lost";
    if (s >= 3)
        return "wheel coming off";
    return (collapsed >> i) & 1 ? "suspension collapsed" : "wheel damaged";
}

// The message for a car a crash left undrivable: the broken corners (broken_corners) by the game's own state,
// whoever broke them: the addon's suspension model or the game's own damage. 0.51.127 test (20:24:36-39): the
// addon's model bent the RR, then the game broke the FR itself (contacts at 10 m/s that were too light for the
// model); no message named the FR and the panel said RR. Two corners are both named, the worst first. nullptr: no
// corner broken.
const char *undrivable_message(uint32_t packed, uint32_t collapsed, char *text, size_t n)
{
    auto what = [packed, collapsed](int i) { return corner_damage_name(packed, collapsed, i); };
    uint32_t mask = broken_corners(packed, collapsed);
    int broken = 0, at = -1, other = -1;
    unsigned worst = 0;
    bool wheels_only = true;  // only wheels the game damaged: no other corner broke
    for (int i = 0; i < 4; ++i) {
        if (!((mask >> i) & 1))
            continue;
        unsigned s = (packed >> (3 * i)) & 7;
        ++broken;
        wheels_only = wheels_only && s == 2 && !((collapsed >> i) & 1);
        if (s >= worst) {
            other = at;
            worst = s;
            at = i;
        } else {
            other = i;
        }
    }
    if (!broken)
        return nullptr;
    if (broken > 2) {
        snprintf(text, n, "%d corners broken. The car cannot be driven.", broken);
    } else if (broken == 2) {
        char second[16];
        snprintf(second, sizeof second, "%s", susp::corner_name(other));
        second[0] = char(tolower(uint8_t(second[0])));
        snprintf(text, n, "%s %s, %s %s. The car cannot be driven.", susp::corner_name(at), what(at), second,
                 what(other));
    } else {
        snprintf(text, n, "%s %s. The car cannot be driven.", susp::corner_name(at), what(at));
    }
    return worst >= 5 ? "WHEEL LOST" : wheels_only ? "WHEEL DAMAGE" : "SUSPENSION FAILURE";
}

// The chance that a car with one broken corner can still crawl on, by the game's corner state. A collapsed
// suspension (state 2: the wheel still on, it drives like a flat tyre; 0.51.125 race 17:18:46 drove 20 s on a
// collapsed FL) 85 % at the front and, since 0.51.132, 60 % at the rear (test report: 'the car is always dead'; a rear
// corner never limped before, 6 of the 12 crashes the model decided in 0.51.127-0.51.130). A front wheel coming
// loose (3: D past the game's wheel detach limit W, no terminal message yet) 35 %. Never once the game tears the
// wheel off itself (4: D past its parts detach limit P, the game's terminal crash damage, which switches the engine
// off: 0.51.132 test 23:05:15, 5918 rpm, 611 at the impact, then 0 while the car rolled 75 -> 0 km/h in 5th gear;
// 0 of 7 such cars drove in the logs), once the wheel is gone (5), nor with a rear wheel coming loose.
unsigned limp_chance(unsigned state, int corner)
{
    bool front = corner >= 2;
    return state >= 4 ? 0 : state == 3 ? (front ? 35 : 0) : front ? 85 : 60;
}

// Game thread. A crash broke a corner: decide once whether the car can still be driven. Only a single broken corner
// can limp (limp_chance). While it limps, a wheel that comes off, another broken corner or the limping rear wheel
// coming off stops it. A wheel the game only damaged (state 2, a flat tyre) is no broken corner (broken_corners)
// unless no other corner broke. Corner order is RL, RR, FL, FR like the UDP format (visual_wheel_fl reads
// owner+0x138 = corner 2), so the fronts are 2 and 3.
void decide_limp(uintptr_t car)
{
    if (limp_state.load() == 1 && car) {
        static uint64_t next_check = 0;
        uint64_t now = GetTickCount64();
        if (now < next_check)
            return;
        next_check = now + 500;
        bool ok = false;
        uint32_t packed = corner_states(car, ok), collapsed = susp::collapsed_corners();
        uint32_t mask = broken_corners(packed, collapsed);
        int at = limp_corner.load(), broken = 0, gone = 0;
        for (int i = 0; ok && i < 4; ++i) {
            broken += (mask >> i) & 1;
            gone += ((packed >> (3 * i)) & 7) >= 5;
        }
        // Another corner broke: one that is not the limping one (a wheel the game only damaged does not count
        // next to it, broken_corners); a limping rear corner whose wheel comes off stops the car too.
        bool more = at >= 0 && at < 4 ? (mask & ~(1u << at)) != 0 : broken > 1;
        bool loose_rear = at >= 0 && at < 2 && ((packed >> (3 * at)) & 7) >= 3;
        if (ok && (more || gone || loose_rear)) {
            crash_corners = packed;
            limp_state = 0;
            limp_corner = -1;
            swofty::log("[td-probe] crash: corners 0x%03x, %s, the car cannot be driven any more", packed,
                        more ? "another corner broke" : "the wheel came off");
            char text[120];
            const char *title = undrivable_message(packed, collapsed, text, sizeof text);
            if (title && swofty::hud::available())
                swofty::hud::toast(title, text, 6);
            susp::hold_minor_toasts(6000);
        }
        return;
    }
    if (limp_state.load() != -1 || !car)
        return;
    bool ok = false;
    uint32_t packed = corner_states(car, ok);
    if (!ok)
        return;  // retried next update; the component may not be readable this frame
    uint32_t collapsed = susp::collapsed_corners(), mask = broken_corners(packed, collapsed), wheels = 0;
    int broken = 0, corner = -1;
    for (int i = 0; i < 4; ++i) {
        if ((mask >> i) & 1) {
            ++broken;
            corner = i;
        } else if (((packed >> (3 * i)) & 7) == 2) {
            wheels |= 1u << i;
        }
    }
    unsigned chance = 0, roll = roll_percent();
    if (broken == 1)
        chance = limp_chance((packed >> (3 * corner)) & 7, corner);
    roll = forced_roll(debug_limp_roll, chance, roll);
    crash_corners = packed;
    limp_state = roll < chance ? 1 : 0;
    limp_corner = limp_state.load() == 1 ? corner : -1;
    float gd[4];
    susp::game_damage(gd);
    swofty::log("[td-probe] crash: corners 0x%03x, %d broken (last %d), limp chance %u%%, roll %u -> %s; the game's "
                "suspension damage RL %.2f RR %.2f FL %.2f FR %.2f", packed, broken, corner, chance, roll,
                limp_state.load() ? "the car can limp on" : "the car cannot be driven", double(gd[0]), double(gd[1]),
                double(gd[2]), double(gd[3]));
    if (wheels)
        swofty::log("[td-probe] crash: corners 0x%x only have a wheel the game damaged (state 2, a flat tyre): not "
                    "counted as broken", wheels);
    limp_watch_left = 30.0;
    // The limping corner and what broke there, e.g. "Rear-left suspension collapsed".
    char what[64] = "";
    if (limp_state.load() == 1)
        snprintf(what, sizeof what, "%s %s", susp::corner_name(corner), corner_damage_name(packed, collapsed, corner));
    if (limp_state.load() == 1 && !practice_like()) {
        race_stop_left = race_stop_grace;
        swofty::log("[td-probe] crash: race, the car must stop as soon as it is safe (%.0f s of driving)",
                    race_stop_grace);
        char text[120];
        snprintf(text, sizeof text, "%s: pull over as soon as it is safe.", what);
        if (swofty::hud::available())
            swofty::hud::toast("STOP THE CAR", text, 6);
        radio_say(radio_mech_dnf);
    } else if (limp_state.load() == 1) {
        char text[120];
        snprintf(text, sizeof text, "%s. The car can still crawl: bring it back to the garage.", what);
        if (swofty::hud::available())
            swofty::hud::toast(corner >= 2 ? "BROKEN FRONT CORNER" : "BROKEN REAR CORNER", text, 6);
        radio_say(radio_box);
    } else {
        char text[120];
        const char *title = undrivable_message(packed, collapsed, text, sizeof text);
        if (title && swofty::hud::available())
            swofty::hud::toast(title, text, 6);
    }
    // The central notification shows one message: a corner bent a moment later must not replace this one.
    susp::hold_minor_toasts(6000);
}

void power_loss(uintptr_t car)
{
    static int logged = -1;
    static uintptr_t last_car;
    if (log_telemetry && car != last_car) {
        // 0.25.2 crash log: after the crash car +0x170.. read all zero while UDP throttle was 1, so the car object
        // may change on terminal crash damage. Log every change to see it.
        float thr = -1;
        if (car)
            guarded_read(car + 0x188, thr);
        swofty::log("[td-probe] power: player car object %p -> %p (throttle %.2f)", reinterpret_cast<void *>(last_car),
                    reinterpret_cast<void *>(car), double(thr));
        last_car = car;
    }
    FlashbackTracker::Rewind damage_rewind;
    if (car && damage_flashback_tracker.damage_rewind(GetTickCount64(), damage_rewind) &&
        damage_rewind.mask != terminal_mask.load()) {
        uintptr_t engine=0, gearbox=0;
        bool restored=guarded_read(car+0x530,engine) && engine && guarded_read(car+0x538,gearbox) && gearbox;
        for (int i=0; restored && i<7; ++i) {
            float wear=1;
            uint32_t retained_bit = i==0 ? 1u : i==6 ? 4u : 2u;
            restored=guarded_read(i<6 ? engine+part_offsets[i] : gearbox+0x674,wear) &&
                     std::isfinite(wear) && wear>=0 && (wear<0.9f || (damage_rewind.mask & retained_bit));
        }
        if (restored && damage_flashback_tracker.commit_damage(damage_rewind)) {
            bool retain_request=retire_requested && !flashback_tracker.ready(GetTickCount64());
            int pit=player_pit.load();
            bool keep_timeline=retain_request || damage_rewind.mask;
            reset_car_state(false,keep_timeline,damage_rewind.mask!=0);
            terminal_mask=damage_rewind.mask;
            retire_requested=retain_request;
            player_broken=retain_request || damage_rewind.mask;
            if (player_broken) player_pit=pit;
            swofty::log("[td-probe] flashback: restored damage mask=0x%x; earlier retirement=%d",damage_rewind.mask,int(retain_request));
        }
    }
    if (car && player_broken && player_pit.load() >= 1 && player_pit.load() <= 3 &&
        flashback_tracker.consume_retained_resume(GetTickCount64())) {
        int pit = player_pit.load();
        // Resuming after the lock preserves the failure/request. Re-arm only
        // the approach stage; old future garage reports must not fire again.
        if (pit >= 1 && pit <= 3) {
            cancel_crew_work();
            retire_in = -1.0;
            still_for = 0;
            engine_off_standing = false;
            retired_engine_off_vehicle = -1;
            engine_off_vehicle = -1;
            engine_kill_vehicle = -1;
            pushing = pit >= 2; prepare_at_box = pit >= 2;
            pit_clearance_hold_vehicle = -1;
            garage_return_wanted = false; exit_check_at = 0;
            swofty::log("[td-probe] flashback: resumed after damage/request in pit state %d; keeping retirement and rearming approach", pit);
        }
    }
    if (car && flashback_tracker.ready(GetTickCount64())) {
        uintptr_t engine = 0, gearbox = 0;
        bool restored = guarded_read(car + 0x530, engine) && engine &&
                        guarded_read(car + 0x538, gearbox) && gearbox;
        for (int i = 0; restored && i < 7; ++i) {
            float wear = 1;
            restored = guarded_read(i < 6 ? engine + part_offsets[i] : gearbox + 0x674, wear) &&
                       std::isfinite(wear) && wear >= 0 && wear < 0.9f;
        }
        if (restored) {
            swofty::log("[td-probe] flashback: resumed before damage/request with restored parts; clearing the lock");
            reset_car_state();
        }
    }
    // Repair research: while the dead car sits in its practice garage, log its part wear (ICE, MGU-K, MGU-H, ES,
    // TC, CE, gearbox) whenever it changes, to see what a part swap or the game's repair does to it.
    if (car_dead && car) {
        static int last_parts[7] = {-1, -1, -1, -1, -1, -1, -1};
        int parts[7] = {-1, -1, -1, -1, -1, -1, -1};
        uintptr_t eng = 0, gb = 0;
        float v = 0;
        if (guarded_read(car + 0x530, eng) && eng)
            for (int i = 0; i < 6; i++)
                if (guarded_read(eng + part_offsets[i], v))
                    parts[i] = int(v * 1000.0f);
        if (guarded_read(car + 0x538, gb) && gb && guarded_read(gb + 0x674, v))
            parts[6] = int(v * 1000.0f);
        if (log_telemetry && memcmp(parts, last_parts, sizeof parts)) {
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
        if (failed) {
            garage_part_failed = true;
            for (int i = 0; i < 7; ++i)
                if (parts[i] >= 900)
                    garage_failed_types.fetch_or(1u << i);
        }
        if (readable && !failed && garage_part_failed && !(terminal_mask.load() & 0x18) && !garage_withdrawn.load()) {
            swofty::log("[td-probe] repair: the failed part was replaced, your car may leave the garage again");
            int pit=player_pit.load();
            reset_car_state();
            player_pit=pit;
            // Engine state flags at engine+0x2530 (UDP snapshot writer 0x142d00a63): bit 0 blown, bit 1 seized. The
            // swap resets the wear but keeps them, and a seized engine made the out-lap AI stop (0.32.1 test).
            uint8_t flags = 0;
            if (eng && guarded_read(eng + 0x2530, flags) && (flags & 3)) {
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
        if (terminal_mask.load() & 0x18) {
            why = 8;
            decide_limp(car);
            if (limp_state.load() == 1)
                why = 11;
        }
        if (!why && guarded_read(car + 0x530, engine) && engine) {
            if (guarded_read(engine + part_offsets[0], v) && v >= 1.0f)
                why = 1;
            for (int i = 1; i < 6 && !why; i++)
                if (guarded_read(engine + part_offsets[i], v) && v >= 1.0f)
                    why = 2 + i;
        }
        if (!why && guarded_read(car + 0x538, gearbox) && gearbox && guarded_read(gearbox + 0x674, v) &&
            v >= 1.0f)
            why = 2;
        // The game's own (random) failures end in the same terminal message. If no part reads as failed, its mask
        // decides (bit 0 engine, 1 power unit, 2 gearbox; the corners 3/4 above). Aero or body damage only (10)
        // keeps full drive since 0.51.126 and the car is called in (heavy_damage_call).
        uint32_t mask = terminal_mask.load();
        if (!why && mask)
            why = mask & 1 ? 1 : mask & 4 ? 2 : mask & 2 ? 9 : 10;
        cap = why == 1 || why == 2 || why == 8 ? 0.0f : why == 11 ? limp_power : why && why != 10 ? pu : 1.0f;
    }
    // Terminal crash damage (suspension): the engine physics keeps idling, so the engine sound stops after a random
    // 3-6 s, as a stalled engine would.
    static uint64_t crash_off_at;
    uint64_t now = GetTickCount64();
    if (why != 8)
        crash_off_at = 0;
    else if (!crash_off_at)
        crash_off_at = now + 3000 + now % 3001;
    bool off = why == 1 || (why == 8 && now >= crash_off_at) || (player_broken && engine_off_standing) ||
               (retired_engine_off_vehicle.load() >= 0 && retired_engine_off_vehicle.load() == player_vehicle.load());
    engine_off_vehicle = off && engine_off_sound ? player_vehicle.load() : -1;
    engine_kill_vehicle = off ? player_vehicle.load() : -1;
    replay_terminal_vehicle = keep_control && player_broken && player_pit.load() == 1 && !session_ended ?
                                  player_vehicle.load() : -1;
    float t = test_cap.load();
    bool parked_retirement = retired_drive_lock_vehicle.load() >= 0 && retired_drive_lock_vehicle.load() == player_vehicle.load();
    throttle_hook.cap = parked_retirement ? 0.0f : t >= 0 ? t : std::min(cap, warning_cap.load());
    if (why != logged) {
        static const char *const names[] = {"none", "ICE", "gearbox", "MGU-K", "MGU-H", "ES", "turbo", "CE", "crash damage",
                                             "power unit (mask)", "heavy aero or body damage (mask)",
                                             "limping on a broken corner"};
        swofty::log("[td-probe] power: failed part %s, throttle cap %.2f", names[why], double(cap));
        logged = why;
    }
}

// Crash damage repair, the way the game's multiplayer damage sync (network::VehicleMechanicalManager::update,
// 0x1432495e0) mirrors a remote car: the car's F1SuspensionDamage component (car+0x5d0, owner+8 = car, vtable
// 0x145bf8bd0) packs 3 bits per corner (0x1446568e0: 0 intact, 1 puncture, 2 suspension broken, 3-5 detach stages; corners RL, RR, FL, FR), and
// 0x144656a80(component, packed) moves every corner to the given state. State 0 reattaches the wheel (part+0x38 = 0,
// slot 0xa8 removes the loose body), clears the broken suspension flags (wheel+0x20), the suspension damage
// (wheel+0x98) and a puncture. The front wing (car+0x5c8) takes Car_Damage_SetComponentDamage 0x144660cd0(wing, 0).
using wheel_state_get_fn = uint32_t (*)(void *);
using wheel_state_apply_fn = void (*)(void *, uint32_t);
using component_damage_fn = void (*)(void *, uint32_t);
wheel_state_get_fn wheel_state_get = nullptr;
wheel_state_apply_fn wheel_state_apply = nullptr;
component_damage_fn front_wing_damage_set = nullptr;
std::atomic<bool> red_flag_fix_now{false};  // set by the red flag tick, done on the game thread
bool crash_repair_available() { return wheel_state_get && wheel_state_apply; }
void find_crash_repair()
{
    uintptr_t get = 0, apply = 0, wing = 0;
    int a = swofty::api->scan(".xtls", "56 57 48 83 EC 28 48 89 CE 48 8B 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? B8 01 00 00 00 87 05 "
                              "?? ?? ?? ?? 48 8B 46 08 8B 4E 68 33 4E 70", &get);
    int b = swofty::api->scan(".xtls", "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 98 00 00 00 C5 78 29 84 24 80 00 00 00 "
                              "C5 F8 29 7C 24 70 C5 F8 29 74 24 60 89 D6 48 89 CF 48 8B 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? "
                              "B8 01 00 00 00 87 05 ?? ?? ?? ?? 4C 8B 6F 08 48 89 F9 E8", &apply);
    int c = swofty::api->scan(".xtls", "56 57 48 83 EC 58 C5 F8 29 7C 24 40 C5 F8 29 74 24 30 89 D7 48 89 CE C5 FA 10 41 38 "
                              "C5 FA 10 49 48 C5 F2 58 71 4C", &wing);
    if (a == 1 && b == 1) {
        wheel_state_get = reinterpret_cast<wheel_state_get_fn>(get);
        wheel_state_apply = reinterpret_cast<wheel_state_apply_fn>(apply);
    }
    if (c == 1)
        front_wing_damage_set = reinterpret_cast<component_damage_fn>(wing);
    swofty::log("[td-probe] crash repair: corner state %d/%d hits, front wing %d hits", a, b, c);
}
// The car's suspension damage component (checked: its owner pointer +8 is the car), or 0.
uintptr_t suspension_damage_of(uintptr_t car)
{
    uintptr_t owner = 0, back = 0;
    if (!car || !guarded_read(car + 0x5d0, owner) || !owner || !guarded_read(owner + 8, back) || back != car)
        return 0;
    return owner;
}
// Game thread. Packed corner states of the car (3 bits per corner, 0 intact).
uint32_t corner_states(uintptr_t car, bool &ok)
{
    uintptr_t owner = suspension_damage_of(car);
    ok = owner && wheel_state_get;
    return ok ? wheel_state_get(reinterpret_cast<void *>(owner)) : 0;
}
// Debug override of a dice roll: "success" passes whenever there is a chance at all, "fail" never passes.
unsigned forced_roll(std::string_view mode, unsigned chance, unsigned roll)
{
    if (mode == "success" && chance)
        return 0;
    if (mode == "fail")
        return 100;
    return roll;
}
unsigned roll_percent()
{
    static std::mt19937 rng{static_cast<uint32_t>(GetTickCount64() ^ GetCurrentProcessId())};
    return unsigned(rng() % 100);
}
bool has_detached_corner(uint32_t packed)
{
    for (int i = 0; i < 4; ++i)
        if (((packed >> (3 * i)) & 7) >= 3)
            return true;
    return false;
}
// Game thread. Number of corners not intact, -1 unknown.
int damaged_corners(uintptr_t car)
{
    uintptr_t owner = suspension_damage_of(car);
    if (!owner || !wheel_state_get)
        return -1;
    uint32_t packed = wheel_state_get(reinterpret_cast<void *>(owner)), n = 0;
    for (int i = 0; i < 4; ++i)
        n += ((packed >> (3 * i)) & 7) >= 2;  // 1 is only a puncture
    return int(n);
}
// Game thread: the team replaces the broken corner and the front wing.
void repair_crash_damage(uintptr_t car)
{
    uintptr_t owner = suspension_damage_of(car), wing = 0;
    if (!owner || !crash_repair_available()) {
        swofty::log("[td-probe] crash repair: not possible (car %p, component %p)", reinterpret_cast<void *>(car),
                    reinterpret_cast<void *>(owner));
        return;
    }
    uint32_t before = wheel_state_get(reinterpret_cast<void *>(owner));
    wheel_state_apply(reinterpret_cast<void *>(owner), 0);
    susp::request_reset("repair");
    uint32_t after = wheel_state_get(reinterpret_cast<void *>(owner));
    bool wing_done = false;
    if ((terminal_mask.load() & 0x40) && front_wing_damage_set && guarded_read(car + 0x5c8, wing) && wing) {
        front_wing_damage_set(reinterpret_cast<void *>(wing), 0);
        wing_done = true;
    }
    swofty::log("[td-probe] crash repair: corners 0x%03x -> 0x%03x, front wing %s", before, after,
                wing_done ? "repaired" : "untouched");
}

// Game thread, right after the game updated the record. During a red flag repair wait, a record that is no longer
// terminal means the game's repairs fixed the car: clear the addon's broken state so it takes part in the restart.
void read_terminal_record(uintptr_t sys, uint8_t vehicle)
{
    uint64_t count = 0;
    uint32_t overall = 0, live = 0, states[23] = {};  // record +0x10 .. +0x6c: parts at +0x10, overall at +0x68
    uintptr_t rec = sys + 0x50 + uint64_t(vehicle) * 0x70;
    if (!sys || !guarded_read(sys + 0xc20, count) || vehicle >= count || !guarded_copy(rec + 0x10, states, sizeof states))
        return;
    overall = states[(0x68 - 0x10) / 4];
    for (int i = 0; i < 11; ++i)
        if (states[i] == 5)
            live |= 1u << i;
    if (player_broken && (int(overall) != live_terminal_state.load() || live != live_terminal_mask.load()))
        swofty::log("[td-probe] terminal record: state %u, parts at terminal 0x%x", overall, live);
    // Without the flashback hook: the record of a broken car on track turning normal again is a flashback to before
    // the damage (the red flag repair and the garage have their own paths).
    if (!flashback_hooked && player_broken && terminal_mask.load() && live_terminal_state.load() == 5 && overall != 5 &&
        !live && player_pit.load() == 1 && !red_flag_repair_wait.load())
    {
        flashback_used_at = GetTickCount64();
        flashback_count.fetch_add(1);
    }
    live_terminal_state = int(overall);
    live_terminal_mask = live;
    if (red_flag_repair_wait.load() && overall != 5 && !live && red_flag_repair_wait.exchange(false)) {
        swofty::log("[td-probe] red flag: the game repaired your car (mask was 0x%x), it takes part in the restart",
                    terminal_mask.load());
        reset_car_state(false);
        red_flag_repaired_toast = true;
    }
}

// The player's speed straight from the game, so the addon does not need the game's UDP telemetry. The game's own
// car telemetry writer (0x142cf9820) reads it like this: body = [[car+0x558]+0x340]+0x18, velocity (m/s) at body+0x220,
// speed in km/h = |velocity| * 3600 * 0.001.
std::atomic<int> game_speed{-1};
std::atomic<uint64_t> game_speed_ms{0};
void read_game_speed(uintptr_t car)
{
    uintptr_t physics = 0, rigid = 0, body = 0;
    float v[4] = {};
    if (!car || !guarded_read(car + 0x558, physics) || !physics || !guarded_read(physics + 0x340, rigid) || !rigid ||
        !guarded_read(rigid + 0x18, body) || !body || !guarded_copy(body + 0x220, v, sizeof v))
        return;
    float kmh = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]) * 3.6f;
    if (!std::isfinite(kmh) || kmh > 1000.0f)
        return;
    game_speed = int(kmh);
    game_speed_ms = GetTickCount64();
}

// Issue #10: an AI car with terminal damage normally stands for about 14 s, then the game's virtual session puts it
// in its garage (setVehicleStatus(v, 9) from 0x142f73486). Log once per second what a watched car does, so a car the
// game never removes (suspected after a flashback inside those 14 s) shows up as STUCK. Reads only.
void ai_watch_tick(uintptr_t sys, uint8_t v)
{
    static uint64_t next[32];
    uint64_t now = GetTickCount64();
    if (v >= 32 || now < next[v])
        return;
    next[v] = now + 1000;
    uintptr_t car = car_of(sys, v), physics = 0, rigid = 0, body = 0;
    int32_t record = -1;
    float vel[4] = {}, throttle = -1.0f, kmh = -1.0f;
    guarded_read(sys + 0x50 + uint64_t(v) * 0x70 + 0x68, record);  // 5 = terminal
    if (car)
        guarded_read(car + 0x188, throttle);
    if (car && guarded_read(car + 0x558, physics) && physics && guarded_read(physics + 0x340, rigid) && rigid &&
        guarded_read(rigid + 0x18, body) && body && guarded_copy(body + 0x220, vel, sizeof vel))
        kmh = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2] + vel[3] * vel[3]) * 3.6f;
    int pit = int(observed_pit_state[v].load()) - 1;
    double t = double(now - ai_watch_since[v].load()) / 1000.0;
    bool done = pit == 9 || t > 120;
    bool stuck = !done && t > 25 && kmh >= 0 && kmh < 2;
    // Every second for the first 20 s, then only changes worth seeing.
    if (t <= 20 || done || stuck)
        swofty::log("[td-probe] AI watch: vehicle=%u +%.0fs pit=%d record=%d speed=%.0f throttle=%.2f flashbacks=%u%s", v,
                    t, pit, record, double(kmh), double(throttle), flashback_count.load(),
                    done ? " (done)" : stuck ? " STUCK" : "");
    if (done || stuck)
        ai_watch_mask.fetch_and(~(1u << v));
}

double uniform01();
void *(*orig_failure_register)(void *, const void *) = nullptr;
// The player's per-car failure object (manager entries mgr+0x198..+0x1a0, 0x10 bytes, object at +8; obj+0x70 car,
// obj+0x78 vehicle), only when it belongs to the car being driven now.
// Calls f(vehicle, obj) for every car's failure object of the current session's manager.
template <class F> void for_each_failure_obj(F f)
{
    uint64_t mgr = failure_mgr.load(), begin = 0, end = 0;
    if (!mgr || !guarded_read(mgr + 0x198, begin) || !guarded_read(mgr + 0x1a0, end) || !begin || end < begin ||
        end - begin > 0x10 * 64)
        return;
    for (uint64_t e = begin; e < end; e += 0x10) {
        uint64_t obj = 0;
        uint8_t idx = 0xff;
        if (guarded_read(e + 8, obj) && obj && guarded_read(obj + 0x78, idx) && idx < 32)
            f(int(idx), uintptr_t(obj));
    }
}
uintptr_t failure_obj_of(int vehicle)
{
    uintptr_t found = 0;
    for_each_failure_obj([&](int v, uintptr_t obj) {
        if (v == vehicle && !found)
            found = obj;
    });
    return found;
}
// Game thread. Cached: the manager walk costs about 47 reads and ran twice on every player update (radio, slow
// shifts; 0.51.117 review: 3.8 ms/s). The cached object is checked with two reads (its car and vehicle) and looked up
// again when the manager, the vehicle or the car changes.
uintptr_t player_failure_obj(uintptr_t car)
{
    static uint64_t cached_mgr = 0;
    static int cached_vehicle = -1;
    static uintptr_t cached_car = 0, cached_obj = 0;
    if (!car)
        return 0;
    uint64_t mgr = failure_mgr.load(), obj_car = 0;
    int vehicle = player_vehicle.load();
    uint8_t idx = 0xff;
    if (cached_obj && mgr == cached_mgr && vehicle == cached_vehicle && car == cached_car &&
        guarded_read(cached_obj + 0x70, obj_car) && obj_car == car && guarded_read(cached_obj + 0x78, idx) &&
        int(idx) == vehicle)
        return cached_obj;
    uintptr_t obj = failure_obj_of(vehicle);
    cached_mgr = mgr;
    cached_vehicle = vehicle;
    cached_car = car;
    cached_obj = obj && guarded_read(obj + 0x70, obj_car) && obj_car == car ? obj : 0;
    return cached_obj;
}

// Game thread: play the next pending engineer line. Once per session it logs the planner's own radio names (cfg =
// [[obj+0x98]+0xb8]: +0x1c8.. fault imminent/occurred/fixed, +0x2a0.. failure suspected .. retirement destination),
// which shows the name format the speech interface takes.
void radio_tick(uintptr_t car)
{
    static uint64_t last_play = 0;
    static uintptr_t logged_for = 0;
    // Nothing to say, or the last line less than 6 s ago: no reads (this runs on every player update).
    uint32_t pending = radio_pending.load();
    if (!pending || GetTickCount64() < last_play + 6000)
        return;
    uintptr_t obj = player_failure_obj(car), ctx = 0, speech = 0, vtable = 0, fn = 0;
    if (!obj || !guarded_read(obj + 0x98, ctx) || !ctx || !guarded_read(ctx + 8, speech) || !speech ||
        !guarded_read(speech, vtable) || !vtable || !guarded_read(vtable + 0x98, fn) || !fn)
        return;
    if (logged_for != obj) {
        logged_for = obj;
        uintptr_t cfg = 0;
        if (guarded_read(ctx + 0xb8, cfg) && cfg) {
            static const uint32_t offsets[] = {0x1c8, 0x1d0, 0x1e8, 0x1f0, 0x1f8, 0x200, 0x208, 0x2a0, 0x2a8, 0x2b0,
                                               0x2b8, 0x2d0, 0x2e0};
            for (uint32_t off : offsets) {
                uintptr_t str = 0;
                char text[64] = {};
                if (guarded_read(cfg + off, str) && str && guarded_copy(str, text, sizeof text - 1))
                    swofty::log("[td-probe] radio: the game's line at cfg+0x%x is \"%.*s\"", off,
                                int(strnlen(text, sizeof text - 1)), text);
            }
        }
    }
    int line = 0;
    while (!(pending & (1u << line)))
        ++line;
    radio_pending.fetch_and(~(1u << line));
    last_play = GetTickCount64();
    using play_fn = uint64_t (*)(void *, const char *, uint64_t, uint64_t);
    uint64_t r = reinterpret_cast<play_fn>(fn)(reinterpret_cast<void *>(speech), radio_names[line], 0, 0);
    swofty::log("[td-probe] radio: played %s (result 0x%llx)", radio_names[line], (unsigned long long)r);
}

// Game thread: arm the game's own breakdown sequence (re/game-failure-scheduler.md section 4) for the requested cars:
// part +0xa0, state +0xa1 = 0, lap +0xa4 = the car's lap now, fraction +0xa8 = 0 (due at once), +0xac time, +0xb0,
// +0xb1 = 0 (a real failure, not the false alarm). AI cars: the part breaks at once and the AI retires the car the
// stock way. The player: the "failure suspected" radio, a few seconds later the part breaks ("confirmed"), then the
// RETIRED hand-over the addon handles as a game failure. A player car whose sequence cannot be armed gets the
// addon's own failure instead.
void arm_breakdowns()
{
    uint32_t want = breakdown_request.exchange(0);
    if (!want)
        return;
    int player = player_vehicle.load();
    for (int v = 0; v < 32; ++v) {
        if (!(want & (1u << v)))
            continue;
        uint8_t part = breakdown_part[v].load();
        uintptr_t obj = failure_obj_of(v);
        // Lap 0, fraction 0 is due in any lap (the trigger fires on `lap < current lap` or the same lap with the
        // fraction reached), so the lap getter is not needed (it gave -1 in a Grand Prix race).
        int lap = obj ? game_lap_of(obj) : -1;
        uint8_t state = 0xff, old_part = 0xff;
        bool ok = obj && guarded_read(obj + 0xa1, state) && guarded_read(obj + 0xa0, old_part) &&
                  // a sequence already running (states 1-4) is left alone
                  (old_part >= 7 || state == 0 || state >= 5);
        if (ok) {
            auto *o = reinterpret_cast<volatile uint8_t *>(obj);
            uint32_t l = 0;
            float f = 0.0f;
            o[0xa0] = part;
            o[0xa1] = 0;
            memcpy(const_cast<uint8_t *>(o) + 0xa4, &l, 4);
            memcpy(const_cast<uint8_t *>(o) + 0xa8, &f, 4);
            memset(const_cast<uint8_t *>(o) + 0xac, 0, 4);
            o[0xb0] = 0;
            o[0xb1] = 0;
            if (v == player && !failure_planner_obj.load())
                failure_planner_obj = obj;
            if (v == player)
                breakdown_watch = {obj, part, GetTickCount64()};
        }
        swofty::log("[td-probe] breakdown: %s car %d part %u at lap %d%s", ok ? "armed" : "could not arm",
                    v, unsigned(part), lap, v == player ? " (yours)" : "");
        if (!ok && v == player)
            trigger_failure(part);
    }
}

void breakdown_watch_tick()
{
    breakdown_watch_state &b = breakdown_watch;
    if (!b.obj)
        return;
    uint8_t state = 0xff, part = 0xff;
    if (player_broken || !guarded_read(b.obj + 0xa1, state) || !guarded_read(b.obj + 0xa0, part) || part != b.part ||
        state != 0) {
        if (!player_broken)
            swofty::log("[td-probe] breakdown: the game took it (part %u, state %u)", unsigned(part), unsigned(state));
        b = {};
        return;
    }
    if (GetTickCount64() < b.at + 15000)
        return;
    swofty::log("[td-probe] breakdown: the game did not start part %u in 15 s, the addon breaks it",
                unsigned(b.part));
    reinterpret_cast<volatile uint8_t *>(b.obj)[0xa0] = 7;  // disarmed (7 = no part)
    uint8_t p = b.part;
    b = {};
    trigger_failure(p);
}

// Game thread: register the requested temporary fault for the player's car the way the game's own player fault
// planning does (0x1409b37a0(obj, record), record template of 0x1409bb078). It starts at once: the game plays the
// "fault imminent" radio, applies it a few seconds later ("fault occurred") and removes it after the duration
// ("fault fixed"). Flashback safe (the game re-applies its records on a rewind).
void register_fault(uintptr_t car)
{
    // 5, 15, 30 and 60 s after registering: log how far the game got with the newest record (obj+0xc8..+0xd0):
    // +4/+8 start lap and fraction (rewritten when it starts), +0x10 start time, +0x14 warning time (the "fault
    // imminent" radio, -1 before), +0x18 active, +0x19 finished, +0x1a allowed (needs the vehicle's faults component).
    // 0.51.93 log 20261103-094201: allowed 1 but not active after 12 s.
    static uint64_t registered_at = 0;
    static int checks_done = 0;
    static const uint64_t check_after[] = {5000, 15000, 30000, 60000};
    if (registered_at && checks_done < 4 && GetTickCount64() >= registered_at + check_after[checks_done]) {
        uint64_t obj = player_failure_obj(car), b = 0, e = 0;
        uint8_t rec[0x34] = {};
        uint8_t flag88 = 0;
        if (obj && guarded_read(obj + 0xc8, b) && guarded_read(obj + 0xd0, e) && e >= b + 0x34 &&
            guarded_copy(e - 0x34, rec, sizeof rec)) {
            guarded_read(obj + 0x88, flag88);
            uint32_t type = 0, lap0 = 0;
            float frac0 = 0, dur = 0, t0 = 0, warn = 0;
            memcpy(&type, rec, 4);
            memcpy(&lap0, rec + 4, 4);
            memcpy(&frac0, rec + 8, 4);
            memcpy(&dur, rec + 0xc, 4);
            memcpy(&t0, rec + 0x10, 4);
            memcpy(&warn, rec + 0x14, 4);
            // Effect size: record +0x2c (rolled from the game's database for types 0/1), and what the car got:
            // engine+0x2534 (engine fault) and gearbox+0x67c (shift fault), both added to a drivetrain term in
            // 0x144678360. In the 0.51.94 test slow shifts did not feel slower.
            float magnitude = 0, engine_fault = -1, shift_fault = -1;
            memcpy(&magnitude, rec + 0x2c, 4);
            uintptr_t engine = 0, gearbox = 0;
            if (guarded_read(car + 0x530, engine) && engine)
                guarded_read(engine + 0x2534, engine_fault);
            uint32_t gear_missing = 99, gear = 99, target = 99;
            float shift_scale = -1, up_time = -1, down_time = -1;
            auto xor_float = [](uintptr_t a, uintptr_t k) {
                uint32_t x = 0, y = 0;
                float f = -1;
                if (guarded_read(a, x) && guarded_read(k, y)) {
                    x ^= y;
                    memcpy(&f, &x, 4);
                }
                return f;
            };
            if (guarded_read(car + 0x538, gearbox) && gearbox) {
                guarded_read(gearbox + 0x67c, shift_fault);
                guarded_read(gearbox + 0x664, gear_missing);
                guarded_read(gearbox + 0x66c, shift_scale);
                guarded_read(gearbox + 0x680, gear);
                guarded_read(gearbox + 0x684, target);
                up_time = xor_float(gearbox + 0x170, gearbox + 0x178);
                down_time = xor_float(gearbox + 0x180, gearbox + 0x188);
            }
            swofty::log("[td-probe] fault: after %llu s type %u start lap %u fraction %.2f duration %.1f, start time "
                        "%.1f, warning time %.1f, active %u finished %u allowed %u, obj+0x88 %u, lap now %d, "
                        "magnitude %.4f, engine fault %.4f, shift fault %.4f, missing gear index %u (gearbox %u), "
                        "shift time scale %.2f (up %.3f s, down %.3f s), gear %u target %u (%llu records)",
                        (unsigned long long)(check_after[checks_done] / 1000), type, lap0, double(frac0), double(dur),
                        double(t0), double(warn), rec[0x18], rec[0x19], rec[0x1a], flag88, game_lap_of(obj),
                        double(magnitude), double(engine_fault), double(shift_fault), rec[0x30],
                        gear_missing, double(shift_scale), double(up_time), double(down_time), gear, target,
                        (unsigned long long)((e - b) / 0x34));
        } else {
            swofty::log("[td-probe] fault: the record could not be read");
        }
        ++checks_done;
    }
    uint64_t req = fault_request.load();
    if (!req || !fault_request.compare_exchange_strong(req, 0))
        return;
    uintptr_t obj = player_failure_obj(car);
    if (!obj || !orig_failure_register) {
        swofty::log("[td-probe] fault: cannot register (failure object %p)", reinterpret_cast<void *>(obj));
        return;
    }
    // Start lap 0, fraction 0: due at once in any lap (the game starts a record when `lap < current lap` or the
    // fraction is reached in the same lap) and writes the real lap and fraction when it starts it. 0.51.92 log
    // 20261103-093048: the lap getter gave -1 in a Grand Prix race, so the fault never registered.
    int lap = game_lap_of(obj);
    // Make game_lap() work from now on (the planner sets it only once the game plans a failure itself).
    if (!failure_planner_obj.load())
        failure_planner_obj = obj;
    alignas(8) uint8_t rec[0x34] = {};
    uint32_t type = uint32_t(req & 0xff), start = 0;
    float frac = 0.0f, duration = float((req >> 8) & 0xffffffff) / 10.0f, unset = -1.0f;
    memcpy(rec + 0x00, &type, 4);
    memcpy(rec + 0x04, &start, 4);
    memcpy(rec + 0x08, &frac, 4);
    memcpy(rec + 0x0c, &duration, 4);
    memcpy(rec + 0x10, &unset, 4);
    memcpy(rec + 0x14, &unset, 4);
    rec[0x1d] = uint8_t((req >> 40) & 1);
    // Missing gear: the gearbox's gear index (0 reverse, 1 neutral, 2 first gear ...: the shift code 0x144742557 skips
    // a requested gear equal to gearbox+0x664 and never checks 0 and 1). 0.51.96 took the AI list at mgr+0x150, whose
    // gear the tester did not notice; the addon now names the gear itself.
    rec[0x30] = uint8_t((req >> 48) & 0xff);
    rec[0x31] = 0x63;
    // Effect size for slow shifts (0 = the game rolls it from its database: 36.6 in log 20261103-104709, not
    // noticeable): bits 56-63 times 10.
    float magnitude = float((req >> 56) & 0xff) * 10.0f;
    if (magnitude > 0)
        memcpy(rec + 0x2c, &magnitude, 4);
    orig_failure_register(reinterpret_cast<void *>(obj), rec);
    registered_at = GetTickCount64();
    checks_done = 0;
    swofty::log("[td-probe] fault: registered %s (type %u) at lap %d for %.0f %s, gear index %u, magnitude %.0f",
                fm::fault_name(int(type)), type, lap, double(duration), rec[0x1d] ? "s" : "laps", rec[0x30],
                double(magnitude));
}

// Slow shifts. The game's Gearbox Fault (type 1) writes its size to gearbox+0x67c, which only feeds a drivetrain
// inertia sum (0x144678360): nothing to feel at 36.6 or 150 (logs 20261103-104709, -110541). The shift time itself
// is the base time (up: +0x170 ^ +0x178, down: +0x180 ^ +0x188) times gearbox+0x66c (state "shiftfaultscale", getter
// 0x1446a6040; used in 0x1446a6b60: the gear changes from +0x680 to the target +0x684 once the time in the shift
// +0x690 reaches it). The gearbox's own damage path raises +0x66c (ShiftTimeFaultScaleMin/Max at +0x4e0/+0x4f0) until
// the clock [gearbox+8]+0x9db0 passes +0x670, then puts it back to 1 (0x1446a6128). So while a slow-shift record of
// the player is active, the addon holds +0x66c at the record's size / 10 and keeps +0x670 one second ahead: it ends
// by itself a second after the fault (or a flashback before it). Base times 0.015 s up, 0.030 s down (log
// 20261103-113432). The addon asks for 3-15 times (fm::slow_shift_severity), the request carries scale x 10.
inline float slow_shift_scale(float magnitude) { return std::clamp(magnitude / 10.0f, 1.0f, 20.0f); }

void slow_shift_tick(uintptr_t car)
{
    static bool was_active = false;
    static uint64_t next_scan = 0;
    static float record_scale = 0;
    // The game's records only change when a fault starts or ends: read them 4 times a second while no slow shift is
    // active, every update while one is (this runs on every player update).
    if (uint64_t now = GetTickCount64(); was_active || now >= next_scan) {
        next_scan = now + 250;
        record_scale = 0;
        uint64_t obj = car ? player_failure_obj(car) : 0, b = 0, e = 0;
        if (obj && guarded_read(obj + 0xc8, b) && guarded_read(obj + 0xd0, e) && e > b && (e - b) % 0x34 == 0) {
            size_t n = std::min<size_t>((e - b) / 0x34, 64);
            uint8_t recs[64 * 0x34];
            if (guarded_copy(e - n * 0x34, recs, n * 0x34))
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t *rec = recs + i * 0x34;
                    uint32_t type = 0;
                    float magnitude = 0;
                    memcpy(&type, rec, 4);
                    memcpy(&magnitude, rec + 0x2c, 4);
                    if (type == fm::f_slow_shift && rec[0x18] && !rec[0x19])
                        record_scale = std::max(record_scale, slow_shift_scale(magnitude));
                }
        }
        uintptr_t gb = 0;
        int missing = 11;
        if (car && guarded_read(car + 0x538, gb) && gb)
            guarded_read(gb + 0x664, missing);
        shown_missing_gear = missing;
    }
    float scale = std::max(record_scale, decline_shift_scale.load());
    uintptr_t gearbox = 0, vehicle = 0;
    float current = 0, clock = 0;
    if (scale > 0 && guarded_read(car + 0x538, gearbox) && gearbox && guarded_read(gearbox + 0x66c, current) &&
        guarded_read(gearbox + 8, vehicle) && vehicle && guarded_read(vehicle + 0x9db0, clock)) {
        auto *g = reinterpret_cast<volatile float *>(gearbox + 0x66c);
        if (current != scale)
            g[0] = scale;  // also lowers it when a fault eases off
        g[1] = clock + 1.0f;  // +0x670
    }
    bool active = scale > 0;
    shown_shift_scale = scale;
    if (active != was_active)
        swofty::log("[td-probe] fault: slow shifts %s (shift time x%.1f)", active ? "on" : "off", double(scale));
    was_active = active;
}

// Rear brake failure (re/brake-torque-path.md). Every physics tick the car update 0x14473fd80 splits the brake torque
// by the bias into each wheel's maximum brake torque wheel+0xf8 (float, XOR-keyed with the dword at +0x100) for
// every car, then runs the brake step 0x1446dc480 (VehicleSim vt+8: rcx = sim, [sim+0x18] = car, xmm1 = dt), which
// stores pedal x that torque x the wheel's brake factor (+0xf0) as the applied torque +0x108 that the wheels and
// the powertrain use. The addon scales the rear wheels' +0xf8 (sys = car+0x580, +0x70 RL, +0x78 RR, skipped when
// byte +0x138 is set, as the game does) right before that step and puts the game's value back after it: no state
// and nothing accumulates, the next tick's split starts fresh. 0.51.106-0.51.113 held +0xf8 on the game thread and
// after the AI's split 0x144676250; the next tick's split overwrote both before the brake step, so the rear share
// stayed at 47 % (0.51.113 telemetry) and nothing could be felt.
// The hook runs for every car, maybe on job threads: atomics only, no log, no lock.
using brake_step_fn = void (*)(void *, float);
brake_step_fn orig_brake_step = nullptr;
constexpr uint8_t brake_step_prologue[] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00};
constexpr const char *brake_step_sig =
    "41 57 41 56 56 57 53 48 81 EC 10 01 00 00 C5 78 29 BC 24 00 01 00 00 C5 78 29 B4 24 F0 00 00 00 C5 78 29 AC 24 "
    "E0 00 00 00 C5 78 29 A4 24 D0 00 00 00 C5 78 29 9C 24 C0 00 00 00 C5 78 29 94 24 B0 00 00 00 C5 78 29 8C 24 A0 "
    "00 00 00 C5 78 29 84 24 90 00 00 00 C5 F8 29 BC 24 80 00 00 00 C5 F8 29 74 24 70 48 89 CE 48 8D 0D ?? ?? ?? ?? "
    "48 8B 46 18 83 B8 24 01 00 00 01";
std::atomic<bool> brake_step_hooked{false};

void on_brake_step(void *sim, float dt)
{
    float f = rear_brake_factor.load(std::memory_order_relaxed);
    uintptr_t car = 0, sys = 0, wheel[2] = {};
    uint32_t old[2] = {}, put[2] = {};
    bool changed = false;
    // [sim+0x18] read directly: the step itself reads it first thing, so it is valid whenever the game calls it.
    if (f != 1.0f && sim && (car = *reinterpret_cast<const volatile uintptr_t *>(reinterpret_cast<uintptr_t>(sim) + 0x18)) &&
        car == brake_car.load(std::memory_order_relaxed) && guarded_read(car + 0x580, sys) && sys) {
        for (int w = 0; w < 2; ++w) {
            uint32_t enc = 0, key = 0;
            uint8_t skip = 1;
            if (!guarded_read(sys + 0x70 + 8 * w, wheel[w]) || !wheel[w] || !guarded_read(wheel[w] + 0x138, skip) ||
                skip || !guarded_read(wheel[w] + 0xf8, enc) || !guarded_read(wheel[w] + 0x100, key)) {
                wheel[w] = 0;
                continue;
            }
            float torque = std::bit_cast<float>(enc ^ key) * f;
            if (!std::isfinite(torque) || torque < 0) {
                wheel[w] = 0;
                continue;
            }
            old[w] = enc;
            put[w] = std::bit_cast<uint32_t>(torque) ^ key;
            *reinterpret_cast<volatile uint32_t *>(wheel[w] + 0xf8) = put[w];
            changed = true;
        }
    }
    orig_brake_step(sim, dt);
    if (!changed)
        return;
    for (int w = 0; w < 2; ++w) {
        if (!wheel[w])
            continue;
        auto *enc = reinterpret_cast<volatile uint32_t *>(wheel[w] + 0xf8);
        if (*enc == put[w])  // still ours (not re-keyed): the game's value back for every other reader
            *enc = old[w];
    }
    brake_steps_scaled.fetch_add(1, std::memory_order_relaxed);
}

void hook_brake_step()
{
    brake_step_hooked = detour("brake step (rear brake failure)", brake_step_sig, brake_step_prologue,
                               sizeof brake_step_prologue, reinterpret_cast<void *>(&on_brake_step),
                               reinterpret_cast<void **>(&orig_brake_step));
}

// Game thread: the player's car and the rear brake factor for the hook; a timed fault (test, debug) ends here.
void brake_fault_tick(uintptr_t car)
{
    brake_car.store(car, std::memory_order_relaxed);
    float loss = rear_brake_loss_target.load();
    uint64_t until = rear_brake_until.load();
    if (loss != 0 && until && GetTickCount64() >= until) {
        rear_brake_loss_target = 0.0f;
        rear_brake_until = 0;
        loss = 0;
        swofty::log("[td-probe] fault: rear brakes back");
    }
    float factor = (1.0f - loss) * (1.0f - brake_fade.load());
    float was = rear_brake_factor.exchange(factor);
    // Once a second while it acts: how many brake steps the hook scaled (the proof it reaches the brakes).
    static uint64_t next_log = 0;
    uint64_t now = GetTickCount64();
    if (factor != 1.0f && (was == 1.0f || now >= next_log)) {
        uint32_t n = brake_steps_scaled.exchange(0);
        if (was == 1.0f)
            swofty::log("[td-probe] brakes: rear brake torque x%.2f from now (brake step hook %s)", double(factor),
                        brake_step_hooked.load() ? "on" : "MISSING, no effect");
        else if (now >= next_log && next_log)
            swofty::log("[td-probe] brakes: rear brake torque x%.2f, %u brake steps scaled in the last 10 s",
                        double(factor), n);
        next_log = now + 10000;
    } else if (factor == 1.0f && was != 1.0f) {
        swofty::log("[td-probe] brakes: rear brake torque back to normal (%u brake steps scaled)",
                    brake_steps_scaled.exchange(0));
        next_log = 0;
    }
}

// Test run, game thread: time every shift (gearbox +0x680 current gear, +0x684 target) to the frame, and count the
// gear each shift ends in (a missing gear must never show up).
void test_shift_tick(uintptr_t car)
{
    static bool shifting = false, up = false;
    static std::chrono::steady_clock::time_point began;
    if (!test_run_on.load()) {
        shifting = false;
        return;
    }
    if (test_shifts.reset.exchange(false)) {
        for (int i = 0; i < 2; ++i) {
            test_shifts.count[i] = 0;
            test_shifts.total_us[i] = 0;
            test_shifts.max_us[i] = 0;
        }
        for (auto &g : test_shifts.gear_seen)
            g = 0;
    }
    uintptr_t gearbox = 0;
    uint32_t gear = 0, target = 0;
    if (!guarded_read(car + 0x538, gearbox) || !gearbox || !guarded_read(gearbox + 0x680, gear) ||
        !guarded_read(gearbox + 0x684, target) || gear > 11 || target > 11)
        return;
    auto now = std::chrono::steady_clock::now();
    if (!shifting && target != gear) {
        shifting = true;
        up = target > gear;
        began = now;
    } else if (shifting && target == gear) {
        shifting = false;
        uint32_t us = uint32_t(std::min<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - began).count(), 10000000));
        int k = up ? 0 : 1;
        ++test_shifts.count[k];
        test_shifts.total_us[k] += us;
        if (us > test_shifts.max_us[k].load())
            test_shifts.max_us[k] = us;
        ++test_shifts.gear_seen[gear];
    }
}

// Share of the last minute at full throttle (car+0x188, after the addon's cap), for nursing a failing part.
void sample_throttle(uintptr_t car)
{
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    double dt = last ? double(now - last) / 1000.0 : 0;
    last = now;
    float throttle = 0;
    if (dt <= 0 || dt > 0.5 || player_pit.load() != 1 || !guarded_read(car + 0x188, throttle))
        return;
    float share = full_throttle_share.load();
    share += float(1.0 - std::exp(-dt / 60.0)) * ((throttle >= 0.98f ? 1.0f : 0.0f) - share);
    full_throttle_share = share;
}

// Test run brake telemetry (game thread). The game's D-BOX motion output (0x144728690) reads the car like this:
// body = [[car+0x558]+0x340]+0x18 with the orientation rows sideways +0x190, up +0x1a0, forward +0x1b0, angular
// velocity +0x210 and velocity +0x220 (world, m/s); per wheel (sys = car+0x580, +0x70.. RL RR FL FR) the rim speed
// wheel+0x188 (angular speed) x wheel+0x3c (radius), a brake value +0x45c it divides by the bias-split maximum,
// and the contact point +0x320. The brake torque applied now is wheel+0x108 XOR +0x110 (brake step 0x1446dc480).
btel::tracker brake_tel;
std::atomic<const char *> brake_summary_step{nullptr};
std::atomic<bool> brake_summary_power{false};  // the step that ended had a power fault (for the power check)
std::atomic<bool> brake_tel_reset{false};
std::atomic<bool> power_summary_due{false};  // the run stopped: log its power steps against all steps without one

// 0 ok, else why not: 1 physics chain, 2 wheel system, 3 body rows unreadable, 4 + w a wheel read, 8 not finite.
int read_brake_sample(uintptr_t car, btel::sample &s, float extra[4])
{
    uintptr_t physics = 0, rigid = 0, body = 0, sys = 0;
    if (!car || !guarded_read(car + 0x558, physics) || !physics || !guarded_read(physics + 0x340, rigid) || !rigid ||
        !guarded_read(rigid + 0x18, body) || !body)
        return 1;
    if (!guarded_read(car + 0x580, sys) || !sys)
        return 2;
    float f[0xa0 / 4];
    if (!guarded_copy(body + 0x190, f, sizeof f))
        return 3;
    // 0.51.112 log: the speed lies along +0x1b0 (forward), +0x190 is sideways.
    const float *side = f, *up = f + 4, *fwd = f + 8, *spin = f + (0x210 - 0x190) / 4, *vel = f + (0x220 - 0x190) / 4;
    auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    s.v_long = std::fabs(dot(vel, fwd));
    s.v_lat = dot(vel, side);
    s.yaw_rate = dot(spin, up);
    for (int w = 0; w < 4; ++w) {
        uintptr_t wheel = 0;
        float omega = 0, radius = 0, raw = 0;
        uint32_t enc = 0, key = 0;
        if (!guarded_read(sys + 0x70 + 8 * w, wheel) || !wheel || !guarded_read(wheel + 0x188, omega) ||
            !guarded_read(wheel + 0x3c, radius) || !guarded_read(wheel + 0x108, enc) ||
            !guarded_read(wheel + 0x110, key) || !guarded_read(wheel + 0x45c, raw))
            return 4 + w;
        s.rim[w] = omega * radius;
        s.torque[w] = std::bit_cast<float>(enc ^ key);
        extra[w] = raw;
    }
    guarded_read(car + 0x188, s.throttle);
    for (float x : {s.v_long, s.v_lat, s.yaw_rate, s.rim[0], s.rim[1], s.rim[2], s.rim[3], s.torque[0], s.torque[1],
                    s.torque[2], s.torque[3]})
        if (!std::isfinite(x))
            return 8;
    return 0;
}

// The run's full-throttle acceleration in the steps without a power fault: what the power steps compare with. Every
// step of the run for the summary at its end.
btel::power_bands power_reference;
btel::power_run power_run;

void log_brake_summary(const char *step, bool power_step)
{
    const btel::step_stats &st = brake_tel.st;
    swofty::log("[td-probe] test run: brakes in %s: %d zones, peak avg %.1f g, rear share avg %.0f %%, lock-ups front "
                "%d rear %d, slides %d, worst slip %.0f deg",
                step, st.zones, st.zones ? st.decel_sum / st.zones / btel::g : 0.0,
                st.zones ? st.share_sum / st.zones * 100.0 : 0.0, st.locks_front, st.locks_rear, st.slides,
                double(st.worst_slip_deg));
    // Power check: per band "accel m/s^2 over seconds", and for a power step the change against the reference.
    char text[320];
    btel::format_bands(text, sizeof text, st.power, power_step ? &power_reference : nullptr);
    swofty::log("[td-probe] test run: power in %s: full-throttle acceleration m/s^2 on the straights: %s%s", step, text,
                power_step ? " (against the steps without a power fault so far)" : "");
    if (!power_step)
        power_reference.add(st.power);
    power_run.add(step, power_step, st.power);
    brake_tel.st = {};
}

// The run is over: each power step against all its steps without a power fault, the later ones included (0.51.115
// log: the baseline right after the race start had the fullest battery, 8.3 m/s^2 at 200-240 km/h against 6.6-6.8 in
// the later steps, so "against the steps so far" overstated the MGU-K loss).
void log_power_summary()
{
    int refs = 0, powers = 0;
    for (int i = 0; i < power_run.n; ++i)
        ++(power_run.e[i].power ? powers : refs);
    if (powers && refs) {
        btel::power_bands ref = power_run.reference();
        char text[320];
        btel::format_bands(text, sizeof text, ref, nullptr);
        swofty::log("[td-probe] test run: power summary, the %d steps without a power fault: %s", refs, text);
        for (int i = 0; i < power_run.n; ++i)
            if (power_run.e[i].power) {
                btel::format_bands(text, sizeof text, power_run.e[i].p, &ref);
                swofty::log("[td-probe] test run: power summary, %s: %s", power_run.e[i].name, text);
            }
    }
    power_run = {};
}

void test_brake_tick(uintptr_t car)
{
    static std::chrono::steady_clock::time_point last{};
    static bool sanity_logged = false;
    static int last_fail = -1;
    static double sample_clock = 0, next_raw = 0;
    if (brake_tel_reset.exchange(false)) {
        brake_tel = {};
        power_reference = {};
        power_run = {};
        sanity_logged = false;
        last = {};
        last_fail = -1;
        sample_clock = next_raw = 0;
    }
    // The summary flag first: when it is set, the last step's summary was posted before it.
    bool summary_due = power_summary_due.exchange(false);
    if (const char *step = brake_summary_step.exchange(nullptr))
        log_brake_summary(step, brake_summary_power.load());
    if (summary_due)
        log_power_summary();
    if (!test_run_on.load())
        return;
    auto now = std::chrono::steady_clock::now();
    btel::sample smp;
    smp.dt = last == std::chrono::steady_clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - last).count();
    last = now;
    float extra[4] = {};
    if (int why = read_brake_sample(car, smp, extra); why) {
        if (why != last_fail)
            swofty::log("[td-probe] brake telemetry: no sample (reason %d: %s)", why,
                        why == 1 ? "physics chain" : why == 2 ? "wheel system" : why == 3 ? "body rows" :
                        why < 8 ? "wheel read" : "not finite");
        last_fail = why;
        return;
    }
    last_fail = 0;
    sample_clock += smp.dt;
    // Raw values: the first sample and every 20 s for the first two minutes, to check the offsets.
    if (!sanity_logged || (sample_clock >= next_raw && sample_clock < 130)) {
        next_raw = sample_clock + 20;
        sanity_logged = true;
        float throttle = -1, other = -1;
        guarded_read(car + 0x188, throttle);
        guarded_read(car + 0x18c, other);
        swofty::log("[td-probe] brake telemetry: %d km/h, v_long %.1f v_lat %.1f m/s, yaw %.2f rad/s, rim RL %.1f RR "
                    "%.1f FL %.1f FR %.1f m/s, torque %.0f %.0f %.0f %.0f, +0x45c %.2f %.2f %.2f %.2f, car+0x188 %.2f "
                    "+0x18c %.2f",
                    game_speed.load(), double(smp.v_long), double(smp.v_lat), double(smp.yaw_rate),
                    double(smp.rim[0]), double(smp.rim[1]), double(smp.rim[2]), double(smp.rim[3]),
                    double(smp.torque[0]), double(smp.torque[1]), double(smp.torque[2]), double(smp.torque[3]),
                    double(extra[0]), double(extra[1]), double(extra[2]), double(extra[3]), double(throttle),
                    double(other));
    }
    int ev = brake_tel.feed(smp);
    if (ev & btel::tracker::slide_started)
        swofty::log("[td-probe] brakes: slide at %.0f km/h, slip %.0f deg, yaw %.2f rad/s%s", smp.v_long * 3.6,
                    double(btel::slip_deg(smp.v_long, smp.v_lat)), double(smp.yaw_rate),
                    brake_tel.in_zone ? ", under braking" : "");
    if (ev & btel::tracker::zone_done) {
        const btel::zone &z = brake_tel.z;
        char locks[32] = {};
        static const char *const names[4] = {"RL", "RR", "FL", "FR"};
        for (int w = 0, at = 0; w < 4; ++w)
            if (z.locked(w))
                at += snprintf(locks + at, sizeof locks - at, "%s%s", at ? " " : "", names[w]);
        swofty::log("[td-probe] brakes: %.0f -> %.0f km/h in %.1f s, peak %.1f g, rear share %.0f %%, lowest wheel "
                    "speed RL %.2f RR %.2f FL %.2f FR %.2f, slip max %.0f deg, yaw max %.2f rad/s%s%s",
                    z.v_in * 3.6, z.v_min * 3.6, z.time, double(z.peak_decel / btel::g), z.rear_share() * 100.0,
                    double(z.min_ratio[0]), double(z.min_ratio[1]), double(z.min_ratio[2]), double(z.min_ratio[3]),
                    double(z.max_slip_deg), double(z.max_yaw), locks[0] ? ", LOCKED " : "", locks);
    }
}

uint64_t on_update_vehicle(void *system, uint8_t vehicle)
{
    if (session_ended)
        return orig_update_vehicle(system, vehicle);
    td_perf::Scope before(td_perf::vehicle_before);
    apply_pending_failure(system, vehicle);
    before.finish();
    uint64_t r = orig_update_vehicle(system, vehicle);
    td_perf::Scope after(td_perf::vehicle_after);
    if (vehicle < 32 && (ai_watch_mask.load(std::memory_order_relaxed) & (1u << vehicle)) &&
        int(vehicle) != player_vehicle.load())
        ai_watch_tick(reinterpret_cast<uintptr_t>(system), vehicle);
    if (susp::ai_wanted()) {
        if (int(vehicle) != player_vehicle.load())
            susp::ai_game_thread(vehicle, car_of(reinterpret_cast<uintptr_t>(system), vehicle));
        else
            susp::ai_forget(vehicle);
    }
    if (int(vehicle) == player_vehicle.load()) {
        uintptr_t car = car_of(reinterpret_cast<uintptr_t>(system), vehicle);
        throttle_hook.car = car;
        if (player_broken || log_telemetry || susp::speed_wanted() || test_run_on.load() || power_speed_wanted.load())
            read_game_speed(car);
        susp::game_thread(car);
        sample_throttle(car);
        register_fault(car);
        slow_shift_tick(car);
        brake_fault_tick(car);
        radio_tick(car);
        test_shift_tick(car);
        test_brake_tick(car);
        arm_breakdowns();
        breakdown_watch_tick();
        if (red_flag_fix_now.exchange(false))
            repair_crash_damage(car);
        garage_repair_finish(car);
        read_terminal_record(reinterpret_cast<uintptr_t>(system), vehicle);
        flashback_check(car);
        power_loss(car);
        return_to_garage();
        game_failure_dsq_now();
    }
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

#include "stopped_player_lookup.hpp"

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
    // as the game does when you leave the garage in practice: SendCarToTrack, then LaneClear once it is in P2.
    // The crew's own SendToTrack report changes nothing while the car is OnTrack.
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

// The engine manager schedules replacements through this AddRepair thunk
// (reference 0x142bf0200). Entries are {u32 kind, float seconds}; ICE is kind 9,
// established by the engine-manager caller and RepairTimesViewModel jump table.
constexpr const char *sig_replacement_repair_site =
    "48 89 F1 89 FA E8 ?? ?? ?? ?? C5 F8 28 74 24 60 C5 F8 28 7C 24 70 C5 78 28 84 24 80 00 00 00";
constexpr const char *sig_repair_session_kind =
    "8B 05 ?? ?? ?? ?? 83 C0 F1 83 F8 02 77 26 80 7D 48 00 0F 85 ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 8B 4D 40 3B 48 10";
using add_repair_fn = uint64_t (*)(void *, uint32_t);
add_repair_fn orig_add_repair;
uintptr_t repair_session_kind;

// Only used inside the game's live AddRepair call. Never retain a repair entry.
bool repair_item(uintptr_t record, uint32_t kind, uintptr_t &item)
{
    item = 0;
    uintptr_t first = 0, last = 0;
    if (!record || !guarded_read(record + 0x20, first) || !guarded_read(record + 0x28, last) ||
        last < first || (last - first) % 8 || (last - first) / 8 > 32 || (!first && last))
        return false;
    for (uintptr_t at = first; at < last; at += 8) {
        uint32_t current = 0;
        if (!guarded_read(at, current))
            return false;
        if (current == kind) {
            item = at;
            return true;
        }
    }
    return true;
}

uint64_t on_add_repair(void *record_ptr, uint32_t kind)
{
    td_perf::Scope before(td_perf::repair);
    auto record = reinterpret_cast<uintptr_t>(record_ptr);
    uint8_t vehicle = 0xff;
    uint32_t session = 0;
    uintptr_t previous = 0;
    // Game session kinds 1..4 are practice, 5..14 qualifying/shootout,
    // 15..17 races. Require an identifiable player and the live game kind.
    bool extend = kind == 9 && keep_control && !session_ended &&
                  guarded_read(record + 0x16, vehicle) && int(vehicle) == player_vehicle.load() &&
                  repair_session_kind && guarded_read(repair_session_kind, session) &&
                  session >= 5 && session <= 14 && repair_item(record, kind, previous) && !previous;
    before.finish();
    uint64_t result = orig_add_repair(record_ptr, kind);
    td_perf::Scope after(td_perf::repair);
    // AddRepair deduplicates requests. Extend only a newly inserted job, so
    // normal recalculation cannot keep resetting a running repair's countdown.
    uintptr_t item = 0;
    float seconds = 0;
    if (extend && repair_item(record, kind, item) && item && guarded_read(item + 4, seconds) &&
        std::isfinite(seconds) && seconds > 0 && seconds < 3600.0f) {
        *reinterpret_cast<volatile float *>(item + 4) = 3600.0f;
        swofty::log("[td-probe] qualifying repair: vehicle %u ICE replacement %.1f -> 3600 s; skip repairs consumes the remaining session",
                    unsigned(vehicle), double(seconds));
    }
    return result;
}

void hook_qualifying_engine_repair()
{
    uintptr_t site = 0, session_site = 0;
    int a = swofty::api->scan(".xtls", sig_replacement_repair_site, &site);
    int b = swofty::api->scan(".xtls", sig_repair_session_kind, &session_site);
    if (a != 1 || b != 1 || !site || !session_site) {
        swofty::log("[td-probe] qualifying repair: signatures %d/%d hits, stock repair times retained", a, b);
        return;
    }
    int32_t target_rel = 0, session_rel = 0;
    memcpy(&target_rel, reinterpret_cast<void *>(site + 6), 4);
    memcpy(&session_rel, reinterpret_cast<void *>(session_site + 2), 4);
    repair_session_kind = session_site + 6 + int64_t(session_rel);
    hook_thunk_at("qualifying ICE replacement repair time", site + 10 + int64_t(target_rel),
                  reinterpret_cast<void *>(&on_add_repair), reinterpret_cast<void **>(&orig_add_repair));
}

// Practice and qualifying: a car that limped into its garage on one broken corner gets the game's own
// "detached wheel" repair job (kind 16, lng_ft_detached_wheel) with a long time: 20-30 min for a broken suspension,
// 30-45 min for a lost wheel. The game shows it as its repair timer. RepairTimes (update 0x142bf50a0, `this` rcx,
// dt xmm1) keeps per-vehicle records at +0x48..+0x50 (stride 0x58: +0x10 active, +0x16 vehicle, +0x20..+0x28 jobs
// {u32 kind, float seconds}) and counts the jobs of active records down. A finished job calls 0x142bf03e0(record,
// kind) and is erased; for kinds 3..16 that resets the game's terminal damage record. When our job finishes, the
// addon rebuilds the corner (repair_crash_damage) and the car may leave again. A car that could not be driven
// (abandoned on track, FIA B1.8.9) gets no job and stays in its garage for the session.
using repair_update_fn = void (*)(void *, float);
repair_update_fn orig_repair_update;
using repair_done_fn = void (*)(void *, uint32_t);
repair_done_fn orig_repair_done;
bool repair_update_hooked = false;
constexpr uint8_t repair_update_prologue[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x70, 0xC5, 0x78, 0x29,
                                              0x4C, 0x24, 0x60, 0xC5, 0x78, 0x29, 0x44, 0x24, 0x50};
constexpr uint8_t repair_done_prologue[] = {0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x41, 0x08,
                                            0x4C, 0x8B, 0x40, 0x30, 0x0F, 0xB6, 0x71, 0x16};

float garage_repair_time(uint32_t corners)
{
    if (debug_garage_repair == std::string_view("fast"))
        return 60.0f;
    if (garage_service_corners.load())
        return std::max(60.0f, garage_service_seconds.load());
    bool wheel = has_detached_corner(corners);
    float lo = wheel ? 1800.0f : 1200.0f, hi = wheel ? 2700.0f : 1800.0f;
    return lo + (hi - lo) * float(roll_percent()) / 99.0f;
}

// The serviced corners for a message: "front-left suspension", "front-left and rear-right suspension",
// "suspension on three corners". Front corners first.
void service_corners_text(uint32_t corners, char *out, size_t n)
{
    int count = 0, pick[2] = {-1, -1};
    for (int i : {2, 3, 0, 1})
        if (corners & (1u << i)) {
            if (count < 2)
                pick[count] = i;
            ++count;
        }
    char a[16] = "", b[16] = "";
    snprintf(a, sizeof a, "%s", susp::corner_name(pick[0]));
    snprintf(b, sizeof b, "%s", susp::corner_name(pick[1]));
    a[0] = char(tolower(uint8_t(a[0])));
    b[0] = char(tolower(uint8_t(b[0])));
    if (count <= 1)
        snprintf(out, n, "%s suspension", a);
    else if (count == 2)
        snprintf(out, n, "%s and %s suspension", a, b);
    else
        snprintf(out, n, "suspension on %s corners", count == 4 ? "all four" : "three");
}

// The game's session kind (1..4 practice, 5..14 qualifying and shootout, 15..17 races, 18 time trial), 0 unknown.
uint32_t game_session_kind()
{
    uint32_t session = 0;
    return repair_session_kind && guarded_read(repair_session_kind, session) ? session : 0;
}

// The player's RepairTimes record, or 0.
uintptr_t player_repair_record(uintptr_t manager)
{
    uintptr_t first = 0, last = 0;
    int vehicle = player_vehicle.load();
    if (vehicle < 0 || !guarded_read(manager + 0x48, first) || !guarded_read(manager + 0x50, last) ||
        !first || last < first || (last - first) % 0x58 || (last - first) / 0x58 > 64)
        return 0;
    for (uintptr_t at = first; at < last; at += 0x58) {
        uint8_t id = 0xff;
        if (guarded_read(at + 0x16, id) && int(id) == vehicle)
            return at;
    }
    return 0;
}

void on_repair_update(void *manager, float dt)
{
    int state = garage_repair.load();
    if (corner_toast_at.load() && GetTickCount64() >= corner_toast_at.load()) {
        corner_toast_at = 0;
        if (state == 2 && swofty::hud::available()) {
            char text[160], time[32];
            int minutes = corner_toast_minutes.load();
            if (minutes > 1)
                snprintf(time, sizeof time, "%d minutes", minutes);
            else
                snprintf(time, sizeof time, "about a minute");
            if (uint32_t corners = garage_service_corners.load()) {
                char where[64];
                service_corners_text(corners, where, sizeof where);
                if (garage_service_crack.load())
                    snprintf(text, sizeof text, "The team found a crack in the %s. The repair takes %s.", where, time);
                else
                    snprintf(text, sizeof text, "The team repairs the damaged %s. This takes %s.", where, time);
                swofty::hud::toast("SUSPENSION REPAIR", text, 8);
            } else {
                snprintf(text, sizeof text, "The team rebuilds the broken corner. This takes %s.", time);
                swofty::hud::toast("CORNER REPAIR", text, 8);
            }
        }
    }
    // A withdrawn car is not repaired: drop the game's own repair jobs (their timer and Skip Repair Time). A failed
    // power unit or gearbox keeps them, the part change runs through them.
    static bool cleared_logged = false;
    if (state == 0 && garage_exit_locked()) {
        uintptr_t record = player_repair_record(reinterpret_cast<uintptr_t>(manager)), first = 0, last = 0;
        if (record && guarded_read(record + 0x20, first) && guarded_read(record + 0x28, last) && first &&
            last > first && (last - first) % 8 == 0 && (last - first) / 8 <= 32) {
            *reinterpret_cast<volatile uintptr_t *>(record + 0x28) = first;
            if (!cleared_logged) {
                cleared_logged = true;
                swofty::log("[td-probe] garage: your car is %s, removed %d repair jobs of the game",
                            garage_withdrawn.load() ? "withdrawn" : "out for this session", int((last - first) / 8));
            }
            // 0.51.123 test: the engine change's timer showed for a moment and went, with no word why.
            static uint64_t toast_after = 0;
            if (!part_lock.load() && swofty::hud::available() && GetTickCount64() >= toast_after) {
                toast_after = GetTickCount64() + 5000;
                bool withdrawn = garage_withdrawn.load();
                swofty::hud::toast(withdrawn ? "CAR WITHDRAWN" : "NO REPAIRS",
                                   withdrawn ? "Your car stopped on track: no repairs in this session. Skip Session to go on."
                                             : "Your car cannot be repaired in this session. Skip Session to go on.",
                                   6);
            }
        }
    } else {
        cleared_logged = false;
    }
    if ((state == 1 || state == 2) && !session_ended && orig_add_repair) {
        uintptr_t record = player_repair_record(reinterpret_cast<uintptr_t>(manager)), item = 0;
        float seconds = 0;
        if (record && repair_item(record, 16, item)) {
            if (state == 1) {
                if (!item)
                    orig_add_repair(reinterpret_cast<void *>(record), 16);
                uint8_t active = 0;
                if (repair_item(record, 16, item) && item && guarded_read(item + 4, seconds) &&
                    std::isfinite(seconds) && guarded_read(record + 0x10, active)) {
                    float t = garage_repair_time(crash_corners.load());
                    *reinterpret_cast<volatile float *>(item + 4) = t;
                    garage_repair_left = t;
                    if (!active)
                        *reinterpret_cast<volatile uint8_t *>(record + 0x10) = 1;
                    garage_repair = 2;
                    swofty::log("[td-probe] garage repair: detached wheel job %.0f -> %.0f s (record %p, active %u, %s)",
                                double(seconds), double(t), reinterpret_cast<void *>(record), unsigned(active),
                                garage_service_corners.load()                ? "damaged suspension"
                                : has_detached_corner(crash_corners.load()) ? "lost wheel"
                                                                             : "broken suspension");
                    // Shown 3 s later: right now the garage camera is still fading in.
                    corner_toast_minutes = int(t / 60.0f + 0.5f);
                    corner_toast_at = GetTickCount64() + 3000;
                }
            } else if (item && guarded_read(item + 4, seconds)) {
                // The job's +4 is its length, not the time left (0.51.133 test 23:37:43-23:38:59: '60 s left' three
                // times, 30 s apart, the job ended after 76 s), so 'minutes left' never went down: the addon counts
                // down itself with the repair update's dt from the time it gave the job.
                float step = std::isfinite(dt) && dt > 0.0f && dt < 1.0f ? dt : 0.0f;
                float left = std::max(0.0f, garage_repair_left.load() - step);
                garage_repair_left = left;
                static uint64_t next_log = 0;
                if (GetTickCount64() >= next_log) {
                    next_log = GetTickCount64() + 30000;
                    swofty::log("[td-probe] garage repair: about %.0f s left (job %.0f s)", double(left),
                                double(seconds));
                }
            }
        }
    }
    orig_repair_update(manager, dt);
}

void on_repair_done(void *record, uint32_t kind)
{
    uint8_t id = 0xff;
    bool ours = kind == 16 && garage_repair.load() == 2 &&
                guarded_read(reinterpret_cast<uintptr_t>(record) + 0x16, id) && int(id) == player_vehicle.load();
    orig_repair_done(record, kind);
    if (ours) {
        garage_repair = 3;
        swofty::log("[td-probe] garage repair: the detached wheel job finished");
    }
}

// Game thread (updateVehicle of the player's car).
void garage_repair_finish(uintptr_t car)
{
    if (garage_repair.load() != 3 || !car)
        return;
    if (uint32_t corners = garage_service_corners.load()) {
        // The service: the model forgets the damage, nothing else changes (the car was never broken). car_dead goes
        // first: at garage_repair 0 the repair update drops the game's jobs of a car that may not leave.
        car_dead = false;
        garage_service_corners = 0;
        garage_service_seconds = 0;
        garage_service_crack = false;
        garage_repair = 0;
        repair_label(false);
        susp::request_reset("repair");
        swofty::log("[td-probe] garage repair: the suspension is repaired (corners 0x%x), your car may leave the "
                    "garage", corners);
        if (swofty::hud::available())
            swofty::hud::toast("CAR REPAIRED", "The suspension is repaired. Your car may leave the garage.", 6);
        return;
    }
    repair_crash_damage(car);
    int pit = player_pit.load();
    reset_car_state();
    player_pit = pit;
    swofty::log("[td-probe] garage repair: your car is rebuilt and may leave the garage");
    if (swofty::hud::available())
        swofty::hud::toast("CAR REPAIRED", "The broken corner is rebuilt. Your car may leave the garage.", 6);
}

// Frame thread (susp::tick): the car reached its practice or qualifying garage with corners that took damage but
// hold. The team services them with the crash repair's job (the detached wheel kind) for `seconds`; the car may not
// leave until it is done (car_dead: garage_exit_locked, the drive out buttons, on_set_vehicle_status). False when
// that cannot run (the addon does not keep control, the repair hooks are missing, another repair runs, time trial):
// the model is then repaired at once, as before 0.51.135.
bool start_garage_service(float seconds, uint32_t corners, bool crack)
{
    if (!keep_control || session_ended || player_broken || car_dead || garage_repair.load() != 0 || !corners ||
        !(seconds > 0) || !repair_update_hooked || !orig_add_repair || game_session_kind() > 14)
        return false;
    garage_service_seconds = seconds;
    garage_service_crack = crack;
    garage_service_corners = corners;
    // garage_repair first: at garage_repair 0 the repair update drops the game's jobs of a car that may not leave.
    garage_repair = 1;
    car_dead = true;
    repair_label(true);
    swofty::log("[td-probe] garage repair: your car is in its garage with damaged suspension (corners 0x%x%s), the "
                "team repairs it: %.0f s", corners, crack ? ", a crack" : "", double(seconds));
    return true;
}

// The garage repair job's name: the game's "detached wheel" job (lng_ft_detached_wheel) also carries the suspension
// repairs, so it reads "Suspension" while one runs (core v8 text overrides; without them the game's name stays).
void repair_label(bool suspension)
{
    static std::atomic<bool> shown{false};
    if (shown.exchange(suspension) == suspension || !swofty::lang::available())
        return;
    auto r = suspension ? swofty::lang::set("lng_ft_detached_wheel", "Suspension")
                        : swofty::lang::clear("lng_ft_detached_wheel");
    swofty::log("[td-probe] garage repair: the repair job's name %s (%s)",
                suspension ? "reads Suspension" : "is the game's again", swofty::lang::name(r));
}

void hook_garage_crash_repair()
{
    repair_update_hooked =
        detour("RepairTimes update",
               "56 57 53 48 83 EC 70 C5 78 29 4C 24 60 C5 78 29 44 24 50 C5 F8 29 7C 24 40 C5 F8 29 74 24 30 48 89 CE "
               "48 8B 41 70 48 8B 48 20 48 8B 89 F0 00 00",
               repair_update_prologue, sizeof repair_update_prologue, reinterpret_cast<void *>(&on_repair_update),
               reinterpret_cast<void **>(&orig_repair_update)) &&
        detour("RepairTimes job finished",
               "56 48 83 EC 30 48 8B 41 08 4C 8B 40 30 0F B6 71 16 49 39 B0 80 01 00 00 0F 86 84 00 00 00 49 83 BC F0 "
               "B0 00 00 00 00 74 79 8D 4A FD 83 F9 0E 73",
               repair_done_prologue, sizeof repair_done_prologue, reinterpret_cast<void *>(&on_repair_done),
               reinterpret_cast<void **>(&orig_repair_done));
    if (!repair_update_hooked)
        swofty::log("[td-probe] garage repair: not available, a limping car stays in its garage");
}

// Practice/qualifying: a car that cannot be repaired in this session (crash damage, a plain retirement) or whose
// corner is still being rebuilt may not leave its garage at all. The garage menu switches its drive out buttons
// off (on_garage_vm_update); a drive out request that still comes (a practice programme start, 0.51.125) is passed
// on and the car comes straight back (on_exit_garage). A failed power unit or gearbox keeps the old path: the part
// swap only shows on a send-out attempt.
bool garage_exit_locked()
{
    return car_dead && keep_control && !session_ended &&
           (garage_withdrawn.load() || (!(terminal_mask.load() & 0x7) && !garage_part_failed) || part_lock.load());
}

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
bool garage_menu_hooked = false;
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
    garage_menu_hooked = detour_at("garage menu update", at, garage_vm_update_prologue, sizeof garage_vm_update_prologue,
                                   reinterpret_cast<void *>(&on_garage_vm_update),
                                   reinterpret_cast<void **>(&orig_garage_vm_update));
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

// vcCommandSelected registration resolves to this callback on the reference EXE
// (0x142f79e40). Its first 17 bytes are whole, RIP-independent instructions.
// Forward normal commands unchanged; the custom command queues retirement on frame.
constexpr const char *sig_radio_selected =
    "56 57 55 53 48 83 EC 28 89 D7 48 89 CE 48 8B 59 40 48 8B 03 48 89 D9 FF 90 90 00 00 00";
constexpr uint8_t radio_selected_prologue[] = {
    0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xec, 0x28, 0x89, 0xd7, 0x48, 0x89, 0xce, 0x48, 0x8b, 0x59, 0x40};
using radio_selected_fn = uint64_t (*)(void *, uint32_t);
radio_selected_fn orig_radio_selected;
// The existing Weather Report id matches the game's DJB2 keyword-name hash at
// runtime; the custom id was confirmed with the visible entry in 0.32.12.
constexpr uint32_t radio_retire_probe_id = 1984770275u; // Terminal Damage Retire Probe

// Inspect the launcher's deployed XML, not the package/store copy. This is a
// bounded read-only check; the original file is managed by the launcher's file.put.
int radio_probe_xml_check(FILE *file)
{
    if (!file)
        return -1;
    std::vector<char> data(256 * 1024);
    size_t bytes = fread(data.data(), 1, data.size(), file);
    if (ferror(file))
        return -1;
    return std::string_view(data.data(), bytes).find("name=\"Terminal Damage Retire Probe\"") !=
                   std::string_view::npos ? 1 : 0;
}

void log_radio_probe_xml()
{
    wchar_t executable[MAX_PATH];
    DWORD size = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    int present = -1;
    if (size && size < MAX_PATH) {
        if (auto slash = std::wcsrchr(executable, L'\\')) {
            std::wstring path(executable, slash + 1);
            path += L"audio\\2025\\speech_recognition.xml";
            FILE *file = _wfopen(path.c_str(), L"rb");
            present = radio_probe_xml_check(file);
            if (file)
                fclose(file);
        }
    }
    swofty::log("[td-probe] radio probe: launcher XML contains test command=%s",
                present < 0 ? "unreadable" : present ? "yes" : "no");
}

void register_radio_menu_probe()
{
    // The launcher file operation remains present even with the native action
    // disabled; keep its label readable in that case too.
    log_radio_probe_xml();
    auto assets = swofty::assets::detail::table();
    if (!assets || !assets->lang_set) {
        swofty::log("[td-probe] radio probe: language API unavailable; custom label not registered");
        return;
    }
    auto label = swofty::assets::lang_set("*", "lng_td_retire_probe", "Retire");
    swofty::log("[td-probe] radio retire: launcher-managed command file, label %s; command id=%u",
                swofty::assets::name(label), radio_retire_probe_id);
    swofty::log("[td-probe] radio retire: engineer reply controlled by Retirement radio voice (restart required)");
}

uint64_t on_radio_selected(void *module, uint32_t command)
{
    uint64_t result = orig_radio_selected(module, command);
    if (command == radio_retire_probe_id && radio_command_probe && !session_ended) {
        swofty::log("[td-probe] radio retire: selected id=%u; queued request (original callback forwarded)", command);
        pending_radio_command = (1ull << 32) | command;
    }
    return result;
}

bool take_radio_command(uint32_t &command)
{
    uint64_t pending = pending_radio_command.exchange(0);
    if (!pending)
        return false;
    command = uint32_t(pending);
    return true;
}

void request_retire(const char *source);

void process_radio_command()
{
    uint32_t command;
    if (!take_radio_command(command) || command != radio_retire_probe_id || !radio_command_probe || session_ended)
        return;
    if (!keep_control || player_vehicle.load() < 0 || player_pit.load() != 1 || retire_in.load() != -1.0) {
        swofty::log("[td-probe] radio retire: ignored (keep control %d, vehicle %d, pit %d, retire timer %.1f)",
                    bool(keep_control), player_vehicle.load(), player_pit.load(), retire_in.load());
        return;
    }
    if (player_broken) {
        // Match the pause menu: another menu selection neither cancels nor shortcuts retirement.
        swofty::log("[td-probe] radio retire again: still pull over or box");
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT", "Pull over and stop, or box", 4);
        return;
    }
    request_retire("radio menu retire");
}

void hook_radio_probe()
{
    if (!radio_command_probe)
        return;
    detour("radio menu command selection", sig_radio_selected, radio_selected_prologue,
           sizeof radio_selected_prologue, reinterpret_cast<void *>(&on_radio_selected),
           reinterpret_cast<void **>(&orig_radio_selected));
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
    if (!guarded_read(record + offset, ptr))
        return "<unreadable pointer>";
    if (!ptr)
        return "<null>";
    char first = 0;
    if (guarded_read(ptr, first) && !first)
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
        if (!vec || !guarded_read(vec + 8, first) || !guarded_read(vec + 0x10, last) ||
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
        guarded_read(at, state);
        guarded_read(at + 0x60, selection);
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
        if (guarded_read(site + 3, rel))
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

// Reference 144664348: isolate ONLY the chassis-plane projection/velocity
// rewrite. Joint creation, radial limit, mesh and attached suspension stay stock.
// No callback, allocation, logging or object mutation in the update hot path.
void emit_tether_plane_stub(emitter &e, uintptr_t stock, uintptr_t skip, uintptr_t vt,
                            uintptr_t original_skip=0, bool load_owner=false)
{
    e.bytes({0x0f,0x86}); auto *already_skip=e.p; e.u32(0); // original jbe
    e.bytes({0x9c,0x50,0x52}); // preserve flags, rax, rdx
    if (load_owner) e.bytes({0x53,0x48,0x8b,0x5e,0x10}); // preserve rbx, live owner
    std::vector<uint8_t *> failures;
    auto fail=[&](uint8_t condition) { e.bytes({0x0f,condition}); failures.push_back(e.p); e.u32(0); };
    e.bytes({0x48,0xba}); e.u64(vt);
    e.bytes({0x48,0x39,0x16}); fail(0x85); // wheel vtable
    e.bytes({0x48,0x39,0x5e,0x10}); fail(0x85); // live owner == rbx
    e.bytes({0x48,0xba}); e.u64(reinterpret_cast<uintptr_t>(&throttle_hook));
    e.bytes({0x48,0x3b,0x1a}); fail(0x85); // current player car
    e.bytes({0x48,0xba}); e.u64(reinterpret_cast<uintptr_t>(&session_ended));
    e.bytes({0x80,0x3a,0x00}); fail(0x85);
    e.bytes({0x48,0xba}); e.u64(reinterpret_cast<uintptr_t>(&player_vehicle));
    e.bytes({0x8b,0x02,0x83,0xf8,0x20}); fail(0x83); // unsigned ID <32, rejects -1
    e.bytes({0x0f,0xb6,0x93,0xe8,0x00,0x00,0x00,0x39,0xd0}); fail(0x85);
    if (load_owner) e.bytes({0x5b});
    e.bytes({0x5a,0x58,0x9d}); e.jmp(skip);
    auto *stock_label=e.p;
    if (load_owner) e.bytes({0x5b});
    e.bytes({0x5a,0x58,0x9d}); e.jmp(stock);
    auto *skip_label=e.p; e.jmp(original_skip ? original_skip : skip);
    auto relative=[](uint8_t *field,uint8_t *target) {
        int32_t displacement=int32_t(target-(field+4)); memcpy(field,&displacement,4);
    };
    relative(already_skip,skip_label);
    for (auto *field:failures) relative(field,stock_label);
}

void hook_tether_distance_experiment()
{
    if (!tether_distance_experiment) return;
    if (wheel_tether_test) {
        swofty::log("[td-probe] tether distance experiment: disable Remove player wheel tethers first; experiment not installed");
        return;
    }
    constexpr const char *sig=
        "C5 F8 2E DC 0F 86 9F 01 00 00 C5 DA 5E CB C5 B0 5C D0 C5 F0 C6 C9 00 C5 E8 59 C9 C5 78 58 C9";
    uintptr_t site=0;
    if (swofty::api->scan(".xtls",sig,&site)!=1 || !site) {
        swofty::log("[td-probe] tether distance experiment: signature unavailable; stock retained");return;
    }
    uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t vt=base+0x5bf9420,update=0;
    const uint8_t branch[]={0x0f,0x86,0x9f,0x01,0x00,0x00};
    uintptr_t at=site+4;
    if (!guarded_read(vt+0x68,update) || update!=base+0x4664080 ||
        memcmp(reinterpret_cast<void *>(at),branch,sizeof branch)) return;
    auto *stub=alloc_near(at);if (!stub) return;
    emitter e{stub};
    // Original under-limit branch retains anchor/length updates at ca5.
    // Over-limit experiment rejoins at the SAME continuation used by the
    // stock hard clamp (ca0 -> 5387), not with its overwritten SIMD temporaries.
    emit_tether_plane_stub(e,at+6,base+0x4665387,vt,at+6+0x19f,true);
    bool installed=jump_to_stub(at,6,stub,e.p);
    swofty::log("[td-probe] tether distance experiment: installed=%d player only; joint and break cutoff retained; restart required",int(installed));
}

void hook_tether_plane_experiment()
{
    if (!tether_plane_experiment) return;
    if (wheel_tether_test) {
        swofty::log("[td-probe] tether plane experiment: disable Remove player wheel tethers first; experiment not installed");
        return;
    }
    constexpr const char *sig=
        "C5 F2 59 96 F0 00 00 00 C4 41 10 57 ED C4 C1 78 2E D5 0F 86 03 01 00 00 C4 C3 31 0C D4 08";
    uintptr_t site=0;
    if (swofty::api->scan(".xtls",sig,&site)!=1 || !site) {
        swofty::log("[td-probe] tether plane experiment: signature unavailable; stock retained"); return;
    }
    uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t vt=base+0x5bf9420, update=0;
    // The signature spans 144664336..353; exact branch and vtable validation.
    uintptr_t at=site+0x12;
    const uint8_t branch[]={0x0f,0x86,0x03,0x01,0x00,0x00};
    if (!guarded_read(vt+0x68,update) || update!=base+0x4664080 ||
        memcmp(reinterpret_cast<void *>(at),branch,sizeof branch)) return;
    auto *stub=alloc_near(at);
    if (!stub) return;
    emitter e{stub}; emit_tether_plane_stub(e,at+6,at+6+0x103,vt);
    bool installed=jump_to_stub(at,6,stub,e.p);
    swofty::log("[td-probe] tether plane experiment: installed=%d player only; physical tether retained; restart required",int(installed));
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
        if (strncmp(nm, "State", 5) || !guarded_read(base + i * crew_state_size + crew_state_slot, fn) || !fn) {
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
        guarded_read(reinterpret_cast<uintptr_t>(system) + 0x68 + uintptr_t(id) * 16, entry) && entry &&
        guarded_read(entry + 4, from) && from == 9 && to != 9) {
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
        !guarded_read(reinterpret_cast<uintptr_t>(system) + 0x68 + uintptr_t(id) * 16, entry) || !entry ||
        !guarded_read(entry + 4, from))
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
    if (!crew_event_vtable || vehicle < 0 || !guarded_read(bus_holder, holder) || !holder ||
        !guarded_read(holder + 8, bus) || !bus)
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
    if (!crew_msg_vtable || vehicle < 0 || !guarded_read(bus_holder, holder) || !holder ||
        !guarded_read(holder + 8, bus) || !bus)
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

void latch_retirement_drive(int vehicle)
{
    if (vehicle<0 || vehicle>=max_vehicles) return;
    retired_drive_lock_vehicle=vehicle;
    throttle_hook.cap=0.0f;
}

// Sess_Result_ChangeVehicleStatus 0x142c7bae0 (result in rcx; u8 vehicle +0, status +0x18, reason +0x19, pending
// request status +0x11d, reason +0x11e, pending flag +0x11f) commits the pending request. At the session end the race
// ruleset (0x142d2b7b0, loop at 0x142d2c26c) requests DNF for every car short of the classification distance, without
// checking for a disqualification, so the player's DSQ became DNF (0.51.64/0.51.65 tests). Keep a disqualified player
// car disqualified: turn that pending DNF back into DSQ.
using change_vehicle_status_fn = uint64_t (*)(void *, double, float, uint8_t, uint32_t, uint32_t);
change_vehicle_status_fn orig_change_vehicle_status = nullptr;
uint64_t on_change_vehicle_status(void *result, double a, float b, uint8_t c, uint32_t d, uint32_t e)
{
    uintptr_t r = reinterpret_cast<uintptr_t>(result);
    uint8_t vehicle = 0xff, status = 0, pending = 0, flag = 0;
    if (r && guarded_read(r, vehicle) && int(vehicle) == player_vehicle.load() &&
        (player_controller_flags.load() & (1u << 2)) && guarded_read(r + 0x18, status) && status == 5 &&
        guarded_read(r + 0x11f, flag) && flag && guarded_read(r + 0x11d, pending) && pending == 4) {
        *reinterpret_cast<volatile uint8_t *>(r + 0x11d) = 5;
        swofty::log("[td-probe] result: kept vehicle %u disqualified (the game asked for DNF)", vehicle);
    }
    return orig_change_vehicle_status(result, a, b, c, d, e);
}
bool hook_change_vehicle_status()
{
    constexpr uint8_t prologue[] = {0x56, 0x48, 0x83, 0xEC, 0x60, 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x50,
                                    0xC5, 0xF9, 0x6F, 0xF1};
    return detour("Sess_Result_ChangeVehicleStatus",
                  "56 48 83 EC 60 C5 F8 29 74 24 50 C5 F9 6F F1 48 89 CE 0F B6 41 18 3C 02 75 69 C5 F9 6E 84 24 98 00 00 00",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_change_vehicle_status),
                  reinterpret_cast<void **>(&orig_change_vehicle_status));
}

// MessageVehicleStatusRequest {vtable, 0, u8 vehicle, u8 status, u8 reason, u8 apply}: the session result handler
// (0x14e716f20, "onStatusRequest() for Vehicle %d") passes it to Sess_Result_ChangeVehicleStatus 0x142c7bae0, which
// sets result+0x18/+0x19 and posts the status-changed message. Status (0x14565c1f8): 2 active, 3 finished, 4 DNF,
// 5 disqualified, 6 not classified, 7 retired. Reason (0x1456e6a28): 1 retired, 3 terminal damage, 6 black flagged,
// 8 canned event failure. A request for the current status is ignored.
bool post_vehicle_status(int vehicle, uint32_t status, uint32_t reason)
{
    uintptr_t holder = 0, bus = 0;
    if (!status_request_vtable || vehicle < 0 || !guarded_read(bus_holder, holder) || !holder ||
        !guarded_read(holder + 8, bus) || !bus)
        return false;
    struct {
        const void *vtable;
        uint64_t zero;
        uint32_t packed;
        uint8_t pad[0x2c];
    } msg{};
    msg.vtable = status_request_vtable;
    msg.packed = uint32_t(vehicle) | status << 8 | reason << 16 | 1u << 24;
    swofty::log("[td-probe] status request: vehicle %d status %u reason %u (0x%08x)", vehicle, status, reason, msg.packed);
    swofty::game::Ne_Post_BusMessage(reinterpret_cast<void *>(bus), &msg);
    return true;
}

// What the pause menu's Retire does, for the player's car.
void retire_player(bool after_dsq = false)
{
    int vehicle = player_vehicle;
    uintptr_t holder = 0, bus = 0;
    if (!status_request_vtable || vehicle < 0 || !guarded_read(bus_holder, holder) || !holder ||
        !guarded_read(holder + 8, bus) || !bus) {
        swofty::log("[td-probe] retire: not possible (vehicle %d)", vehicle);
        return;
    }
    // A disqualified car stays DSQ; a retirement would overwrite the result with DNF.
    if (!after_dsq && (player_controller_flags.load() & (1u << 2))) {
        swofty::log("[td-probe] retire: skipped, vehicle %d is disqualified", vehicle);
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
    latch_retirement_drive(vehicle);
    own_retire_until = GetTickCount64() + 10000;
    // Posting retirement can synchronously deliver NOT_IN_SESSION. Arm the
    // shutdown first; lifecycle cleanup must not restart the DNF car's audio.
    if (engine_off_sound) {
        retired_engine_off_vehicle = vehicle;
        engine_off_vehicle = vehicle;
        swofty::log("[td-probe] engine: keeping vehicle %d off through retirement", vehicle);
    }
    swofty::log("[td-probe] retire: posting MessageVehicleStatusRequest 0x%08x", msg.packed);
    swofty::game::Ne_Post_BusMessage(reinterpret_cast<void *>(bus), &msg);
}

// Red flag: the game's RedFlagRaceStrategy state (Enter 0x142dfbf80, Update 0x142dfcdc0, `this` in rcx) shows the
// pit stop editor where the team repairs and changes tyres. The close callback 0x142dfd2b0 sets state+0xdb1, which
// Update tests. What the team may do follows the FIA Sporting Regulations, B5.14.4 a: during a suspension only the
// listed work is allowed. Item viii is the repair of genuine accident damage including the replacement of
// assemblies; power unit or gearbox work is not listed. So:
//  - mechanical damage (mask bits 0-2): popup, no editor, retire.
//  - lost wheel / broken suspension (bits 3-4): the car limps on only with one broken corner (decide_limp, a
//    dice roll). A car that could not be driven was abandoned on the circuit (B1.8.9): popup, no editor, retire.
//    A limping car is accident damage the team may repair: a second dice roll (80 %, lost wheel 50 %) decides whether
//    it is done in time. Yes: the editor runs; when it closes, the addon rebuilds the corner (and the front wing) the
//    way the game's multiplayer damage sync does (repair_crash_damage). No: popup, no editor, retire.
//  - other accident damage (tyres, aero, bodywork): the editor runs and the game's own red flag repair (loop at
//    0x1435847da, vehicle status 3 "Repaired") restores it.
// Afterwards the addon reads the game's terminal damage record (read_terminal_record): no longer terminal, the car
// takes part in the restart; still terminal 20 s after the editor closed, it retires.
void (*orig_red_flag_strategy_enter)(void *) = nullptr;
std::atomic<uintptr_t> red_flag_state{0};
std::atomic<uint64_t> red_flag_toast_at{0};
std::atomic<bool> red_flag_screen_open{false};
std::atomic<int> red_flag_verdict{0};               // 1 mechanical, 2 corner repair failed, 3 abandoned
std::atomic<uint64_t> red_flag_repair_deadline{0};  // repair wait: retire if still terminal after this tick
std::atomic<bool> red_flag_fix_pending{false};      // corner/front wing repair due when the editor closes
void red_flag_toast_tick()
{
    uint8_t closed = 0;
    uintptr_t st = red_flag_state.load();
    if (red_flag_screen_open.load() && st && guarded_read(st + 0xdb1, closed) && closed &&
        red_flag_screen_open.exchange(false)) {
        swofty::log("[td-probe] red flag: strategy screen closed, retiring your car");
        red_flag_toast_at = GetTickCount64() + 3000;
        retire_player();
    }
    if (red_flag_repaired_toast.exchange(false) && swofty::hud::available())
        swofty::hud::toast("CAR REPAIRED", "Your team repaired the accident damage. You take part in the restart.", 8);
    if (red_flag_repair_wait.load()) {
        uint64_t now = GetTickCount64();
        // The editor closed: the team works now. Give the repairs 20 s.
        if (st && guarded_read(st + 0xdb1, closed) && closed && red_flag_repair_deadline.load() > now + 20000) {
            red_flag_repair_deadline = now + 20000;
            if (red_flag_fix_pending.exchange(false))
                red_flag_fix_now = true;
            swofty::log("[td-probe] red flag: strategy screen closed, waiting for the repairs");
        }
        if (confirmed_retired_vehicle.load() >= 0) {
            red_flag_repair_wait = false;
        } else if (now >= red_flag_repair_deadline.load() && red_flag_repair_wait.exchange(false)) {
            swofty::log("[td-probe] red flag: still terminal after the repairs (state %d, parts 0x%x), retiring your car",
                        live_terminal_state.load(), live_terminal_mask.load());
            retire_player();
            if (swofty::hud::available())
                swofty::hud::toast("REPAIRS NOT COMPLETED",
                                   "The damage could not be repaired before the restart. You will not take part.", 10);
        }
    }
    uint64_t at = red_flag_toast_at.load();
    if (!at || GetTickCount64() < at)
        return;
    red_flag_toast_at = 0;
    const char *title = "CAR SEVERELY DAMAGED";
    const char *text = "Your car was severely damaged and cannot be repaired. You will not take part in the restart.";
    if (red_flag_verdict.load() == 1) {
        title = "POWER UNIT FAILURE";
        text = "No power unit or gearbox work is allowed during a red flag, so your team withdraws the car. "
               "You will not take part in the restart.";
    } else if (red_flag_verdict.load() == 2) {
        text = "Your team cannot rebuild the broken corner before the restart. You will not take part in the restart.";
    } else if (red_flag_verdict.load() == 3) {
        title = "CAR ABANDONED ON TRACK";
        text = "Your car could not be driven on. A car abandoned on the circuit is withdrawn from the race.";
    }
    if (red_flag_screen_open.load() && swofty::popup::available()) {
        auto r = swofty::popup::show(title, text);
        swofty::log("[td-probe] red flag: popup %s", swofty::popup::name(r));
        if (r == swofty::popup::Result::ok) {
            // Skip the pit stop editor: what its close callback (0x142dfd2b0) does. The next tick retires the car.
            uintptr_t st = red_flag_state.load();
            if (st && swofty::mem::readable(st + 0xdb1, 1)) {
                *reinterpret_cast<volatile uint8_t *>(st + 0xdb1) = 1;
                swofty::log("[td-probe] red flag: pit stop editor skipped");
            }
            return;
        }
    }
    if (swofty::hud::available())
        swofty::hud::toast(title, text, 10);
}
void on_red_flag_strategy_enter(void *state)
{
    // If the game enters this state again and again, stay out of the way: no logging, no probing.
    static uint64_t last_enter = 0;
    uint64_t now_enter = GetTickCount64();
    bool repeat = now_enter - last_enter < 1000;
    last_enter = now_enter;
    if (repeat) {
        orig_red_flag_strategy_enter(state);
        return;
    }
    // The game sends its terminal damage message only after a delay: a crash just before the red flag is already in
    // the record but not yet known to the addon. Take it from the record.
    if (keep_control && !player_broken && live_terminal_state.load() == 5 && live_terminal_mask.load()) {
        terminal_mask.fetch_or(live_terminal_mask.load());
        player_broken = true;
        decide_limp(throttle_hook.car);
        swofty::log("[td-probe] red flag: terminal damage 0x%x from the game's record (message still pending)",
                    live_terminal_mask.load());
    }
    uint32_t mask = terminal_mask.load();
    int corners = damaged_corners(throttle_hook.car);
    swofty::log("[td-probe] red flag: strategy screen opens (player vehicle %d, broken %d, terminal mask 0x%x, "
                "game record state %d parts 0x%x, damaged corners %d, pit state at the suspension %d)",
                player_vehicle.load(), int(player_broken.load()), mask, live_terminal_state.load(),
                live_terminal_mask.load(), corners, pit_at_suspension.load());
    orig_red_flag_strategy_enter(state);
    red_flag_state = reinterpret_cast<uintptr_t>(state);
    if (!red_flag_retire || !keep_control || !player_broken || !mask || confirmed_retired_vehicle.load() >= 0)
        return;
    if (retire_in.load() >= 0) {
        swofty::log("[td-probe] red flag: pending retirement timer %.1f s dropped, the red flag rules decide",
                    retire_in.load());
        retire_in = -1.0;
    }
    bool corner_damage = mask & 0x18;
    int verdict = 0;
    unsigned chance = 0, roll = 0;
    if (mask & 0x7) {
        verdict = 1;
    } else if (corner_damage && limp_state.load() != 1) {
        verdict = 3;  // could not be driven: abandoned on the circuit
    } else if (corner_damage) {
        // The car crawled on: whether the team can rebuild the corner in time is a dice roll; a lost wheel is harder.
        chance = ((crash_corners.load() & 0xfff) && corners == 1 && crash_repair_available())
                     ? (has_detached_corner(crash_corners.load()) ? 50 : 80) : 0;
        roll = forced_roll(debug_repair_roll, chance, roll_percent());
        if (roll >= chance)
            verdict = 2;
        swofty::log("[td-probe] red flag: corner repair chance %u%%, roll %u -> %s", chance, roll,
                    verdict ? "not repairable in time" : "the team repairs it");
    }
    if (verdict) {
        if (red_flag_screen_open.exchange(true))
            return;
        red_flag_verdict = verdict;
        swofty::log("[td-probe] red flag: %s; your car retires when you close the screen",
                    verdict == 1 ? "mechanical failure, not repairable under a red flag (FIA B5.14.4 a)" :
                    verdict == 2 ? "broken corner, the team cannot repair it before the restart" :
                                   "broken corner, the car could not be driven: abandoned on the circuit (FIA B1.8.9)");
        red_flag_toast_at = GetTickCount64() + 1500;
        return;
    }
    // Accident damage the team may repair (FIA B5.14.4 a viii): the editor runs as usual. Aero or body damage only
    // counts wherever the car was: it keeps its drive since 0.51.126 (it used to roll out, and on track at the
    // suspension it was taken as abandoned).
    red_flag_fix_pending = corner_damage || (mask & 0x40);
    red_flag_repair_deadline = GetTickCount64() + 600000;
    red_flag_repair_wait = true;
    swofty::log("[td-probe] red flag: accident damage 0x%x may be repaired (%s); waiting for the editor to close", mask,
                red_flag_fix_pending.load() ? "the addon replaces the corner / front wing" : "the game's repairs");
}
bool hook_red_flag_strategy()
{
    constexpr uint8_t enter_prologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
                                          0x55, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x89, 0xCE};
    bool ok = detour("RedFlagRaceStrategy::enter",
                     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 48 89 CE C6 81 B2 0D 00 00 01 80 B9 B0 0D 00 00 00",
                     enter_prologue, sizeof enter_prologue, reinterpret_cast<void *>(&on_red_flag_strategy_enter),
                     reinterpret_cast<void **>(&orig_red_flag_strategy_enter));
    return ok;
}

// The game's AI failure planner (0x1409b2940, `this` in rcx) announces a planned breakdown (radio message) and writes
// the part to fail into the car's failure object (+0xa0). Remember the player's part: when the game then hands the car
// to the AI (RETIRED), the addon breaks that part itself instead.
void (*orig_failure_planner)(void *) = nullptr;
void on_failure_planner(void *self)
{
    uintptr_t a = reinterpret_cast<uintptr_t>(self);
    uint64_t car = 0;
    uint8_t part = 0xff;
    if (a && guarded_read(a + 0x70, car) && car && car == uint64_t(player_car()) && guarded_read(a + 0xa0, part) &&
        (failure_planner_obj = a, game_planned_part.exchange(int(part)) != int(part)))
        swofty::log("[td-probe] player failure planned by the game: part %u (0 ICE,1 MGU-K,2 MGU-H,3 ES,4 TC,5 CE,6 gearbox)", part);
    orig_failure_planner(self);
}
bool hook_failure_planner()
{
    constexpr uint8_t prologue[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x40, 0xC5, 0xF9, 0x29, 0x74, 0x24, 0x30, 0x48, 0x89, 0xCE};
    return detour("AI failure planner", "56 57 53 48 83 EC 40 C5 F9 29 74 24 30 48 89 CE 48 8B 81 98 00 00 00 48 8B 78 58 80 BF 98 00 00 00 00 74",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_failure_planner),
                  reinterpret_cast<void **>(&orig_failure_planner));
}

// The failure scheduler (0x1409b9aa0, `this` in rcx) runs once per session and rolls, per car, whether and which part
// fails. Only the experimental Grand Prix test modes change anything here.
void (*orig_failure_scheduler)(void *) = nullptr;
int choice_int(std::string_view v, int fallback);
// The game's race length (re/game-failure-scheduler.md, code 0x1409b9d86): settings = [[[mgr+8]+0x10]+0x1a8], byte +8
// the race-length index (0-12, the scheduler's tables are indexed by it), dword +0xdf0 ^ +0xdf8 the lap count N the
// game plans its failures over (laps 2..N). Logged at each session start. 0.51.122 log: N 5 in a 5-lap race (the
// car crossed the line 5 times), so race_total_laps uses it when Swofty's telemetry is off and its session context
// has 0 laps for a race (0.51.120 log). The session end clears it; the next session's scheduler call sets it again.
std::atomic<int> game_race_laps{-1}, game_race_length{-1};
void log_race_settings(uint64_t mgr)
{
    uint64_t a = 0, b = 0, settings = 0;
    uint8_t length = 0xff;
    uint32_t n0 = 0, n1 = 0;
    if (!mgr || !guarded_read(mgr + 8, a) || !a || !guarded_read(a + 0x10, b) || !b ||
        !guarded_read(b + 0x1a8, settings) || !settings || !guarded_read(settings + 8, length) ||
        !guarded_read(settings + 0xdf0, n0) || !guarded_read(settings + 0xdf8, n1)) {
        swofty::log("[td-probe] race settings: not readable (manager %p)", reinterpret_cast<void *>(mgr));
        return;
    }
    game_race_laps = int(n0 ^ n1);
    game_race_length = int(length);
    swofty::log("[td-probe] race settings: length index %u, the game's lap count %u (the laps it plans failures over)",
                unsigned(length), n0 ^ n1);
}

void on_failure_scheduler(void *self)
{
    std::string_view mode = gp_failures;
    {
        // Experimental "native" mode: the scheduler rolls three Mersenne Twister numbers per car at session start; the
        // first one (stored at entry+8) is the threshold the failure chance has to beat. Storing 0.0 instead of the
        // roll (vmovss [rbx+8],xmm0 -> xmm8, which is zero) makes the game's own scheduler register a failure.
        // Both sites are fixed addresses of this build: check the whole instruction (C5 FA 11 43 08 vmovss [rbx+8],
        // xmm0; 0F 86 C0 FE FF FF jbe), so another build's code is never written.
        static uint8_t *patch = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x9b9d27);
        bool want = mode == "native";
        DWORD old = 0;
        bool vmovss = patch[-1] == 0xC5 && patch[1] == 0x11 && patch[2] == 0x43 && patch[3] == 0x08;
        if (vmovss && patch[0] == (want ? 0xFA : 0x7A) && VirtualProtect(patch, 1, PAGE_EXECUTE_READWRITE, &old)) {
            patch[0] = want ? 0x7A : 0xFA;
            VirtualProtect(patch, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), patch, 1);
            swofty::log("[td-probe] failure scheduler: threshold roll %s", want ? "forced to 0 (native test)" : "restored");
        }
        // Also skip the "chance > threshold" branch (jbe 0x1409bb3bf) so a record is registered even when the chance is 0
        // at race start (no race-length data in Grand Prix).
        static uint8_t *branch = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x9bb4f9);
        static uint8_t saved[6] = {};
        static bool have_saved = false;
        constexpr uint8_t jbe[6] = {0x0F, 0x86, 0xC0, 0xFE, 0xFF, 0xFF};
        if (!have_saved && !memcmp(branch, jbe, sizeof jbe)) {
            memcpy(saved, branch, 6);
            have_saved = true;
        }
        if (have_saved && want != (branch[0] == 0x90) && VirtualProtect(branch, 6, PAGE_EXECUTE_READWRITE, &old)) {
            if (want) memset(branch, 0x90, 6); else memcpy(branch, saved, 6);
            FlushInstructionCache(GetCurrentProcess(), branch, 6);
            VirtualProtect(branch, 6, old, &old);
            swofty::log("[td-probe] failure scheduler: chance check %s", want ? "skipped (native test)" : "restored");
        }
    }
    orig_failure_scheduler(self);
    {
        uint64_t mgr = 0;
        if (guarded_read(reinterpret_cast<uintptr_t>(self) + 0xf0, mgr) && mgr)
            failure_mgr = mgr;
        log_race_settings(mgr);
        // Realistic failures (not Career): the season's rates replace the failures the game rolled from its own
        // database for every car (part 7 = none); gp_ai_failure_tick plans them instead.
        swofty::career::State st{};
        bool career = swofty::career::state(&st) == swofty::career::Result::ok && st.loaded;
        if (mode == "realistic" && !career) {
            int cleared = 0;
            for_each_failure_obj([&](int, uintptr_t obj) {
                uint8_t part = 7, state = 0;
                if (guarded_read(obj + 0xa0, part) && part < 7 && guarded_read(obj + 0xa1, state) && state == 0) {
                    *reinterpret_cast<volatile uint8_t *>(obj + 0xa0) = 7;
                    ++cleared;
                }
            });
            swofty::log("[td-probe] failure scheduler: realistic failures, %d failure(s) the game rolled removed",
                        cleared);
        }
    }
    if (self && mode == "breakdown") {
        // Experimental: arm the game's own breakdown state machine for the player's car. The scheduler normally
        // fills these per-car object fields (+0xa0 part, +0xa1 state, +0xa4 lap, +0xa8 lap fraction, +0xb1 branch)
        // when a breakdown was rolled; in Grand Prix none is, so write them here and let the game run its planner
        // (state 0 -> 1 -> 2: announce, pull over, hand the car to the AI).
        uintptr_t a = reinterpret_cast<uintptr_t>(self);
        uint64_t mgr = 0, begin = 0, end = 0;
        guarded_read(a + 0xf0, mgr);
        if (mgr) { guarded_read(mgr + 0x198, begin); guarded_read(mgr + 0x1a0, end); }
        int player = player_vehicle.load();
        for (uint64_t e = begin; e && e < end && player >= 0; e += 0x10) {
            uint64_t obj = 0;
            uint8_t idx = 0xff;
            guarded_read(e + 8, obj);
            if (!obj || !guarded_read(obj + 0x78, idx) || idx != uint8_t(player))
                continue;
            uint8_t part = 0;
            std::string_view ft = gp_fault_type;
            part = ft == "random" ? uint8_t((GetTickCount64() / 17) % 6) : uint8_t(choice_int(ft, 0) % 6);
            uint32_t lap = 1;
            float frac = 0.30f;
            uint8_t flag88 = 0xff, oa0 = 0xff, oa1 = 0xff;
            uint32_t oa4 = 0;
            guarded_read(obj + 0x88, flag88);
            guarded_read(obj + 0xa0, oa0);
            guarded_read(obj + 0xa1, oa1);
            guarded_read(obj + 0xa4, oa4);
            auto *o = reinterpret_cast<uint8_t *>(obj);
            DWORD old = 0;
            if (VirtualProtect(o + 0xa0, 0x20, PAGE_READWRITE, &old)) {
                o[0xa0] = part;
                o[0xa1] = 0;
                memcpy(o + 0xa4, &lap, 4);
                memcpy(o + 0xa8, &frac, 4);
                memset(o + 0xac, 0, 4);
                o[0xb0] = 0;
                o[0xb1] = 0;
                VirtualProtect(o + 0xa0, 0x20, old, &old);
            }
            swofty::log("[td-probe] breakdown armed: object %llx car %u, +88 %u, part %u -> %u, state %u -> 0, lap %u -> 1, fraction 0.30",
                        (unsigned long long)obj, unsigned(idx), unsigned(flag88), unsigned(oa0), unsigned(part), unsigned(oa1), oa4);
        }
    }
}
bool hook_failure_scheduler()
{
    constexpr uint8_t prologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0x28, 0x02, 0x00, 0x00};
    return detour("AI failure scheduler", "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 28 02 00 00 C5 78 29 BC 24 10 02 00 00 C5 78 29 B4 24 00 02 00 00 C5 78 29 AC 24 F0 01 00 00 C5 78 29 A4 24 E0 01 00 00 C5 78 29 9C 24 D0 01 00 00 C5 78 29 94 24 C0 01 00 00 C5 78 29 8C 24 B0 01 00 00 C5 78 29 84 24 A0 01 00 00 C5 F8 29 BC 24 90 01 00 00 C5 F8 29 B4 24 80 01 00 00 80 B9 09 01 00 00 00 0F 84 63 21 00 00 48 89 CF 48 8B 41",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_failure_scheduler),
                  reinterpret_cast<void **>(&orig_failure_scheduler));
}

// Probe (0.51.38): failure record registration (0x1409b37a0, rcx = the car's failure object, rdx = 0x34-byte record).
void *on_failure_register(void *list, const void *rec)
{
    if (std::string_view(gp_failures) == "native" && rec) {
        uint32_t w[2] = {};
        for (int i = 0; i < 2; i++) guarded_read(reinterpret_cast<uintptr_t>(rec) + i * 4, w[i]);
        // Record layout (0x34 bytes): +0 type, +4 lap the fault starts in (rolled up to the track's full race length,
        // so 7 or 24 in a short Grand Prix), +8 fraction of that lap, +0xc duration. Pull it to lap 1 so a short test
        // race reaches it.
        uint32_t lap = 1;
        float frac = 0.30f;
        DWORD old = 0;
        auto *dst = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(rec));
        if (VirtualProtect(dst, 12, PAGE_READWRITE, &old)) {
            // The game only ever rolls type 1 (gearbox) here; pick one of the fault types 0-6 at random instead.
            uint32_t type = uint32_t(GetTickCount64() / 11) % 7;
            if (std::string_view(gp_fault_type) != "random")
                type = uint32_t(choice_int(std::string_view(gp_fault_type), int(type)));
            memcpy(dst, &type, 4);
            swofty::log("[td-probe] failure RECORD: type %u -> %u (native test)", w[0], type);
            memcpy(dst + 4, &lap, 4);
            memcpy(dst + 8, &frac, 4);
            VirtualProtect(dst, 12, old, &old);
            swofty::log("[td-probe] failure RECORD: lap %u -> lap 1 fraction 0.30 (native test)", w[1]);
        }
    }
    return orig_failure_register(list, rec);
}
// RuleManager (0x1435676a0 onSessionEnter, rcx = rules). Rules_Disqualify(rules, vehicle) at 0x143571440 marks the
// result DSQ and hands the car to the AI with DISQUALIFIED, as the formation-lap timer does.
std::atomic<uintptr_t> rule_manager{0};
std::atomic<bool> dsq_pending{false};
uint64_t (*orig_rules_enter)(void *, void *, void *) = nullptr;
uint64_t on_rules_enter(void *rules, void *systems, void *components)
{
    rule_manager = reinterpret_cast<uintptr_t>(rules);
    swofty::log("[td-probe] rule manager %p (session enter)", rules);
    return orig_rules_enter(rules, systems, components);
}
bool hook_rules_enter()
{
    constexpr uint8_t prologue[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x4C, 0x89, 0xC7, 0x48, 0x89, 0xD3, 0x48, 0x89, 0xCE};
    return detour("rule manager session enter", "56 57 53 48 83 EC 50 4C 89 C7 48 89 D3 48 89 CE 48 8B 02 48 8D 15 ?? ?? ?? ?? 48 89 D9 FF 50 28 48 89 86 00 1C 00 00",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_rules_enter),
                  reinterpret_cast<void **>(&orig_rules_enter));
}
// On the game thread (updateVehicle of the player's car).
void game_failure_dsq_now()
{
    if (!dsq_pending.exchange(false))
        return;
    uintptr_t rules = rule_manager.load();
    int id = player_vehicle.load();
    static uintptr_t at = 0;
    static int hits = swofty::api->scan(".xtls", "41 56 56 57 53 48 83 EC 48 C5 F9 29 74 24 30 80 FA FF 0F 84", &at);
    void *fn = hits == 1 ? reinterpret_cast<void *>(at) : nullptr;
    if (!rules || id < 0 || !fn) {
        swofty::log("[td-probe] game failure: DSQ not possible (rules %p, car %d, fn %p), retiring instead",
                    reinterpret_cast<void *>(rules), id, fn);
        player_broken = true;
        retire_in = 0.0;
        return;
    }
    swofty::log("[td-probe] game failure: disqualifying car %d (rules %p)", id, reinterpret_cast<void *>(rules));
    reinterpret_cast<void (*)(void *, uint32_t)>(fn)(reinterpret_cast<void *>(rules), uint32_t(id));
    // Rules_IssueDisqualification (0x143572f90) also sets the car's bit in the rule manager's DSQ masks +0x1d08 and
    // +0x1d0c. The session end (ruleset finalisation 0x142d2f440) reads +0x1d08: with the bit the car is classified
    // disqualified (black flagged), without it a car that has not finished becomes a DNF.
    for (uintptr_t off : {uintptr_t(0x1d08), uintptr_t(0x1d0c)}) {
        uint32_t mask = 0;
        if (id < 32 && guarded_read(rules + off, mask))
            *reinterpret_cast<volatile uint32_t *>(rules + off) = mask | (1u << id);
    }
    // Rules_Disqualify only marks the car and hands it to the AI, which stops it on track (DISQUALIFIED sends the AI
    // to its PreLights state). The result status is what takes a car out of the session: the pause menu's Retire
    // requests status 7 (retired). Request 5 (disqualified, black flagged) the same way.
    post_vehicle_status(id, 5, 6);
}

bool hook_failure_register()
{
    constexpr uint8_t prologue[] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCE};
    return detour("failure record register", "41 57 41 56 56 57 53 48 83 EC 20 48 89 D7 48 89 CE 48 8B 81 D0 00 00 00",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_failure_register),
                  reinterpret_cast<void **>(&orig_failure_register));
}

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

void emit_engine_audio_shutdown(emitter &e)
{
    e.bytes({0x50, 0x48, 0xB8});                       // push rax; mov rax, &engine_off_vehicle
    e.u64(reinterpret_cast<uintptr_t>(&engine_off_vehicle));
    e.bytes({0x44, 0x3B, 0x30, 0x75, 0x12});           // cmp r14d, [rax]; jne past override (18 bytes)
    e.bytes({0xB9, 0x08, 0x00, 0x00, 0x00});           // mov ecx, 8
    // Engine activation (0x154a053e0) records active state in audio+0x68 bit 3.
    // If pit-lane logic reactivates it while our cached pit state is already 8,
    // the normal change-only stop path would not run again. Re-arm that path
    // only when active; its normal stop clears bit 3, avoiding per-frame events.
    e.bytes({0xF6, 0x46, 0x68, 0x08, 0x74, 0x07});     // test byte [rsi+0x68], 8; jz past or
    e.bytes({0x80, 0x8E, 0xFA, 0x0F, 0x00, 0x00, 0x20}); // or byte [rsi+0xffa], 0x20 (pit state changed)
    e.bytes({0x58});                                   // pop rax
}

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
    emit_engine_audio_shutdown(e);
    e.jmp(at + 8);
    bool ok = jump_to_stub(at, 8, stub, e.p);
    swofty::log("[td-probe] engine sound: %s audio pit state read at %p", ok ? "hooked" : "could not hook",
                reinterpret_cast<void *>(at));
}

// GameReplaySystem's entry predicate normally rejects terminal state 5 unless
// its terminal-replay permission is set. Keep all other predicate checks intact.
void emit_replay_terminal_gate(emitter &e, uintptr_t allowed, uintptr_t terminal_check)
{
    e.bytes({0x83,0xBC,0x08,0xB8,0,0,0,0x05}); // original cmp [rax+rcx+0xb8],5
    e.bytes({0x74,0x0E}); // terminal -> compare player; otherwise original pass
    e.jmp(allowed);
    e.bytes({0x50,0x48,0xB8}); // preserve rax; load the player override address
    e.u64(reinterpret_cast<uintptr_t>(&replay_terminal_vehicle));
    e.bytes({0x3A,0x18,0x58,0x75,0x0E}); // cmp bl,[rax]; pop rax; jne original terminal check
    e.jmp(allowed);
    e.jmp(terminal_check);
}

void hook_replay_terminal_gate()
{
    constexpr const char *sig =
        "83 BC 08 B8 00 00 00 05 75 0D 80 BE 60 01 00 00 00 0F 84 30 FF FF FF 48 8B 8E 50 01 00 00";
    uintptr_t at = 0;
    int hits = swofty::api->scan(".xtls", sig, &at);
    uint8_t *stub = hits == 1 && at ? alloc_near(at) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] replay entry: terminal gate has %d hits or no stub memory; unchanged", hits);
        return;
    }
    emitter e{stub};
    emit_replay_terminal_gate(e, at + 23, at + 10);
    bool ok = jump_to_stub(at, 10, stub, e.p);
    swofty::log("[td-probe] replay entry: %s player-only terminal restriction at %p; other checks unchanged",
                ok ? "hooked" : "could not hook", reinterpret_cast<void *>(at));
}

// The game's damage rating 0x142f168b0 (rcx the system the addon calls VehicleTerminalDamageSystem, type descriptor
// 0x146096948, its replay recordable "VehicleDamageMonitor" at +0x20; rdx a record: +8 the vehicle wrapper, +0x10..+0x38
// the part levels 0-5 from the threshold tables at +0xc30.., +0x68 the worst). Three callers: updateVehicle (the
// stored record system+0x50+0x70*vehicle; a worst of 5 while system+0xf90 is set starts the timer at record+0x6c
// and then sends MessageVehicleTerminallyDamaged, 0x142f16881), a re-rating of every stored record (0x14f6a4050),
// and the recordable's canJumpIn (vtable slot 9, thunk 0x142f16d70 -> 0x14f696e10), which rates the player's car into
// a record on its stack and refuses the flashback jump-in at worst 5; the game's REPLAY channel then logs
// "canJumpIn() for recordable 'VehicleDamageMonitor' returned false!" (Career practice 0.51.129, 21:20:02). Without
// system+0xf90 every 5 reads 2 (the front wing 2 + 3 * flag). While keep_control handles terminal damage, only that
// stack record is rated as without the flag, so the jump-in is allowed; the stored records, and with them the
// terminal message, stay the game's. The stored record of the player's car feeds the limp watch. The game's pointers
// are valid inside its own call, so they are read directly.
constexpr const char *sig_damage_rating =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 28 48 89 D6 48 89 CF 48 8B 42 08 48 8B 48 58 48 8B 89 30 05 00 00 "
    "F6 81 30 25 00 00 03";
constexpr uint8_t damage_rating_prologue[16] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x28};
uint64_t (*orig_damage_rating)(void *, void *) = nullptr;
std::atomic<int> player_rating{-1};       // the worst level of the player's stored record (limp watch)
std::atomic<uint64_t> jump_in_logged_at{0};

// The parts of a record at level 5, as the bits of the game's terminal message (0 engine, 1 power unit, 2 gearbox,
// 3 a wheel off, 4 suspension, 5 tyres, 6 front wing, 7-10 the rest of the body).
uint32_t rated_terminal_parts(const uint8_t *record)
{
    uint32_t mask = 0;
    for (int i = 0; i < 11; ++i) {
        int32_t level = 0;
        memcpy(&level, record + 0x10 + 4 * i, 4);
        if (level == 5)
            mask |= 1u << i;
    }
    return mask;
}

// A record as the game rates it without system+0xf90: every level 5 reads 2, the worst follows.
void rate_without_terminal(uint8_t *record)
{
    int32_t worst = 0;
    for (int i = 0; i < 11; ++i) {
        int32_t level = 0;
        memcpy(&level, record + 0x10 + 4 * i, 4);
        if (level == 5) {
            level = 2;
            memcpy(record + 0x10 + 4 * i, &level, 4);
        }
        worst = std::max(worst, level);
    }
    memcpy(record + 0x68, &worst, 4);
}

uint64_t on_damage_rating(void *system, void *record)
{
    uint64_t r = orig_damage_rating(system, record);
    auto sys = reinterpret_cast<uintptr_t>(system), rec = reinterpret_cast<uintptr_t>(record);
    uint64_t count = *reinterpret_cast<const uint64_t *>(sys + 0xc20);
    int32_t worst = 0;
    memcpy(&worst, reinterpret_cast<const void *>(rec + 0x68), 4);
    if (rec >= sys + 0x50 && rec < sys + 0x50 + count * 0x70) {
        if (int(uint64_t(rec - sys - 0x50) / 0x70) == player_vehicle.load(std::memory_order_relaxed))
            player_rating.store(worst, std::memory_order_relaxed);
        return r;
    }
    if (worst != 5 || !keep_control)
        return r;
    auto *bytes = static_cast<uint8_t *>(record);
    uint32_t parts = rated_terminal_parts(bytes);
    rate_without_terminal(bytes);
    uint64_t now = GetTickCount64();
    if (now - jump_in_logged_at.load() > 10000) {
        jump_in_logged_at = now;
        swofty::log("[td-probe] flashback: the game rates your car terminal (parts 0x%x) and would refuse the jump-in; "
                    "allowed, the addon handles the damage", parts);
    }
    return r;
}

void hook_damage_rating()
{
    detour("damage rating (flashback jump-in check)", sig_damage_rating, damage_rating_prologue,
           sizeof damage_rating_prologue, reinterpret_cast<void *>(&on_damage_rating),
           reinterpret_cast<void **>(&orig_damage_rating));
}

// The game's own flashback rule: GameFlashbackSystem (replay recordable id 32, registered at 0x1408066b6; constructor
// 0x1407f1c60 writes its vtable 0x1457cdff0 and, at +0x20, the recordable vtable 0x1457ce120). The recordable's
// canJumpIn (vtable slot 9, thunk 0x142bf5800 -> 0x14e44f740 in .data1; rcx = system+0x20) reads only its own fields:
// +0x40 a free flashback passes (the instant replay state copies its free_flashback enter parameter there, 0x142da237c);
// +0x38 flashbacks off refuses (the player's assist apply 0x142424060 sets it to setting == 1, the constructor's default
// is off); +0x48 clear (no limit) passes; else +0x3c used < +0x44 limit. +0x39 is the limit mode (low, medium, high,
// unlimited; the limit for the session type comes from the game's flashback_limits data, 0x14e4456b0). A jump-in with
// flashbacks on and no free flashback counts +0x3c and posts MessageFlashbackUsed (0x142da2a05). The replay checks its
// recordables in order and stops at the first refusal: VehicleDamageMonitor (id 24, the damage rating above) before
// GameFlashbackSystem. The stock terminal damage screen (0x142d988e0) asks the same canJumpIn and offers only the
// replay when it refuses (terminally_damaged_replay_no_restart). Watched only: the result stays the game's, a refusal
// is logged with the fields (at most every 10 s) so a log tells the settings apart from the damage.
constexpr const char *sig_flashback_system_ctor =
    "48 8D 05 ?? ?? ?? ?? 48 89 07 48 8D 05 ?? ?? ?? ?? 48 89 47 20 48 C7 47 28 00 00 00 00 66 C7 47 38 00 03 "
    "C7 47 3C 00 00 00 00 C6 47 40 00 C6 47 48 00";
constexpr uint8_t flashback_rule_body[34] = {0xB0, 0x01, 0x80, 0x79, 0x20, 0x00, 0x74, 0x01, 0xC3, 0x80, 0x79, 0x18,
                                             0x00, 0x74, 0x10, 0x80, 0x79, 0x28, 0x00, 0x74, 0xF3, 0x8B, 0x41, 0x1C,
                                             0x3B, 0x41, 0x24, 0x0F, 0x92, 0xD0, 0xC3, 0x31, 0xC0, 0xC3};
uint64_t (*orig_flashback_rule)(void *) = nullptr;
std::atomic<uint64_t> flashback_rule_logged_at{0};

struct FlashbackRule {
    bool on = false, free = false, limited = false;
    uint8_t mode = 0;
    uint32_t used = 0, limit = 0;
};
// The fields as canJumpIn sees them (recordable = system+0x20).
FlashbackRule read_flashback_rule(const uint8_t *recordable)
{
    FlashbackRule f;
    f.on = recordable[0x18] != 0;
    f.mode = recordable[0x19];
    memcpy(&f.used, recordable + 0x1c, 4);
    f.free = recordable[0x20] != 0;
    memcpy(&f.limit, recordable + 0x24, 4);
    f.limited = recordable[0x28] != 0;
    return f;
}
bool flashback_rule_allows(const FlashbackRule &f)
{
    return f.free || (f.on && (!f.limited || f.used < f.limit));
}
const char *flashback_rule_reason(const FlashbackRule &f)
{
    if (flashback_rule_allows(f))
        return "allowed";
    return f.on ? "no flashbacks left in this session" : "flashbacks are off in this game's settings";
}
const char *flashback_mode_name(uint8_t mode)
{
    static const char *const names[] = {"low", "medium", "high", "unlimited"};
    return mode < 4 ? names[mode] : "?";
}

uint64_t on_flashback_rule(void *recordable)
{
    uint64_t r = orig_flashback_rule(recordable);
    if (r & 0xff)
        return r;
    uint64_t now = GetTickCount64();
    if (now - flashback_rule_logged_at.load() > 10000) {
        flashback_rule_logged_at = now;
        FlashbackRule f = read_flashback_rule(static_cast<const uint8_t *>(recordable));
        swofty::log("[td-probe] flashback: the game's flashback rule refuses the jump-in (not the damage): %s; "
                    "flashbacks %s, limit %s%s, used %u of %u, free flashback %d",
                    flashback_rule_reason(f), f.on ? "on" : "off", flashback_mode_name(f.mode),
                    f.limited ? "" : " (not counted)", f.used, f.limit, int(f.free));
    }
    return r;
}

// The constructor gives the recordable vtable; its slot 9 must lead (through jmp thunks) to the body above.
void hook_flashback_rule()
{
    uintptr_t ctor = 0;
    int n = swofty::api->scan(".xtls", sig_flashback_system_ctor, &ctor);
    if (n != 1 || !ctor) {
        swofty::log("[td-probe] flashback rule: constructor signature has %d hits, not watched", n);
        return;
    }
    int32_t rel = 0;
    memcpy(&rel, reinterpret_cast<const void *>(ctor + 13), 4);
    uintptr_t slot = ctor + 17 + int64_t(rel) + 9 * 8, fn = 0;
    if (!guarded_read(slot, fn) || !fn) {
        swofty::log("[td-probe] flashback rule: vtable slot %p unreadable, not watched", reinterpret_cast<void *>(slot));
        return;
    }
    uintptr_t body = fn;
    for (int i = 0; i < 3 && swofty::mem::readable(body, 5) && *reinterpret_cast<const uint8_t *>(body) == 0xE9; ++i) {
        memcpy(&rel, reinterpret_cast<const void *>(body + 1), 4);
        body = body + 5 + int64_t(rel);
    }
    if (!swofty::mem::readable(body, sizeof flashback_rule_body) ||
        memcmp(reinterpret_cast<const void *>(body), flashback_rule_body, sizeof flashback_rule_body)) {
        swofty::log("[td-probe] flashback rule: slot %p leads to %p, not the expected check; not watched",
                    reinterpret_cast<void *>(slot), reinterpret_cast<void *>(body));
        return;
    }
    orig_flashback_rule = reinterpret_cast<decltype(orig_flashback_rule)>(fn);
    void *handler = reinterpret_cast<void *>(&on_flashback_rule);
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void *>(slot), 8, PAGE_READWRITE, &old))
        return;
    memcpy(reinterpret_cast<void *>(slot), &handler, 8);
    VirtualProtect(reinterpret_cast<void *>(slot), 8, old, &old);
    swofty::log("[td-probe] watching the game's flashback rule (GameFlashbackSystem canJumpIn, vtable slot %p)",
                reinterpret_cast<void *>(slot));
}

// Flashback without UDP. The instant replay state's update (0x142da2760) posts MessageFlashbackUsed (vtable
// 0x1457d4030) through its own queued-post thunk (0x142cb7130, this is its only caller, at 0x142da2a53) when the player
// resumes from a flashback. The game then rewinds the cars, including the terminal damage record and the part wear.
constexpr const char *sig_flashback_used_site =
    "4C 8B B7 68 0F 00 00 41 80 7E 38 00 74 40 41 0F B6 46 40 84 C0 75 04 41 FF 46 3C 48 8B 0D ?? ?? ?? ?? 48 C7 44 "
    "24 48 00 00 00 00 48 8D 15 ?? ?? ?? ?? 48 89 54 24 40 88 44 24 50 48 8B 49 08 48 8D 54 24 40 E8 ?? ?? ?? ??";
uint64_t (*orig_flashback_used)(void *, void *) = nullptr;
uint64_t on_flashback_used(void *bus, void *msg)
{
    uint64_t r = orig_flashback_used(bus, msg);
    flashback_used_at = GetTickCount64();
    flashback_count.fetch_add(1);
    if (uint32_t w = ai_watch_mask.load())
        swofty::log("[td-probe] AI watch: flashback with watched AI cars 0x%x", w);
    swofty::log("[td-probe] flashback: used (broken %d, terminal mask 0x%x, retire request %d, pit state %d)",
                int(player_broken.load()), terminal_mask.load(), int(retire_requested.load()), player_pit.load());
    return r;
}
void hook_flashback_used()
{
    uintptr_t at = 0;
    int hits = swofty::api->scan(".xtls", sig_flashback_used_site, &at);
    if (hits != 1 || !at) {
        swofty::log("[td-probe] flashback: signature has %d hits, using the terminal record only", hits);
        return;
    }
    uintptr_t call = at + 0x44;
    int32_t rel;
    memcpy(&rel, reinterpret_cast<const void *>(call + 1), sizeof rel);
    flashback_hooked = hook_thunk_at("MessageFlashbackUsed post", call + 5 + int64_t(rel),
                                     reinterpret_cast<void *>(&on_flashback_used),
                                     reinterpret_cast<void **>(&orig_flashback_used));
}

// No engine, power unit or gearbox part at failure (1.0). Just before the game's own failure the failing part reads
// 0.99 (record state 4), so the old 0.9 bound kept the lock after such a flashback (0.51.82 test).
bool no_failed_part(uintptr_t car)
{
    uintptr_t engine = 0, gearbox = 0;
    bool ok = car && guarded_read(car + 0x530, engine) && engine && guarded_read(car + 0x538, gearbox) && gearbox;
    for (int i = 0; ok && i < 7; ++i) {
        float wear = 1;
        ok = guarded_read(i < 6 ? engine + part_offsets[i] : gearbox + 0x674, wear) && std::isfinite(wear) &&
             wear >= 0 && wear < 1.0f;
    }
    return ok;
}

// The player's pit state as the PitLaneSystem has it now ([system + v*16 + 0x68] -> +4, as in setVehicleStatus), or
// -1. A flashback rewinds it; whether the game posts MessagePitStateChanged for that is not known, so player_pit is
// read again after one (reset_car_state would otherwise leave it at OnTrack).
int live_pit_state(int vehicle)
{
    uintptr_t sys = pit_lane_system.load(), entry = 0;
    uint32_t state = 0xffffffff;
    if (!sys || vehicle < 0 || vehicle >= 0x1b || !guarded_read(sys + 0x68 + uintptr_t(vehicle) * 16, entry) ||
        !entry || !guarded_read(entry + 4, state) || state >= 16)
        return -1;
    return int(state);
}
void resync_pit_after_flashback()
{
    int pit = live_pit_state(player_vehicle.load());
    if (pit < 0 || pit == player_pit.load())
        return;
    swofty::log("[td-probe] flashback: pit state %d -> %d %s (rewound)", player_pit.load(), pit, label(pit_states, pit));
    player_pit = pit;
}

// Game thread, after the terminal record was read. For 2 s after a flashback (counted from the first update after it:
// the game stays paused about 1.6 s after the message, and the rewound record shows on that first update):
//  - a retirement request without damage is cancelled (the flashback undoes it; request again if wanted). Only a real
//    request: a failure the addon applied whose terminal message is still pending also has no mask yet;
//  - a record that is no longer terminal and no failed part: the flashback went back before the damage, the car is
//    yours again with full power. This also undoes a retirement: the game lets you flash back after a retirement and
//    races the car again (0.51.82 test). A failure the game scheduled comes back, so its next RETIRED hand-over is
//    dropped again and the part breaks again, as the first time;
//  - still terminal after 2 s: the damage stays, the retirement starts over from the rewound position (no stale
//    standstill timer, crew, push or garage work). Damage the rewind removed leaves the mask.
// A withdrawn or red-flagged car is left alone.
void flashback_check(uintptr_t car)
{
    static uint64_t window = 0;
    uint64_t now = GetTickCount64();
    if (flashback_used_at.exchange(0))
        window = now + 2000;
    if (!window)
        return;
    constexpr uint32_t suspended = (1u << 3) | (1u << 10) | (1u << 11);
    bool retired = confirmed_retired_vehicle.load() >= 0 || retire_in.load() <= -2.0;
    if (!keep_control || (!player_broken && !retired) || car_dead || session_ended || red_flag_screen_open.load() ||
        red_flag_repair_wait.load() || (player_controller_flags.load() & suspended)) {
        window = 0;
        return;
    }
    uint32_t mask = terminal_mask.load();
    int state = live_terminal_state.load();
    uint32_t live = live_terminal_mask.load();
    if (retire_requested && !mask && !retired) {
        window = 0;
        reset_car_state();
        resync_pit_after_flashback();
        swofty::log("[td-probe] flashback: your retirement request is cancelled");
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT CANCELLED", "Flashback used", 3);
        return;
    }
    if (state != 5 && !live && no_failed_part(car)) {
        window = 0;
        reset_car_state();
        resync_pit_after_flashback();
        player_controller_flags.fetch_and(~(1u << 4));
        own_retire_until = 0;
        status7_at = 0;
        game_failure_deadline = 0;
        game_failure_lap = -1;
        dsq_pending = false;
        int part = game_failure_part.exchange(-1);
        if (part >= 0)
            game_planned_part = part;
        swofty::log("[td-probe] flashback: back before the damage (game record state %d%s), your car is whole again%s",
                    state, retired ? ", retirement undone" : "",
                    part >= 0 ? "; the game's failure may come back and is handled the same way" : "");
        return;
    }
    if (now < window)
        return;
    window = 0;
    if (retired) {
        swofty::log("[td-probe] flashback: your car is retired and still broken (record state %d, parts 0x%x)", state,
                    live);
        return;
    }
    if (state == 5 && live && (live & mask) == live && live != mask) {
        terminal_mask = live;
        if (!(live & 0x18)) {
            limp_state = -1;
            limp_corner = -1;
            crash_corners = 0;
        }
        swofty::log("[td-probe] flashback: damage 0x%x -> 0x%x (the rewind removed part of it)", mask, live);
    }
    cancel_crew_work();
    retire_in = -1.0;
    still_for = 0;
    engine_off_standing = false;
    retired_engine_off_vehicle = -1;
    pit_clearance_hold_vehicle = -1;
    garage_return_wanted = false;
    exit_check_at = 0;
    resync_pit_after_flashback();
    if (limp_state.load() == 1 && race_stop_left.load() > 0)
        race_stop_left = race_stop_grace;
    swofty::log("[td-probe] flashback: after the damage (record state %d, parts 0x%x); still broken, retirement starts "
                "over (pit state %d)", state, live, player_pit.load());
    // Rewound into the box or the push: no new box arrival starts the push again, so the car retires in its box.
    if (int pit = player_pit.load(); !practice_like() && pit >= 4 && pit <= 8) {
        retire_in = 2.0;
        swofty::log("[td-probe] flashback: rewound inside the pit box (pit state %d), the car retires there", pit);
    }
}

uintptr_t player_car()
{
    return throttle_hook.car;
}


// Retire request (hotkey, pause menu or radio): nothing breaks and power stays. You pull over
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
        flashback_tracker.clear_lock();
        swofty::log("[td-probe] %s: retirement cancelled", source);
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT CANCELLED", {}, 3);
    } else if (!player_broken && retire_in.load() > -2.0 && player_pit.load() == 1) {
        retire_requested = true;
        flashback_tracker.mark(GetTickCount64());
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
        own_retire_until = GetTickCount64() + 10000;
        return false;
    }
    if (player_controller_flags.load() & (1u << 2)) {
        // Disqualified: the AI holds the car. Ask again for the disqualified result, which ends your session; a
        // retirement would turn the DSQ into a DNF.
        swofty::log("[td-probe] pause menu retire: car is disqualified, requesting the DSQ result again");
        post_vehicle_status(player_vehicle.load(), 5, 6);
        return true;
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

// Lap (0-based) the game counts for the player, read like the failure update does (getter 0x46046a0 with the failure
// object's car lookup). -1 when it cannot be read.
int game_lap_of(uintptr_t obj);
int game_lap() { return game_lap_of(failure_planner_obj.load()); }
// The lap (0-based) of the car whose failure object this is.
int game_lap_of(uintptr_t obj)
{
    uint64_t c98 = 0, c20 = 0;
    uint8_t idx = 0xff;
    if (!obj || !guarded_read(obj + 0x98, c98) || !c98 || !guarded_read(c98 + 0x20, c20) || !c20 || !guarded_read(obj + 0x78, idx) || idx == 0xff) {
        static uintptr_t logged = 0;
        if (obj && logged != obj) {
            logged = obj;
            swofty::log("[td-probe] game lap: no lap for failure object %p (+0x98 %p, +0x20 %p, vehicle %u)",
                        reinterpret_cast<void *>(obj), reinterpret_cast<void *>(c98), reinterpret_cast<void *>(c20),
                        unsigned(idx));
        }
        return -1;
    }
    // A fixed address of this build: call it only when its first instructions match (push rsi; sub rsp, 0x20;
    // movzx r8d, dl; mov rax, [rcx+0x38]).
    static const auto getter = [] {
        constexpr uint8_t prologue[] = {0x56, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x0F, 0xB6, 0xC2, 0x48, 0x8B, 0x41, 0x38};
        uintptr_t at = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x46046a0;
        uint8_t code[sizeof prologue] = {};
        bool ok = guarded_copy(at, code, sizeof code) && !memcmp(code, prologue, sizeof prologue);
        if (!ok)
            swofty::log("[td-probe] game failure: lap getter not found (other game build?), the 10 min fallback applies");
        return ok ? reinterpret_cast<uintptr_t (*)(uint64_t, uint32_t)>(at) : nullptr;
    }();
    if (!getter)
        return -1;
    uintptr_t a = getter(c20, idx);
    uint16_t l1 = 0, l2 = 0;
    if (!a || !guarded_read(a + 8, l1) || !guarded_read(a + 0x10, l2))
        return -1;
    return int(int16_t(l1 ^ l2));
}

// The race's laps from Swofty's telemetry, else from the session context (session_laps), else from the game's race
// settings (game_race_laps, the laps the game plans its failures over); 0 when none has them.
int race_total_laps(const char **source = nullptr)
{
    swofty::telemetry::SessionState st{};
    st.version = swofty::telemetry::version;
    if (swofty::telemetry::session(&st) == swofty::telemetry::Result::ok && st.total_laps > 0 && st.total_laps < 200) {
        if (source)
            *source = "Swofty telemetry";
        return int(st.total_laps);
    }
    if (int laps = session_laps.load(); laps > 0) {
        if (source)
            *source = "session context";
        return laps;
    }
    if (int laps = game_race_laps.load(); laps > 0 && laps < 200) {
        if (source)
            *source = "the game's race settings";
        return laps;
    }
    return 0;
}

// The fixed-chance modes' failure point: on its lap, after the planned share of a lap (the last full lap's time, 90 s
// on the first lap); then the time trigger in gp_failure_tick takes over. The lap is read twice a second of driving.
void gp_point_tick()
{
    static double next_read = 0;
    if (gp_point.lap < 0) {
        next_read = 0;
        return;
    }
    if (gp_clock < next_read)
        return;
    next_read = gp_clock + 0.5;
    int lap = game_lap();
    if (lap < 0)
        return;
    if (lap != gp_lap_seen) {
        if (gp_lap_seen >= 0 && lap == gp_lap_seen + 1)
            gp_lap_time = gp_clock - gp_lap_started;
        gp_lap_seen = lap;
        gp_lap_started = gp_clock;
    }
    if (lap < gp_point.lap)
        return;
    double lap_time = gp_lap_time > 20 ? gp_lap_time : 90.0;
    gp_at = lap > gp_point.lap ? gp_clock : std::max(gp_clock, gp_lap_started + gp_point.in_lap * lap_time);
    swofty::log("[td-probe] gp failures: the planned lap %d has come, the part fails in %.0f s", gp_point.lap + 1,
                gp_at - gp_clock);
    gp_point = {};
}

// Grand Prix failures (modes without the game's failure options): once the player has driven a while, roll once per
// session whether a part fails and when, then break a random part the same way the debug failure does. Also ends the
// window after the game's own scripted failure: disqualify (or retire) a car that neither pitted nor retired in time.
void gp_plan_realistic();
void gp_fault_tick();
void gp_ai_failure_tick();
void gp_warning_tick(double dt);
void start_power_fault(int type, double length, bool quick);
void end_power_fault(const char *why);
void power_tick(double dt);
void gp_break(int outcome);
void gearbox_decline_tick(double dt);
// Black flag with an orange disc (FIA Appendix H 2.5.4.1 e): shown at the line, the driver must stop at his pit on the
// next lap. The window after the game's failure is over once the lap after the failure's lap is over (laps 0-based,
// -1 unknown: then only the 10-minute fallback ends it).
constexpr bool pit_window_over(int failure_lap, int now_lap) { return failure_lap >= 0 && now_lap > failure_lap + 1; }
// A race car with only aero or body damage on track and no pit window (the damage came in the pit lane, the car
// drove through its box, or a flashback undid the pit entry or part of the damage) is called in (again).
void heavy_damage_tick()
{
    uint32_t mask = terminal_mask.load();
    constexpr uint32_t out = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 10) | (1u << 11);
    if (!keep_control || !player_broken || !mask || (mask & 0x1f) || game_failure_deadline.load() ||
        pit_call_ended.load() || player_pit.load() != 1 || session_ended || retire_in.load() != -1.0 ||
        dsq_pending || confirmed_retired_vehicle.load() >= 0 || red_flag_repair_wait.load() ||
        red_flag_screen_open.load() || (player_controller_flags.load() & out) || practice_like())
        return;
    heavy_damage_call(mask);
}
void gp_failure_tick(double dt)
{
    gearbox_decline_tick(dt);
    heavy_damage_tick();
    if (uint64_t window = game_failure_deadline.load()) {
        // The 10-minute fallback counts driving time: not the pause menu (no fresh speed while paused).
        static uint64_t window_seen = 0;
        static double window_driven = 0;
        if (window != window_seen) {
            window_seen = window;
            window_driven = 0;
        }
        if (player_speed() >= 0)
            window_driven += dt;
        if (session_ended || confirmed_retired_vehicle.load() >= 0 || player_vehicle.load() < 0) {
            game_failure_deadline = 0;
        } else if (player_controller_flags.load() & ((1u << 10) | (1u << 11))) {
            // Red flag or session suspension: the game holds the car and its teleport ends this window.
        } else if (player_pit.load() != 1) {
            // In the pit lane: the car is allowed to be there, the garage handling takes over.
            game_failure_deadline = 0;
            swofty::log("[td-probe] %s: car reached the pits, no disqualification", pit_call_name());
        } else {
            int start = game_failure_lap.load(), now_lap = start >= 0 ? game_lap() : -1;
            bool lap_over = pit_window_over(start, now_lap);
            // The game's lap could not be read: your line crossings count instead (the second one ends the next lap).
            if (start < 0 && game_failure_crossing.load() >= 0)
                lap_over = player_crossings.load() > game_failure_crossing.load() + 1;
            if (lap_over || window_driven >= 600.0) {
                game_failure_deadline = 0;
                pit_call_ended = true;
                const char *why = lap_over ? "the next lap ended" : "10 minutes of driving passed";
                if (std::string_view(game_failure_timeout) == "dsq") {
                    swofty::log("[td-probe] %s: %s without pitting or retiring, disqualifying the car", pit_call_name(),
                                why);
                    if (swofty::hud::available())
                        swofty::hud::toast("DISQUALIFIED", "Ignored the call to the pits", 4);
                    dsq_pending = true;
                } else {
                    swofty::log("[td-probe] %s: %s without pitting or retiring, retiring the car", pit_call_name(), why);
                    if (swofty::hud::available())
                        swofty::hud::toast("RETIRED", pit_call_reason.load() ? "Heavy damage" : "Mechanical failure", 4);
                    player_broken = true;
                    retire_in = 0.0;
                }
            }
        }
    }
    std::string_view mode = gp_failures;
    bool realistic = mode == "realistic";
    int percent = realistic ? 1 : mode == "off" ? 0 : choice_int(mode, 0);
    // The mode changed mid-session (a preset or by hand): the planned failure goes and the new mode plans its own.
    // Leaving Realistic also ends its warning signs (their power loss and brake fade froze before) and its partial
    // fault plan. A part that already failed stays failed; a gearbox decline under way runs to its end.
    static int last_plan = -1;  // 1000 realistic, else the percent (0 off); -1 before the first frame
    if (int plan = realistic ? 1000 : percent; plan != last_plan) {
        if (last_plan == 1000) {
            power_warn = {};
            brake_fade = 0.0f;
            shown_warning = -1;
            gp_warn = {};
            gp_fault_at = -1;
        }
        if (last_plan >= 0) {
            if (gp_at >= 0)
                swofty::log("[td-probe] gp failures: the mode is now %.*s, the planned failure is dropped",
                            int(mode.size()), mode.data());
            gp_at = -1;
            gp_point = {};
        }
        last_plan = plan;
    }
    if (percent <= 0) {
        gp_clock = 0; gp_at = -1;
        gp_point = {};
        return;
    }
    if (session_ended || practice_like() || player_broken || player_vehicle.load() < 0 || player_pit.load() != 1 ||
        pending_failure.load() || retire_requested)
        return;  // the clocks stand in the pit lane or once the car is out (power_tick gives full power there)
    // Nursing: during the warning signs the part wears slower with less time at full throttle.
    bool warning = realistic && gp_at >= 0 && gp_at < 1e17 && gp_clock >= gp_warn.warn_at;
    gp_clock += warning ? dt * fm::nursing_factor(full_throttle_share.load()) : dt;
    if (realistic) {
        if (gp_at < 0 && gp_clock > 20.0)
            gp_plan_realistic();
        gp_fault_tick();
        gp_ai_failure_tick();
        gp_warning_tick(dt);
        if (gp_at >= 0 && gp_clock >= gp_at && gp_at < 1e17) {
            gp_at = 1e18;
            power_warn = {};
            gp_break(gp_warn.outcome);
        }
        return;
    }
    if (gp_at < 0 && gp_clock > 20.0) {
        // The chance is for the whole race; the failure comes anywhere between 5 % and 95 % of its laps (fm::race_point).
        // Without the lap count the old window stays: 1 to 15 minutes of driving. The rolls used to come from the tick
        // count, so they were tied to each other.
        bool fails = percent >= 100 || uniform01() < percent / 100.0;
        int laps = fails && percent < 100 ? race_total_laps() : 0;
        gp_point = {};
        if (!fails)
            gp_at = 1e18;
        else if (percent >= 100)
            gp_at = gp_clock + 15.0 + uniform01() * 45.0;
        else if (laps > 0) {
            gp_point = fm::race_point(uniform01(), laps);
            gp_at = 1e18;  // gp_point_tick sets the time on its lap
        } else
            gp_at = gp_clock + 60.0 + uniform01() * 840.0;
        if (gp_point.lap >= 0)
            swofty::log("[td-probe] gp failures (%.*s): a part will fail in lap %d of %d", int(mode.size()), mode.data(),
                        gp_point.lap + 1, laps);
        else
            swofty::log("[td-probe] gp failures (%.*s): %s", int(mode.size()), mode.data(),
                        gp_at >= 1e17 ? "no failure this race"
                        : percent >= 100 ? "a part will fail within a minute (test)"
                                         : "a part will fail within 15 minutes of driving (the race length is unknown)");
    }
    gp_point_tick();
    if (gp_at >= 0 && gp_clock >= gp_at && gp_at < 1e17) {
        gp_at = 1e18;
        int part = std::min(6, int(uniform01() * 7));
        swofty::log("[td-probe] gp failures: breaking part %d", part);
        trigger_failure(part);
    }
}

// Debug (Ctrl+Shift+F7): replay a Career scripted failure the way the game does it: its failure planner picks the part
// for your car (on_failure_planner sets game_planned_part), then the scheduler hands the car to the AI
// (0x1409be12c: setController(vehicle, AI, RETIRED) with no retire request from you). With Keep control after a
// scripted failure the addon takes the hand-over over (note_controller_switch); otherwise the AI keeps the car and
// the part reaches terminal damage 30 s later (Flo's case: TERMINAL_DAMAGE followed 31 s after the hand-over).
// 0.51.123 log: without the planned part (a condition since 0.51.63) the AI took the car.
std::atomic<uint64_t> scripted_part_at{0};
void trigger_scripted_failure()
{
    void *sys = controller_system.load();
    int vehicle = player_vehicle.load();
    if (session_ended || !sys || vehicle < 0) {
        swofty::log("[td-probe] scripted failure: no controller system or player vehicle (system %p, vehicle %d)", sys, vehicle);
        return;
    }
    int part = choice_int(debug_failure_part, 0);
    game_planned_part = part >= 0 && part <= 6 ? part : 0;
    swofty::log("[td-probe] scripted failure: the game's planner picks part %d, then its RETIRED hand-over of vehicle %d",
                game_planned_part.load(), vehicle);
    on_set_controller(sys, uint64_t(vehicle), 1, 4);
    if (game_planned_part.load() == -1 && game_failure_part.load() >= 0) {
        swofty::log("[td-probe] scripted failure: the addon kept the car (part %d broken, pit window open)",
                    game_failure_part.load());
        return;
    }
    game_planned_part = -1;
    if (confirmed_retired_vehicle.load() == vehicle) {
        swofty::log("[td-probe] scripted failure: the AI has the car, part failure in 30 s");
        scripted_part_at = GetTickCount64() + 30000;
    } else {
        swofty::log("[td-probe] scripted failure: no hand-over happened");
    }
}
void scripted_failure_tick()
{
    uint64_t at = scripted_part_at.load();
    if (!at || GetTickCount64() < at || !scripted_part_at.compare_exchange_strong(at, 0))
        return;
    swofty::log("[td-probe] scripted failure: part failure now");
    trigger_failure();
}

void poll_hotkey()
{
    // Every hotkey is Ctrl+Shift+<key>. Without both held no setting and no other key is read and every key counts as
    // up (0.51.120: four setting reads and up to 18 key reads a frame before).
    bool held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    bool debug_on = held && debug_failure_hotkey;
    bool retire_on = held && retire_hotkey;
    auto key_down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    // Debug: Ctrl+Shift+F9 cycles the player's throttle cap: 0.0 (engine off) -> 0.3 -> automatic.
    static bool cut_was_down;
    bool cut = debug_on && key_down(VK_F9);
    if (cut && !cut_was_down) {
        float t = test_cap.load();
        test_cap = t < 0 ? 0.0f : t < 0.1f ? 0.3f : -1.0f;
        swofty::log("[td-probe] throttle test: cap %.1f (car %p, hooked %d)", double(test_cap.load()),
                    reinterpret_cast<void *>(player_car()), int(throttle_hooked.load()));
    }
    cut_was_down = cut;
    static bool failure_was_down, retire_was_down;
    bool down = debug_on && key_down(function_key(debug_failure_key, 10));
    if (down && !failure_was_down)
        trigger_failure();
    failure_was_down = down;
    // Debug: Ctrl+Shift+F4 once starts the test run with you driving, twice quickly with the AI driving; while it
    // runs, it stops it.
    static bool test_was_down;
    bool test_key = debug_on && key_down(VK_F4);
    switch (test_key_action(test_key && !test_was_down, GetTickCount64(), test_run_on.load())) {
    case 1: test_run_start(false); break;
    case 2: test_run_start(true); break;
    case 3: test_run_stop_by_you(); break;
    default: break;
    }
    test_was_down = test_key;
    static bool scripted_was_down;
    // Debug: Ctrl+Shift+F6 registers the next partial failure (the game's temporary faults) at once, cycling the types.
    static bool fault_was_down;
    static int fault_next = 0;
    bool fault_key = debug_on && key_down(VK_F6);
    if (fault_key && !fault_was_down) {
        static const int types[] = {fm::f_slow_shift, fm::f_missing_gear, fm::f_gear_jammed, fm::f_drs,
                                    fm::f_ers_modes,  fm::f_ers_glitch,   fm::f_lean_fuel,   fm::f_rear_brakes,
                                    fm::f_mguk_loss,  fm::f_battery_derate, fm::f_misfire,   -1};
        constexpr int n_types = int(sizeof types / sizeof types[0]);
        int t = types[fault_next++ % n_types];
        if (t < 0) {
            // Last in the cycle: a whole gearbox decline in 2 minutes (both ways in turn), ending in the failure.
            static int path = 0;
            swofty::log("[td-probe] debug fault: gearbox decline requested");
            start_gearbox_decline(120.0, path++ % 2);
        } else if (t == fm::f_rear_brakes) {
            swofty::log("[td-probe] debug fault: rear brake failure requested");
            start_rear_brake_fault(fm::rear_brake_change(uniform01(), uniform01()), 60);
        } else if (fm::power_kind_of_fault(t) != pl::none) {
            swofty::log("[td-probe] debug fault: %s requested (60 s)", fm::fault_name(t));
            start_power_fault(t, 60, true);
        } else {
        fm::FaultSpec spec = fm::fault_spec(t);
        double duration = spec.seconds ? 60.0 : 3.0;  // short, so a test sees the "fixed" radio too
        // Missing gear: 3rd (index 4), used in most corners. Slow shifts: a random severity like the real ones.
        fm::ShiftSeverity sev = fm::slow_shift_severity(uniform01(), uniform01());
        fault_request = uint64_t(t) | (uint64_t(duration * 10.0) << 8) | (uint64_t(spec.seconds ? 1 : 0) << 40) |
                        (uint64_t(t == fm::f_missing_gear ? 4 : 0xff) << 48) |
                        (uint64_t(t == fm::f_slow_shift ? sev.scale : 0) << 56);
        if (t == fm::f_slow_shift)
            swofty::log("[td-probe] debug fault: %s requested (%s, x%d)", fm::fault_name(t), sev.name, sev.scale);
        else
            swofty::log("[td-probe] debug fault: %s requested", fm::fault_name(t));
        }
    }
    fault_was_down = fault_key;
    bool scripted = debug_on && key_down(VK_F7);
    if (scripted && !scripted_was_down)
        trigger_scripted_failure();
    scripted_was_down = scripted;
    scripted_failure_tick();
    down = retire_on && key_down(function_key(retire_key, 11));
    if (down && !retire_was_down)
        request_retire("retire hotkey");
    retire_was_down = down;
}

#include "suspension_damage.hpp"

// ---- Realistic Grand Prix failures (gp_failures "realistic", failure_era) and their warning signs ----
double uniform01()
{
    static std::mt19937_64 rng{GetTickCount64() * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId()};
    return double(rng() >> 11) * (1.0 / 9007199254740992.0);
}

void gp_plan_realistic()
{
    const fm::Season &season = fm::season_of(failure_era);
    double u = uniform01();
    double in = fm::seconds_until_failure(season, u > 0 ? u : 1e-12);
    gp_at = gp_clock + in;
    gp_warn = {};
    gp_warn.outcome = fm::pick_outcome(fm::era_of(season), uniform01());
    fm::Warning w = fm::warning_of(gp_warn.outcome);
    double lead = 0;
    if (uniform01() < w.chance) {
        lead = w.min_lead_s + uniform01() * (w.max_lead_s - w.min_lead_s);
        gp_warn.warn_at = std::max(gp_clock, gp_at - lead);
    }
    // Logged as minutes of driving: a full Grand Prix is about 90, so most sessions end before.
    swofty::log("[td-probe] gp failures (%.*s %s, %.1f %% per Grand Prix): %s would fail after %.0f more minutes of "
                "driving, %s",
                int(season.id.size()), season.id.data(), fm::era_of(season).name, season.race_chance * 100.0, fm::outcome_name(gp_warn.outcome), in / 60.0,
                lead > 0 ? "with warning signs before" : "without warning");
}

// AI cars: each car fails with the season's rate on the same drive clock (a full Grand Prix is about 90 minutes of
// driving), the part from the era's shares; suspension and brake failures, which the game's sequence cannot do,
// become one of the power unit parts. Not in Career (the game's own model with the teams' R&D).
void gp_ai_failure_tick()
{
    if (!failure_mgr.load())
        return;
    const fm::Season &season = fm::season_of(failure_era);
    if (!ai_failures_rolled) {
        swofty::career::State st{};
        if (swofty::career::state(&st) == swofty::career::Result::ok && st.loaded) {
            ai_failures_rolled = true;
            for (double &t : ai_fail_at)
                t = 1e18;
            return;
        }
        ai_failures_rolled = true;
        const fm::Era &era = fm::era_of(season);
        fm::Era pu = era;
        pu.share[fm::suspension] = pu.share[fm::brakes] = 0;
        int planned = 0;
        for (int v = 0; v < 32; ++v) {
            ai_fail_at[v] = gp_clock + fm::seconds_until_failure(season, std::max(uniform01(), 1e-12));
            ai_fail_part[v] = fm::pick_outcome(pu, uniform01());
            planned += ai_fail_at[v] < gp_clock + fm::gp_minutes * 60.0;
        }
        swofty::log("[td-probe] gp failures: AI cars use the %.*s rate, %d of 32 slots would fail within a full race "
                    "distance", int(season.id.size()), season.id.data(), planned);
    }
    int player = player_vehicle.load();
    for_each_failure_obj([&](int v, uintptr_t) {
        if (v == player || ai_fail_at[v] >= 1e17 || gp_clock < ai_fail_at[v])
            return;
        ai_fail_at[v] = 1e18;
        swofty::log("[td-probe] gp failures: AI car %d, %s fails", v, fm::outcome_name(ai_fail_part[v]));
        breakdown_part[v] = uint8_t(ai_fail_part[v]);
        breakdown_request.fetch_or(1u << v);
    });
}

// One partial failure per session at most, with the era's chance, as the game's own temporary faults. Not in
// Career: there the game plans its own faults, and a registered fault is written into the career save.
void gp_fault_tick()
{
    if (gp_fault_at < 0) {
        const fm::Season &season = fm::season_of(failure_era);
        const fm::FaultEra &fe = fm::fault_era_of(season);
        swofty::career::State st{};
        bool career = swofty::career::state(&st) == swofty::career::Result::ok && st.loaded;
        double in = career ? 1e18 : fm::seconds_until_fault(season, std::max(uniform01(), 1e-12));
        gp_fault_at = gp_clock + in;
        gp_fault_type_rolled = fm::pick_fault(fe, uniform01());
        swofty::log("[td-probe] gp failures: partial failure (%.1f %% per Grand Prix)%s: %s after %.0f more minutes",
                    fm::fault_race_chance(season) * 100.0, career ? " off in Career" : "", fm::fault_name(gp_fault_type_rolled),
                    in / 60.0);
    }
    if (gp_fault_at >= 1e17 || gp_clock < gp_fault_at)
        return;
    gp_fault_at = 1e18;
    fm::FaultSpec spec = fm::fault_spec(gp_fault_type_rolled);
    double duration = spec.min_duration + uniform01() * (spec.max_duration - spec.min_duration);
    // Missing gear: 2nd to 7th (gearbox index 3-8); slow shifts: a mild, moderate or severe shift time (the game's own
    // roll is too small to notice).
    uint64_t gear = gp_fault_type_rolled == fm::f_missing_gear ? uint64_t(3 + int(uniform01() * 6) % 6) : 0xff;
    fm::ShiftSeverity sev = fm::slow_shift_severity(uniform01(), uniform01());
    uint64_t magnitude = gp_fault_type_rolled == fm::f_slow_shift ? uint64_t(sev.scale) : 0;
    if (gp_fault_type_rolled == fm::f_rear_brakes) {
        start_rear_brake_fault(fm::rear_brake_change(uniform01(), uniform01()), 0);
        return;
    }
    if (fm::power_kind_of_fault(gp_fault_type_rolled) != pl::none) {
        start_power_fault(gp_fault_type_rolled, spec.seconds ? duration : 0.0, false);
        return;
    }
    fault_request = uint64_t(gp_fault_type_rolled) | (uint64_t(duration * 10.0) << 8) |
                    (uint64_t(spec.seconds ? 1 : 0) << 40) | (gear << 48) | (magnitude << 56);
    swofty::log("[td-probe] gp failures: partial failure now: %s", fm::fault_name(gp_fault_type_rolled));
}

void gp_warning_tick(double dt)
{
    (void)dt;
    int o = gp_warn.outcome;
    if (o < 0 || gp_at < 0 || gp_at >= 1e17 || gp_clock < gp_warn.warn_at) {
        power_warn = {};
        brake_fade = 0.0f;
        shown_warning = -1;
        return;
    }
    shown_warning = o == fm::gearbox ? -1 : o;
    double lead = std::max(1.0, gp_at - gp_warn.warn_at), t = gp_clock - gp_warn.warn_at;
    if (!gp_warn.announced) {
        gp_warn.announced = true;
        swofty::log("[td-probe] gp failures: warning signs for the %s start (%.0f s of driving left at full load)",
                    fm::outcome_name(o), gp_at - gp_clock);
        if (o == fm::gearbox) {
            // The gearbox declines in stages and fails at the end itself (its own clock, nursed the same way).
            start_gearbox_decline(gp_at - gp_clock, uniform01() < 0.5 ? fm::decline_hydraulics : fm::decline_lost_gear);
            gp_at = 1e18;
            return;
        }
        int kind = fm::warning_kind(o, uniform01());
        power_warn = pl::start(kind, lead, uniform01, true);
        const char *text = "";
        switch (kind) {
        case pl::leak: text = "Oil pressure is dropping. Lift and coast to save the engine."; break;
        case pl::dead_cylinder: text = "We have a misfire, the engine is losing a cylinder. Short shift, keep it going."; break;
        case pl::mguk_loss: text = "MGU-K temperatures are high, it is derating. Manage it."; break;
        case pl::battery_derate: text = "Battery temperatures are high, it is derating on the straights. Lift and coast."; break;
        case pl::turbo: text = "The turbo is losing boost. Lift and coast, short shift."; break;
        case pl::mguh: text = "MGU-H problem, the battery is not recharging. Manage the energy."; break;
        case pl::electrical: text = "We see an electrical problem. Keep going for now."; break;
        default:
            if (o == fm::brakes)
                text = "Brake temperatures are critical. Lift and coast into the braking zones.";
            break;
        }
        if (kind != pl::none) {
            pl::State end = power_warn;
            end.t = lead;
            swofty::log("[td-probe] power: warning signs as %s over %.0f s, %.0f %% less power at the end%s",
                        pl::kind_name(kind), lead, double(pl::steady_loss(end)) * 100.0,
                        pl::cutting(end) || pl::rough(end) ? ", with cut-outs or rough running" : "");
        }
        if (swofty::hud::available() && *text)
            swofty::hud::toast("ENGINEER", text, 6);
        static const int line_of[fm::n_outcomes] = {radio_engine_a, radio_mgu,     radio_turbo,
                                                    radio_energy_store, radio_turbo, radio_control_electronics,
                                                    radio_gearbox,  -1,            radio_brakes};
        radio_say(line_of[o]);
    }
    // The course runs on the failure clock, which nursing slows.
    if (power_warn.kind != pl::none && t > power_warn.t)
        pl::advance(power_warn, t - power_warn.t, uniform01);
    brake_fade = o == fm::brakes ? float(fm::brake_fade(t / lead)) : 0.0f;
}

// ---- Partial power faults (MGU-K lost, battery derate, misfire) and the engine power each frame ----
void start_power_fault(int type, double length, bool quick)
{
    int kind = fm::power_kind_of_fault(type);
    if (kind == pl::none)
        return;
    power_fault = pl::start(kind, length, uniform01, false, quick);
    power_fault_pitted = false;
    power_fault_test = quick;
    power_fault_brake_at = !quick && kind == pl::mguk_loss && uniform01() < fm::mguk_brake_chance
                               ? fm::mguk_brake_after(uniform01()) : -1.0;
    char how[96] = "", until[32] = "to the flag";
    if (kind == pl::battery_derate)
        snprintf(how, sizeof how, " above 260 km/h, building over %.0f s, worst until %.0f s", power_fault.build_s,
                 power_fault.hold_s);
    else if (kind == pl::mguk_loss && power_fault.intermittent)
        snprintf(how, sizeof how, ", comes and goes");
    else if (kind == pl::mguk_loss && power_fault.build_s > 0)
        snprintf(how, sizeof how, ", half of it for the first %.0f s", power_fault.build_s);
    else if (kind == pl::misfire)
        snprintf(how, sizeof how, ", rough running");
    if (length > 0)
        snprintf(until, sizeof until, "for %.0f s", length);
    swofty::log("[td-probe] fault: %s: %.0f %% less power%s, %s", fm::fault_name(type), double(power_fault.depth) * 100.0,
                how, until);
    if (power_fault_brake_at >= 0)
        swofty::log("[td-probe] fault: the rear brakes will overheat after %.0f s without the MGU-K", power_fault_brake_at);
    const char *text = kind == pl::mguk_loss ? "We have lost the MGU-K, about 160 horsepower down. Keep going."
                       : kind == pl::battery_derate ? "Battery temperatures are high, it is derating on the straights. "
                                                      "Lift and coast to cool it."
                                                    : "We have a misfire, about 80 horsepower down. We are looking for a fix.";
    radio_say(kind == pl::mguk_loss ? radio_mgu : kind == pl::battery_derate ? radio_ers_derating : radio_engine_b);
    if (swofty::hud::available())
        swofty::hud::toast("ENGINEER", text, 6);
}

void end_power_fault(const char *why)
{
    if (power_fault.kind == pl::none)
        return;
    swofty::log("[td-probe] fault: %s over after %.0f s (%s)", pl::kind_name(power_fault.kind), power_fault.t, why);
    // A test step's fault ends quietly: the next step starts at once with its own radio, and the "fixed" line queued
    // behind it (0.51.114 log: it played 6 s into the misfire step).
    if (!power_fault_test && (power_fault.kind == pl::misfire || power_fault.kind == pl::battery_derate)) {
        radio_say(radio_fault_fixed);
        if (swofty::hud::available())
            swofty::hud::toast("ENGINEER", power_fault.kind == pl::misfire ? "Misfire fixed, full power back."
                                                                          : "Battery temperatures are normal again, "
                                                                            "full deployment.", 5);
    }
    power_fault = {};
    power_fault_brake_at = -1;
    power_fault_test = false;
}

// Frame thread: the player's engine power share as an engine.power_scale layer (a scale below the engine kill's
// set, so a dead engine stays dead), or as the throttle cap when the core has no car layers.
void apply_power(float scale)
{
    namespace live = swofty::cars::live;
    static live::Layer layer;
    static float applied = 1.0f;
    static int applied_car = -1;
    static bool layers_ok = true, verified = false;
    int car = session_ended ? -1 : player_vehicle.load();
    scale = car < 0 ? 1.0f : std::clamp(std::round(scale * 100.0f) / 100.0f, 0.0f, 1.0f);  // whole percent
    if (!layers_ok) {
        warning_cap = scale;
        return;
    }
    if (scale == applied && car == applied_car)
        return;
    applied_car = car;
    if (scale >= 1.0f) {
        layer.release();
        applied = 1.0f;
        return;
    }
    layer = live::Layer::scale(live::Cars::car(live::Car(uint32_t(car))), "engine.power_scale", scale, 50);
    if (!layer.ok()) {
        layers_ok = false;
        warning_cap = scale;
        swofty::log("[td-probe] power: no engine.power_scale layer (result %d), the throttle cap takes the power loss",
                    int(layer.result()));
        return;
    }
    applied = scale;
    if (!verified) {
        verified = true;
        auto v = live::Car(uint32_t(car)).get(live::Field::engine_power_scale);
        swofty::log("[td-probe] power: engine.power_scale layer on car %d, now %.2f (asked %.2f)", car, v ? double(*v) : -1.0,
                    double(scale));
    }
}

void power_tick(double dt)
{
    bool on_track = !session_ended && player_vehicle.load() >= 0 && !player_broken && !retire_requested;
    bool driving = on_track && player_pit.load() == 1;
    if (power_fault.kind != pl::none) {
        if (!on_track) {
            // nothing: the car is out or the session over (reset_session clears it)
        } else if (player_pit.load() != 1) {
            power_fault_pitted = true;
        } else if (power_fault_pitted && power_fault.kind == pl::misfire) {
            end_power_fault("a power cycle in the pits fixed it");
        } else {
            double nurse = power_fault.kind == pl::battery_derate ? fm::nursing_factor(full_throttle_share.load()) : 1.0;
            pl::advance(power_fault, dt * nurse, uniform01);
            if (power_fault.length > 0 && power_fault.t >= power_fault.length) {
                end_power_fault("cleared");
            } else if (power_fault_brake_at >= 0 && power_fault.t >= power_fault_brake_at) {
                power_fault_brake_at = -1;
                if (rear_brake_loss_target.load() == 0) {
                    swofty::log("[td-probe] fault: without the MGU-K the rear brakes overheat");
                    start_rear_brake_fault(0.35 + 0.25 * uniform01(), 0);
                }
            }
        }
    }
    power_speed_wanted = power_warn.kind == pl::battery_derate || power_fault.kind == pl::battery_derate;
    float v = float(std::max(0, player_speed()));
    float scale = driving ? pl::power(power_warn, v) * pl::power(power_fault, v) : 1.0f;
    float lw = pl::steady_loss(power_warn), lf = pl::steady_loss(power_fault);
    shown_power_loss = on_track ? 1.0f - (1.0f - lw) * (1.0f - lf) : 0.0f;
    shown_power_kind = !on_track ? int(pl::none) : lf >= lw && power_fault.kind != pl::none ? power_fault.kind : power_warn.kind;
    shown_power_cuts = on_track && (pl::cutting(power_warn) || pl::cutting(power_fault));
    // The log: every change of the steady power by 2 % or more.
    static float logged = 1.0f;
    float steady = (1.0f - lw) * (1.0f - lf);
    if (std::fabs(steady - logged) >= 0.02f || (steady == 1.0f && logged != 1.0f)) {
        swofty::log("[td-probe] power: engine at %.0f %% (%s %.0f %%, %s %.0f %%)%s", double(steady) * 100.0,
                    pl::kind_name(power_warn.kind), double(lw) * 100.0, pl::kind_name(power_fault.kind),
                    double(lf) * 100.0, power_speed_wanted.load() ? ", the derate only above 230-260 km/h" : "");
        logged = steady;
    }
    apply_power(scale);
}

void gp_break(int outcome)
{
    swofty::log("[td-probe] gp failures: %s fails now", fm::outcome_name(outcome));
    if (outcome >= fm::ice && outcome <= fm::gearbox) {
        int v = player_vehicle.load();
        if (v >= 0 && v < 32 && failure_mgr.load()) {
            breakdown_part[v] = uint8_t(outcome);
            breakdown_request.fetch_or(1u << v);
        } else {
            trigger_failure(outcome);
        }
    } else if (outcome == fm::suspension) {
        susp::collapse(int(uniform01() * 4) & 3, "mechanical failure");
        radio_say(radio_mech_dnf);
    } else if (outcome == fm::brakes) {
        if (swofty::hud::available())
            swofty::hud::toast("BRAKE FAILURE", "Retire the car", 4);
        radio_say(radio_mech_dnf);
        player_broken = true;
        retire_in = 3.0;
    }
}

// Gearbox decline: stage 0 at the start, 1 at half, 2 at four fifths of the length, then the gearbox fails through the
// game's breakdown sequence. The first stage registers the game's own fault for its radio (slow shifts or the lost
// gear, for the rest of the race); the shift time comes from decline_shift_scale (slow_shift_tick).
void start_gearbox_decline(double length, int path)
{
    gearbox_decline = {};
    gearbox_decline.path = path;
    gearbox_decline.length = std::max(10.0, length);
    if (path == fm::decline_lost_gear)
        gearbox_decline.gear = 3 + int(uniform01() * 6) % 6;  // 2nd-7th
    swofty::log("[td-probe] gearbox decline: %s, the gearbox fails after %.0f s of driving",
                path == fm::decline_hydraulics ? "hydraulics (slow shifts)" : "lost gear", gearbox_decline.length);
}

void gearbox_decline_tick(double dt)
{
    gearbox_decline_state &d = gearbox_decline;
    if (d.path < 0)
        return;
    if (session_ended || player_broken || player_vehicle.load() < 0 || retire_requested) {
        d = {};
        decline_shift_scale = 0.0f;
        shown_decline_stage = -1;
        return;
    }
    if (player_pit.load() != 1 || pending_failure.load())
        return;  // the clock stands in the pit lane
    d.clock += dt * fm::nursing_factor(full_throttle_share.load());
    int stage = fm::decline_stage(d.clock / d.length);
    shown_decline_stage = stage < 3 ? stage : -1;
    if (stage == d.stage)
        return;
    d.stage = stage;
    if (stage >= 3) {
        swofty::log("[td-probe] gearbox decline: the gearbox fails now");
        shown_decline_stage = -1;
        d = {};
        decline_shift_scale = 0.0f;
        gp_break(fm::gearbox);
        return;
    }
    int scale = fm::decline_shift_scale(d.path, stage, uniform01());
    decline_shift_scale = scale > 1 ? float(scale) : 0.0f;
    static const char *const gear_names[] = {"reverse", "neutral", "1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th"};
    char text[128] = {};
    if (stage == 0) {
        // The game's fault for its radio: 999 laps (the rest of the race), started at once. A test run's decline asks
        // for three times its length in seconds instead: a run stopped early would leave slow shifts to the flag.
        uint64_t type = d.path == fm::decline_hydraulics ? fm::f_slow_shift : fm::f_missing_gear;
        uint64_t tenths = d.test ? uint64_t(std::min(9990.0, 30.0 * d.length)) : 9990;
        fault_request = type | (tenths << 8) | (uint64_t(d.test ? 1 : 0) << 40) | (uint64_t(d.gear) << 48) |
                        (uint64_t(d.path == fm::decline_hydraulics ? scale : 0) << 56);
        if (d.path == fm::decline_hydraulics)
            snprintf(text, sizeof text, "Gearbox pressure is dropping, shifts are slow. Short shift, stay off the kerbs.");
        else
            snprintf(text, sizeof text, "We have lost %s gear. Skip it, stay off the kerbs.",
                     d.gear < 10 ? gear_names[d.gear] : "a");
    } else if (stage == 1) {
        snprintf(text, sizeof text, "Gearbox is getting worse, shifts are rough. Short shift, no kerbs.");
        radio_say(radio_gearbox);
    } else {
        snprintf(text, sizeof text, "We are losing the gearbox. Keep it going as long as you can.");
        radio_say(radio_gearbox_jam);
    }
    swofty::log("[td-probe] gearbox decline: stage %d after %.0f of %.0f s, shift time x%d", stage, d.clock, d.length,
                scale);
    if (swofty::hud::available())
        swofty::hud::toast("ENGINEER", text, 6);
}

// Test run steps: a baseline, every partial failure for 45 s (seconds, so each ends with the "fixed" radio), then a
// 90 s gearbox decline that ends in the gearbox failure. Each step logs the shifts it saw.
struct test_step {
    const char *name;
    int fault;       // fm::FaultType, 0 = none, -1 = gearbox decline, -2 radio lines
    int gear;        // missing gear index (4 = 3rd), 0xff none
    int magnitude;   // slow shifts: shift time scale; rear brakes: % lost (< 0 gained)
    double seconds;  // how long the step runs
};
constexpr test_step test_steps[] = {
    {"baseline", 0, 0xff, 0, 40},
    {"slow shifts x8", fm::f_slow_shift, 0xff, 8, 80},
    {"missing 3rd gear", fm::f_missing_gear, 4, 0, 80},
    {"gear jammed", fm::f_gear_jammed, 0xff, 0, 80},
    {"DRS failure", fm::f_drs, 0xff, 0, 80},
    {"ERS modes locked", fm::f_ers_modes, 0xff, 0, 80},
    {"ERS glitch", fm::f_ers_glitch, 0xff, 0, 80},
    {"lean fuel", fm::f_lean_fuel, 0xff, 0, 80},
    {"rear brakes weak", fm::f_rear_brakes, 0xff, 60, 60},
    {"rear brakes grabbing", fm::f_rear_brakes, 0xff, -80, 60},
    {"MGU-K lost", fm::f_mguk_loss, 0xff, 0, 50},
    {"battery derate", fm::f_battery_derate, 0xff, 0, 50},
    {"misfire", fm::f_misfire, 0xff, 0, 40},
    {"radio lines", -2, 0xff, 0, 7.0 * int(n_radio_lines) + 5},
    {"gearbox decline", -1, 0xff, 0, 120},
};
constexpr int n_test_steps = int(sizeof test_steps / sizeof test_steps[0]);
// When you drive (about 5 min): what changed in 0.51.114 and what you can feel. The log compares each power step's
// full-throttle acceleration with the steps without a power fault. The grabbing rear brakes come late: they spun
// the car into the wall in the 0.51.114 test, before the weak brakes and the gearbox decline had run.
constexpr test_step drive_steps[] = {
    {"baseline", 0, 0xff, 0, 30},
    {"MGU-K lost", fm::f_mguk_loss, 0xff, 0, 40},
    {"battery derate", fm::f_battery_derate, 0xff, 0, 40},
    {"misfire", fm::f_misfire, 0xff, 0, 30},
    {"rear brakes weak", fm::f_rear_brakes, 0xff, 60, 40},
    {"rear brakes grabbing", fm::f_rear_brakes, 0xff, -80, 40},
    {"gearbox decline", -1, 0xff, 0, 60},
};
constexpr int n_drive_steps = int(sizeof drive_steps / sizeof drive_steps[0]);
struct test_run_state {
    bool active = false;
    bool ai = false;      // the AI drives (else you do)
    bool paused = false;  // the car is out: a flashback to before the damage continues the run
    bool decline_over = false;  // the gearbox decline step: the decline failed, the game's breakdown comes
    int step = -1, reason_try = 0;
    double clock = 0, step_clock = 0, moving_check = 0;
    double speed_sum = 0, speed_time = 0;  // km/h x s this step, for the average
    int top_speed = 0;
    int vehicle = -1;
};
test_run_state test_run;
constexpr int32_t test_reasons[] = {14, 7};  // ROBODRIVER, BENCHMARK

// Ctrl+Shift+F4: one press starts the run with you driving once no second press came within test_double_ms, two
// presses start it with the AI driving, a press while it runs stops it. Returns 0 nothing, 1 start (you drive),
// 2 start (AI drives), 3 stop.
constexpr uint64_t test_double_ms = 600;
uint64_t test_key_pending_at = 0;
int test_key_action(bool edge, uint64_t now, bool running)
{
    if (edge) {
        if (running) {
            test_key_pending_at = 0;
            return 3;
        }
        if (test_key_pending_at && now - test_key_pending_at <= test_double_ms) {
            test_key_pending_at = 0;
            return 2;
        }
        test_key_pending_at = now;
        return 0;
    }
    if (test_key_pending_at && now - test_key_pending_at > test_double_ms) {
        test_key_pending_at = 0;
        return 1;
    }
    return 0;
}
void test_run_reset()
{
    if (test_run.active) {
        swofty::log("[td-probe] test run: stopped (a new session)");
        power_summary_due = true;
    }
    test_run = {};
    test_run_on = false;
}

void test_run_log_shifts(const char *step, bool power_step = false)
{
    char gears[96] = {};
    int at = 0;
    for (int g = 0; g < 12; ++g)
        if (uint32_t n = test_shifts.gear_seen[g].load())
            at += snprintf(gears + at, sizeof gears - at, " [%d]=%u", g, n);
    uint32_t nu = test_shifts.count[0].load(), nd = test_shifts.count[1].load();
    swofty::log("[td-probe] test run: %s: %u upshifts avg %.0f ms max %.0f ms, %u downshifts avg %.0f ms max %.0f ms, "
                "shifts ended in gear index%s, speed avg %.0f top %d now %d km/h",
                step, nu, nu ? test_shifts.total_us[0].load() / 1000.0 / nu : 0.0, test_shifts.max_us[0].load() / 1000.0,
                nd, nd ? test_shifts.total_us[1].load() / 1000.0 / nd : 0.0, test_shifts.max_us[1].load() / 1000.0,
                at ? gears : " (none)", test_run.speed_time > 0 ? test_run.speed_sum / test_run.speed_time : 0.0,
                test_run.top_speed, game_speed.load());
    test_shifts.reset = true;
    brake_summary_power = power_step;
    brake_summary_step = step;
    test_run.speed_sum = test_run.speed_time = 0;
    test_run.top_speed = 0;
}

// The AI takes the car (on) or gives it back. Only the ROBODRIVER/BENCHMARK flag: a RETIRED or other AI reason the
// game set stays.
void test_run_hand_to_ai(bool on)
{
    void *sys = controller_system.load();
    if (test_run.ai && sys && !session_ended && test_run.vehicle >= 0 && test_run.vehicle == player_vehicle.load())
        orig_set_controller(sys, uint64_t(test_run.vehicle), on ? 1 : 0, test_reasons[test_run.reason_try]);
}

// The effects the test started end with it: its power fault, a timed rear brake fault, its gearbox decline.
void test_run_end_effects(const char *why)
{
    if (power_fault_test)
        end_power_fault(why);
    if (rear_brake_until.load()) {  // a timed one is the test's
        rear_brake_loss_target = 0.0f;
        rear_brake_until = 0;
    }
    if (gearbox_decline.test) {
        swofty::log("[td-probe] gearbox decline: ended with the test run");
        fault_request = 0;  // its slow-shift record, if the game thread has not registered it yet
        gearbox_decline = {};
        decline_shift_scale = 0.0f;
        shown_decline_stage = -1;
    }
}

void test_run_stop(const char *why)
{
    if (!test_run.active)
        return;
    swofty::log("[td-probe] test run: stopped (%s)", why);
    if (!test_run.paused)  // a paused run gave the car back already
        test_run_hand_to_ai(false);
    test_run = {};
    test_run_on = false;
    power_summary_due = true;
    test_run_end_effects("test run over");
    radio_pending = 0;  // 0.51.112 log: the radio lines step kept talking after the run stopped
    if (swofty::hud::available())
        swofty::hud::toast("TEST RUN", why, 4);
}

// Ctrl+Shift+F4 while the run goes: the step so far is logged (shifts, brakes, power) before it stops.
void test_run_stop_by_you()
{
    const test_run_state &t = test_run;
    const test_step *steps = t.ai ? test_steps : drive_steps;
    int n = t.ai ? n_test_steps : n_drive_steps;
    if (t.active && !t.paused && t.step >= 0 && t.step < n)
        test_run_log_shifts(steps[t.step].name, fm::power_kind_of_fault(steps[t.step].fault) != pl::none);
    test_run_stop("stopped by you");
}

void test_run_start(bool ai)
{
    void *sys = controller_system.load();
    int v = player_vehicle.load();
    if (session_ended || !sys || v < 0 || player_pit.load() != 1) {
        swofty::log("[td-probe] test run: needs a running session with your car on track (system %p, vehicle %d, "
                    "pit state %d)", sys, v, player_pit.load());
        return;
    }
    test_run = {};
    test_run.active = true;
    test_run.ai = ai;
    test_run.vehicle = v;
    test_shifts.reset = true;
    brake_tel_reset = true;
    test_run_on = true;
    if (!ai) {
        // You drive: the steps begin at once.
        test_run.step = 0;
        swofty::log("[td-probe] test run: started, you drive vehicle %d", v);
        if (swofty::hud::available())
            swofty::hud::toast("TEST RUN", "You drive: MGU-K lost, battery derate, misfire, weak and grabbing rear "
                                           "brakes, gearbox decline (about 5 min). Flat out on the straights. "
                                           "Ctrl+Shift+F4 stops.",
                               6);
        return;
    }
    swofty::log("[td-probe] test run: started, handing vehicle %d to the AI (%s)", v, label(reasons, test_reasons[0]));
    orig_set_controller(sys, uint64_t(v), 1, test_reasons[0]);
    if (swofty::hud::available())
        swofty::hud::toast("TEST RUN", "The AI drives, the addon runs every fault (about 12 min)", 5);
}

// Start step t.step.
void test_run_begin_step(const test_step *steps, int n_steps)
{
    test_run_state &t = test_run;
    t.step_clock = 0;
    t.decline_over = false;
    const test_step &n = steps[t.step];
    swofty::log("[td-probe] test run: step %d of %d: %s", t.step, n_steps - 1, n.name);
    if (n.fault == -1) {
        // The step waits for the car to stop (test_run_tick).
        start_gearbox_decline(std::max(30.0, 0.75 * n.seconds), fm::decline_hydraulics);
        gearbox_decline.test = true;
    } else if (n.fault == fm::f_rear_brakes) {
        start_rear_brake_fault(n.magnitude / 100.0, n.seconds - 5);
    } else if (fm::power_kind_of_fault(n.fault) != pl::none) {
        start_power_fault(n.fault, n.seconds - 2, true);
    } else if (n.fault == -2) {
        // Every engineer line in turn (6 s apart on the game thread), so the log names what you hear.
        for (int i = 0; i < n_radio_lines; ++i)
            radio_say(i, true);
    } else if (n.fault > 0) {
        // 45 s, counted in seconds (record +0x1d), started at once.
        fault_request = uint64_t(n.fault) | (uint64_t(450) << 8) | (uint64_t(1) << 40) | (uint64_t(n.gear) << 48) |
                        (uint64_t(n.magnitude) << 56);
    }
}

// The car is out (a crash or a failure): the run waits, its step's effects end, the AI gives the car back. The
// gearbox decline as the last step ends in the planned failure: the run is done.
void test_run_pause(const test_step *steps, int n_steps)
{
    test_run_state &t = test_run;
    const test_step *s = t.step >= 0 && t.step < n_steps ? &steps[t.step] : nullptr;
    test_run_log_shifts(s ? s->name : "last step", s && fm::power_kind_of_fault(s->fault) != pl::none);
    if (s && s->fault == -1 && t.step == n_steps - 1) {
        test_run_stop("all steps done, the gearbox failed as planned");
        return;
    }
    if (t.step >= n_steps) {
        test_run_stop("the car is out, all steps were done");
        return;
    }
    if (confirmed_retired_vehicle.load() >= 0) {
        test_run_stop("the car retired");
        return;
    }
    test_run_end_effects("the car is out");
    test_run_hand_to_ai(false);
    t.paused = true;
    swofty::log("[td-probe] test run: paused in %s, the car is out; a flashback to before the damage continues the "
                "run", s ? s->name : "the wait for the AI");
    if (swofty::hud::available())
        swofty::hud::toast("TEST RUN", "Paused: the car is out. Flash back to before the damage to continue.", 5);
}

// The car is whole again (a flashback to before the damage, or a repair in practice): the run goes on with the next
// step, or the same step again if less than half of it ran.
void test_run_resume(const test_step *steps, int n_steps)
{
    test_run_state &t = test_run;
    t.paused = false;
    test_run_hand_to_ai(true);
    if (t.step < 0) {
        t.clock = 0;
        swofty::log("[td-probe] test run: continues, the car is whole again; waiting for the AI to drive");
        return;
    }
    if (t.step >= n_steps) {
        swofty::log("[td-probe] test run: continues, the car is whole again (all steps done, the AI keeps driving)");
        return;
    }
    bool again = t.step_clock < steps[t.step].seconds / 2;
    if (!again && ++t.step >= n_steps) {
        test_run_stop("all steps done");
        return;
    }
    swofty::log("[td-probe] test run: continues, the car is whole again (flashback or repair), with %s",
                again ? "the same step" : "the next step");
    if (swofty::hud::available())
        swofty::hud::toast("TEST RUN", steps[t.step].name, 4);
    test_run_begin_step(steps, n_steps);
}

void test_run_tick(double dt)
{
    test_run_state &t = test_run;
    bool restarted = test_run_restarted.exchange(false);
    if (!t.active)
        return;
    if (restarted) {
        test_run_stop("the race restarted");
        return;
    }
    if (session_ended || player_vehicle.load() != t.vehicle) {
        test_run_stop("session ended");
        return;
    }
    const test_step *steps = t.ai ? test_steps : drive_steps;
    int n_steps = t.ai ? n_test_steps : n_drive_steps;
    bool out = player_broken || confirmed_retired_vehicle.load() >= 0;
    if (t.paused) {
        if (confirmed_retired_vehicle.load() >= 0)
            test_run_stop("the car retired");
        else if (!out && player_pit.load() == 1)
            test_run_resume(steps, n_steps);
        return;
    }
    if (out) {
        test_run_pause(steps, n_steps);
        return;
    }
    t.clock += dt;
    t.step_clock += dt;
    if (int v = game_speed.load(); v >= 0) {
        t.speed_sum += v * dt;
        t.speed_time += dt;
        t.top_speed = std::max(t.top_speed, v);
    }
    // The AI must move the car: after 15 s try the next controller reason.
    if (t.step < 0) {
        if (t.clock < 15)
            return;
        if (game_speed.load() < 20) {
            void *sys = controller_system.load();
            if (sys && t.reason_try + 1 < int(sizeof test_reasons / sizeof test_reasons[0])) {
                orig_set_controller(sys, uint64_t(t.vehicle), 0, test_reasons[t.reason_try]);
                ++t.reason_try;
                swofty::log("[td-probe] test run: the car stands (%d km/h), trying %s", game_speed.load(),
                            label(reasons, test_reasons[t.reason_try]));
                orig_set_controller(sys, uint64_t(t.vehicle), 1, test_reasons[t.reason_try]);
                t.clock = 0;
                return;
            }
            test_run_stop("the AI does not drive the car");
            return;
        }
        swofty::log("[td-probe] test run: the AI drives (%d km/h with %s)", game_speed.load(),
                    label(reasons, test_reasons[t.reason_try]));
        t.step = 0;
        t.step_clock = 0;
        test_shifts.reset = true;
        return;
    }
    if (t.step >= n_steps)
        return;  // done: the AI keeps the car until Ctrl+Shift+F4 (no hand-back at full speed)
    const test_step &s = steps[t.step];
    if (s.fault == -1 && gearbox_decline.path < 0 && !t.decline_over) {
        t.decline_over = true;
        swofty::log("[td-probe] test run: the gearbox decline is over, the game's breakdown stops the car (about 40 s)");
    }
    if (t.step_clock < s.seconds)
        return;
    // The gearbox decline step ends when the car stops (test_run_pause: the planned end), at most after three times its
    // time: nursing slows the decline's clock, and the game's breakdown stops the car about 40 s after the decline
    // (0.51.115 log: failed at 08:00:05, planned by the game at 08:00:35, the car out at 08:00:43; the step had ended
    // the run at 08:00:18).
    if (s.fault == -1 && t.step_clock < 3 * s.seconds)
        return;
    test_run_log_shifts(s.name, fm::power_kind_of_fault(s.fault) != pl::none);
    // A step's own effects end with it.
    if (power_fault_test && fm::power_kind_of_fault(s.fault) != pl::none)
        end_power_fault("test step over");
    if (++t.step >= n_steps) {
        if (!t.ai) {
            test_run_stop("all steps done");
            return;
        }
        swofty::log("[td-probe] test run: all steps done, the AI keeps driving (Ctrl+Shift+F4 takes the car back)");
        if (swofty::hud::available())
            swofty::hud::toast("TEST RUN", "All steps done. Ctrl+Shift+F4 takes the car back.", 6);
        return;
    }
    test_run_begin_step(steps, n_steps);
}

// Damage panel (issue #15): a header and a row of icon tiles at the top right (damage_icons.hpp), in the race
// HUD's style: engine, ERS, gearbox, brakes, suspension, power. Bar, icon and label block in yellow, orange or red
// by how bad it is. "auto" shows the badges with a problem, "always" all six while on track, "off" none.
// Render thread: reads atomics only.
dicons::icon icon_of_outcome(int o)
{
    switch (o) {
    case fm::mguk:
    case fm::es:
    case fm::ce:
        return dicons::ers;
    case fm::gearbox:
        return dicons::gearbox;
    case fm::suspension:
        return dicons::suspension;
    case fm::brakes:
        return dicons::brakes;
    default:
        return dicons::engine;  // ICE, turbo, MGU-H
    }
}

// The badges the panel shows now, in icon order (also used by the test). all: the fine ones too.
int damage_badges(dicons::badge *out, bool all)
{
    using namespace dicons;
    badge b[n_icons];
    for (int i = 0; i < n_icons; ++i)
        b[i].icon = uint8_t(i);
    // Raise a badge to level l; the label of the worst problem wins, the first one on a tie.
    auto raise = [&](int i, int l, const char *fmt = nullptr, double v = 0) {
        badge &x = b[i];
        if (l < x.level || (l == x.level && (x.label[0] || !fmt)))
            return;
        if (l > x.level)
            x.label[0] = 0;
        x.level = uint8_t(l);
        if (fmt)
            snprintf(x.label, sizeof x.label, fmt, v);
    };
    uint32_t mask = terminal_mask.load() | live_terminal_mask.load();
    if (mask & 1)
        raise(engine, broken, "OUT");
    if (mask & 2)
        raise(ers, broken, "OUT");
    if (mask & 4)
        raise(gearbox, broken, "OUT");
    if (int part = game_failure_part.load(); part >= 0 && !(mask & 7))
        raise(icon_of_outcome(part), broken, "OUT");
    if (int w = shown_warning.load(); w >= 0)
        raise(icon_of_outcome(w), problem);
    if (shown_power_cuts.load())
        raise(power, problem, "CUTS");
    else if (float loss = shown_power_loss.load(); loss >= 0.02f)
        raise(power, problem, "-%.0f%%", double(loss) * 100.0);
    if (int k = shown_power_kind.load(); k != pl::none && (shown_power_loss.load() >= 0.02f || shown_power_cuts.load()))
        raise(k == pl::mguk_loss || k == pl::battery_derate || k == pl::mguh ? ers : engine, problem);
    if (int st = shown_decline_stage.load(); st >= 0)
        raise(gearbox, st == 0 ? minor : st == 1 ? problem : broken);
    if (float sc = shown_shift_scale.load(); sc > 1.0f)
        raise(gearbox, problem, "x%.0f", double(sc));
    if (int g = shown_missing_gear.load(); g >= 2 && g <= 10)
        raise(gearbox, problem, "NO %.0f", double(g - 1));
    if (float r = rear_brake_loss_target.load(); r != 0)
        raise(brakes, problem, r > 0 ? "-%.0f%%" : "+%.0f%%", std::fabs(double(r)) * 100.0);
    // Corners RL, RR, FL, FR (2 bits each: fine, bent, weakened, collapsed) from the addon's model; a corner the
    // crash broke (broken_corners of crash_corners) is red too, limping or not (0.51.127: the game broke the FR, the
    // model had only bent the RR, the tile said RR; 0.51.128: a wheel the game only damaged, a flat tyre, was red).
    // The suspension tile names the worst corners ("FL", "FRONT", "x3"), limping says "LIMP".
    uint32_t corners = susp::corner_view.load(), collapsed = 0;
    for (int i = 0; i < 4; ++i)
        if (((corners >> (2 * i)) & 3) == 3)
            collapsed |= 1u << i;
    bool limping = limp_state.load() == 1;
    uint32_t crash = broken_corners(crash_corners.load(), collapsed);
    int worst = fine;
    for (int i = 0; i < 4; ++i) {
        uint8_t st = uint8_t((corners >> (2 * i)) & 3);
        if ((crash >> i) & 1)
            st = broken;
        b[suspension].wheel[i] = st;
        worst = std::max(worst, int(st));
    }
    if (limping)
        raise(suspension, broken, "LIMP");
    if (worst != fine) {
        unsigned at_worst = 0;
        for (int i = 0; i < 4; ++i)
            if (b[suspension].wheel[i] == worst)
                at_worst |= 1u << i;
        raise(suspension, worst, corner_label(at_worst));
    }
    int n = 0;
    for (int i = 0; i < n_icons; ++i)
        if (all || b[i].level != fine)
            out[n++] = b[i];
    return n;
}

// dicons pen on the Swofty overlay.
struct overlay_pen {
    SwoftyPaint *p;
    void line(float x0, float y0, float x1, float y1, float w, uint32_t c)
    {
        swofty::overlay::line(p, x0, y0, x1, y1, w, c);
    }
    void polyline(const float *xy, int n, float w, uint32_t c, bool closed)
    {
        swofty::overlay::polyline(p, xy, uint32_t(n), w, c, closed);
    }
    void polygon(const float *xy, int n, uint32_t c) { swofty::overlay::polygon(p, xy, uint32_t(n), c); }
    void circle(float cx, float cy, float r, uint32_t c) { swofty::overlay::circle(p, cx, cy, r, c); }
    // dicons rings are centred on r; the overlay draws its outline inside the radius.
    void ring(float cx, float cy, float r, float w, uint32_t c)
    {
        swofty::overlay::circle_outline(p, cx, cy, r + w / 2, w, c);
    }
    void arc(float cx, float cy, float r, float a0, float a1, float w, uint32_t c)
    {
        swofty::overlay::arc(p, cx, cy, r, a0, a1, w, c);
    }
    void rrect(float x, float y, float w, float h, float r, uint32_t c)
    {
        swofty::overlay::rect_rounded(p, x, y, w, h, r, c);
    }
    void rrect_outline(float x, float y, float w, float h, float r, float t, uint32_t c)
    {
        swofty::overlay::rect_rounded_outline(p, x, y, w, h, r, t, c);
    }
    void text(float x, float y, float size, uint32_t c, const char *s, int align)
    {
        namespace ov = swofty::overlay;
        ov::TextStyle st;
        st.size = size;
        st.rgba = c;
        st.tracking = size * 0.04f;
        st.align = (align == 0 ? ov::align::left : align == 1 ? ov::align::center : ov::align::right) |
                   ov::align::middle;
        ov::text(p, x, y, st, s);
    }
};

void paint_damage_panel(SwoftyPaint *paint, void *)
{
    namespace ov = swofty::overlay;
    std::string_view mode = damage_panel;
    if (mode == "off" || session_ended || player_vehicle.load() < 0)
        return;
    // The session is over for you (NOT_IN_SESSION: finished or retired, the results and the replays after them):
    // no panel (0.51.124: it stayed up after the finish and over the replay).
    if (player_controller_flags.load() & (1u << 3))
        return;
    dicons::badge b[dicons::n_icons];
    int n = damage_badges(b, mode == "always");
    if (n == 0)
        return;
    // Top right, under the lap timing box (21 % down the safe area).
    ov::Rect safe = ov::safe_area(paint);
    overlay_pen pen{paint};
    dicons::draw_panel(pen, safe.x + safe.w, safe.y + safe.h * 0.21f, ov::ui_scale(paint), b, n);
}

void on_event(const SwoftyEvent *ev, void *)
{
    if (!strcmp(ev->name, SWOFTY_EVENT_SESSION_START)) {
        session_laps = 0;
        session_laps_read = false;
        player_crossings = 0;
        reset_session();
        session_ended = false;
    } else if (!strcmp(ev->name, SWOFTY_EVENT_SESSION_END)) {
        if (int n = player_crossings.load(); n > 0)
            swofty::log("[td-probe] session end: your car crossed the line %d times (the game's lap count %d, length "
                        "index %d)", n, game_race_laps.load(), game_race_length.load());
        game_race_laps = -1;  // the next session's failure scheduler call reads them again
        game_race_length = -1;
        session_ended = true;
        reset_session(true);
        pit_lane_system = 0;
        controller_system = nullptr;
    }
    swofty::log("[td-probe] event %s a=%lld b=%lld", ev->name, (long long)ev->a, (long long)ev->b);
    if (!strcmp(ev->name, SWOFTY_EVENT_RACE_START)) {
        // What the fixed-chance failures can spread over (gp_point_tick).
        const char *source = "";
        if (int laps = race_total_laps(&source); laps > 0)
            swofty::log("[td-probe] race: %d laps (%s)", laps, source);
        else
            swofty::log("[td-probe] race: lap count unknown (no Swofty telemetry, no session context, the game's race "
                        "settings say %d laps, length index %d); 10-50 %% failures come within 15 minutes of driving",
                        game_race_laps.load(), game_race_length.load());
    }
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
    guarded_read(module + 0x60, now[32]);
    for (int i = 0; i < 32 && now[32]; i++)
        guarded_read(now[32] + 0x838 + uintptr_t(i) * 8, now[i]);
    if (!memcmp(now, last, sizeof now))
        return;
    memcpy(last, now, sizeof now);
    int v = player_vehicle.load();
    uintptr_t instance = 0;
    if (v >= 0 && v < 0x20)
        guarded_read(module + 0x68 + uintptr_t(v) * 8, instance);
    swofty::log("[td-probe] garage: crew slots: table %p, player vehicle %d instance %p",
                reinterpret_cast<void *>(now[32]), v, reinterpret_cast<void *>(instance));
    for (int i = 0; i < 32; i++) {
        if (!now[i])
            continue;
        uint64_t q[12] = {};
        for (int k = 0; k < 12; k++)
            guarded_read(now[i] + k * 8, q[k]);
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
        flashback_tracker.packet(b, size_t(n), GetTickCount64());
        damage_flashback_tracker.packet(b, size_t(n), GetTickCount64());
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

// Player speed in km/h, read from the game's car every frame (read_game_speed); UDP telemetry is the fallback. -1 if
// neither is fresh (paused, between sessions).
int player_speed()
{
    if (session_ended)
        return -1;
    uint64_t now = GetTickCount64(), game = game_speed_ms.load();
    if (game && game > telemetry_not_before.load() && now - game <= 500)
        return game_speed.load();
    int p = udp_player.load();
    uint64_t last = udp_last_ms.load();
    if (p < 0 || p >= udp_cars || !last || last <= telemetry_not_before.load() || now - last > 500)
        return -1;
    return udp_speed[p].load();
}

// A broken car that stands still for 10 s retires, like the game's own retirement: on track, and also stranded in the
// pit lane outside the box (EnteringPitLane, LeavingPitLane, driving through). The box states 3-13 have their own flow.
bool standstill_pit_state(int pit)
{
    return pit == 1 || pit == 2 || pit == 14 || pit == 15;
}
// Race: the grace for a car crawling on a broken corner counts only while it drives on track; standing still
// the 10 s standstill retirement takes over, in the pit lane the box flow. Held while the game holds the car.
void race_stop_tick(double dt)
{
    double left = race_stop_left.load();
    if (left <= 0)
        return;
    if (!player_broken || limp_state.load() != 1 || session_ended || practice_like()) {
        race_stop_left = 0;
        return;
    }
    int speed = player_speed();
    if (player_pit.load() != 1 || retire_in.load() != -1.0 || speed <= 5 ||
        (player_controller_flags.load() & ((1u << 10) | (1u << 11))))
        return;
    left -= dt;
    race_stop_left = left > 0 ? left : 0;
    if (left > 0)
        return;
    swofty::log("[td-probe] crash: the car was not stopped within %.0f s of driving on a broken corner, retiring",
                race_stop_grace);
    if (swofty::hud::available())
        swofty::hud::toast("RETIRED", "Unsafe car: you did not stop", 4);
    retire_in = 0.0;
}

// For 30 s after a crash decision, every 2 s: what the car does (0.51.130; a test report: in Career practice a limping car
// showed 35 % throttle but would not drive or shift). Gear index 0 reverse, 1 neutral, 2 first; the gearbox refuses
// every shift while flags 0x1c are set (0x144742350: 0x04 stuck, 0x08 timed, 0x10 a missing gear asked for), and
// flags 0x03 are its failure. Corners: 3 bits each, RL RR FL FR (corner_states).
void limp_watch_tick(double dt)
{
    static double since = 0;
    double left = limp_watch_left.load();
    if (left <= 0) {
        since = 0;
        return;
    }
    limp_watch_left = left - dt > 0 ? left - dt : 0;
    since += dt;
    if (since < 2.0)
        return;
    since = 0;
    uintptr_t car = player_car(), gearbox = 0;
    int32_t gear = -1, target = -1;
    uint8_t flags = 0;
    float wear = -1;
    if (car && guarded_read(car + 0x538, gearbox) && gearbox) {
        guarded_read(gearbox + 0x680, gear);
        guarded_read(gearbox + 0x684, target);
        guarded_read(gearbox + 0x658, flags);
        guarded_read(gearbox + 0x674, wear);
    }
    bool ok = false;
    uint32_t corners = car ? corner_states(car, ok) : 0;
    // 0.51.133: the engine's state flags (engine+0x2530: bit 0 blown, bit 1 seized, 0x18 ERS faults) and the UDP rpm
    // (-1 without fresh UDP data), to find what switches the engine off after the game's own terminal crash damage.
    uintptr_t engine = 0;
    uint8_t engine_flags = 0xff;
    if (car && guarded_read(car + 0x530, engine) && engine && !guarded_read(engine + 0x2530, engine_flags))
        engine_flags = 0xff;
    int p = udp_player.load(), rpm = -1;
    if (p >= 0 && p < udp_cars && udp_last_ms.load() && GetTickCount64() - udp_last_ms.load() < 1000)
        rpm = udp_rpm[p].load();
    swofty::log("[td-probe] limp watch: speed %d km/h, gear index %d (target %d), gearbox flags 0x%02x wear %.2f, "
                "corners 0x%03x, throttle cap %.2f, engine kill %d, engine flags 0x%02x, rpm %d, limp %d, controller "
                "flags 0x%x, pit state %d, the game's damage rating %d", player_speed(), gear, target, flags,
                double(wear), ok ? corners : 0xfffu, double(throttle_hook.cap), engine_kill_vehicle.load(), engine_flags,
                rpm, limp_state.load(), player_controller_flags.load(), player_pit.load(), player_rating.load());
}

void standstill_retire(double dt)
{
    int speed = player_speed();
    if ((player_controller_flags.load() & ((1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<10)|(1u<<11))) || !player_broken || retire_in.load() != -1.0 || !standstill_pit_state(player_pit.load()) || speed < 0) {
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
        swofty::log("[td-probe] retire: your broken car stood still for 10 s (pit state %d), retiring", player_pit.load());
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
    // Only when the game's own speed never arrived this session: while paused (flashback screen) it is just stale
    // (0.51.94 log 20261103-094910 warned during a flashback). Only time without it counts (0.51.114 log: the time
    // before a race restart made it warn at the restart).
    bool game_speed_seen = game_speed_ms.load() > telemetry_not_before.load();
    waited = v >= 0 && !fresh && !game_speed_seen && player_pit.load() == 1 ? waited + dt : 0;
    if (waited > 15 && warned_vehicle != v && !udp_last_ms.load() && !game_speed_seen && player_speed() < 0) {
        warned_vehicle = v;
        swofty::log("[td-probe] speed: no speed from the game's car and no UDP car telemetry (packets %llu). Set Telemetry Settings: UDP on, "
                    "127.0.0.1, port %u, format 2026. Without it, standing still does not retire a broken car (use the "
                    "retire hotkey).",
                    (unsigned long long)udp_packets.load(), unsigned(udp_port()));
    }
    since += dt;
    if (!log_telemetry || since < 3.0)
        return;
    since = 0;
    if (!fresh) {
        if (player_speed() >= 0)
            swofty::log("[td-probe] speed: %d km/h from the game's car (no UDP), cap=%.2f", player_speed(),
                        double(throttle_hook.cap));
        return;
    }
    swofty::log("[td-probe] udp: player %d (vehicle %d) speed=%d km/h rpm=%d gear=%d throttle=%.2f cap=%.2f", p, v,
                udp_speed[p].load(), udp_rpm[p].load(), udp_gear[p].load(), udp_throttle[p].load(),
                double(throttle_hook.cap));
}

// Optional telemetry diagnostics: engine flags on change, then rpm/gear/speed
// every second for 20 s after failure. This is not needed for audio shutdown.
void probe_engine(double dt)
{
    if (!log_telemetry)
        return;
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

void abandon_unavailable_push()
{
    // No real crew means no push reports: synthesizing them teleported a
    // race car into the garage when the practice mapping was not available.
    crew_spawn_wanted = false;
    crew_seq_t = -1.0;
    crew_request_in = -1.0;
    crew_msg_in = -1.0;
    crew_request = 0;
    crew_msg = 0;
    pending_crew = -1;
    pushing = false;
    pushed = false;
    retire_in = 2.0;
    swofty::log("[td-probe] garage: no usable crew; retiring in the box without push reports (load practice first)");
    if (swofty::hud::available())
        swofty::hud::toast("NO GARAGE CREW", "Load practice first; retiring in your box", 5);
}

bool expire_timer(std::atomic<double> &timer,double dt,double inactive=-1.0)
{
    double old=timer.load();
    while (old>=0) {
        double next=old-dt;
        if (timer.compare_exchange_weak(old,next<0 ? inactive : next)) return next<0;
    }
    return false;
}
bool active_push_work() { return !session_ended && keep_control && player_broken && pushing; }

void publish_player_state()
{
    int vehicle=player_vehicle.load(), retired=confirmed_retired_vehicle.load();
    uint32_t state=TD_UNAVAILABLE, mask=terminal_mask.load();
    if (retired>=0 && retired<max_vehicles && (vehicle<0 || vehicle==retired)) {
        vehicle=retired;state=TD_RETIRED;
    } else if (!session_ended.load() && vehicle>=0 && vehicle<max_vehicles) {
        if (player_pit.load()==9) state=TD_IN_GARAGE;
        else if (retire_requested || pushing || retire_in.load()>=0) state=TD_RETIRING;
        else if (player_broken || car_dead || mask) state=TD_LIMPING;
        else state=TD_RUNNING;
    } else {vehicle=-1;mask=0;}
    td_state::service.publish(state,vehicle,mask);
}

// Frame thread: keep the engine.power_scale = 0 layer on exactly the car in engine_kill_vehicle.
void apply_engine_kill()
{
    namespace live = swofty::cars::live;
    static live::Layer layer;
    static int applied = -1;
    int want = session_ended ? -1 : engine_kill_vehicle.load();
    if (want == applied)
        return;
    if (applied >= 0) {
        layer.release();
        swofty::log("[td-probe] engine: drive torque back for car %d", applied);
    }
    applied = want;
    if (want < 0)
        return;
    layer = live::Layer::set(live::Cars::car(live::Car(uint32_t(want))), "engine.power_scale", 0.0f, 100);
    auto v = live::Car(uint32_t(want)).get(live::Field::engine_power_scale);
    if (layer.ok())
        swofty::log("[td-probe] engine: switched off for car %d, no drive torque (engine.power_scale now %.2f)", want,
                    v ? double(*v) : -1.0);
    else
        swofty::log("[td-probe] engine: could not cut the drive torque of car %d (result %d), throttle cap only", want,
                    int(layer.result()));
}

// The game's own wheel detach limits (F1SuspensionDamage WheelDetachScale W 1.0 and PartsDetachScale P 1.5,
// re/collision-impact.md section 6): a corner whose damage D reaches P is the game's "wheel coming off", its terminal
// crash damage, and that switches the engine off (limp_chance). It was 60 % of the crashes that killed the car in
// 0.51.127-0.51.130 (18 of 30), 14 of the 18 below D 3.0; the front wing's overflow adds into D too (0x14465fdc0: the
// part over the wing's cap / its damage scale x the front damage scale), it does not set D to P. With the addon's
// suspension model on (test report after 0.51.132: 'the car is always dead'), W and P are game_detach_factor x higher (W 2,
// P 3, the wheel gone at 6), so a moderate hit is the model's to judge (hold, bend, break now or later, limp) and only
// a big crash still tears the wheel off. The player's car, or every car when the model runs for the AI too; without
// the AI model the AI keeps the game's limits.
constexpr float game_detach_factor = 2.0f;

// Frame thread: the two scale layers by field id (the layer path names are the core's), moved with the player's car
// (a restart in another car) or on every car. Without the core's car layers the game's limits stay.
void apply_detach_limits()
{
    static uint32_t tokens[2] = {};
    static int applied = -2;  // -2 none, -1 every car, >= 0 the player's car
    static bool failed = false, late_logged = false;
    static uint64_t retry_at = 0;
    if (failed || !susp::enabled)
        return;
    int player = player_vehicle.load();
    int want = susp::ai::enabled ? -1 : player >= 0 ? player : applied;
    if (want == applied || want == -2 || GetTickCount64() < retry_at)
        return;
    auto t = swofty::detail::live_car_ops_v2();
    if (!t)
        return;  // the core's car layers are not ready yet (or missing): tried again next frame
    for (uint32_t &token : tokens)
        if (token)
            t->unlayer(std::exchange(token, 0u));
    applied = want;
    const uint32_t fields[2] = {SWOFTY_CAR_SUSP_WHEEL_DETACH_SCALE, SWOFTY_CAR_SUSP_PARTS_DETACH_SCALE};
    uint32_t index = want < 0 ? SWOFTY_CAR_ALL : uint32_t(want);
    for (int k = 0; k < 2; ++k) {
        int r = t->layer("addon", index, fields[k], SWOFTY_CAR_OP_SCALE, game_detach_factor, 50, &tokens[k]);
        if (r != SWOFTY_ASSET_OK) {
            for (uint32_t &token : tokens)
                if (token)
                    t->unlayer(std::exchange(token, 0u));
            applied = -2;
            if (r == SWOFTY_ASSET_LATE) {  // the car or the flow thread is not there yet: again in 2 s
                retry_at = GetTickCount64() + 2000;
                if (!std::exchange(late_logged, true))
                    swofty::log("[td-probe] suspension: the game's wheel detach limits not raised yet (result 1), "
                                "trying again");
                return;
            }
            failed = true;
            swofty::log("[td-probe] suspension: the game's wheel detach limits stay (no car layer, result %d)", r);
            return;
        }
    }
    float w = -1, p = -1;
    if (player >= 0 && t->get) {
        if (t->get(uint32_t(player), nullptr, fields[0], &w) != SWOFTY_ASSET_OK)
            w = -1;
        if (t->get(uint32_t(player), nullptr, fields[1], &p) != SWOFTY_ASSET_OK)
            p = -1;
    }
    swofty::log("[td-probe] suspension: the game's wheel detach limits x%.1f on %s (your car now: wheel detach %.2f, "
                "parts detach %.2f; -1 until it is built): moderate hits are the suspension model's",
                double(game_detach_factor), want < 0 ? "every car" : "your car", double(w), double(p));
}

void on_frame(double dt, void *)
{
    td_perf::Scope frame_scope(td_perf::frame_total);  // destroyed last: includes the player state publish
    struct PublishOnExit {
        ~PublishOnExit()
        {
            td_perf::Scope publish(td_perf::frame_state);
            publish_player_state();
            td_state::service.dispatch();
        }
    } publish_on_exit;
    td_perf::report();
    td_perf::report_frame(!session_ended && player_vehicle.load() >= 0);
    {
        td_perf::Scope menu(td_perf::frame_menu);
        report_pit_clearance();
        if (race_restart_pending.exchange(false)) {
            reset_race_faults();
            swofty::log("[td-probe] restart: the last race's failures, faults and plans are cleared");
        }
        red_flag_toast_tick();
        preset_tick();
        welcome_tick(GetTickCount64());
        apply_engine_kill();
        apply_detach_limits();
    }
    if (session_ended)
        return;
    {
        td_perf::Scope input(td_perf::frame_input);
        process_radio_command();
        uint64_t request = pending_failure.load();
        if (request && GetTickCount64() >= (request >> 16) && pending_failure.compare_exchange_strong(request, 0))
            swofty::log("[td-probe] debug failure: request timed out without a player update");
        poll_hotkey();
    }
    {
        td_perf::Scope suspension(td_perf::frame_suspension);
        susp::tick(debug_suspension_test, flashback_count.load());
    }
    {
        td_perf::Scope cars(td_perf::frame_cars);
        part_lock_tick();
        static uint32_t crew_flashbacks = 0;
        if (uint32_t fb = flashback_count.load(); fb != crew_flashbacks) {
            crew_flashbacks = fb;
            remove_owned_crew_after_flashback();
        }
    }
    {
        td_perf::Scope failures(td_perf::frame_failures);
        gp_failure_tick(dt);
    }
    {
        td_perf::Scope test(td_perf::frame_test);
        test_run_tick(dt);
    }
    {
        td_perf::Scope power(td_perf::frame_power);
        power_tick(dt);
    }
    td_perf::Scope rest(td_perf::frame_cars);  // to the end: probes, crew pushes, retire timers
    probe_crew_entry(dt);
    probe_speed(dt);
    probe_engine(dt);
    if (player_controller_flags.load() & (1u<<5)) return;
    if (expire_timer(crew_request_in,dt) && active_push_work()) post_crew_event(crew_request.exchange(0));
    if (int request=pending_crew.exchange(-1); request>=0 && active_push_work() && crew_spawn_wanted)
        post_crew_event(uint32_t(request));
    if (expire_timer(crew_msg_in,dt) && active_push_work()) post_crew_msg(crew_msg.exchange(0));
    if (spawn_crew && crew_spawn_now.exchange(false) && active_push_work() && !crew_spawn_wanted.exchange(true)) {
        if (spawn_player_crew()) crew_seq_t=0.0;
        else abandon_unavailable_push();
    }
    if (double t=crew_seq_t.load(); t>=0 && active_push_work() && crew_spawn_wanted) {
        double n=t+dt;
        if (crew_seq_t.compare_exchange_strong(t,n>=0.8 ? -1.0 : n)) {
            if (t<0.3 && n>=0.3 && active_push_work()) post_crew_event(crew_prepare_push);
            if (t<0.8 && n>=0.8 && active_push_work()) post_crew_event(crew_push);
        }
    }
    standstill_retire(dt);
    race_stop_tick(dt);
    limp_watch_tick(dt);
    // A retirement during the red flag hand-over (controller flags 10/11, about 5.5 s before the teleport) ended the
    // race at once and left the camera on another car (0.50.2 log 900136fe). The standstill timer already waits
    // (0.50.13); the box, push and game-failure timers wait here too.
    constexpr uint32_t held_flags = (1u << 10) | (1u << 11);
    bool held = (player_controller_flags.load() & held_flags) && retire_in.load() >= 0;
    static bool held_logged = false;
    if (held && !held_logged)
        swofty::log("[td-probe] retire: timer %.1f s held while the game holds the car (flags 0x%x)", retire_in.load(),
                    player_controller_flags.load());
    held_logged = held;
    if (!held && expire_timer(retire_in,dt,-2.0) && player_broken) {
        if (practice_like()) {
            if (player_pit.load() == 1) {
                garage_withdrawn = true;  // stopped on track: abandoned on the circuit (FIA 26.18 / B1.8.9)
                // 0.51.123 test: nothing said why the car stayed in its garage and the engine change did not run.
                if (swofty::hud::available())
                    swofty::hud::toast("CAR WITHDRAWN",
                                       "Your car stopped on track: out for this session (FIA). Skip Session to go on.", 8);
            }
            garage_return_wanted = true;
        } else {
            retire_player();
        }
    }
}

}

extern "C" SWOFTY_EXPORT const TdApi *TerminalDamageGetApi(uint32_t version)
{
    return version==TERMINAL_DAMAGE_API_VERSION ? &td_state::api : nullptr;
}

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

// ---- Presets (presets.hpp) ----
namespace {
constexpr const char *addon_id = "nick.terminal-damage-probe";
constexpr const char *preset_file = "preset_applied.txt";  // the preset applied last, in the addon's storage
std::atomic<bool> preset_changed{false}, preset_option_changed{false};  // set by the settings watchers

// An option a preset sets, as the settings command takes it ("true"/"false" for a toggle); empty if unknown.
std::string preset_current(std::string_view o)
{
    auto tf = [](bool b) { return std::string(b ? "true" : "false"); };
    auto ch = [](std::string_view v) { return std::string(v); };
    if (o == "keep_control") return tf(keep_control);
    if (o == "keep_game_failure") return tf(keep_game_failure);
    if (o == "gp_failures") return ch(gp_failures);
    if (o == "damaged_power") return ch(damaged_power);
    if (o == "red_flag_retire") return tf(red_flag_retire);
    if (o == "game_failure_timeout") return ch(game_failure_timeout);
    if (o == "damage_panel") return ch(damage_panel);
    if (o == "engineer_radio") return tf(engineer_radio);
    if (o == "pause_menu_retire") return tf(pause_menu_retire);
    return {};
}

struct preset_state {
    bool started = false;
    uint64_t start_at = 0;
    int applying = -1;  // the preset being applied, -1 none
    int entry = 0, set = 0, failed = 0, tries = 0;
    bool restart = false, in_game = false;  // in_game: picked in ADDON SETTINGS, not in the launcher
    uint64_t next_try = 0, quiet_until = 0;  // no Custom check until the applied values have settled
    std::string stored;
    bool has_stored = false;
    bool legacy = false;  // the kept file is from before 0.75 (no ";v2" mark): Custom was the default then
    std::string popup;  // the result for the menus (a toast is drawn only in a session), empty: none
    uint64_t first = 0, popup_next = 0;
};
preset_state preset_rt;

void preset_store(std::string_view v)
{
    if (preset_rt.has_stored && preset_rt.stored == v && !preset_rt.legacy)
        return;
    auto r = swofty::storage::write(preset_file, std::string(v) + ";v2");
    preset_rt.stored = std::string(v);
    preset_rt.has_stored = true;
    preset_rt.legacy = false;
    if (r != swofty::storage::Result::ok)
        swofty::log("[td-probe] preset: could not save which preset was applied (storage %d); a preset picked in the "
                    "launcher then applies at every start", int(r));
}

void preset_begin(int p, const char *why)
{
    preset_rt.applying = p;
    preset_rt.entry = preset_rt.set = preset_rt.failed = preset_rt.tries = 0;
    preset_rt.restart = false;
    preset_rt.in_game = !strcmp(why, "picked in the game");
    preset_rt.next_try = 0;
    swofty::log("[td-probe] preset: applying %s (%s)", presets::titles[p], why);
}

// The result for the player. In testing the option list did not change with the preset. The open ADDON SETTINGS screen
// keeps the rows it showed (the core publishes them when the screen opens), and the launcher runs no addon code: a
// preset picked there applies at the next game start. Both are said here and in the preset's description
// (0.51.135).
void preset_result_text(char *out, size_t n, int p, const preset_state &st)
{
    char failed[80] = "";
    if (st.failed)
        snprintf(failed, sizeof failed, " %d could not be set: update Swofty, or set them by hand.", st.failed);
    if (st.set == 0 && st.failed == 0)
        snprintf(out, n, "%s: every option already matches.", presets::titles[p]);
    else
        snprintf(out, n, "%s%s: %d option(s) changed.%s%s%s", presets::titles[p],
                 st.in_game ? "" : " (picked in the launcher)", st.set,
                 st.in_game && st.set ? " Reopen ADDON SETTINGS to see them." : "",
                 st.restart ? " Some need a game restart." : "", failed);
}

// One pass over the table: each option the preset sets differently goes through the settings command. A "late"
// answer is tried again after half a second (up to 20 times), anything else is logged and skipped.
void preset_apply_step(uint64_t now)
{
    preset_state &st = preset_rt;
    if (now < st.next_try)
        return;
    int p = st.applying;
    while (st.entry < presets::n_entries) {
        const presets::Entry &e = presets::table[st.entry];
        if (preset_current(e.option) == e.value[p]) {
            ++st.entry;
            st.tries = 0;
            continue;
        }
        auto r = swofty::hub::set_option(addon_id, e.option, e.value[p]);
        if (r == swofty::hub::Result::late && ++st.tries <= 20) {
            st.next_try = now + 500;
            return;
        }
        if (r == swofty::hub::Result::ok) {
            ++st.set;
            st.restart |= e.restart;
        } else {
            ++st.failed;
            swofty::log("[td-probe] preset: %s could not be set to %s (%s)", e.option, e.value[p], swofty::hub::name(r));
        }
        ++st.entry;
        st.tries = 0;
    }
    swofty::log("[td-probe] preset: %s applied, %d option(s) changed%s%s", presets::titles[p], st.set,
                st.failed ? ", some could not be set (update Swofty, or set them by hand)" : "",
                st.restart ? "; some take effect after a game restart" : "");
    // In the menus a toast is not drawn, so there it is a popup (preset_tick).
    char text[200];
    preset_result_text(text, sizeof text, p, st);
    if (!session_ended && player_vehicle.load() >= 0) {
        if (swofty::hud::available())
            swofty::hud::toast("PRESET", text, 6);
    } else {
        st.popup = text;
        st.popup_next = 0;
    }
    preset_store(presets::names[p]);
    st.applying = -1;
    st.quiet_until = now + 2000;
    preset_option_changed = false;
}

// Frame thread, every frame (menus too). At the start: apply a preset picked in the launcher, or turn the preset to
// Custom if one of its options was changed there. In the game: apply a picked preset; a changed option turns it to
// Custom.
void preset_tick()
{
    preset_state &st = preset_rt;
    uint64_t now = GetTickCount64();
    if (!st.first)
        st.first = now;
    // The result popup: at once for a pick in ADDON SETTINGS, on the main menu for a pick in the launcher.
    if (!st.popup.empty() && now >= st.popup_next && (session_ended || player_vehicle.load() < 0)) {
        st.popup_next = now + 1000;
        if (st.in_game || welcome_menu_ready(now - st.first)) {
            auto r = swofty::popup::show("TERMINAL DAMAGE PRESET", st.popup);
            swofty::log("[td-probe] preset: popup %s", swofty::popup::name(r));
            if (r != swofty::popup::Result::late)
                st.popup.clear();
        }
    }
    if (!st.started) {
        if (!st.start_at)
            st.start_at = now + 3000;  // let the core settle
        if (now < st.start_at)
            return;
        st.started = true;
        std::string kept;
        st.has_stored = swofty::storage::read(preset_file, kept) == swofty::storage::Result::ok;
        while (!kept.empty() && (kept.back() == '\n' || kept.back() == '\r' || kept.back() == ' '))
            kept.pop_back();
        // Since 0.75 the name is followed by ";v2"; a file without it was written while Custom was the default.
        size_t mark = kept.find(';');
        st.legacy = st.has_stored && mark == std::string::npos;
        if (mark != std::string::npos)
            kept.resize(mark);
        st.stored = kept;
        std::string cur = std::string(std::string_view(preset));
        int p = presets::index_of(cur);
        int differ = presets::differing(p, [](const char *o) { return preset_current(o); });
        switch (presets::at_start(st.has_stored, st.stored, cur, differ, st.legacy)) {
        case presets::Start::apply: preset_begin(p, "picked in the launcher"); break;
        case presets::Start::to_custom:
            swofty::log("[td-probe] preset: %d option(s) differ from %s, the preset shows Custom", differ,
                        presets::titles[p]);
            swofty::hub::set_option(addon_id, "preset", "custom");
            preset_store("custom");
            break;
        case presets::Start::remember: preset_store(cur); break;
        case presets::Start::nothing: break;
        }
        if (st.legacy && st.applying < 0)
            preset_store(st.stored);  // the kept file in the 0.75 form
        preset_changed = false;
        preset_option_changed = false;
        st.quiet_until = now + 2000;
        return;
    }
    if (preset_changed.exchange(false)) {
        std::string cur = std::string(std::string_view(preset));
        if (int p = presets::index_of(cur); p >= 0)
            preset_begin(p, "picked in the game");
        else
            preset_store(cur);
    }
    if (st.applying >= 0) {
        preset_apply_step(now);
        return;
    }
    if (now < st.quiet_until || !preset_option_changed.exchange(false))
        return;
    int p = presets::index_of(std::string_view(preset));
    if (int differ = presets::differing(p, [](const char *o) { return preset_current(o); }); p >= 0 && differ > 0) {
        swofty::log("[td-probe] preset: %d option(s) changed from %s, the preset shows Custom", differ,
                    presets::titles[p]);
        if (swofty::hub::set_option(addon_id, "preset", "custom") == swofty::hub::Result::ok)
            preset_store("custom");
    }
}

}  // namespace

SWOFTY_ON_CHANGE(preset, std::string_view)
{
    preset_changed = true;
}

// ---- Welcome popup (0.51.120): once per install, on the main menu, pointing to the preset and the option texts ----
namespace {
// In the addon's storage once the popup was queued. 0.51.121: a new name, as 0.51.120's welcome_shown.txt was
// written when the popup came with the legal screen (log 20261104-161542: 17:16:14, the main menu at 17:16:37).
constexpr const char *welcome_file = "welcome_seen.txt";
// F1 25's front-end home state: it enters with the main menu (logs 0.51.119 and 0.51.120: 7-10 s before the menu
// gets its ADDON SETTINGS item) and exits into a session.
constexpr uint32_t home_flow = 0x957880c6u;
struct welcome_state {
    uint64_t first = 0, next = 0;  // the first frame; the next look (twice a second)
    bool done = false;             // queued, shown before, or no storage to remember it in
};
welcome_state welcome_rt;

// The main menu is up: the home state entered 10 s ago or more. A game build without that state, or a core without
// the flow states: 3 minutes after the first frame.
bool welcome_menu_ready(uint64_t since_first_ms)
{
    swofty::career::Status st = swofty::career::Status::none;
    uint64_t age = 0;
    if (swofty::career::flow_status(home_flow, &st, &age) == swofty::career::Result::ok)
        return (st == swofty::career::Status::entering || st == swofty::career::Status::updating) && age >= 10000;
    return since_first_ms >= 180000;
}

std::string welcome_text()
{
    int p = presets::index_of(std::string_view(preset));
    char text[512];
    snprintf(text, sizeof text,
             "Terminal Damage is on. Pick how hard it is in ADDON SETTINGS > Terminal Damage > Preset:\n"
             "Casual: no random failures, forgiving rules.\n"
             "Realistic: real failure rates, FIA rules.\n"
             "Hardcore: Realistic, no damage panel, less power when broken.\n"
             "Your preset now: %s. Every option has a description; suspension damage is experimental and off. "
             "This message shows once.",
             p >= 0 ? presets::titles[p] : "Custom");
    return text;
}

// Frame thread, every frame until done. The core shows the popup at once on top of the current screen (it found the
// game's message system without waiting for a game message), so wait for the main menu.
void welcome_tick(uint64_t now)
{
    welcome_state &w = welcome_rt;
    if (w.done)
        return;
    if (!w.first) {
        w.first = now;
        return;
    }
    if (now < w.next || (!session_ended && player_vehicle.load() >= 0))
        return;  // in a session: wait for the menus
    w.next = now + 500;
    if (!welcome_menu_ready(now - w.first))
        return;
    if (!swofty::storage::available()) {
        w.done = true;
        swofty::log("[td-probe] welcome: no addon storage on this core, no welcome popup (it could not be shown once)");
        return;
    }
    std::string seen;
    auto r = swofty::storage::read(welcome_file, seen);
    if (r == swofty::storage::Result::late) {
        w.next = now + 5000;
        return;
    }
    w.done = true;
    if (r == swofty::storage::Result::ok)
        return;  // shown before
    auto shown = swofty::popup::show("TERMINAL DAMAGE", welcome_text());
    swofty::log("[td-probe] welcome: popup %s", swofty::popup::name(shown));
    if (shown == swofty::popup::Result::ok &&
        swofty::storage::write(welcome_file, std::string_view("shown 0.51.121\n")) != swofty::storage::Result::ok)
        swofty::log("[td-probe] welcome: could not save that the popup was shown; it comes again at the next start");
}
}  // namespace
SWOFTY_ON_CHANGE(keep_game_failure, bool) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(gp_failures, std::string_view) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(damaged_power, std::string_view) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(red_flag_retire, bool) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(game_failure_timeout, std::string_view) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(damage_panel, std::string_view) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(engineer_radio, bool) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(pause_menu_retire, bool) { preset_option_changed = true; }

SWOFTY_ON_CHANGE(keep_control, bool on)
{
    preset_option_changed = true;
    swofty::log("[td-probe] keep_control %s", on ? "on" : "off");
}

SWOFTY_ON_CHANGE(performance_diagnostics, bool on)
{
    td_perf::enabled = on;
    swofty::log("[td-probe] private-hook performance counters %s", on ? "on" : "off");
}

SWOFTY_ON_CHANGE(crew_asset_diagnostics, bool on)
{
    crew_diagnostics_active = on;
}

#include "pit_retirement_clearance.hpp"

SWOFTY_ON_LOAD
{
    td_perf::enabled = bool(performance_diagnostics);
    crew_diagnostics_active = bool(crew_asset_diagnostics);
    if (udp_port_choice != std::string_view("off"))
        CloseHandle(CreateThread(nullptr, 0, udp_thread, nullptr, 0, nullptr));
    else
        swofty::log("[td-probe] udp: off (speed comes from the game)");
    reset_session();
    hook_asset_diagnostics();
    patch_garage_assets_in_race();
    guard_crew_scene_read();
    hook_throttle();
    configure_player_wheels(player_wheel_behavior);
    hook_wheel_tether_test();
    hook_tether_plane_experiment();
    hook_tether_distance_experiment();
    hook_engine_audio();
    hook_retire_local_player();
    register_radio_menu_probe();
    hook_radio_probe();
    hook_qualifying_engine_repair();
    hook_garage_crash_repair();
    hook_garage_menu();
    hook_exit_garage();
    attach_session_events();
    hook_brake_step();
    if (swofty::overlay::available()) {
        auto r = swofty::overlay::on_paint(&paint_damage_panel);
        swofty::log("[td-probe] damage panel: %s", r == swofty::overlay::Result::ok ? "on the overlay" : "overlay refused");
    } else {
        swofty::log("[td-probe] damage panel: no overlay on this core");
    }
    hook_thunk("VehicleControllerSystem::setController", sig_set_controller,
               reinterpret_cast<void *>(&on_set_controller), reinterpret_cast<void **>(&orig_set_controller));
    hook_thunk("Ai MessagePitStateChanged handler", sig_pit_state_changed,
               reinterpret_cast<void *>(&on_pit_state_changed), reinterpret_cast<void **>(&orig_pit_state_changed));
    hook_update_vehicle();
    hook_replay_terminal_gate();
    hook_damage_rating();
    hook_flashback_rule();
    hook_flashback_used();
    hook_vslot("DeploymentPointsAccumulatorStoppedCars::update", sig_stopped_cars_update,
               reinterpret_cast<void *>(&on_stopped_cars_update), reinterpret_cast<void **>(&orig_stopped_cars_update));
    if (crew_asset_diagnostics)
        hook_vslot("DeploymentPointsAccumulatorDebris::update", sig_debris_update,
                   reinterpret_cast<void *>(&on_debris_update), reinterpret_cast<void **>(&orig_debris_update));
    find_terminal_message();
    find_status_request();
    find_crew_event();
    find_pit_entry();
    find_crew_msg();
    hook_pit_stop_job();
    hook_pit_clearance();
    find_garage_crew_spawn();
    find_hud_restart_cleanup();
    if (!hook_damage_session_restart()) {
        restart_cleanup_available=false;
        spawn_crew=nullptr;
        swofty::log("[td-probe] garage: restart cleanup unavailable; selective race spawning disabled");
    }
    hook_crew_states();
    hook_red_flag_strategy();
    find_crash_repair();
    susp::install(suspension_damage, debug_suspension_test, suspension_damage_ai);
    hook_change_vehicle_status();
    hook_failure_planner();
    hook_rules_enter();
    hook_failure_register();
    hook_failure_scheduler();
    detour("PitLaneSystem::setVehicleStatus", sig_set_vehicle_status, set_vehicle_status_prologue,
           sizeof set_vehicle_status_prologue, reinterpret_cast<void *>(&on_set_vehicle_status),
           reinterpret_cast<void **>(&orig_set_vehicle_status));
    for (const char *e : {SWOFTY_EVENT_SESSION_START, SWOFTY_EVENT_SESSION_END, SWOFTY_EVENT_RACE_START,
                          SWOFTY_EVENT_STATE, SWOFTY_EVENT_FLAG, SWOFTY_EVENT_RESULTS})
        swofty::events::subscribe(e, on_event);
    swofty::events::on_frame(on_frame);
    swofty::log("[td-probe] loaded version=0.75.0, keep_control=%d asset_diagnostics=%d garage_assets_in_race=%d crew_in_race=%d perf_counters=%d",
                bool(keep_control), bool(crew_asset_diagnostics), bool(garage_assets_in_race),
                bool(garage_crew_in_race), int(td_perf::enabled.load()));
}

SWOFTY_ADDON()
