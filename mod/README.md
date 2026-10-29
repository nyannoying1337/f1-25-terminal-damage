# Terminal Damage (Swofty addon for F1 25) — beta

When your car breaks, the game normally takes it away from you on the spot. With this mod you keep
the car and deal with the failure like a real driver: limp on, pull over, or bring it to your box.

## 0.50.0 beta release notes

**Known issues: NOT TESTED, MIGHT BE BROKEN:** qualifying ICE replacement / Skip repairs,
practice replacement timing, and the new flashback recovery have not been verified in game.
Latest pit-entry audio and missing-crew fallback changes also need in-game confirmation.
See the [full changelog](https://github.com/nyannoying1337/f1-25-terminal-damage/blob/main/CHANGELOG.md).

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

1. Steam version of F1 25 and the Swofty Launcher (tested with 1.5.8).
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

**Load a practice session first, then the race.** The mod learns which garage belongs to which car
in practice. Without it, the crew does not come out in the race (the car is put into the garage
without a usable crew, the car retires in its box without a push).

## Settings

- **Power with a broken turbo/ERS part:** 20–50 % throttle, or no drive at all.
- **Retire hotkey:** on/off and which F key (with Ctrl+Shift).
- **Retirement radio voice:** on/off for the engineer's reply to radio-menu Retire.
  Restart the game after changing it. Retire remains usable with the voice off.
- **UDP telemetry port:** see above.
- Options starting with *Debug:* are for testing and bug reports. Leave them off.

## Known issues (beta)

- Race garage pushing needs a practice session loaded first in the same game process.
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

## Testing 0.50.0 beta

1. Race: pull over, wait for the 10 s retirement, check the DNF camera stays silent, then restart
   from results and check normal engine sound and grid position. DNF audio passed Nick's .17 test.
2. Career qualifying: change the ICE and skip repairs. At least 60 minutes should consume the
   remaining qualifying time. Practice should retain the game's usual replacement time.
   **Not tested, might be broken:** qualifying ICE replacement/Skip repairs and practice timing.
   Nick cannot currently test this because all available engines have been used.
3. Radio: Retire should stay last and work with its voice reply both on and off after restart.
4. Performance: enable Debug: performance counters, drive the affected scene for at least
   30 seconds, and send loader.log plus perf.log. New `[td-probe] perf:` summaries report only
   addon work in private vehicle, crew, stopped-car and repair hooks every 10 seconds. They
   exclude original game calls and GPU/asset costs. Counters are off by default.

0.32.22 adds flashback recovery using the game's UDP FLBK event (format 2026 required).
After rewinding before the damage/retirement request, resumed telemetry plus healthy live
parts clears the addon's power/audio/garage lock and pending retirement/crew requests.
Rewinding to a point after the failure retains the lock. No parts are repaired by the addon.
Test ICE failure -> rewind before failure -> normal throttle/audio and no delayed retirement;
also test a rewind that leaves the failure in the timeline. Runtime verification is pending.

0.32.21 also uses guarded process-memory copies for live player vehicle resolution and
power-loss/repair reads. Vehicle and component pointers are still resolved on every update;
no sampling delay or cross-frame pointer cache was introduced. Verify healthy driving,
terminal power loss and component replacement, with counters enabled for a paired capture.

0.32.20 batches the stopped-car list reads and validates an index hint against the live vehicle ID.
It uses guarded process-memory copies instead of repeated heap-region queries; no game pointer
is retained across callbacks. The temporary stopped-car exclusion flag is unchanged.
Capture loader.log and perf.log with counters on after terminal damage to measure the gain.

0.32.19 adds two pit-lane checks: with a failed ICE, engine sound should stay off at pit entry;
with radio Retire and a usable crew, the car should be pushed into the garage. Load practice
first in the same game process for the race animation. If no crew is available, a HUD message
explains the fallback: retirement in the box, without fabricated push reports/garage teleport.
Both changes still need in-game verification.

0.32.18 removes obsolete AI-update limp-setting work, skips healthy crew inspection, gates
engine/repair/crew detail logs behind diagnostics, and avoids installing the diagnostic-only
Debris hook by default. It does not change the loaded crew assets or claim a measured FPS gain.
If lag remains, compare the same scene with Garage crew assets in the race off after a full
restart. This disables the real garage-push animation for that comparison.

## Reporting problems

Send `<F1 25 folder>/swofty/loader.log` **before** starting the game again (every start
overwrites it). Say what you did and roughly when.
