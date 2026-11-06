// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// SWOFTY_ON_LOAD installs every hook; SWOFTY_ADDON.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, at file scope.
// It is not a standalone header; it uses what the fragments before it define.

SWOFTY_ON_LOAD
{
    td_perf::enabled = bool(performance_diagnostics);
    crew_diagnostics_active = bool(crew_asset_diagnostics);
    if (udp_port_choice != std::string_view("off"))
        CloseHandle(CreateThread(nullptr, 0, udp_thread, nullptr, 0, nullptr));
    else
        swofty::log("[td-probe] udp: off (speed comes from the game)");
    reset_session();
    hook_asset_diagnostics();
    patch_garage_assets_in_race();
    guard_crew_scene_read();
    hook_throttle();
    configure_player_wheels(player_wheel_behavior);
    hook_wheel_tether_test();
    hook_tether_plane_experiment();
    hook_tether_distance_experiment();
    hook_engine_audio();
    hook_retire_local_player();
    register_radio_menu_probe();
    hook_radio_probe();
    hook_qualifying_engine_repair();
    hook_garage_crash_repair();
    hook_garage_menu();
    hook_exit_garage();
    attach_session_events();
    hook_brake_step();
    if (swofty::overlay::available()) {
        auto r = swofty::overlay::on_paint(&paint_damage_panel);
        swofty::log("[td-probe] damage panel: %s", r == swofty::overlay::Result::ok ? "on the overlay" : "overlay refused");
    } else {
        swofty::log("[td-probe] damage panel: no overlay on this core");
    }
    hook_thunk("VehicleControllerSystem::setController", sig_set_controller,
               reinterpret_cast<void *>(&on_set_controller), reinterpret_cast<void **>(&orig_set_controller));
    hook_thunk("Ai MessagePitStateChanged handler", sig_pit_state_changed,
               reinterpret_cast<void *>(&on_pit_state_changed), reinterpret_cast<void **>(&orig_pit_state_changed));
    hook_update_vehicle();
    hook_replay_terminal_gate();
    hook_damage_rating();
    hook_flashback_rule();
    hook_flashback_used();
    hook_vslot("DeploymentPointsAccumulatorStoppedCars::update", sig_stopped_cars_update,
               reinterpret_cast<void *>(&on_stopped_cars_update), reinterpret_cast<void **>(&orig_stopped_cars_update));
    if (crew_asset_diagnostics)
        hook_vslot("DeploymentPointsAccumulatorDebris::update", sig_debris_update,
                   reinterpret_cast<void *>(&on_debris_update), reinterpret_cast<void **>(&orig_debris_update));
    find_terminal_message();
    find_status_request();
    find_crew_event();
    find_pit_entry();
    find_crew_msg();
    hook_pit_stop_job();
    hook_pit_clearance();
    find_garage_crew_spawn();
    find_hud_restart_cleanup();
    if (!hook_damage_session_restart()) {
        restart_cleanup_available=false;
        spawn_crew=nullptr;
        swofty::log("[td-probe] garage: restart cleanup unavailable; selective race spawning disabled");
    }
    hook_crew_states();
    hook_red_flag_strategy();
    find_crash_repair();
    susp::install(suspension_damage, debug_suspension_test, suspension_damage_ai);
    hook_change_vehicle_status();
    hook_failure_planner();
    hook_rules_enter();
    hook_failure_register();
    hook_failure_scheduler();
    detour("PitLaneSystem::setVehicleStatus", sig_set_vehicle_status, set_vehicle_status_prologue,
           sizeof set_vehicle_status_prologue, reinterpret_cast<void *>(&on_set_vehicle_status),
           reinterpret_cast<void **>(&orig_set_vehicle_status));
    for (const char *e : {SWOFTY_EVENT_SESSION_START, SWOFTY_EVENT_SESSION_END, SWOFTY_EVENT_RACE_START,
                          SWOFTY_EVENT_STATE, SWOFTY_EVENT_FLAG, SWOFTY_EVENT_RESULTS})
        swofty::events::subscribe(e, on_event);
    swofty::events::on_frame(on_frame);
    swofty::log("[td-probe] loaded version=0.75.9, keep_control=%d asset_diagnostics=%d garage_assets_in_race=%d crew_in_race=%d perf_counters=%d",
                bool(keep_control), bool(crew_asset_diagnostics), bool(garage_assets_in_race),
                bool(garage_crew_in_race), int(td_perf::enabled.load()));
}

SWOFTY_ADDON()
