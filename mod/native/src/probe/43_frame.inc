// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Engine kill, game suspension layers, on_frame.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Frame thread: keep the engine.power_scale = 0 layer on exactly the car in engine_kill_vehicle.
void apply_engine_kill()
{
    namespace live = swofty::cars::live;
    static live::Layer layer;
    static int applied = -1;
    int want = session_ended ? -1 : engine_kill_vehicle.load();
    if (want == applied)
        return;
    if (applied >= 0) {
        layer.release();
        swofty::log("[td-probe] engine: drive torque back for car %d", applied);
    }
    applied = want;
    if (want < 0)
        return;
    layer = live::Layer::set(live::Cars::car(live::Car(uint32_t(want))), "engine.power_scale", 0.0f, 100);
    auto v = live::Car(uint32_t(want)).get(live::Field::engine_power_scale);
    if (layer.ok())
        swofty::log("[td-probe] engine: switched off for car %d, no drive torque (engine.power_scale now %.2f)", want,
                    v ? double(*v) : -1.0);
    else
        swofty::log("[td-probe] engine: could not cut the drive torque of car %d (result %d), throttle cap only", want,
                    int(layer.result()));
}

// The game's own wheel detach limits (F1SuspensionDamage WheelDetachScale W 1.0 and PartsDetachScale P 1.5,
// re/collision-impact.md section 6): a corner whose damage D reaches P is the game's "wheel coming off", its terminal
// crash damage, and that switches the engine off (limp_chance). It was 60 % of the crashes that killed the car in
// 0.51.127-0.51.130 (18 of 30), 14 of the 18 below D 3.0; the front wing's overflow adds into D too (0x14465fdc0: the
// part over the wing's cap / its damage scale x the front damage scale), it does not set D to P. With the addon's
// suspension model on (test report after 0.51.132: 'the car is always dead'), W and P are game_detach_factor x higher (W 2,
// P 3, the wheel gone at 6), so a moderate hit is the model's to judge (hold, bend, break now or later, limp) and only
// a big crash still tears the wheel off. The player's car, or every car when the model runs for the AI too; without
// the AI model the AI keeps the game's limits.
constexpr float game_detach_factor = 2.0f;

// The layers on the game's own suspension damage while the addon's model runs: the detach limits x
// game_detach_factor, and the front and rear damage scales set back to stock. Another addon can raise those scales
// (Heavier Damage's "Suspension & wheel damage": front 0.1 and rear 0.01 at Normal, 100x stock); its damage then adds
// up with the model's, and the front scale also sizes the front wing's overflow into FL/FR, which broke front corners
// in nose hits without any wheel contact (0.51.115 test at Miami: FL 58.4 and 57.6). The speed threshold stays the
// game's (or the other addon's). With the model off nothing is layered: the game's or the other addon's values apply.
struct GameSuspLayer {
    uint32_t field, op;
    float value;
    const char *name;
};
constexpr GameSuspLayer game_susp_layers[] = {
    {SWOFTY_CAR_SUSP_WHEEL_DETACH_SCALE, SWOFTY_CAR_OP_SCALE, game_detach_factor, "wheel detach limit"},
    {SWOFTY_CAR_SUSP_PARTS_DETACH_SCALE, SWOFTY_CAR_OP_SCALE, game_detach_factor, "parts detach limit"},
    {SWOFTY_CAR_SUSP_FRONT_DAMAGE_SCALE, SWOFTY_CAR_OP_SET, susp::stock_front_scale, "front damage scale"},
    {SWOFTY_CAR_SUSP_REAR_DAMAGE_SCALE, SWOFTY_CAR_OP_SET, susp::stock_rear_scale, "rear damage scale"},
};
constexpr int game_susp_layer_n = int(sizeof game_susp_layers / sizeof game_susp_layers[0]);

