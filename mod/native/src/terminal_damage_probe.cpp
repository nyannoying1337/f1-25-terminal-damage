// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Terminal Damage: preserve player control after failure, apply power loss,
// and retire through a roadside stop or the player's garage crew.
//
// The addon finds its hook sites in F1_25.exe by unique byte signatures and installs them in SWOFTY_ON_LOAD
// (probe/47_on_load.inc): `jmp rel32` thunk redirects (hook_thunk), absolute-jump detours (detour), vtable slot
// patches (hook_vslot) and emitted code stubs, besides the Swofty events and the bus message wrap.
//
// This file is the umbrella of ONE translation unit: first the SWOFTY_SETTING defaults, then the ordered include list.
// The fragments probe/NN_*.inc and the modules that need the state before them are included inside one anonymous
// namespace; a fragment sees only what comes before it, so the order matters. The exported API, the presets, the
// welcome popup and SWOFTY_ON_LOAD follow at file scope. The tests include this file. The other src/*.hpp are
// separate modules, mostly plain logic without game access, that the tests can also include alone.

// winsock2.h has to come before windows.h, which the SDK headers include.
#include <winsock2.h>

// The SDK headers cast GetProcAddress to their table getters.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#include <swofty/events.hpp>
#include <swofty/game/core.hpp>
#include <swofty/mem.hpp>
#include <swofty/mixin.hpp>
#include <swofty/systems/cars.hpp>
#include <swofty/systems/sessions.hpp>
#include <swofty/systems/spectator.hpp>
#pragma clang diagnostic pop

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <mutex>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>
#include <windows.h>
#include "performance_probe.hpp"
#include "guarded_copy.hpp"
#include "garage_assignment.hpp"
#include "player_state_service.hpp"
#include "presets.hpp"

// The SDK header casts GetProcAddress to the table getter.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#include <swofty/hub.hpp>
#include <swofty/hud.hpp>
#include <swofty/lang.hpp>
#include <swofty/overlay.hpp>
#include <swofty/storage.hpp>
#include <swofty/telemetry.hpp>
#include <swofty/popup.hpp>
#include <swofty/assets.hpp>
#include <swofty/career.hpp>
#pragma clang diagnostic pop

SWOFTY_SETTING(bool, keep_control, true);
SWOFTY_SETTING(bool, rnd_reliability, true);  // 0.75.6: a team's finished Durability R&D lowers its failure rate (Career)
SWOFTY_SETTING(bool, engine_off_sound, true);
SWOFTY_SETTING(bool, pause_menu_retire, true);
SWOFTY_SETTING(bool, radio_command_probe, true);
SWOFTY_SETTING(bool, performance_diagnostics, false);
SWOFTY_SETTING(std::string_view, player_wheel_behavior, "stock");
bool wheel_tether_test=false;
SWOFTY_SETTING(bool, ai_wheel_tethers, false);
bool tether_plane_experiment=false;
bool tether_distance_experiment=false;
bool tether_collision_experiment=false;
SWOFTY_SETTING(bool, log_telemetry, false);
SWOFTY_SETTING(bool, debug_failure_hotkey, false);  // test keys, off by default
SWOFTY_SETTING(std::string_view, debug_failure_key, "F10");
SWOFTY_SETTING(std::string_view, debug_failure_part, "0");
SWOFTY_SETTING(std::string_view, damaged_power, "0.3");
SWOFTY_SETTING(bool, retire_in_pit_box, true);
SWOFTY_SETTING(bool, red_flag_retire, true);
SWOFTY_SETTING(bool, keep_game_failure, true);
SWOFTY_SETTING(std::string_view, game_failure_timeout, "dsq");
// Debug: force the crash dice rolls (random, success, fail) instead of playing many races.
SWOFTY_SETTING(std::string_view, debug_limp_roll, "random");
SWOFTY_SETTING(std::string_view, debug_repair_roll, "random");
SWOFTY_SETTING(std::string_view, debug_garage_repair, "normal");
// Debug: front-left suspension geometry/damage test (suspension_damage.hpp), off, toe, camber or damage.
SWOFTY_SETTING(std::string_view, debug_suspension_test, "off");
SWOFTY_SETTING(std::string_view, gp_failures, "realistic");  // the Realistic preset (0.75)
SWOFTY_SETTING(std::string_view, gp_fault_type, "random");
SWOFTY_SETTING(std::string_view, failure_era, "2025");
SWOFTY_SETTING(bool, engineer_radio, true);
SWOFTY_SETTING(std::string_view, damage_panel, "auto");
SWOFTY_SETTING(bool, garage_push, true);
SWOFTY_SETTING(bool, pit_retire_clearance, true);
SWOFTY_SETTING(bool, ai_terminal_cleanup, true);  // 0.75.8: put AI wrecks the game never removes in their garage (issue #10)
SWOFTY_SETTING(bool, garage_crew_in_race, true);
SWOFTY_SETTING(bool, retire_hotkey, true);
SWOFTY_SETTING(std::string_view, retire_key, "F11");
SWOFTY_SETTING(bool, crew_asset_diagnostics, false);
SWOFTY_SETTING(std::string_view, udp_port_choice, "off");
SWOFTY_SETTING(bool, garage_assets_in_race, true);
SWOFTY_SETTING(bool, suspension_damage, false);  // experimental and opt-in since 0.75 (Heavier Damage users)
SWOFTY_SETTING(bool, suspension_damage_ai, false);
SWOFTY_SETTING(std::string_view, preset, "realistic");  // the defaults are the Realistic preset (0.75)
std::atomic<bool> crew_diagnostics_active{false}; // cached for the frequently called crew callbacks

