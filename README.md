# Terminal Damage — F1 25 addon (Swofty) — beta

When your car breaks, F1 25 normally takes it away from you on the spot. With this addon you keep the
car and deal with the failure like a real driver: limp on with reduced power, pull over and retire,
or bring it to your box where your garage crew pushes it into the garage.

**Download:** install it from the Addon Browser in the Swofty Launcher, or get the zip from [Releases](../../releases) and import it.

Features, setup (UDP telemetry, format 2026) and known issues: [mod/README.md](mod/README.md).

## 0.50.0 beta

Radio-menu retirement with an optional engineer reply, restart/audio fixes,
flashback recovery and reduced CPU work in vehicle/stopped-car hooks.

**Not tested, might be broken:** qualifying ICE replacement / Skip repairs,
practice replacement timing, and the new flashback recovery. The latest pit-entry
audio and missing-crew fallback also need runtime confirmation.

Read the [changelog](CHANGELOG.md) for changes and limitations.

## Building

Needs [zig](https://ziglang.org/) 0.15 on PATH.

```
mod\native\build.ps1      # Windows
mod/native/build.sh       # Linux (cross-compiles the Windows DLL)
```

The DLL lands in `mod/native/`. Zip the contents of the `mod` folder (swofty.json at the zip root) to import it in the Swofty Launcher.

## License

MIT, see [LICENSE](LICENSE). The headers in `mod/native/include` belong to the Swofty addon SDK.