// Frame thread: the game_susp_layers by field id (the layer path names are the core's), moved with the player's car (a
// restart in another car) or on every car. A field the core refuses keeps its value, the others still apply; without
// the core's car layers nothing changes.
void apply_game_suspension_layers()
{
    static uint32_t tokens[game_susp_layer_n] = {};
    static bool refused[game_susp_layer_n] = {};
    static int applied = -2;  // -2 none, -1 every car, >= 0 the player's car
    static bool late_logged = false;
    static uint64_t retry_at = 0;
    if (!susp::enabled)
        return;
    int player = player_vehicle.load();
    int want = susp::ai::enabled ? -1 : player >= 0 ? player : applied;
    if (want == applied || want == -2 || GetTickCount64() < retry_at)
        return;
    int usable = 0;
    for (bool r : refused)
        usable += !r;
    if (!usable)
        return;
    auto t = swofty::detail::live_car_ops_v2();
    if (!t)
        return;  // the core's car layers are not ready yet (or missing): tried again next frame
    auto release = [&] {
        for (uint32_t &token : tokens)
            if (token)
                t->unlayer(std::exchange(token, 0u));
    };
    release();
    applied = want;
    uint32_t index = want < 0 ? SWOFTY_CAR_ALL : uint32_t(want);
    for (int k = 0; k < game_susp_layer_n; ++k) {
        if (refused[k])
            continue;
        const GameSuspLayer &l = game_susp_layers[k];
        int r = t->layer("addon", index, l.field, l.op, l.value, 50, &tokens[k]);
        if (r == SWOFTY_ASSET_OK)
            continue;
        tokens[k] = 0;
        if (r == SWOFTY_ASSET_LATE) {  // the car or the flow thread is not there yet: all of them again in 2 s
            release();
            applied = -2;
            retry_at = GetTickCount64() + 2000;
            if (!std::exchange(late_logged, true))
                swofty::log("[td-probe] suspension: the layers on the game's suspension damage are not on yet "
                            "(result 1), trying again");
            return;
        }
        refused[k] = true;
        swofty::log("[td-probe] suspension: the game's %s stays as it is (no car layer, result %d)", l.name, r);
    }
    char on[256] = "", now[256] = "";
    size_t on_len = 0, now_len = 0;
    for (int k = 0; k < game_susp_layer_n; ++k) {
        const GameSuspLayer &l = game_susp_layers[k];
        float v = -1;
        if (player < 0 || !t->get || t->get(uint32_t(player), nullptr, l.field, &v) != SWOFTY_ASSET_OK)
            v = -1;
        if (now_len < sizeof now)
            now_len += size_t(snprintf(now + now_len, sizeof now - now_len, "%s%s %g", k ? ", " : "", l.name,
                                       double(v)));
        if (!refused[k] && on_len < sizeof on)
            on_len += size_t(snprintf(on + on_len, sizeof on - on_len, "%s%s %s%g", on_len ? ", " : "", l.name,
                                      l.op == SWOFTY_CAR_OP_SCALE ? "x" : "= ", double(l.value)));
    }
    swofty::log("[td-probe] suspension: layers on the game's own suspension damage on %s: %s (your car now: %s; -1 "
                "until it is built): moderate hits are the model's, another addon's raised scales do not add up",
                want < 0 ? "every car" : "your car", on[0] ? on : "none", now);
}

