// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Session, crash, repair and plan state.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// The player's car has terminal damage the game was not told about, so they drive it themselves.
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
// The dead car in its garage was seen with a failed part. Atomic: set by power_loss (game thread), cleared by the pit
// state hook and return_to_garage (game thread) and by reset_car_state (game thread from power_loss, event thread from
// the session events), read by power_loss (game thread), part_lock_tick (frame thread) and the garage exit checks
// (garage_lock_reason, garage_exit_locked).
std::atomic<bool> garage_part_failed{false};
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
// The rest of the red flag state (29_red_flag.inc explains the flow). Declared here so reset_race_faults can clear all
// of it: a session that ended with the strategy screen open must not leave red_flag_screen_open or a stale
// red_flag_state behind, which would block the flashback and heavy damage checks or retire the next session's car.
std::atomic<uintptr_t> red_flag_state{0};
std::atomic<uint64_t> red_flag_toast_at{0};
std::atomic<bool> red_flag_screen_open{false};
std::atomic<int> red_flag_verdict{0};               // 1 mechanical, 2 corner repair failed, 3 abandoned
std::atomic<uint64_t> red_flag_repair_deadline{0};  // repair wait: retire if still terminal after this tick
std::atomic<bool> red_flag_fix_pending{false};      // corner/front wing repair due when the editor closes
std::atomic<bool> red_flag_fix_now{false};          // set by the red flag tick, done on the game thread
// Tick of the game's MessageFlashbackUsed (the player resumed from a flashback), 0 = none pending. Read by
// flashback_check on the game thread. Without the hook, a terminal record going back to normal on track stands in.
std::atomic<uint64_t> flashback_used_at{0};
std::atomic<uint32_t> flashback_count{0};  // every flashback, for the suspension damage undo
// Issue #10: AI cars after their terminal damage message, until they reach their garage (or are whole again). Per
// car: the tick of the message, the tick the car started to stand (0: moving), the tick the addon asked for its
// garage (ai_cleanup.hpp). ai_watch_asked has a bit for each car the addon asked, once.
std::atomic<uint32_t> ai_watch_mask{0};
std::atomic<uint32_t> ai_watch_asked{0};
std::atomic<uint64_t> ai_watch_since[32];
std::atomic<uint64_t> ai_watch_still_since[32];
std::atomic<uint64_t> ai_watch_asked_at[32];
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
extern bool enabled;  // the suspension model runs (suspension_damage.hpp)
}
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
// With the suspension model on (0.76): the game lap by which a car limping on a collapsed corner must be in the pit
// lane (the officials' black-and-orange flag: in at the end of the lap), -1 none. Replaces the 20 s stop.
std::atomic<int> race_stop_lap{-1};
// The same rule when the game's lap cannot be read: the line crossing count that retires the car, -1 none.
std::atomic<int> race_stop_crossing{-1};
std::atomic<uint32_t> crash_corners{0};
// Practice/qualifying garage repair: 0 none, 1 wanted, 2 job running, 3 finished. It rebuilds the corner a car limped
// in on (crash_corners) or, since 0.51.135, services the corners that took damage but hold (garage_service_corners,
// started by susp::tick when the car reaches its garage).
std::atomic<int> garage_repair{0};
std::atomic<uint32_t> garage_service_corners{0};  // bit i = corner i; 0: no service
std::atomic<float> garage_service_seconds{0};
std::atomic<bool> garage_service_crack{false};
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
bool send_ai_car_to_garage(uint8_t vehicle);  // issue #10, defined next to return_to_garage
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
// Damage panel (issue #15): values the game and frame threads publish for the render thread.
std::atomic<float> shown_shift_scale{0.0f};  // slow shifts in force (times the shift time), 0 none
std::atomic<int> shown_missing_gear{11};     // gearbox +0x664, 11 = none
std::atomic<int> shown_warning{-1};          // fm::Outcome whose warning signs run, -1 none
std::atomic<int> shown_decline_stage{-1};    // gearbox decline stage, -1 none
