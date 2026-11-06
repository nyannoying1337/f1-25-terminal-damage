// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#pragma once

// Issue #10: AI cars with terminal damage. The game removes them itself: about 14 s after the terminal message its
// virtual session sets a retired car InGarage (setVehicleStatus(v, 9)); a car that one of the addon's realistic
// failures broke stands 15 to 110 s first. The logs of 2026-11-03 to 06 (59 watched cars) also hold the cases where
// that removal never came: after a flashback inside the window four cars stood 4.5 minutes until the session ended
// (0.75.3), and one retired car was not removed for 131 s without any flashback (0.75.5). decide() says when the
// addon should do the stock removal itself. Pure logic; ai_watch_step (probe/11_power_loss.inc) gathers the inputs.
namespace aic {

inline constexpr int record_terminal = 5;       // the terminal damage record state of a terminal car
inline constexpr float still_kmh = 2.0f;        // below this speed a car stands
inline constexpr double track_after_s = 120.0;  // on track the game gets two minutes (the slowest removal seen: 110 s)
inline constexpr double track_still_s = 30.0;   //   and the car must have stood this long
inline constexpr double lane_after_s = 30.0;    // in the pit lane or at the pit exit the wreck blocks the others
inline constexpr double lane_still_s = 12.0;
inline constexpr double healed_after_s = 3.0;   // a car that is no longer terminal this long after its message is whole
inline constexpr double give_up_s = 400.0;      // stop watching a car after this long
inline constexpr double asked_wait_s = 15.0;    // how long to wait for the garage after asking

// Pit states of the game (pit_states[] in the probe): 0 InactiveInGarage, 1 OnTrack, 2 EnteringPitLane,
// 7 LeavingPitBox, 9 InGarage, 13 LeavingGarage, 14 LeavingPitLane, 15 InPitLane_DrivingThrough. The pit box and
// push states (3 to 6, 8) belong to the crew and are never touched. -1: no pit message seen for the car, so it has
// been on the track all session.
inline bool on_track(int pit) { return pit == -1 || pit == 1; }
inline bool in_pit_lane(int pit) { return pit == 2 || pit == 7 || pit == 13 || pit == 14 || pit == 15; }
inline bool in_garage(int pit) { return pit == 0 || pit == 9; }

enum class Action {
    keep_watching,
    in_garage,       // the game removed it: done
    healed,          // a flashback made it whole again: done
    give_up,         // watched long enough: done
    send_to_garage,  // do the removal now
};

struct Input {
    double since_terminal = 0;   // seconds since the terminal damage message
    double standing = 0;         // seconds the car has stood below still_kmh (0: moving, or speed unknown)
    int record = -1;             // terminal record state of the car, -1 unknown
    int pit = -1;                // observed pit state, -1 none seen
    bool racing = false;         // a race or sprint that is running: no safety car, red flag or formation lap
    bool enabled = true;         // the ai_terminal_cleanup option
    bool already_asked = false;  // the addon asked for this car already: once per car
};

inline Action decide(const Input &in)
{
    if (in_garage(in.pit))
        return Action::in_garage;
    if (in.since_terminal > healed_after_s && in.record >= 0 && in.record != record_terminal)
        return Action::healed;
    if (in.since_terminal > give_up_s)
        return Action::give_up;
    if (!in.enabled || !in.racing || in.already_asked)
        return Action::keep_watching;
    if (on_track(in.pit) && in.since_terminal >= track_after_s && in.standing >= track_still_s)
        return Action::send_to_garage;
    if (in_pit_lane(in.pit) && in.since_terminal >= lane_after_s && in.standing >= lane_still_s)
        return Action::send_to_garage;
    return Action::keep_watching;
}

}  // namespace aic
