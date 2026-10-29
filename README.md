# Terminal Damage — F1 25 addon (Swofty) — beta

When your car breaks, F1 25 normally takes it away from you on the spot. With this addon you keep the
car and deal with the failure like a real driver: limp on with reduced power, pull over and retire,
or bring it to your box where your garage crew pushes it into the garage.

**Download:** see [Releases](../../releases). Import the zip in the Swofty Launcher.

Features, setup (UDP telemetry, format 2026) and known issues: [mod/README.md](mod/README.md).

## Building

Needs [zig](https://ziglang.org/) 0.15 on PATH.

```
mod\native\build.ps1      # Windows
mod/native/build.sh       # Linux (cross-compiles the Windows DLL)
```

The DLL lands in `mod/native/`. Zip the `mod` folder to import it in the Swofty Launcher.

## License

MIT, see [LICENSE](LICENSE). The headers in `mod/native/include` belong to the Swofty addon SDK.
