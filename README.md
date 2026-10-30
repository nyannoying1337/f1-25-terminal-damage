# Terminal Damage — F1 25 addon (Swofty) — beta

When your car breaks, F1 25 normally takes it away from you on the spot. With this addon you keep the
car and deal with the failure like a real driver: limp on with reduced power, pull over and retire,
or bring it to your box where your garage crew pushes it into the garage.

**Download:** install it from the Addon Browser in the Swofty Launcher, or get the zip from [Releases](../../releases) and import it.

Features, setup (UDP telemetry, format 2026) and known issues: [mod/README.md](mod/README.md).

## 0.51.8 beta

Optional player and AI wheel-tether removal, independently selectable and both
**off by default**. Restart after changing either setting. Replay/flashback
overhead with these options is reduced.

Spectator addons can subscribe to player-state changes through the documented API;
Terminal Damage does not switch cameras itself. Existing gameplay limitations
are listed in the addon README and changelog.

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
