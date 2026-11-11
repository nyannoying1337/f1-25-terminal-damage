// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Standstill retirement, probes, push helpers, player state publish.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// A broken car that stands still for 10 s retires, like the game's own retirement: on track, and also stranded in the
// pit lane outside the box (EnteringPitLane, LeavingPitLane, driving through). The box states 3-13 have their own flow.
bool standstill_pit_state(int pit)
{
    return pit == 1 || pit == 2 || pit == 14 || pit == 15;
}
// Race: the grace for a car crawling on a broken corner counts only while it drives on track; standing still
// the 10 s standstill retirement takes over, in the pit lane the box flow. Held while the game holds the car.
void race_stop_tick(double dt)
{
    double left = race_stop_left.load();
    if (left <= 0)
        return;
    if (!player_broken || limp_state.load() != 1 || session_ended || practice_like()) {
        race_stop_left = 0;
        race_stop_lap = -1;
        race_stop_crossing = -1;
        return;
    }
    int speed = player_speed();
    if (player_pit.load() != 1 || retire_in.load() != -1.0 || speed <= 5 ||
        (player_controller_flags.load() & ((1u << 10) | (1u << 11))))
        return;
    if (int by = race_stop_lap.load(); by >= 0) {  // the model's collapsed corner: in by the end of the next lap
        int lap = game_lap();
        if (lap < 0 || lap < by)
            return;
        race_stop_left = 0;
        race_stop_lap = -1;
        swofty::log("[td-probe] crash: the limping car stayed out past lap %d, retiring", by);
        if (swofty::hud::available())
            swofty::hud::toast("RETIRED", "Unsafe car: it was not brought in", 4);
        retire_in = 0.0;
        return;
    }
    if (int by = race_stop_crossing.load(); by >= 0) {  // the same, by line crossings
        if (player_crossings.load() < by)
            return;
        race_stop_left = 0;
        race_stop_crossing = -1;
        swofty::log("[td-probe] crash: the limping car crossed the line twice without coming in, retiring");
        if (swofty::hud::available())
            swofty::hud::toast("RETIRED", "Unsafe car: it was not brought in", 4);
        retire_in = 0.0;
        return;
    }
    left -= dt;
    race_stop_left = left > 0 ? left : 0;
    if (left > 0)
        return;
    swofty::log("[td-probe] crash: the car was not stopped within %.0f s of driving on a broken corner, retiring",
                race_stop_grace);
    if (swofty::hud::available())
        swofty::hud::toast("RETIRED", "Unsafe car: you did not stop", 4);
    retire_in = 0.0;
}

// For 30 s after a crash decision, every 2 s: what the car does (0.51.130; a test report: in Career practice a limping car
// showed 35 % throttle but would not drive or shift). Gear index 0 reverse, 1 neutral, 2 first; the gearbox refuses
// every shift while flags 0x1c are set (0x144742350: 0x04 stuck, 0x08 timed, 0x10 a missing gear asked for), and
// flags 0x03 are its failure. Corners: 3 bits each, RL RR FL FR (corner_states).
void limp_watch_tick(double dt)
{
    static double since = 0;
    double left = limp_watch_left.load();
    if (left <= 0) {
        since = 0;
        return;
    }
    limp_watch_left = left - dt > 0 ? left - dt : 0;
    since += dt;
    if (since < 2.0)
        return;
    since = 0;
    uintptr_t car = player_car(), gearbox = 0;
    int32_t gear = -1, target = -1;
    uint8_t flags = 0;
    float wear = -1;
    if (car && guarded_read(car + 0x538, gearbox) && gearbox) {
        guarded_read(gearbox + 0x680, gear);
        guarded_read(gearbox + 0x684, target);
        guarded_read(gearbox + 0x658, flags);
        guarded_read(gearbox + 0x674, wear);
    }
    bool ok = false;
    uint32_t corners = car ? corner_states(car, ok) : 0;
    // 0.51.133: the engine's state flags (engine+0x2530: bit 0 blown, bit 1 seized, 0x18 ERS faults) and the UDP rpm
    // (-1 without fresh UDP data), to find what switches the engine off after the game's own terminal crash damage.
    uintptr_t engine = 0;
    uint8_t engine_flags = 0xff;
    if (car && guarded_read(car + 0x530, engine) && engine && !guarded_read(engine + 0x2530, engine_flags))
        engine_flags = 0xff;
    int p = udp_player.load(), rpm = -1;
    if (p >= 0 && p < udp_cars && udp_last_ms.load() && GetTickCount64() - udp_last_ms.load() < 1000)
        rpm = udp_rpm[p].load();
    swofty::log("[td-probe] limp watch: speed %d km/h, gear index %d (target %d), gearbox flags 0x%02x wear %.2f, "
                "corners 0x%03x, throttle cap %.2f, engine kill %d, engine flags 0x%02x, rpm %d, limp %d, controller "
                "flags 0x%x, pit state %d, the game's damage rating %d", player_speed(), gear, target, flags,
                double(wear), ok ? corners : 0xfffu, double(throttle_hook.cap), engine_kill_vehicle.load(), engine_flags,
                rpm, limp_state.load(), player_controller_flags.load(), player_pit.load(), player_rating.load());
}

