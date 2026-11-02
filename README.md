# Terminal Damage — F1 25 addon (Swofty) — beta

When your car breaks, F1 25 normally takes it away from you on the spot. With this addon you keep the
car and deal with the failure like a real driver: limp on with reduced power, pull over and retire,
or bring it to your box where your garage crew pushes it into the garage.

**Download:** install it from the Addon Browser in the Swofty Launcher, or get the zip from [Releases](../../releases) and import it.

Features, setup and known issues: [mod/README.md](mod/README.md).

## 0.51.85 beta

- Flashbacks work without UDP telemetry: back before the damage, the car is whole again with full power.
- A flashback before one of the game's own mechanical failures no longer takes your power away or lets the AI retire or disqualify your car.
- A broken car stranded in the pit lane retires after 10 s standing still; an engine that is off gives no drive.

## 0.51.80 beta

- Fix: a car withdrawn for the session could still go on track through a practice programme.

## 0.51.79 beta

- No UDP telemetry needed any more: the addon reads your speed from the game.
- One broken front corner may let the car crawl back to the pits (dice roll).
- Red flags follow the FIA rules: failed power units and abandoned cars retire, a limping car may be repaired and restart.
- Practice and qualifying: a car that limps into its garage is rebuilt there (20-45 min); a car that did not make it back is withdrawn for the session.
- A disqualification stays DSQ in the results.

Read the [changelog](CHANGELOG.md) for changes and limitations.

## Addon integration

Spectator addons can use the versioned [player-state API](docs/player-state-api.md)
to observe retirement and recovery without polling game memory.

## Building

Needs [zig](https://ziglang.org/) 0.15 on PATH.

```
mod\native\build.ps1      # Windows
mod/native/build.sh       # Linux (cross-compiles the Windows DLL)
```

The DLL lands in `mod/native/`. Zip the contents of the `mod` folder (swofty.json at the zip root) to import it in the Swofty Launcher.

## License and attribution

**[Terminal Damage Attribution License 1.0](LICENSE)** ? a custom license.
Published mods/projects using substantial portions must visibly show:

> Uses work from Terminal Damage by nyannoying

Include the [project link](https://github.com/nyannoying1337/f1-25-terminal-damage)
and preserve the copyright/license notices in redistributed copies. See LICENSE
for the complete terms. Earlier MIT releases retain their MIT terms.

The Swofty SDK files (`mod/native/include/swofty*`) retain their own licenses;
Terminal Damage's `terminal_damage_api.h` uses the project license.
