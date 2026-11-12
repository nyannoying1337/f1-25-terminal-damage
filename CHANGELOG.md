# Terminal Damage 0.76.3 beta

The addon is now off in Time Trial. Everything from 0.76.2 is in this release.

## Fixed

- **Time Trial.** The addon does nothing there now: no suspension damage, no failures, no retirement handling. It used to run in Time Trial as in every other session.

# Terminal Damage 0.76.2 beta

The addon no longer contains any network code, and in Career early Durability upgrades now count more than late ones. Everything from 0.76.1 is in this release.

## Changed

- **Career R&D, Durability upgrades.** Every upgrade used to take the same 1 % off your team's failure rate. Now the first ones count most: the first upgrade takes almost 2 % off, the first third of them gives half of the effect, and the last ones add little. All of them done still halves the failure rate. Real cars got more reliable the same way: the big problems were fixed first.
- **No network code.** The debug option *UDP telemetry port* is removed, and with it the addon's only network code (a listener for the game's own telemetry on your PC, off by default). Nothing changes in the game: the addon reads your speed from the game itself.

# Terminal Damage 0.76.1 beta

Each team's own failure rate works again, and the 2026 failure rates include the São Paulo Grand Prix. Everything from 0.76.0 is in this release.

## Fixed

- **Failures per team.** In Grand Prix and custom races the addon could not tell which team a car belongs to, so every car got the season's average failure chance. Each team's cars break down at that team's own rate again.

## Changed

- **2026 failure rates, now up to round 20 (São Paulo).** An average 2026 car fails in 12.5 % of Grands Prix (was 12.9 %). Battery failures come a little more often, and Aston Martin is still the team that breaks down most.

# Terminal Damage 0.76.0 beta

Suspension damage is rebuilt: how hard you hit now decides whether a corner bends, cracks or breaks, with no dice and no timers. Flashbacks work after a crash in the pit lane, and the damage panel has a new look. Everything from 0.75.15 is in this release.

## Read this first

- **Updating from 0.75.15:** nothing to set up. *Suspension damage from hits* is still experimental and off by default. If you turn it on, the new *Suspension strength* option starts on Realistic.

## New

- **Suspension damage, rebuilt** (option *Suspension damage from hits*). What happens depends only on how hard you hit. A light hit **bends** a corner: the car pulls to one side, and you can drive on. A medium hit **damages** it: kerbs then wear it down, and it can **crack** and break. A heavy hit **breaks** it: the car limps on that corner. A very heavy hit **rips the wheel off**.
- **Nose and wing crashes count too.** A crash with the front or the rear of the car now reaches the nearest corners, not only a hit on a wheel.
- **New option: Suspension strength.** Realistic (default): a hit straight into a wall at about 25 km/h breaks a corner, about 45 km/h rips the wheel off. Forgiving takes more, Brutal takes less. Needs a game restart.
- **Race, broken corner: box, box.** With suspension damage on, a broken corner no longer means "stop the car within 20 seconds". Drive slowly into the pits this lap or the next. The crew pushes the car into the garage, and your race ends there. Stay out longer and the car retires.
- **Flashback after a crash in the pit lane.** A crash at the pit entry, where the pit speed limit shows, blocked Instant Replay and flashbacks, so you had to retire. Flashbacks work there now.
- **Plain messages.** The suspension messages and option texts say what you feel and what to do: bent, damaged, cracked, broken.
- **Damage panel, new look.** A glass panel with rounded tiles and a coloured label under each part. When something is badly damaged, the panel's edge glows in that colour.

## Known issues

- Suspension damage is still experimental, and its tuning can change.
- A hard landing (after a big kerb, or flying off the track) does not break a healthy corner yet.

# Terminal Damage 0.75.15 beta

The addon's Swofty manifest now says that it supports the Steam version of F1 25 only and needs Swofty Launcher 1.8.36 or newer. Nothing changes in the game. Everything from 0.75.14 is in this release.

# Terminal Damage 0.75.14 beta

Two attempted fixes: no car warnings in the Career weekend menu, and Terminal Damage works together with Cool-down Lap. Everything from 0.75.12 is in this release (0.75.13 was never released).

## New

