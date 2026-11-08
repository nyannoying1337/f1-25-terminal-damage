// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// The per-car update hook that drives everything.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

uint64_t on_update_vehicle(void *system, uint8_t vehicle)
{
    if (session_ended)
        return orig_update_vehicle(system, vehicle);
    td_perf::Scope before(td_perf::vehicle_before);
    apply_pending_failure(system, vehicle);
    before.finish();
    uint64_t r = orig_update_vehicle(system, vehicle);
    td_perf::Scope after(td_perf::vehicle_after);
    if (vehicle < 32 && (ai_watch_mask.load(std::memory_order_relaxed) & (1u << vehicle)) &&
        int(vehicle) != player_vehicle.load())
        ai_watch_tick(reinterpret_cast<uintptr_t>(system), vehicle);
    if (susp::ai_wanted()) {
        if (int(vehicle) != player_vehicle.load())
            susp::ai_game_thread(vehicle, car_of(reinterpret_cast<uintptr_t>(system), vehicle));
        else
            susp::ai_forget(vehicle);
    }
    if (int(vehicle) == player_vehicle.load()) {
        player_update_ms = GetTickCount64();
        cooldown_tick(player_update_ms.load());
        uintptr_t car = car_of(reinterpret_cast<uintptr_t>(system), vehicle);
        throttle_hook.car = car;
        if (player_broken || log_telemetry || susp::speed_wanted() || test_run_on.load() || power_speed_wanted.load() ||
            !player_has_moved.load())
            read_game_speed(car);
        susp::game_thread(car);
        sample_throttle(car);
        register_fault(car);
        slow_shift_tick(car);
        brake_fault_tick(car);
        radio_tick(car);
        test_shift_tick(car);
        test_brake_tick(car);
        arm_breakdowns();
        breakdown_watch_tick();
        if (red_flag_fix_now.exchange(false))
            repair_crash_damage(car);
        garage_repair_finish(car);
        read_terminal_record(reinterpret_cast<uintptr_t>(system), vehicle);
        flashback_check(car);
        power_loss(car);
        return_to_garage();
        game_failure_dsq_now();
    }
    return r;
}

// The 14-byte prologue is plain pushes and `sub rsp, imm8`, so it runs unchanged in a trampoline; the entry gets
// `jmp [rip+0]` to the handler.
bool hook_update_vehicle()
{
    return detour("VehicleTerminalDamageSystem::updateVehicle", sig_update_vehicle, update_vehicle_prologue,
                  sizeof update_vehicle_prologue, reinterpret_cast<void *>(&on_update_vehicle),
                  reinterpret_cast<void **>(&orig_update_vehicle));
}
