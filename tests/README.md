# Tests

One standalone program per `tests/*_test.cpp`. Each has its own `main` and no test framework: a test passes when it
builds without a compiler warning and exits with code 0. A failed `assert` aborts the program.

## Run

From the repository root, in PowerShell:

    powershell -NoProfile -File tests\run_tests.ps1

It builds every test in parallel (`zig c++ -std=c++23 -O1 -Wall -Wextra`, zig 0.15.2 from `.tools`), runs them from the
repository root, prints `pass=N fail=M` and exits 1 on a failure. Nothing is written into the repository.

| Switch | Does |
| --- | --- |
| `-Name a,b` | only these tests: file name without `.cpp`, `_test` optional, wildcards work (`-Name '*tether*'`) |
| `-Jobs N` | parallel builds (default: the logical processors, at most 16) |
| `-Detail` | one line per test with build and run seconds |
| `-AllowWarnings` | report compiler warnings instead of failing the test |
| `-Tools` | also build `tools/*.cpp` and run them once |
| `-RunTimeoutSec N` | kill a test that runs longer than N seconds (default 120) |
| `-OutDir <dir>` | zig cache, executables and logs (default `%TEMP%\terminal-damage-tests`; outside the repository) |
| `-Repo <dir>` | test another checkout, for example the public mirror |

## Writing a test

1. **The NDEBUG preamble.** zig defines `NDEBUG` at `-O1` and `-O2`, and with `NDEBUG` every `assert` compiles to
   nothing: a failing test would pass. Every test starts with

       #ifdef NDEBUG
       #undef NDEBUG
       #endif

   before its first `#include`. Keep it.
2. **`test_util.hpp`.** Include it last, after the other includes (it needs only the standard library, so header-only
   tests use it too). It makes a forgotten preamble a compile error (`#error`), makes a failed assert end the program at
   once (exit code 3; without it Windows Error Reporting holds the abort for 2.5 s, up to 9 s under load), and offers
   what the tests used to copy: `tu::quiet_log` (the game's log function, discarded), `tu::close_to(a, b, eps)`,
   `tu::addr(p)` (a pointer as `uintptr_t`) and `tu::put(buffer, offset, value)` (write a value into fake game memory;
   the `std::vector<uint8_t>` overload checks the range).
3. **The `near` hazard.** `windows.h` defines `near` and `far` as empty macros. A helper called `near(a, b, eps)` then
   compiles to a comma expression and an `assert` on it is always true. Name it `close_to`.
4. **New reset statements.** `reset_test.cpp` sets every field `reset_car_state`, `reset_race_faults` and
   `reset_session` clear and checks the defaults. When a reset function gets a new statement, add its field there.

## Which tests include the product

Every test includes the umbrella `mod/native/src/terminal_damage_probe.cpp` (the whole addon as one translation unit,
with `windows.h`), so it can reach the addon's internals, except these: `brake_telemetry_test`,
`garage_assignment_test`, `power_loss_test` and `team_model_test` include only `src/*.hpp` modules, and
`signature_syntax_test` reads the sources as text. `manifest_presets_test` and `signature_syntax_test` read files
relative to the repository root, where the runner starts every test.

## What each test covers

| Test | Covers |
| --- | --- |
| `ai_cleanup_test` | issue #10: when an AI wreck the game never removes is put in its garage (thresholds, once per car, never during a safety car or red flag, practice, the crew's pit states or for the player; the option) |
| `ai_suspension_test` | AI cars' suspension damage: counted contacts, bend and collapse, the retire timer, a new session forgets all |
| `asset_diagnostics_test` | asset change diagnostics: paths, invalid and empty vectors, forwarding, diagnostics on and off |
| `brake_telemetry_test` | `brake_telemetry.hpp`: braking zones, rear brake share, locked wheels, slides, the power check (header only) |
| `crew_diagnostics_test` | crew diagnostics never call predicates with an unknown ABI; a restart leaves the grid alone |
| `damage_rating_test` | the game's damage rating hook: only the flashback check's own record is rated without the terminal level |
| `flashback_message_test` | flashback without UDP (`MessageFlashbackUsed` and the terminal record) and the pit lane standstill retirement |
| `flashback_rule_test` | the game's flashback rule is only watched; a refusal is logged with its reason at most every 10 s |
| `flashback_test` | flashback with UDP telemetry: resume before the damage, repair gate, crash mask, retention and cleanup |
| `garage_assignment_test` | `garage_assignment.hpp`: spawn slot and teammate lookup, changed sessions, duplicate or unreadable data |
| `garage_restart_test` | race restart and the garage crew: push HUD reason released, owned crew removed, foreign crews kept |
| `logic_transitions_test` | the six audited lifecycle conflicts (NOT_IN_SESSION, controller prep, rewinds, garage return, part swap) |
| `manifest_presets_test` | `mod/swofty.json` against the code: 255-byte texts, defaults, preset values, `SWOFTY_SETTING`s, version |
| `performance_cleanup_test` | healthy AI updates skip settings and logs; idle crew skipped; private timing stays optional |
| `pit_retirement_clearance_test` | `pit_retirement_clearance.hpp`: model visibility, restoration, teammate, session and toggle guards |
| `pitlane_audio_crew_test` | the emitted engine audio override on a synthetic audio object; missing crew schedules no fake push |
| `player_lifecycle_test` | queued player failure order, AI isolation, expiry, session cleanup, DNF audio through cleanup |
| `player_state_service_test` | the exported player state API: ABI, notifications, retirement confirmation, reset, unsubscribe |
| `power_loss_test` | `power_loss.hpp` and `failure_model.hpp`: each power course against real cases (header only) |
| `qualifying_repair_test` | the qualifying repair hook: stock work kept, only a new player ICE job extended |
| `radio_probe_test` | the retire command in the radio menu: forwarding, queued ids only, session guards |
| `realistic_failures_test` | the realistic Grand Prix failures: era shares, warning signs, power faults, rear brakes, gearbox decline, AI cars, teams |
| `replay_terminal_gate_test` | the emitted replay terminal gate stub (player only) and the lifecycle guards that clear it |
| `reset_test` | every field `reset_car_state`, `reset_race_faults` and `reset_session` clear |
| `road_to_1_0_test` | regressions by issue and version: garage crew, red and black flags, garage lock, restarts, limp, repairs |
| `signature_syntax_test` | the byte signatures in the sources: hex tokens and `??`, single spaces, 8 fixed bytes at least, none twice |
| `stopped_player_test` | the stopped-cars lookup and update hook: bounds, unreadable data, forwarding |
| `suspension_damage_test` | `suspension_damage.hpp`: hit thresholds, failing and cracked corners, kerbs, which contacts count |
| `team_model_test` | `team_model.hpp`: per-team failure rates add up to the seasons' totals; team ids, slots, R&D (header only) |
| `test_run_test` | the Ctrl+Shift+F4 test run, shift timing, engineer radio, damage panel tiles, breakdown watch |
| `tether_collision_test` | the wheel tether collision exclusion: exact endpoints of a fresh player tether only |
| `tether_plane_test` | the emitted tether plane stub and the `configure_player_wheels` options |
| `vehicle_memory_test` | guarded reads of the live vehicle, component caps, ICE audio, repair unlock |
| `wheel_tether_test` | the wheel tether options for the player and the AI, the fresh player wheel path |