void on_frame(double dt, void *)
{
    td_perf::Scope frame_scope(td_perf::frame_total);  // destroyed last: includes the player state publish
    struct PublishOnExit {
        ~PublishOnExit()
        {
            td_perf::Scope publish(td_perf::frame_state);
            publish_player_state();
            td_state::service.dispatch();
        }
    } publish_on_exit;
    td_perf::report();
    td_perf::report_frame(!session_ended && player_vehicle.load() >= 0);
    {
        td_perf::Scope menu(td_perf::frame_menu);
        report_pit_clearance();
        if (race_restart_pending.exchange(false)) {
            reset_race_faults();
            swofty::log("[td-probe] restart: the last race's failures, faults and plans are cleared");
        }
        red_flag_toast_tick();
        preset_tick();
        welcome_tick(GetTickCount64());
        apply_engine_kill();
        apply_game_suspension_layers();
    }
    if (session_ended)
        return;
    {
        td_perf::Scope input(td_perf::frame_input);
        process_radio_command();
        uint64_t request = pending_failure.load();
        if (request && GetTickCount64() >= (request >> 16) && pending_failure.compare_exchange_strong(request, 0))
            swofty::log("[td-probe] debug failure: request timed out without a player update");
        poll_hotkey();
    }
    {
        td_perf::Scope suspension(td_perf::frame_suspension);
        susp::tick(debug_suspension_test, flashback_count.load());
    }
    {
        td_perf::Scope cars(td_perf::frame_cars);
        part_lock_tick();
        team_ids_from_memory();
        read_rnd_durability();
        team_rates_log_tick();
        rnd_probe_tick();
        team_id_probe_tick();
        static uint32_t crew_flashbacks = 0;
        if (uint32_t fb = flashback_count.load(); fb != crew_flashbacks) {
            crew_flashbacks = fb;
            remove_owned_crew_after_flashback();
        }
    }
    // on_frame gets real seconds on every rendered frame, also in the pause menu, where the car does not move (no
    // fresh speed): there the wear clocks stand (the failure plans of your car and the AI's, warning signs, a gearbox
    // decline, a power fault). In 0.75.1 they ran on through a 7-minute pause (Montreal test).
    double drive_dt = drive_seconds(dt);
    {
        td_perf::Scope failures(td_perf::frame_failures);
        gp_failure_tick(drive_dt);
    }
    {
        td_perf::Scope test(td_perf::frame_test);
        test_run_tick(dt);
    }
    {
        td_perf::Scope power(td_perf::frame_power);
        power_tick(drive_dt);
    }
    td_perf::Scope rest(td_perf::frame_cars);  // to the end: probes, crew pushes, retire timers
    probe_crew_entry(dt);
    probe_speed(dt);
    probe_engine(dt);
    if (player_controller_flags.load() & (1u<<5)) return;
    if (expire_timer(crew_request_in,dt) && active_push_work()) post_crew_event(crew_request.exchange(0));
    if (int request=pending_crew.exchange(-1); request>=0 && active_push_work() && crew_spawn_wanted)
        post_crew_event(uint32_t(request));
    if (expire_timer(crew_msg_in,dt) && active_push_work()) post_crew_msg(crew_msg.exchange(0));
    if (spawn_crew && crew_spawn_now.exchange(false) && active_push_work() && !crew_spawn_wanted.exchange(true)) {
        if (spawn_player_crew()) crew_seq_t=0.0;
        else abandon_unavailable_push();
    }
    if (double t=crew_seq_t.load(); t>=0 && active_push_work() && crew_spawn_wanted) {
        double n=t+dt;
        if (crew_seq_t.compare_exchange_strong(t,n>=0.8 ? -1.0 : n)) {
            if (t<0.3 && n>=0.3 && active_push_work()) post_crew_event(crew_prepare_push);
            if (t<0.8 && n>=0.8 && active_push_work()) post_crew_event(crew_push);
        }
    }
    standstill_retire(dt);
    race_stop_tick(dt);
    limp_watch_tick(dt);
    // A retirement during the red flag hand-over (controller flags 10/11, about 5.5 s before the teleport) ended the
    // race at once and left the camera on another car (0.50.2 log 900136fe). The standstill timer already waits
    // (0.50.13); the box, push and game-failure timers wait here too.
    constexpr uint32_t held_flags = (1u << 10) | (1u << 11);
    bool held = (player_controller_flags.load() & held_flags) && retire_in.load() >= 0;
    static bool held_logged = false;
    if (held && !held_logged)
        swofty::log("[td-probe] retire: timer %.1f s held while the game holds the car (flags 0x%x)", retire_in.load(),
                    player_controller_flags.load());
    held_logged = held;
    if (!held && expire_timer(retire_in,dt,-2.0) && player_broken) {
        if (practice_like()) {
            if (player_pit.load() == 1) {
                garage_withdrawn = true;  // stopped on track: abandoned on the circuit (FIA 26.18 / B1.8.9)
                // 0.51.123 test: nothing said why the car stayed in its garage and the engine change did not run.
                if (swofty::hud::available())
                    swofty::hud::toast("CAR WITHDRAWN",
                                       "Your car stopped on track: out for this session (FIA). Skip Session to go on.", 8);
            }
            garage_return_wanted = true;
        } else {
            retire_player();
        }
    }
}
