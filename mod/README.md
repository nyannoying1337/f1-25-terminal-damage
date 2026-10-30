# Terminal Damage (Swofty addon for F1 25) — beta

When your car breaks, the game normally takes it away from you on the spot. With this mod you keep
the car and deal with the failure like a real driver: limp on, pull over, or bring it to your box.

## 0.51.12 beta

- Replace the separate player tether switches with **Player wheel behavior**: Stock, Reduced jolting (experimental), or Loose wheels.
- Reduced jolting keeps the tethers and reduces the violent bouncing seen with detached wheels. Wheels can still move oddly and pass through the chassis or cockpit.
- Loose wheels removes player tethers. **Loose AI wheels** remains a separate option. Stock and AI removal off are the defaults; restart after changing either.
- Simplify the launcher settings by hiding garage implementation and debug controls. Their existing defaults remain unchanged.
- Reduce repeated checks during detached-wheel creation and remove routine collision-experiment logging.

**Upgrading:** choose your preferred Player wheel behavior again. Earlier player tether switches are replaced by the dropdown; the new default is Stock.

Known issues: reduced jolting is still experimental and is not a complete realistic tether simulation. Mid-push flashbacks and red-flag camera behavior remain limited. Qualifying ICE replacement / Skip repairs and practice replacement timing remain **NOT TESTED, MIGHT BE BROKEN**.

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
  - terminal crash damage (e.g. broken suspension): no drive, the car rolls out
- **Engine sound:** a failed engine plays the engine stop sound and stays silent. After terminal
  crash damage the engine dies after 3 to 6 s, and a broken or retiring car that stands still
  switches its engine off after 6 s (setting "Engine sound off after a failure").
- **The game's own random failures** are handled the same way as the failures above.
- **Stand still on track for 10 s** with a broken car: you retire.
- **Drive into the pit lane** with a broken car (race): no tyre stop. Your garage crew comes out,
  pushes the car into the garage, then you retire.
- **Practice and qualifying:** instead of retiring (which would end the whole session), your car is
  sent back to its garage and stays there for the rest of the session. Career: fit a new part
  (e.g. a new engine) in the garage, then leave the garage. Without a new part the car is sent
  straight back to the garage.
- **Engine changes in qualifying:** a newly scheduled player ICE replacement takes at least
  60 minutes. Skip repairs should consume the remaining qualifying time. Practice replacement
  times and other repairs stay unchanged. **Not tested, might be broken.**
- **Ctrl+Shift+F11:** retire like a real driver. Nothing breaks and you keep full power: pull over
  and stand still for 10 s, or drive into your box. Press again to cancel while still on track.
- **Retire in the pause menu** works the same way on track (any controller or wheel): pull over or
  box. A HUD message confirms the request.
- Your stopped car does not trigger a safety car or red flag.

## Setup (once)

1. Steam version of F1 25 and Swofty Launcher 1.5.13 or newer.
2. Import the zip in the Swofty Launcher and restart the game.
3. In the game: Options > Settings > Telemetry Settings:
   UDP Telemetry **On**, Broadcast **Off**, IP **127.0.0.1**, Port **20777**, Send Rate **20 Hz**,
   UDP Format **2026**.

The mod reads your speed from the game's UDP telemetry. Without it, standing still does not retire
a broken car: drive into your box, or use Retire in the pause menu.

**Using SimHub, CrewChief or another telemetry app on port 20777?** Only one program can listen on
a port. Pick another port in the addon settings (*UDP telemetry port*, e.g. 20778) and let your
telemetry app forward the game's data to it (SimHub: UDP forwarding). Restart the game after
changing the port.

## Each game start

Restart the game after installing or updating the addon. You can load a race
directly; no practice session or saved vehicle-ID mapping is needed.

## Settings

- **Power with a broken turbo/ERS part:** 20–50 % throttle, or no drive at all.
- **Retire hotkey:** on/off and which F key (with Ctrl+Shift).
- **Retirement radio voice:** on/off for the engineer's reply to radio-menu Retire.
  Restart the game after changing it. Retire remains usable with the voice off.
- **UDP telemetry port:** see above.
- Options starting with *Debug:* are for testing and bug reports. Leave them off.

## Known issues (beta)

- Stopped retirement requires the car to be OnTrack. A car stranded in the pit lane
  or box exit (LeavingPitLane) does not start the 10-second timer.
- Rewinding into the middle of the garage push or box animation is not fully covered.
- A red flag during terminal damage can still cause AI/spectator camera issues.
- Qualifying ICE replacement / Skip repairs and practice replacement timing are
  **NOT TESTED, MIGHT BE BROKEN**.
- Hiding pit equipment does not establish that all associated collision is removed.
- AI cars with terminal damage can stop at the pit exit or on track.
- Terminal crash damage keeps the practice/qualifying car in its garage even after repairs.
- Career replacement is checked on send-out; without a new part the car can briefly reach the
  pit exit before returning. Qualifying ICE repair time still needs an in-game regression test.
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