- **Attempt: no failures or warning signs in the Career weekend menu.** After qualifying, the game loads the race behind the weekend menu with your car standing in it. The addon took that for racing: its failure clock ran in the menu, and a planned failure could show its warning signs (the CAR WARNING panel, the engineer's radio) before the race began. The failure clock now starts once your car moves.
- **Attempt: Cool-down Lap compatibility (issue #19).** Cool-down Lap (by vmknolle) lets you keep driving after the chequered flag and holds back the game's end-of-race hand-over of your car, which Terminal Damage uses to know your race is over. Depending on which addon hooked first, Terminal Damage could take the cool-down lap for racing: a planned failure or a pit call could still come. Cool-down Lap 0.38.8 and newer tell Terminal Damage when your race is over and which AI cars it parks in parc ferme; Terminal Damage stops its failures there and leaves those cars alone. Without Cool-down Lap nothing changes.

# Terminal Damage 0.75.12 beta

The addon's Swofty manifest now names its source repository, as Swofty recommends for addons with native code. Nothing changes in the game. Everything from 0.75.11 is in this release.

# Terminal Damage 0.75.11 beta

Two small fixes: End Session works again in practice and qualifying, and an attempt to bring back your car's engine sound in the race replay after a retirement. Everything from 0.75.10 is in this release.

## New

- **End Session works in practice and qualifying.** The game uses the same function for End Session there as for Retire in a race, and the addon turned it into a retirement: you were asked to pull over and the session went on. End Session now ends the session at once, as in the game. In a race, Retire in the pause menu still asks you to pull over or box first.
- **Attempt: engine sound in the race replay after a retirement.** The addon keeps a retired car's engine off through the retirement camera. When you watched the full race replay from the results, your car could stay silent in it. The sound now comes back once the game puts your car away after the race.

# Terminal Damage 0.75.10 beta

One small fix: the addon now recognises practice and qualifying when another addon, such as the Spectator HUD, hooks the pit lane before it. Everything from 0.75.9 is in this release.

## New

- **Practice and qualifying garage handling works next to the Spectator HUD.** The addon learns that you are in a practice or qualifying garage from the game's pit status function. An addon that hooks that function before this one loads (the Spectator HUD does) blocked the addon's own hook, so it could not tell a qualifying garage from a race: a damaged suspension was never repaired in the garage. The addon now hooks the function the way Swofty expects, so it works next to other addons in any load order. If it ever cannot find the pit lane, the log says so after 30 seconds.

# Terminal Damage 0.75.9 beta

One small fix: the failure plans of a race can no longer fire on the results screen. Everything from 0.75.8 is in this release.

## New

- **The failure plans stop when the race is over.** In a Career race the game ends the session for everybody at once, for example when you retire, but the addon only hears that the session has ended when you leave the results screen, sometimes minutes later. Until then its failure clock ran on: in one of my logs a failure planned for an AI car was armed 37 seconds into the results screen (nothing visible happened). Now the failure plans, the pit window after a failure and the gearbox decline stand still as soon as the game ends the session, and they run again when you restart the race or start the next session.

# Terminal Damage 0.75.8 beta

AI wrecks that the game forgets are cleared, and a few small fixes. Everything from 0.75.6 is in this release (0.75.7 was never released).

## Read this first

- **Updating from 0.75.6:** nothing to set up. Your options stay as they are. If you are new, the setup is in the description (Quick start and Game settings): in the game's own settings put *Car Damage* and *Car Damage Rate* on **Simulation**, and keep *Mechanical failures in Grand Prix* on **Realistic**.

## New

- **AI cars with terminal damage that the game never removes are put in their garage.** The game removes a crashed AI car about 15 seconds after the damage and a car that one of the addon's failures broke after up to two minutes. After a flashback it sometimes did not remove them at all: in my logs four cars stood on the track until the session ended, and one retired car was never removed. Now the addon puts such a car in its garage after two minutes on the track, or after 30 seconds in the pit lane or at the pit exit, and only after the car has stood still. It never does this during a safety car, a red flag or a formation lap, never in practice or qualifying, and never for your car. The hidden option *Send abandoned AI wrecks to their garage* (on by default) switches it off; the log lines start with *AI cleanup:*.
- **A red flag no longer leaks into the next session.** A session that ended while the red flag strategy screen was open left its state behind: in the next session it could block the flashback and heavy damage handling, or retire your car from the old screen's state. It is cleared now. (Found by reading the code, not seen in a log yet.)
- **A flashback no longer loses a damage record to a timing race.** In rare cases the record the addon keeps of a crash was dropped when the game's telemetry stamp was a few milliseconds ahead of the addon's clock.
- **Small fixes:** the UDP telemetry (off by default) waits instead of spinning on a receive error and logs it, and a garage flag is safe across threads.
- **Game settings:** the description now says that the game's *Player Faults* options (*Fault Frequency*, *Fault Types*) can stay on Off and Low, and the presets text says that all three presets also switch *Keep control after terminal damage*, *Keep control after a scripted failure* and *Engineer radio for damage and failures* on.
- **Source code:** the source in the repository is split into topic files (mod/native/src/probe) and comes with a test runner. The behavior is the same.

## Something not working?

Every part has its own option. Turn off only the part that gives you trouble for now (for example *Mechanical failures in Grand Prix*, *Damage status panel*, *Engineer radio* or *Suspension damage from hits*), keep the rest, and send me your log: loader.log in the newest folder of %LOCALAPPDATA%\Swofty\sessions. Post it on OverTake or open an issue on GitHub, and say what happened and roughly when.

## Known issues

- Realistic failures in Career are new: one gearbox failure from the real rates has been seen in a Career race so far, and it played out as designed. An AI car that fails in Career stays where it stopped for up to two minutes before the game takes it to its garage.
- AI cars with terminal damage stand where they stopped until the game takes them to their garage: about 15 seconds after a crash, up to two minutes after a failure the addon planned. If the game never does (it happened after flashbacks), the addon puts the car in its garage after two minutes on track or 30 seconds in the pit lane, never during a safety car or red flag.
- The Career R&D effect is small (about 1 % per Durability upgrade) and its cap of half with every upgrade done is a first guess: I will tune it once I have seen how fast the AI teams research.
- Suspension damage is experimental: its tuning can still change.
- Career: the "PART FITTED" message named in the 0.75.0 notes may not show when you fit the new part.
- Career is not fully tested with the limping, red flag and garage repair rules yet.
- No safety car HUD while your car is terminally damaged.
- Qualifying ICE replacement, Skip repairs and practice replacement timing: **not tested, might be broken**.

# Terminal Damage 0.75.6 beta

Every team now breaks down at its own real rate, and in Career and My Team a team's finished Durability R&D lowers it. Everything from 0.75.5, 0.75.4 and 0.75.3 is in this release.

## Read this first

- **Set the game's damage to Simulation.** Terminal Damage is built and tested with the game's own damage settings on **Simulation**: in the game's settings (the *Damage Settings*, not ADDON SETTINGS) set *Car Damage* to Simulation and *Car Damage Rate* to Simulation. The game decides how much damage a crash does, and the limping, the broken corners, STOP THE CAR and the terminal damage rules of the addon all work on that damage. With *Car Damage* on Off the game does no crash damage at all, so none of it can happen. Reduced and Standard are not tested: if you use them and something looks wrong, tell me what you set.
- **The failures need Realistic.** *Mechanical failures in Grand Prix* is **Realistic** by default (the Realistic preset) and the failures below work only with it. They come in races and sprints, not in practice or qualifying, and a shorter race gets fewer.
- **Pick the Failure season.** The team rates follow *Failure season* (default 2025). In 2023 to 2025 the teams hardly differed, so their rates are nearly equal; the big differences are in 2026 (Aston Martin and Cadillac fail far more often than the rest), 2014, 2015 and 2017. Pick the season of your car mod. The factor and the chance of every team in every season are in the [table of team failure factors](https://github.com/nyannoying1337/f1-25-terminal-damage/blob/main/docs/team-failure-factors.md).
- **Updating from 0.75.5:** nothing to set up. Your options stay as they are. In a Career you already have, a team that has researched Durability gets its benefit at once.

## New

- **Each team has its own failure rate.** With *Mechanical failures in Grand Prix* on Realistic, a team's cars now break down as often as that team's did in the season you pick, from its real retirements (2012-2026), instead of one rate for every car. In 2026 an Aston Martin car fails in about 38 % of Grands Prix, a Cadillac in about 28 %, a Mercedes in about 8 %. The season's overall rate is unchanged: the rates of the cars in your race add up to it, whichever teams are on the grid. A team's rate is pulled towards the season's average when its numbers are few. It works in Career too. A team the addon does not know gets the average; the log lists the teams it found and their rates (the line starting *teams: rates now*).
- **Career R&D matters for failures again.** In Career and My Team a team's finished Durability upgrades (the R&D department of that name) lower its failure rate: every team starts with the same five done, and each further finished upgrade takes about 1 % off, up to half with all of them done. It counts for you and for the AI teams, whose R&D the game develops during the season, and only for the team that did the research: when you research, the AI cars do not get worse, and when the AI teams research they get more reliable. Only researches that succeed count (the game lets some fail; a failed one does not count, and the same upgrade can be started again). New option *R&D durability lowers failures (Career)*, on by default; with it off only the real team rates count. At the start of a Career nothing changes. The log shows each team's Durability progress in the line starting *rd:*.

## Something not working?

Every part has its own option. Turn off only the part that gives you trouble for now (for example *Mechanical failures in Grand Prix*, *Damage status panel*, *Engineer radio* or *Suspension damage from hits*), keep the rest, and send me your log: loader.log in the newest folder of %LOCALAPPDATA%\Swofty\sessions. Post it on OverTake or open an issue on GitHub, and say what happened and roughly when.

## Known issues

- Realistic failures in Career are new: a failure of yours, or of an AI car, from the real rates has not been seen in a Career race yet (the test setting has). In Career an AI car that fails stayed where it stopped for about two minutes before the game took it to its garage.
- The Career R&D effect is small (about 1 % per Durability upgrade) and its cap of half with every upgrade done is a first guess: I will tune it once I have seen how fast the AI teams research.
- Suspension damage is experimental: its tuning can still change.
- Career: the "PART FITTED" message named in the 0.75.0 notes may not show when you fit the new part.
- Career is not fully tested with the limping, red flag and garage repair rules yet.
- No safety car HUD while your car is terminally damaged.
- Qualifying ICE replacement, Skip repairs and practice replacement timing: **not tested, might be broken**.

# Terminal Damage 0.75.5 beta

Realistic failures now work in Career and My Team too (experimental). Everything from 0.75.4 and 0.75.3 is in this release.

## Read this first

- **Career and My Team now use the realistic failures.** With *Mechanical failures in Grand Prix* on **Realistic** (the default), the game's own random failures are replaced by the season's real rates, for you and for the AI cars. It is new and has had only short tests in Career. If it gets in your way, set that option to Off: the game's own Career failures then stay as they were.
- **Updating from 0.75.4:** nothing to set up. Your options stay as they are.

## New

- **Realistic failures in Career and My Team:** yours, the AI cars' and the partial faults (slow shifts, a lost gear and so on), with the season's real rates (*Failure season*) and the warning signs. The game's own rolled failures are removed, and so is the game's own temporary fault for your car (it would be written into the save next to ours). So the teams' R&D reliability no longer decides who breaks down: every car gets the season's average rate. The AI cars keep the game's temporary faults.
- The *100 % (test)* setting of *Mechanical failures in Grand Prix* now fails you and two AI cars 15-60 s into the race, and it only breaks parts the season's cars have (no MGU-H in a 2026 car).

## Something not working?

Every part has its own option. Turn off only the part that gives you trouble for now (for example *Mechanical failures in Grand Prix*, *Damage status panel*, *Engineer radio* or *Suspension damage from hits*), keep the rest, and send me your log: loader.log in the newest folder of %LOCALAPPDATA%\Swofty\sessions. Post it on OverTake or open an issue on GitHub, and say what happened and roughly when.

## Known issues

- Realistic failures in Career are new: a failure of yours, or of an AI car, from the real rates has not been seen in a Career race yet (the test setting has). In Career an AI car that fails stayed where it stopped for about two minutes before the game took it to its garage.
- Suspension damage is experimental: its tuning can still change.
- Career: the "PART FITTED" message named in the 0.75.0 notes may not show when you fit the new part.
- Career is not fully tested with the limping, red flag and garage repair rules yet.
- No safety car HUD while your car is terminally damaged.
- Qualifying ICE replacement, Skip repairs and practice replacement timing: **not tested, might be broken**.

# Terminal Damage 0.75.4 beta

Realistic failures work again with the default settings. Everything from 0.75.3 is in this release too: suspension damage from hits works together with Heavier Damage, and the pause menu stops the failure clocks.

## Read this first

- **On 0.75.3? Update.** With *Suspension damage from hits* off (the default), 0.75.3 gave no realistic failures at all, neither to you nor to the AI cars.
- **Updating from 0.75.0:** nothing to set up. Your options stay as they are. What else is new is in the 0.75.3 notes below.

## Fixes

- **Realistic failures work again with *Suspension damage from hits* off (the default).** In 0.75.3 the failure clocks ran only while the addon read your car's speed, which it does only for suspension damage or a broken car, so neither your car nor the AI cars got a failure. The clocks now run while the game updates your car, and they still stop in the pause menu.

# Terminal Damage 0.75.3 beta

**Use 0.75.4:** with *Suspension damage from hits* off (the default), realistic failures do not work in 0.75.3.

Suspension damage from hits now works together with Heavier Damage, and the pause menu stops the failure clocks.

## Read this first

- **Updating from 0.75.0:** nothing to set up. Your options stay as they are.
- **Using Heavier Damage?** You can now turn on *Suspension damage from hits* with it. Heavier Damage's *Suspension & wheel damage* setting can stay where it is.

## New

- **Suspension damage from hits works with Heavier Damage.** While the option is on, the game's own suspension damage stays at stock on the cars it covers (your car; every car with *Suspension damage for AI cars*). Heavier Damage's *Suspension & wheel damage* (100x stronger at Normal) no longer adds up with it, also not in nose hits, where the front wing's damage spills into the front corners. Heavier Damage's wing damage and its other settings stay. With the option off, Heavier Damage's values apply as before.

## Fixes

- The pause menu stops the failure clocks: planned failures (yours and the AI's), warning signs, a gearbox decline and a power fault no longer move on while the game is paused.
- Less work per frame in races with realistic failures: the AI cars' failure check runs only when one is due.

## Something not working?

Every part has its own option. Turn off only the part that gives you trouble for now (for example *Mechanical failures in Grand Prix*, *Damage status panel*, *Engineer radio* or *Suspension damage from hits*), keep the rest, and send me your log: loader.log in the newest folder of %LOCALAPPDATA%\Swofty\sessions. Post it on OverTake or open an issue on GitHub, and say what happened and roughly when.

## Known issues

- Suspension damage is experimental: its tuning can still change.
- Career: the "PART FITTED" message named in the 0.75.0 notes may not show when you fit the new part.
- Career is not fully tested with the limping, red flag and garage repair rules yet.
- No safety car HUD while your car is terminally damaged.
- Qualifying ICE replacement, Skip repairs and practice replacement timing: **not tested, might be broken**.

# Terminal Damage 0.75.0 beta

The biggest update so far: realistic failures with warning signs, a damage HUD, engineer radio, presets and more FIA rules. Everything since 0.51.85 is in this release.

## Read this first

- **Presets:** the first option sets the main options at once: Casual, **Realistic (default)** or Hardcore. A one-time message on the main menu points to it.
- **The defaults are now the Realistic preset.** *Mechanical failures in Grand Prix* is **Realistic** (was Off), unless you set it yourself. Options you changed before stay as they are; the preset then shows Custom.
- **Suspension damage from hits is experimental and off by default.** Turn it on in the options if you want to try it (restart the game). Using Heavier Damage? Keep it off, or set Heavier Damage's *Suspension & wheel damage* to *Stock*: both mods damage the suspension, and the two add up.
- **Debug options** are now at the bottom of the list, and all of them are off, the test keys too.

## New

- **Realistic failures** (*Mechanical failures in Grand Prix: Realistic*, the default): parts fail at the real rate of the season you pick in *Failure season* (2012-2026), fewer in short races. Many give warning signs first: power loss, cut-outs, slow or rough shifts, a lost gear, rear brake trouble. Lift and coast to nurse the part. Some faults stay partial (slow shifts, a missing or jammed gear, DRS, ERS, lean fuel, a rear brake-by-wire fault). AI cars break down too. Not in Career, which keeps the game's own failures.
- **Damage status panel** (option *Damage status panel*: Auto, Always on track, Off): icon tiles at the top right of the race HUD for engine, ERS, gearbox, brakes, suspension and power, yellow, orange or red by how bad, with a short label (OUT, -15%, CUTS, x6, NO 3, FL, LIMP). Auto shows only what is wrong.
- **Engineer radio** for warning signs, failures, brake problems and broken corners, with the game's own voice lines. Messages titled ENGINEER give advice on screen.
- **Presets** (Casual, Realistic, Hardcore). Picked in the game: reopen ADDON SETTINGS to see the changes. Picked in the launcher: they apply at the next game start.
- **Crashes:** one broken corner may let the car crawl on: a collapsed suspension 85 % at the front, now also 60 % at the rear; a front wheel coming loose 35 %. Never once the game tears the wheel off (that stops the engine) or the wheel is gone. Losing the wheel or breaking another corner while crawling stops the car. Crash messages name the broken corners.
- **Race, broken corner (FIA):** "STOP THE CAR". Pull over as soon as it is safe or get to the pit lane; after 20 s of driving on track the car retires as unsafe. In practice and qualifying you still crawl back to the garage.
- **Heavy aero or body damage** that the game calls terminal (front wing, sidepods, floor, rear wing, diffuser): the car keeps its drive and you get "HEAVY DAMAGE": pit by the end of the next lap or retire. At a red flag it may be repaired. Before, the car lost all drive without a word.
- **More FIA rules:** after the game's own scripted failure, pit by the end of the next lap or retire (black flag with an orange disc). A car stopped on track in practice or qualifying stays withdrawn. A car left on track without drive at a red flag is abandoned.
- **Garage (practice and qualifying):** the "minutes left" of a corner rebuild now counts down; a withdrawn car says so ("CAR WITHDRAWN"). Career: a car with a failed engine or gearbox stays in the garage until you fit a new part in the Power Unit menu.
- **Fixed-chance failures** (10-50 %) now come at a random point of the race distance, using the game's own lap count.
- **Suspension damage from hits (experimental, off by default):** a clear hit on a wheel can bend that corner (the car pulls), weaken it, leave a hidden crack that lets go later, or break it. A badly damaged corner can collapse within minutes, usually over a kerb. Scrapes, boards and normal kerbs do not break a healthy corner. With it on, the game tears a wheel off only in big crashes. Shown on the damage panel and the game's damage MFD page, repaired in the practice and qualifying garage (4-20 minutes; the car stays in until it is done), undone by a flashback. *Suspension damage for AI cars* is a separate experimental option.

## Fixes

- After heavy damage a flashback could be watched but not resumed (X did nothing). Now it can, as long as the game itself has a flashback for you (flashbacks on, some left).
- A wheel the game only damaged (a flat tyre) counted as a broken corner: a broken front corner then could not limp, and the damage panel showed that wheel in red.
- The replay after a retirement has your car's engine sound again.
- A race restart clears the previous race's failures, warning signs and damage.
- Starting a practice programme with a car that may not leave its garage froze the garage screen. The car now comes straight back, with a message that says why.
- A "corner bent" message no longer hides a failure message shown a moment before.
- A flashback during the box stop or the garage push removes the race crew the addon called out.
- A retirement timer or pit deadline no longer runs out while the game holds your car at a red flag.
- Less work per frame, also after a retirement.

## Something not working?

Every part has its own option. Turn off only the part that gives you trouble for now (for example *Mechanical failures in Grand Prix*, *Damage status panel*, *Engineer radio* or *Suspension damage from hits*), keep the rest, and send me your log: loader.log in the newest folder of %LOCALAPPDATA%\Swofty\sessions. Post it on OverTake or open an issue on GitHub, and say what happened and roughly when.

## Known issues

- Suspension damage is experimental; it adds up with other damage mods.
- Career is not fully tested with the limping, red flag and garage repair rules yet.
- No safety car HUD while your car is terminally damaged.
- Qualifying ICE replacement, Skip repairs and practice replacement timing: **not tested, might be broken**.

## Debug options

All at the bottom of the list, labelled *Debug:*, and off. Leave them alone unless you are asked to test something.

- *Debug: UDP telemetry port* (was *UDP telemetry port*): only for the debug telemetry log; the addon reads your speed from the game itself.
- *Debug: fault type for the game's own failure*: only with *Game's own failure* in *Mechanical failures in Grand Prix*.
- *Debug: hotkey triggers a failure*: **off by default** (it was on in the test builds). Test keys, all with Ctrl+Shift: F10 the engine (ICE) fails, F7 a Career scripted failure, F6 the next partial failure, F9 cycles a throttle cap, F4 a test run of all faults (once: you drive, twice quickly: the AI drives).
- *Debug: crash limp roll*, *Debug: red flag repair roll*: force the dice rolls.
- *Debug: garage corner repair time*: about a minute instead of the normal repair times.
- *Debug: suspension test*: Ctrl+Shift+F8 steps toe, camber or suspension damage on the front-left wheel.


# Terminal Damage 0.51.85 beta

Flashback fixes on top of 0.51.80.

## Flashbacks

- **Flashbacks work without UDP telemetry.** Since 0.51.79 the mod no longer needs UDP, but a flashback still did: after a flashback to before a crash or failure the car stayed without throttle. The mod now sees the game's own flashback: back before the damage, the car is whole again with full power. A retirement you only requested (nothing broken) is cancelled by a flashback.
- **The game's own mechanical failures** (Random failures on): a flashback to just before the failure took all power away, and the AI then retired the car. Now the car is whole again with full power. If the game's failure comes back, it is handled like the first time: you keep the car.
- A flashback right after such a failure no longer loses the failure, and a healthy car can no longer be disqualified for "not pitting" after it.
- A flashback after a retirement (the game allows it) gives you your car back.
- After a flashback the mod reads your pit state back from the game.

## Other

- **A broken car stranded in the pit lane** (pit entry, pit exit or driving through) now retires after 10 s standing still, like on track. The box and garage have their own flow.
- **Engine off means no drive:** when the engine of a broken car is off, it gives no drive torque at all, so changing gears no longer moves the car slightly.
- Cleanup: unused code removed (one game hook less) and safer checks before the mod touches fixed game addresses.

# Terminal Damage 0.51.80 beta

- **Fix:** a car withdrawn for the rest of a practice or qualifying session could still go on track by starting a practice programme. The garage menu now switches off Practice Programmes too, next to drive out and flying lap.

# Terminal Damage 0.51.79 beta

Big update since 0.51.54: crashes can now leave you limping, red flags follow the FIA rules, broken cars get repaired (or stay out) in practice and qualifying, and the mod no longer needs the game's UDP telemetry.

## No UDP telemetry needed any more

- The mod now reads your speed directly from the game. Standing still for 10 s with a broken car retires you even with the game's UDP telemetry switched off.
- The mod's own UDP listener is now **off by default**, so it no longer blocks port 20777 for SimHub, CrewChief or other telemetry apps.
- **Upgrading:** if you only switched on UDP in the game for this mod, you can switch it off again.

## Crashes: limping on a broken front corner

- If a crash breaks exactly **one front corner**, there is a chance the car can still crawl on: 70 % with a broken suspension, 35 % with a lost wheel. A **BROKEN FRONT CORNER** message appears and the car keeps about a third of its power. Bring it back to the pits.
- A broken **rear** corner, or two or more broken corners: the car cannot be driven. Pull over; after 10 s standing still you retire.
- If a limping car hits something again and breaks another corner, it stops for good.

## Red flags follow the FIA rules

When a red flag comes out while your car is terminally damaged, the strategy screen decides what your team may do:

- **Engine, power unit or gearbox failure:** cannot be repaired during a red flag. A message tells you so and your car retires.
- **A car that could not be driven** (stopped on track): it was abandoned, so it retires.
- **A car limping on one broken front corner:** your team may rebuild the corner during the red flag. A dice roll decides: 80 % for a broken suspension, 50 % for a lost wheel. If it works, the corner and the front wing are repaired and you take part in the restart (**CAR REPAIRED**). If not, **REPAIRS NOT COMPLETED** and your car retires.
- Other accident damage (wings, bodywork) is repaired by the game as before.

## Practice and qualifying: repairs in the garage

- **Limp into the pit lane** on a broken front corner: the car goes into your garage and the team rebuilds the corner. This takes **20-30 minutes for a broken suspension and 30-45 minutes for a lost wheel**. The game shows the repair timer and a **CORNER REPAIR** message tells you how long. You can wait, or use the game's own skip / fast forward (this uses up session time). When it is done, **CAR REPAIRED** appears and you can drive out again. If the session ends first, your session is over.
- **A car that did not make it back** (retired on track, or Retire from Session) is **withdrawn for the rest of the session**: no repair timer, and the garage menu no longer offers drive out or flying lap (practice programmes too since 0.51.80). Your car is ready again in the next session. Before, the car briefly appeared at the pit exit and was sent straight back.
- An engine or gearbox failure still works as before: fit a new part in the garage (Career), then leave.

## Disqualification stays a disqualification

- If you are disqualified after a scripted failure (you did not pit before the end of the lap), the results screen now shows **DSQ** instead of DNF. The game used to overwrite it at the end of the session.
- Choosing Retire in the pause menu after a disqualification keeps the DSQ.

## Performance

- Fewer game memory reads every frame, and no UDP thread running unless you switch it on.

## New debug settings

For testing only, leave them on **Random** / **Normal**:

- **Debug: crash limp roll:** force the limp chance to always succeed or always fail.
- **Debug: red flag repair roll:** force the red flag repair to always succeed or always fail.
- **Debug: garage corner repair time:** 1 minute instead of 20-45 minutes.

## Known issues

- Career has not been fully tested with these changes yet.
- The garage repair was tested with a lost wheel; a broken suspension with the wheel still attached is not tested in game yet.
- Qualifying engine replacement times are still **NOT TESTED, MIGHT BE BROKEN**.
- A red flag during terminal damage can still cause AI/spectator camera issues.

# Terminal Damage 0.51.54 beta

Fix: a red flag could hang on a black screen in Career.

- The game's RETIRED hand-over is only kept for a failure the game planned for your car while you race on track. At a red flag, safety car or in the garage it passes through unchanged.
- If the red flag strategy screen is entered repeatedly, the addon no longer logs or probes it.

# Terminal Damage 0.51.53 beta (experimental)

- **Scripted failures in Career (experimental, still being tested):** failure chances and timing stay the game's own. When the game would hand your car to the AI, you keep control and the failed part limits your power. Retire yourself (pause menu, radio, hotkey, or by stopping for 10 s with UDP telemetry on).
- If you do not retire within 60 s, you are disqualified. The setting "After a scripted failure..." switches this to a normal retirement.
- Grand Prix test modes "Game's own failure" and "Game's own breakdown" reuse the game's failure scheduler; for testing only.

# Terminal Damage 0.51.14 beta

- Requires Swofty launcher 1.7.0 or newer.
- Use the launcher's new player-control event instead of the game telemetry call to track your car. No visible change in game.
- Standing-still retirement still needs the game's UDP telemetry (Settings > Telemetry: UDP on, 127.0.0.1, port 20777, format 2026).

# Terminal Damage 0.51.13 beta

- Fix the launcher's "Could not read ops/radio-probe-assets.json" note under What it changes. The Retirement radio voice option now picks its radio file by name instead of with a condition the launcher preview cannot read. Nothing changes in game.

# Terminal Damage 0.51.12 beta

- Replace the separate player tether switches with **Player wheel behavior**: Stock, Reduced jolting (experimental), or Loose wheels.
- Reduced jolting keeps the tethers and reduces the violent bouncing seen with detached wheels. Wheels can still move oddly and pass through the chassis or cockpit.
- Loose wheels removes player tethers. **Loose AI wheels** remains a separate option. Stock and AI removal off are the defaults; restart after changing either.
- Simplify the launcher settings by hiding garage implementation and debug controls. Their existing defaults remain unchanged.
- Reduce repeated checks during detached-wheel creation and remove routine collision-experiment logging.

**Upgrading:** choose your preferred Player wheel behavior again. Earlier player tether switches are replaced by the dropdown; the new default is Stock.

Known issues: reduced jolting is still experimental and is not a complete realistic tether simulation. Mid-push flashbacks and red-flag camera behavior remain limited. Qualifying ICE replacement / Skip repairs and practice replacement timing remain **NOT TESTED, MIGHT BE BROKEN**.

---

# Terminal Damage 0.51.8 beta

## Added

- Separate **Remove AI wheel tethers** option alongside **Remove player wheel tethers**. Both are **off by default** and require a game restart.
- Detached wheels become loose debris in the selected scope. Suspension damage and wheel collisions stay unchanged.
- Player-state interface for spectator addons to react to confirmed retirement and recovery. Terminal Damage does not switch cameras itself.

## Improved

- Reduce replay/flashback overhead when wheel-tether removal is enabled.
- Remove temporary automatic wheel diagnostics.
- Clarify the license and visible-credit requirements in the documentation.

## Known limitations

- Stopped retirement requires OnTrack; a car stranded in the pit lane or box exit does not trigger the 10-second timer.
- Rewinding into the middle of a garage push/box animation is not fully covered.
- Red flags during terminal damage can still cause AI/spectator camera issues.
- **Qualifying ICE replacement / Skip repairs and practice replacement timing: NOT TESTED, MIGHT BE BROKEN.**
- Hiding pit equipment does not guarantee collision removal. Intended for offline play; some cars and modes need more testing.

## Code reuse

Terminal Damage Attribution License 1.0 requires visible credit and a project link
when publishing a mod/project that uses substantial portions of this work. See
LICENSE. Earlier MIT releases keep their MIT terms; third-party SDK licenses stay
unchanged.

---

# Terminal Damage 0.51.3 beta

## Added

- Optional **Remove player wheel tethers** setting, **off by default**.
- Enable it if detached wheels pull or shake your car. Broken player wheels become loose debris; suspension damage and AI wheel tethers stay unchanged.
- **Restart the game after changing the setting.**

## Known limitations

- Stopped retirement requires OnTrack; a car stranded in the pit lane or box exit does not trigger the 10-second timer.
- Rewinding into the middle of a garage push/box animation is not fully covered.
- Red flags during terminal damage can still cause AI/spectator camera issues.
- **Qualifying ICE replacement / Skip repairs and practice replacement timing: NOT TESTED, MIGHT BE BROKEN.**
- Hiding pit equipment does not guarantee collision removal. Intended for offline play; some cars and modes need more testing.

---

# Terminal Damage 0.51.1 beta

## Fixed

- Stop the AI driving away after a voluntary retirement when you park on track.
- Power stays available until retirement completes. The car can still roll on a slope.

## Known limitations

- Stopped retirement requires OnTrack; a car stranded in the pit lane or box exit does not trigger the 10-second timer.
- Rewinding into the middle of a garage push/box animation is not fully covered.
- Red flags during terminal damage can still cause AI/spectator camera issues.
- **Qualifying ICE replacement / Skip repairs and practice replacement timing: NOT TESTED, MIGHT BE BROKEN.**
- Wheel-tether instability remains under investigation. Hiding pit equipment does not guarantee collision removal.

---

# Terminal Damage 0.51.0 beta

## Changed

- Clear tyre-stop equipment and seated tyre crew during race retirement, preserving the garage push crew and your teammate's stop.
- Learn garage assignments directly in the race; practice no longer needs to be loaded first.
- Fix restarting during a retirement push leaving the car in the garage or the HUD hidden.
- Improve flashback handling when retirement requests and later damage overlap, and cancel old crew/audio work after a rewind, restart or garage return.
- Keep the teammate-safe box-entry signal working when equipment clearance is disabled.
- Reduce temporary diagnostic logging during normal play.

## Known limitations

- Stopped retirement requires OnTrack; a car stranded in the pit lane or box exit does not trigger the 10-second timer.
- Rewinding into the middle of a garage push/box animation is not fully covered.
- Red flags during terminal damage can still cause AI/spectator camera issues.
- **Qualifying ICE replacement / Skip repairs and practice replacement timing: NOT TESTED, MIGHT BE BROKEN.**
- Collision removal is not guaranteed by hiding pit equipment. Offline sessions only; some cars and modes need more coverage.

## Code reuse

This release uses the Terminal Damage Attribution License 1.0. Published mods or
projects using substantial portions must visibly credit nyannoying and link to
Terminal Damage. Earlier MIT releases keep their MIT terms. See LICENSE.

---

# Terminal Damage 0.50.2 beta

## Fixed

- Instant Replay / flashbacks can now be opened after terminal damage while you still control your car. Rewinding to before the damage lets you continue driving again.
- Confirmed in practice, qualifying and race sessions. Crash damage and an engine failure were covered in the practice check.

## Known issues

- **Red flag while terminally damaged:** the game can take control or switch to the spectator view, followed by a delayed retirement. This is still under investigation and is not fixed in this update.
- **Qualifying ICE replacement / Skip repairs: NOT TESTED, MIGHT BE BROKEN.** The intended replacement time is at least 60 minutes so the remaining qualifying time expires.
- **Practice engine replacement timing: NOT TESTED, MIGHT BE BROKEN.** Normal game timing is intended to remain unchanged.
- Race garage pushing still requires loading practice first in the same game process. The latest pit-entry audio and missing-crew fallback changes still need in-game confirmation.

UDP format 2026 is required for the addon's telemetry features. For reports, send loader.log before restarting the game; include perf.log for performance problems.

---

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
