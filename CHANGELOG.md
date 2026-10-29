# Terminal Damage 0.50.0 beta

## Added

- Retire through the radio command menu. The entry is last in the standard lists; request retirement, pull over and stop for 10 seconds, or drive into your box. Selecting it again does not cancel or fast-forward retirement.
- Optional engineer voice reply for radio retirement, controlled by the separate Retirement radio voice toggle (restart required).
- Flashback recovery: rewinding before terminal damage or a retirement request can release the addon's power/audio/garage lock once resumed UDP telemetry and restored live parts confirm recovery. Requires UDP format 2026.

## Fixed and improved

- Race/session restart cleanup, including stale garage crew requests and engine-audio shutdown state.
- Engine shutdown audio handling through retirement and pit entry.
- Missing race garage crew now produces an explanatory message and retirement in the box instead of fabricated push reports/garage teleportation. Load practice first in the same game process to enable the real race garage push.
- Qualifying garage-return crash fix and component-swap handling.
- Reduced repeated memory-query work in stopped-car and player-vehicle updates, removed obsolete AI-update work and unnecessary diagnostics. Paired local captures show substantially lower measured hook costs; this is not a guarantee that all FPS drops are fixed.
- Optional Debug: performance counters for private-hook timing in loader.log.

## Known issues / not tested

- **Qualifying engine replacement / Skip repairs: NOT TESTED, MIGHT BE BROKEN.** The intended behavior is a minimum 60-minute ICE replacement so Skip repairs consumes the remaining qualifying session. It has not been verified in game.
- **Practice engine replacement timing: NOT TESTED, MIGHT BE BROKEN.** The game's normal timing is intended to remain unchanged.
- **Flashbacks: Instant Replay/flashback entry can be blocked after terminal damage.** This is a known issue under investigation. The recovery path cannot help if replay entry is blocked. UDP format 2026 and a fresh pre-damage timeline sample are required. Very short rewinds near the failure may retain the lock.
- Latest pit-entry audio and missing-crew fallback changes still need in-game confirmation. Race garage pushing still requires loading practice first in the same game process.
- Career component replacement is detected when leaving the garage. Without a new part, the car may briefly appear at pit exit before it is returned.

For reports, include loader.log and perf.log from the affected run before restarting the game.
