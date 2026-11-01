# Terminal Damage (Swofty addon for F1 25) — beta

When your car breaks, the game normally takes it away from you on the spot. With this mod you keep
the car and deal with the failure like a real driver: limp on, pull over, or bring it to your box.

## 0.51.79 beta

- **No UDP telemetry needed any more.** The mod reads your speed from the game. Its own UDP
  listener is off by default, so SimHub, CrewChief and other apps keep port 20777.
- **Crashes:** one broken front corner may still let the car crawl back to the pits (dice roll).
  A rear corner or two broken corners: the car cannot be driven.
- **Red flags follow the FIA rules:** power unit / gearbox failures and abandoned cars retire; a
  car limping on one broken front corner may be repaired by the team (dice roll) and restart.
- **Practice and qualifying:** a car that limps into its garage is rebuilt there (20-45 minutes,
  the game's repair timer). A car that did not make it back is withdrawn for the session.
- **Disqualification stays DSQ** in the results instead of turning into DNF.

Full details: https://github.com/nyannoying1337/f1-25-terminal-damage/blob/main/CHANGELOG.md

## What it does

- **Radio menu: Retire:** request retirement while on track, then pull over and stop for 10 s
  or drive into your box. Nothing breaks and power stays available. Selecting it again does
  not cancel or speed up retirement. Requires Keep control after terminal damage.
  The engineer's reply can be disabled separately with Retirement radio voice (restart required).

- **You keep your car after terminal damage** (engine, power unit, gearbox or crash damage).
- **The failure shows up in how the car drives:**
  - engine (ICE) or gearbox failed: no drive, the car rolls out
  - turbo, MGU-K, MGU-H, battery or control electronics failed: reduced power (30 % throttle by
    default, adjustable)
  - terminal crash damage: no drive, the car rolls out
  - exception: **one broken front corner** may let the car crawl on with about a third of its
    power (70 % chance with a broken suspension, 35 % with a lost wheel). A message tells you.
    If the limping car breaks another corner, it stops.
- **Engine sound:** a failed engine plays the engine stop sound and stays silent. After terminal
  crash damage the engine dies after 3 to 6 s, and a broken or retiring car that stands still
  switches its engine off after 6 s (setting "Engine sound off after a failure").
- **The game's own random failures** are handled the same way as the failures above.
  **Experimental:** in Career and My Team the game's own failure chances and timing are used
  unchanged. When the game would hand your car to the AI, you keep control instead. Get to the
  pits before the end of the lap or retire, otherwise you are disqualified (setting "After a
  scripted failure..." switches this to a normal retirement). The results screen shows DSQ.
  The "Game's own failure" and "Game's own breakdown" Grand Prix modes are test options for this.
- **Stand still on track for 10 s** with a broken car: you retire.
- **Drive into the pit lane** with a broken car (race): no tyre stop. Your garage crew comes out,
  pushes the car into the garage, then you retire.
- **Red flag** while your car is terminally damaged (FIA rules):
  - engine, power unit or gearbox failure: cannot be repaired during a red flag, you retire
  - a car that could not be driven (abandoned on track): you retire
  - a car limping on one broken front corner: the team may rebuild it during the red flag
    (80 % chance for a suspension, 50 % for a lost wheel). Success: you take part in the
    restart. Failure: you retire.
  - other accident damage (wings, bodywork) is repaired by the game as usual
- **Practice and qualifying:** retiring would end the whole session, so your car goes back to its
  garage instead.
  - **limped in on a broken front corner:** the team rebuilds the corner, 20-30 minutes for a
    broken suspension, 30-45 minutes for a lost wheel (the game's repair timer). Wait, or use
    the game's skip / fast forward (uses session time). Then you can drive out again.
  - **did not make it back** (retired on track or Retire from Session): withdrawn for the rest of
    the session. The garage menu offers no drive out or flying lap; the car is ready again in the
    next session.
  - **engine or gearbox failure:** Career: fit a new part (e.g. a new engine) in the garage, then
    leave the garage. Without a new part the car is sent straight back to the garage.
- **Engine changes in qualifying:** a newly scheduled player ICE replacement takes at least
  60 minutes. Skip repairs should consume the remaining qualifying time. Practice replacement
  times and other repairs stay unchanged. **Not tested, might be broken.**
- **Ctrl+Shift+F11:** retire like a real driver. Nothing breaks and you keep full power: pull over
  and stand still for 10 s, or drive into your box. Press again to cancel while still on track.
- **Retire in the pause menu** works the same way on track (any controller or wheel): pull over or
  box. A HUD message confirms the request.
- Your stopped car does not trigger a safety car or red flag.

## Setup (once)

1. Steam version of F1 25 and Swofty Launcher 1.7.0 or newer.
2. Install the addon in the Swofty Launcher and restart the game.

The game's UDP telemetry is **not** needed. The *UDP telemetry port* setting only feeds the debug
telemetry log; leave it off.

## Each game start

Restart the game after installing or updating the addon. You can load a race
directly; no practice session or saved vehicle-ID mapping is needed.

## Settings

- **Power with a broken turbo/ERS part:** 20–50 % throttle, or no drive at all.
- **Retire hotkey:** on/off and which F key (with Ctrl+Shift).
- **Retirement radio voice:** on/off for the engineer's reply to radio-menu Retire.
  Restart the game after changing it. Retire remains usable with the voice off.
- **FIA red flag rules for a terminally damaged car:** on by default.
- Options starting with *Debug:* are for testing and bug reports. Leave them on their defaults
  (*Random*, *Normal*, off).

## Known issues (beta)

- Career has not been fully tested with the limping, red flag and garage repair rules yet.
- The garage repair was tested with a lost wheel; a broken suspension with the wheel still attached
  is not tested in game yet.
- Stopped retirement requires the car to be OnTrack. A car stranded in the pit lane
  or box exit (LeavingPitLane) does not start the 10-second timer.
- Rewinding into the middle of the garage push or box animation is not fully covered.
- A red flag during terminal damage can still cause AI/spectator camera issues.
- Qualifying ICE replacement / Skip repairs and practice replacement timing are
  **NOT TESTED, MIGHT BE BROKEN**.
- Hiding pit equipment does not establish that all associated collision is removed.
- AI cars with terminal damage can stop at the pit exit or on track.
- Career replacement is checked on send-out; without a new part the car can briefly reach the
  pit exit before returning.
- Real random failures, F2, time trial, split screen and career/My Team in depth need more testing.
- Game updates can disable hooks until signatures are updated.

Radio Retire is last in the four standard command lists. Swofty manages replacement of
speech_recognition.xml; another mod replacing it can conflict. No experimental file overlay
is required. Radio menu retirement disables the native action; Retirement radio voice chooses
voice/silent XML after restart. A repeated selection can request speech again. Other retirement
routes do not add a voice reply. Disable the addon to remove its command-file operation.

## Checks when reporting a problem

Include the session type, car, what happened before the failure, and whether you
used a flashback or restart. For a retirement problem, say whether the car was
fully on track or still in the pit lane. For a pit problem, mention a teammate stop.
Enable Debug: performance counters only when collecting a performance report.

## Reporting problems

Send `<F1 25 folder>/swofty/loader.log` **before** starting the game again (every start
overwrites it). Say what you did and roughly when.

## License and reuse

**Terminal Damage Attribution License 1.0**, see LICENSE. Published mods/projects
using substantial portions must visibly show **Uses work from Terminal Damage by
nyannoying** and link to https://github.com/nyannoying1337/f1-25-terminal-damage.
Redistributed copies must retain the copyright and license notices. See LICENSE
for the complete terms. Earlier MIT releases retain their MIT terms.
Third-party SDK files retain their own licenses; terminal_damage_api.h uses the
Terminal Damage project license.