void standstill_retire(double dt)
{
    int speed = player_speed();
    if ((player_controller_flags.load() & ((1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<10)|(1u<<11))) || !player_broken || retire_in.load() != -1.0 || !standstill_pit_state(player_pit.load()) || speed < 0) {
        still_for = 0;
        // Once retiring the engine stays off; it only comes back for a car that is no longer broken.
        if (!player_broken)
            engine_off_standing = false;
        return;
    }
    still_for = speed <= 2 ? still_for + dt : 0;
    // Pulled over: the driver switches the engine off after 6 s, before the retirement at 10 s.
    if (still_for >= 6.0 && !engine_off_standing)
        swofty::log("[td-probe] engine: standing still for 6 s, engine off");
    engine_off_standing = still_for >= 6.0;
    if (still_for >= 10.0) {
        engine_off_standing = true;
        swofty::log("[td-probe] retire: your broken car stood still for 10 s (pit state %d), retiring", player_pit.load());
        still_for = 0;
        retire_in = 0.0;
    }
}

// Warns once per session when no UDP car telemetry has arrived at all (UDP off, wrong format or port); with
// log_telemetry also logs the player's UDP values every 3 s.
void probe_speed(double dt)
{
    static double since, waited;
    static int warned_vehicle = -1;
    int p = udp_player.load(), v = player_vehicle.load();
    bool fresh = p >= 0 && p < udp_cars && udp_last_ms.load() && GetTickCount64() - udp_last_ms.load() < 1000;
    // Only when the game's own speed never arrived this session: while paused (flashback screen) it is just stale
    // (0.51.94 log 20261103-094910 warned during a flashback). Only time without it counts (0.51.114 log: the time
    // before a race restart made it warn at the restart).
    bool game_speed_seen = game_speed_ms.load() > telemetry_not_before.load();
    waited = v >= 0 && !fresh && !game_speed_seen && player_pit.load() == 1 ? waited + dt : 0;
    if (waited > 15 && warned_vehicle != v && !udp_last_ms.load() && !game_speed_seen && player_speed() < 0) {
        warned_vehicle = v;
        swofty::log("[td-probe] speed: no speed from the game's car and no UDP car telemetry (packets %llu). Set Telemetry Settings: UDP on, "
                    "127.0.0.1, port %u, format 2026. Without it, standing still does not retire a broken car (use the "
                    "retire hotkey).",
                    (unsigned long long)udp_packets.load(), unsigned(udp_port()));
    }
    since += dt;
    if (!log_telemetry || since < 3.0)
        return;
    since = 0;
    if (!fresh) {
        if (player_speed() >= 0)
            swofty::log("[td-probe] speed: %d km/h from the game's car (no UDP), cap=%.2f", player_speed(),
                        double(throttle_hook.cap));
        return;
    }
    swofty::log("[td-probe] udp: player %d (vehicle %d) speed=%d km/h rpm=%d gear=%d throttle=%.2f cap=%.2f", p, v,
                udp_speed[p].load(), udp_rpm[p].load(), udp_gear[p].load(), udp_throttle[p].load(),
                double(throttle_hook.cap));
}