void report_pit_clearance();
bool release_retirement_box_entry();

namespace {

#include "probe/01_game_signatures.inc"  // game build signatures and the first hook pointers
#include "flashback_tracker.hpp"
#include "failure_model.hpp"
#include "team_model.hpp"
#include "damage_icons.hpp"
#include "brake_telemetry.hpp"
#include "ai_cleanup.hpp"
#include "cooldown_link.hpp"

#include "probe/02_core_state.inc"  // session, crash, repair and plan state

#include "probe/03_failure_state.inc"  // radio lines, rear-brake, test-run, plan and power state

#include "probe/04_crew_pit_state.inc"  // crew and pit-entry state, controller reasons, pit state names

#include "probe/05_controller_switch.inc"  // controller hand-over tracking, set-controller hook, participants scan

#include "probe/06_session_teams_events.inc"  // team ids from the session, per-team rates, session events

#include "probe/07_pit_state_hooks.inc"  // pit state hook, relays, thunk hooks, pit stop job and crew signatures

#include "probe/08_garage_crew.inc"  // the player's garage crew: spawn and removal

#include "probe/09_resets.inc"  // per-car, per-race and per-session resets

#include "probe/10_crash_limp.inc"  // corner damage names, limp chance and the crash limp decision

#include "probe/11_power_loss.inc"  // power_loss (throttle cap per failed part), crash repair, readers

#include "probe/12_failure_objects_faults.inc"  // failure objects, engineer radio, breakdowns, game faults, brake hook

#include "probe/13_test_samplers.inc"  // debug samplers: shift timing, throttle, brake telemetry

#include "probe/14_update_vehicle.inc"  // the per-car update hook that drives everything

#include "stopped_player_lookup.hpp"

#include "probe/15_stopped_cars_hooks.inc"  // stopped cars and debris hooks, vtable hooks, crew state logging

#include "probe/16_detour.inc"  // detour_at / detour: absolute-jump trampolines

#include "probe/17_wheel_tethers.inc"  // wheel detach hook and the wheel tether options

#include "probe/18_qualifying_repair.inc"  // qualifying and practice engine repair hook

#include "probe/19_garage_repair.inc"  // garage crash repair jobs, service text, garage exit lock

#include "probe/20_rnd_probe.inc"  // R&D probe (0.75.6, read only)

#include "probe/21_team_ids_rnd_rates.inc"  // team ids from game memory, Durability R&D shares, team rate log

#include "probe/22_team_id_probe.inc"  // team id probe (0.75.6, read only)

#include "probe/23_part_lock_garage_menu.inc"  // part lock, garage menu and exit hooks, crew state handlers

#include "probe/24_radio_retire.inc"  // the retire command in the radio menu

#include "probe/25_asset_diagnostics.inc"  // asset change diagnostics

#include "probe/26_code_stubs_tether_experiments.inc"  // emitted code stubs and the tether plane / distance experiments

#include "probe/27_race_garage_assets_crew_states.inc"  // garage assets in a race, crew state hooks

#include "probe/28_vehicle_status_retire.inc"  // vehicle status hook, bus posts, retire_player

#include "probe/29_red_flag.inc"  // red flag strategy

#include "probe/30_failure_scheduler.inc"  // failure planner, scheduler and register hooks, game failure DSQ

#include "probe/31_garage_return_failure_helpers.inc"  // return to garage, scripted failure and hotkey helpers

#include "probe/32_throttle_audio_replay_stubs.inc"  // throttle store, engine audio and replay terminal gate stubs

#include "probe/33_damage_rating_flashback_rule.inc"  // damage rating, flashback rule and flashback used-count hooks

#include "probe/34_flashback_retire.inc"  // flashback check, retire request, retire-local-player hook

#include "probe/35_race_laps_failure_tick.inc"  // game lap and race length, heavy damage tick, gp_failure_tick

#include "probe/36_scripted_failure_hotkeys.inc"  // scripted failure and the Ctrl+Shift hotkeys

#include "suspension_damage.hpp"

#include "probe/37_realistic_failures.inc"  // realistic Grand Prix failures: plan, AI cars, faults, warnings

#include "probe/38_power_faults_gearbox_decline.inc"  // partial power faults, gp_break, gearbox decline

#include "probe/39_test_run.inc"  // the Ctrl+Shift+F4 test run

#include "probe/40_damage_panel.inc"  // the damage panel overlay

#include "probe/41_events_udp.inc"  // session events, the UDP telemetry thread, player speed

#include "probe/42_standstill_probes_push.inc"  // standstill retirement, probes, push helpers, player state publish

#include "probe/43_frame.inc"  // engine kill, game suspension layers, on_frame

}

#include "probe/44_api_bus.inc"  // the exported player state API and the bus message wrap

#include "probe/45_presets.inc"  // presets

#include "probe/46_welcome_watchers.inc"  // welcome popup and setting watchers

#include "pit_retirement_clearance.hpp"

#include "probe/47_on_load.inc"  // SWOFTY_ON_LOAD installs every hook; SWOFTY_ADDON
