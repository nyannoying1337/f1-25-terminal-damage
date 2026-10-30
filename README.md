# Terminal Damage — F1 25 addon (Swofty) — beta

When your car breaks, F1 25 normally takes it away from you on the spot. With this addon you keep the
car and deal with the failure like a real driver: limp on with reduced power, pull over and retire,
or bring it to your box where your garage crew pushes it into the garage.

**Download:** install it from the Addon Browser in the Swofty Launcher, or get the zip from [Releases](../../releases) and import it.

Features, setup (UDP telemetry, format 2026) and known issues: [mod/README.md](mod/README.md).

## 0.51.3 beta

Optional **Remove player wheel tethers**, **off by default**. Enable it if loose
wheels pull or shake your car. Player wheels become loose debris; suspension
damage and AI wheel tethers stay unchanged. Restart after changing the setting.

## 0.51.1 beta

Fixes the AI driving away after a voluntary retirement when you park on track.
Power stays available until retirement completes; the car can still roll on a slope.

## 0.51.0 beta

Race retirement clears tyre-stop equipment while preserving the garage push crew
and teammate stops. Load directly into a race; practice is no longer required.
Restart and flashback cleanup also covers old crew work, power/audio locks and
the HUD hide reason left by an interrupted push.

Known limitations include stopped retirement in the pit lane, rewinding into the
middle of a push, red-flag camera/control behavior, and untested qualifying ICE
replacement / Skip repairs and practice repair timing.

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

## License

Terminal Damage Attribution License1.0, see [LICENSE](LICENSE). Published projects
using substantial portions must visibly credit **nyannoying - Terminal Damage**
and link to this repository. Earlier MIT releases keep their MIT terms.
The headers in `mod/native/include` retain the Swofty SDK's own licenses.
