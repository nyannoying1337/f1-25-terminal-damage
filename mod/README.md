# Terminal Damage (Swofty addon for F1 25) — beta

When your car breaks, the game normally takes it away from you on the spot. With this mod you keep
the car and deal with the failure like a real driver: limp on, pull over, or bring it to your box.

## What it does

- **You keep your car after terminal damage** (engine, power unit, gearbox or crash damage).
- **The failure shows up in how the car drives:**
  - engine (ICE) or gearbox failed: no drive, the car rolls out
  - turbo, MGU-K, MGU-H, battery or control electronics failed: reduced power (30 % throttle by
    default, adjustable)
  - terminal crash damage (e.g. broken suspension): no drive, the car rolls out
- **Stand still on track for 10 s** with a broken car: you retire.
- **Drive into the pit lane** with a broken car (race): no tyre stop. Your garage crew comes out,
  pushes the car into the garage, then you retire.
- **Practice and qualifying:** instead of retiring (which would end the whole session), your car is
  sent back to its garage and stays there for the rest of the session.
- **Ctrl+Shift+F11:** your car counts as broken (pull over or drive into your box). Press again to
  retire at once.
- Your stopped car does not trigger a safety car or red flag.

## Setup (once)

1. Steam version of F1 25 and the Swofty Launcher (tested with 1.5.8).
2. Import the zip in the Swofty Launcher and restart the game.
3. In the game: Options > Settings > Telemetry Settings:
   UDP Telemetry **On**, Broadcast **Off**, IP **127.0.0.1**, Port **20777**, Send Rate **20 Hz**,
   UDP Format **2026**.

The mod reads your speed from the game's UDP telemetry. Without it, standing still does not retire
a broken car (use Ctrl+Shift+F11 instead).

**Using SimHub, CrewChief or another telemetry app on port 20777?** Only one program can listen on
a port. Pick another port in the addon settings (*UDP telemetry port*, e.g. 20778) and let your
telemetry app forward the game's data to it (SimHub: UDP forwarding). Restart the game after
changing the port.

## Each game start

**Load a practice session first, then the race.** The mod learns which garage belongs to which car
in practice. Without it, the crew does not come out in the race (the car is put into the garage
without the animation instead).

## Settings

- **Power with a broken turbo/ERS part:** 20–50 % throttle, or no drive at all.
- **Retire hotkey:** on/off and which F key (with Ctrl+Shift).
- **UDP telemetry port:** see above.
- Options starting with *Debug:* are for testing and bug reports. Leave them off.

## Known issues (beta)

- Race: the crew only comes out if a practice session was loaded earlier in the same game start.
- Practice/qualifying: if you try to drive out of the garage with a broken car, the camera stays in
  the box. Use the pause menu to leave the session.
- AI cars with terminal damage can stop at the pit exit or on track.
- A broken engine idles instead of switching off.
- Not tested yet: career / My Team in depth, F2 cars, time trial, split screen.
- After a game update some features may switch themselves off until the mod is updated.

## Reporting problems

Send `<F1 25 folder>/swofty/loader.log` **before** starting the game again (every start
overwrites it). Say what you did and roughly when.
