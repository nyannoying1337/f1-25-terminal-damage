# Terminal Damage 0.51.8 beta

## Added

- Separate **Remove AI wheel tethers** option alongside **Remove player wheel tethers**. Both are **off by default** and require a game restart.
- Detached wheels become loose debris in the selected scope. Suspension damage and wheel collisions stay unchanged.
- Player-state interface for spectator addons to react to confirmed retirement and recovery. Terminal Damage does not switch cameras itself.

## Improved

- Reduce replay/flashback overhead when wheel-tether removal is enabled.
- Remove temporary automatic wheel diagnostics.
- Clarify the license and visible-credit requirements in the documentation.

## Known limitations

- Stopped retirement requires OnTrack; a car stranded in the pit lane or box exit does not trigger the 10-second timer.
- Rewinding into the middle of a garage push/box animation is not fully covered.
- Red flags during terminal damage can still cause AI/spectator camera issues.
- **Qualifying ICE replacement / Skip repairs and practice replacement timing: NOT TESTED, MIGHT BE BROKEN.**
- Hiding pit equipment does not guarantee collision removal. Intended for offline play; some cars and modes need more testing.

## Code reuse

Terminal Damage Attribution License 1.0 requires visible credit and a project link
when publishing a mod/project that uses substantial portions of this work. See
LICENSE. Earlier MIT releases keep their MIT terms; third-party SDK licenses stay
unchanged.

---

# Terminal Damage 0.51.3 beta

## Added

- Optional **Remove player wheel tethers** setting, **off by default**.
- Enable it if detached wheels pull or shake your car. Broken player wheels become loose debris; suspension damage and AI wheel tethers stay unchanged.
- **Restart the game after changing the setting.**

## Known limitations

- Stopped retirement requires OnTrack; a car stranded in the pit lane or box exit does not trigger the 10-second timer.
- Rewinding into the middle of a garage push/box animation is not fully covered.
- Red flags during terminal damage can still cause AI/spectator camera issues.
- **Qualifying ICE replacement / Skip repairs and practice replacement timing: NOT TESTED, MIGHT BE BROKEN.**
- Hiding pit equipment does not guarantee collision removal. Intended for offline play; some cars and modes need more testing.

---

# Terminal Damage 0.51.1 beta

## Fixed

- Stop the AI driving away after a voluntary retirement when you park on track.
- Power stays available until retirement completes. The car can still roll on a slope.

## Known limitations

- Stopped retirement requires OnTrack; a car stranded in the pit lane or box exit does not trigger the 10-second timer.
- Rewinding into the middle of a garage push/box animation is not fully covered.
- Red flags during terminal damage can still cause AI/spectator camera issues.
- **Qualifying ICE replacement / Skip repairs and practice replacement timing: NOT TESTED, MIGHT BE BROKEN.**
- Wheel-tether instability remains under investigation. Hiding pit equipment does not guarantee collision removal.

---

# Terminal Damage 0.51.0 beta

## Changed

- Clear tyre-stop equipment and seated tyre crew during race retirement, preserving the garage push crew and your teammate's stop.
- Learn garage assignments directly in the race; practice no longer needs to be loaded first.
- Fix restarting during a retirement push leaving the car in the garage or the HUD hidden.
- Improve flashback handling when retirement requests and later damage overlap, and cancel old crew/audio work after a rewind, restart or garage return.
- Keep the teammate-safe box-entry signal working when equipment clearance is disabled.
- Reduce temporary diagnostic logging during normal play.

## Known limitations

- Stopped retirement requires OnTrack; a car stranded in the pit lane or box exit does not trigger the 10-second timer.
- Rewinding into the middle of a garage push/box animation is not fully covered.
- Red flags during terminal damage can still cause AI/spectator camera issues.
- **Qualifying ICE replacement / Skip repairs and practice replacement timing: NOT TESTED, MIGHT BE BROKEN.**
- Collision removal is not guaranteed by hiding pit equipment. Offline sessions only; some cars and modes need more coverage.

## Code reuse

This release uses the Terminal Damage Attribution License 1.0. Published mods or
projects using substantial portions must visibly credit nyannoying and link to
Terminal Damage. Earlier MIT releases keep their MIT terms. See LICENSE.

---

# Terminal Damage 0.50.2 beta

## Fixed

- Instant Replay / flashbacks can now be opened after terminal damage while you still control your car. Rewinding to before the damage lets you continue driving again.
- Confirmed in practice, qualifying and race sessions. Crash damage and an engine failure were covered in the practice check.

## Known issues

- **Red flag while terminally damaged:** the game can take control or switch to the spectator view, followed by a delayed retirement. This is still under investigation and is not fixed in this update.
- **Qualifying ICE replacement / Skip repairs: NOT TESTED, MIGHT BE BROKEN.** The intended replacement time is at least 60 minutes so the remaining qualifying time expires.
- **Practice engine replacement timing: NOT TESTED, MIGHT BE BROKEN.** Normal game timing is intended to remain unchanged.
- Race garage pushing still requires loading practice first in the same game process. The latest pit-entry audio and missing-crew fallback changes still need in-game confirmation.

UDP format 2026 is required for the addon's telemetry features. For reports, send loader.log before restarting the game; include perf.log for performance problems.

---

# Terminal Damage 0.50.0 beta

## Added

- Retire through the radio command menu. The entry is last in the standard lists; request retirement, pull over and stop for 10 seconds, or drive into your box. Selecting it again does not cancel or fast-forward retirement.
- Optional engineer voice reply for radio retirement, controlled by the separate Retirement radio voice toggle (restart required).
- Flashback recovery: rewinding before terminal damage or a retirement request can release the addon's power/audio/garage lock once resumed UDP telemetry and restored live parts confirm recovery. Requires UDP format 2026.

## Fixed and improved

- Race/session restart cleanup, including stale garage crew requests and engine-audio shutdown state.
- Engine shutdown audio handling through retirement and pit entry.
- Missing race garage crew now produces an explanatory message and retirement in the box instead of fabricated push reports/garage teleportation. Load practice first in the same game process to enable the real race garage push.
- Qualifying garage-return crash fix and component-swap handling.
- Reduced repeated memory-query work in stopped-car and player-vehicle updates, removed obsolete AI-update work and unnecessary diagnostics. Paired local captures show substantially lower measured hook costs; this is not a guarantee that all FPS drops are fixed.
- Optional Debug: performance counters for private-hook timing in loader.log.

## Known issues / not tested

- **Qualifying engine replacement / Skip repairs: NOT TESTED, MIGHT BE BROKEN.** The intended behavior is a minimum 60-minute ICE replacement so Skip repairs consumes the remaining qualifying session. It has not been verified in game.
- **Practice engine replacement timing: NOT TESTED, MIGHT BE BROKEN.** The game's normal timing is intended to remain unchanged.
- **Flashbacks: Instant Replay/flashback entry can be blocked after terminal damage.** This is a known issue under investigation. The recovery path cannot help if replay entry is blocked. UDP format 2026 and a fresh pre-damage timeline sample are required. Very short rewinds near the failure may retain the lock.
- Latest pit-entry audio and missing-crew fallback changes still need in-game confirmation. Race garage pushing still requires loading practice first in the same game process.
- Career component replacement is detected when leaving the garage. Without a new part, the car may briefly appear at pit exit before it is returned.

For reports, include loader.log and perf.log from the affected run before restarting the game.
