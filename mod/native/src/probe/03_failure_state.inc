// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Radio lines, rear-brake, test-run, plan and power state.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

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
// Debug test run (Ctrl+Shift+F4): the game's AI drives your car (setController reason ROBODRIVER, BENCHMARK as the
// fallback) while the addon runs every partial failure in turn and logs what the gearbox did. test_run_on gates the
// game-thread shift measurement.
std::atomic<bool> test_run_on{false};
std::atomic<bool> test_run_restarted{false};  // the race restarted: the run stops on the next frame
// The race restarted (no session end or start comes): on_frame resets the race's failures and faults
// (reset_race_faults) on the next frame.
std::atomic<bool> race_restart_pending{false};
void test_run_reset();
void test_run_start(bool ai);
int test_key_action(bool edge, uint64_t now, bool running);
void test_run_stop_by_you();
void preset_tick();
void welcome_tick(uint64_t now);
bool welcome_menu_ready(uint64_t since_first_ms);
// The shift stats are filled on the game thread and read and reset per step of the test run.
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
// registered for the player's car on the game thread, 0 = none. Packed as type | duration*10 << 8 (32 bits) | seconds
// flag << 40 (1 seconds, 0 laps) | gear index << 48 (0xff none) | slow-shift scale << 56 (the record's effect size is
// the scale times 10, 0 = the game rolls it).
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
// Each car's team (0.75.6): the team id of every participant of the session, read at the first control change of a
// session (vehicle id -> registry id, "" unknown). The failure plans scale a car's rate with its team's (team_model.hpp).
// team_pending: a session just started and the ids are still to come; the AI plan waits for them for 8 s at most.
std::mutex team_mu;
std::vector<std::string> team_of_car(32);
std::atomic<bool> team_ids_ready{false};
std::atomic<bool> team_pending{false};
std::atomic<uint64_t> team_pending_at{0};
// Counts the times the ids were read: a plan made before that (the ids came late) is made again if nothing has started.
std::atomic<uint32_t> team_epoch{0};
// Each car's team key (the entrant's team stat id) and each team's finished Durability upgrades (done of all the
// department's nodes, total), read from the game's R&D data once a Career session (team_mu).
struct RndShare {
    uint32_t key = 0;
    int done = 0, total = 0;
};
std::vector<uint32_t> team_key_of_car(32, 0);
std::vector<RndShare> rnd_shares;
std::atomic<bool> rnd_shares_read{false};
std::atomic<bool> rates_logged{false};  // this session's team rates were logged (team_rates_log_tick)
uint32_t ai_plan_epoch = 0, player_plan_epoch = 0, fault_plan_epoch = 0;
bool ai_failure_started = false;
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
// Your car has moved this race (above moved_kmh). The Career weekend menu after qualifying runs the race's session
// with your car standing OnTrack in the game's pit records and no practice flag, and the race itself later runs in
// that same session (log 2026-11-06 08:45, context still qualifying_1): the failure clocks ran in the menu, a brake
// failure was planned and its warning signs shown there (Nick, 2026-11-08 03:25 Suzuka). They start once the car moves.
std::atomic<bool> player_has_moved{false};
constexpr int moved_kmh = 10;
// Share of the last minute the player ran at full throttle (game thread): lift and coast lowers it.
std::atomic<float> full_throttle_share{0.65f};
// Read by the throttle stub (game thread): when the car being updated is `car`, its throttle is capped at `cap`.
// `car` is set from updateVehicle on the game thread and only compared, never dereferenced by the stub.
struct alignas(16) throttle_hook_data {
    volatile uintptr_t car;
    volatile float cap;
};
throttle_hook_data throttle_hook{0, 1.0f};
// The emitted stubs read this record as raw memory: car at +0 as eight bytes (hook_throttle: cmp r12, [rax]; the tether
// plane stub: cmp rbx, [rdx]) and cap at +8 as a float (hook_throttle: vminss xmm0, xmm0, [rax+8]).
static_assert(offsetof(throttle_hook_data, car) == 0 && sizeof(throttle_hook.car) == 8,
              "the stubs read throttle_hook.car at +0 as eight bytes");
static_assert(offsetof(throttle_hook_data, cap) == 8 && sizeof(throttle_hook.cap) == 4,
              "the stubs read throttle_hook.cap at +8 as a float");
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