// Optional telemetry diagnostics: engine flags on change, then rpm/gear/speed
// every second for 20 s after failure. This is not needed for audio shutdown.
void probe_engine(double dt)
{
    if (!log_telemetry)
        return;
    static uint32_t last_state = 0xffffffff;
    static double broken_for = -1, since;
    int p = udp_player.load();
    if (p < 0 || p >= udp_cars)
        return;
    uint32_t state = udp_engine_state.load();
    if (state != last_state && state != 0xffffffff) {
        last_state = state;
        swofty::log("[td-probe] engine: udp damage gearbox=%u%% engine=%u%% ice_wear=%u%% blown=%u seized=%u "
                    "(rpm=%d gear=%d speed=%d mask=0x%x)",
                    state & 0xff, state >> 8 & 0xff, state >> 16 & 0xff, state >> 24 & 1, state >> 25 & 1,
                    udp_rpm[p].load(), udp_gear[p].load(), udp_speed[p].load(), terminal_mask.load());
    }
    if (!player_broken) {
        broken_for = -1;
        return;
    }
    if (broken_for < 0)
        broken_for = since = 0;
    broken_for += dt;
    since += dt;
    if (broken_for > 20.0 || since < 1.0)
        return;
    since = 0;
    swofty::log("[td-probe] engine: broken %.0f s rpm=%d gear=%d speed=%d throttle=%.2f cap=%.2f mask=0x%x",
                broken_for, udp_rpm[p].load(), udp_gear[p].load(), udp_speed[p].load(), udp_throttle[p].load(),
                double(throttle_hook.cap), terminal_mask.load());
}

void abandon_unavailable_push()
{
    // No real crew means no push reports: synthesizing them teleported a
    // race car into the garage when the practice mapping was not available.
    crew_spawn_wanted = false;
    crew_seq_t = -1.0;
    crew_request_in = -1.0;
    crew_msg_in = -1.0;
    crew_request = 0;
    crew_msg = 0;
    pending_crew = -1;
    pushing = false;
    pushed = false;
    retire_in = 2.0;
    swofty::log("[td-probe] garage: no usable crew; retiring in the box without push reports (load practice first)");
    if (swofty::hud::available())
        swofty::hud::toast("NO GARAGE CREW", "Load practice first; retiring in your box", 5);
}

bool expire_timer(std::atomic<double> &timer,double dt,double inactive=-1.0)
{
    double old=timer.load();
    while (old>=0) {
        double next=old-dt;
        if (timer.compare_exchange_weak(old,next<0 ? inactive : next)) return next<0;
    }
    return false;
}
bool active_push_work() { return !session_ended && keep_control && player_broken && pushing; }

void publish_player_state()
{
    int vehicle=player_vehicle.load(), retired=confirmed_retired_vehicle.load();
    uint32_t state=TD_UNAVAILABLE, mask=terminal_mask.load();
    if (retired>=0 && retired<max_vehicles && (vehicle<0 || vehicle==retired)) {
        vehicle=retired;state=TD_RETIRED;
    } else if (!session_ended.load() && vehicle>=0 && vehicle<max_vehicles) {
        if (player_pit.load()==9) state=TD_IN_GARAGE;
        else if (retire_requested || pushing || retire_in.load()>=0) state=TD_RETIRING;
        else if (player_broken || car_dead || mask) state=TD_LIMPING;
        else state=TD_RUNNING;
    } else {vehicle=-1;mask=0;}
    td_state::service.publish(state,vehicle,mask);
}
