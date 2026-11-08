// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Power_loss (throttle cap per failed part), crash repair, readers.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Throttle cap for the player's broken car (game thread, inside updateVehicle): a failed ICE or gearbox gives no
// drive (cap 0, the car rolls out), a failed MGU-K, MGU-H, ES, turbo or CE leaves `damaged_power` of the throttle.
// Ctrl+Shift+F9 (test) overrides it.
void power_loss(uintptr_t car)
{
    static int logged = -1;
    static uintptr_t last_car;
    if (log_telemetry && car != last_car) {
        // 0.25.2 crash log: after the crash car +0x170.. read all zero while UDP throttle was 1, so the car object
        // may change on terminal crash damage. Log every change to see it.
        float thr = -1;
        if (car)
            guarded_read(car + 0x188, thr);
        swofty::log("[td-probe] power: player car object %p -> %p (throttle %.2f)", reinterpret_cast<void *>(last_car),
                    reinterpret_cast<void *>(car), double(thr));
        last_car = car;
    }
    FlashbackTracker::Rewind damage_rewind;
    if (car && damage_flashback_tracker.damage_rewind(GetTickCount64(), damage_rewind) &&
        damage_rewind.mask != terminal_mask.load()) {
        uintptr_t engine=0, gearbox=0;
        bool restored=guarded_read(car+0x530,engine) && engine && guarded_read(car+0x538,gearbox) && gearbox;
        for (int i=0; restored && i<7; ++i) {
            float wear=1;
            uint32_t retained_bit = i==0 ? 1u : i==6 ? 4u : 2u;
            restored=guarded_read(i<6 ? engine+part_offsets[i] : gearbox+0x674,wear) &&
                     std::isfinite(wear) && wear>=0 && (wear<0.9f || (damage_rewind.mask & retained_bit));
        }
        if (restored && damage_flashback_tracker.commit_damage(damage_rewind)) {
            bool retain_request=retire_requested && !flashback_tracker.ready(GetTickCount64());
            int pit=player_pit.load();
            bool keep_timeline=retain_request || damage_rewind.mask;
            reset_car_state(false,keep_timeline,damage_rewind.mask!=0);
            terminal_mask=damage_rewind.mask;
            retire_requested=retain_request;
            player_broken=retain_request || damage_rewind.mask;
            if (player_broken) player_pit=pit;
            swofty::log("[td-probe] flashback: restored damage mask=0x%x; earlier retirement=%d",damage_rewind.mask,int(retain_request));
        }
    }
    if (car && player_broken && player_pit.load() >= 1 && player_pit.load() <= 3 &&
        flashback_tracker.consume_retained_resume(GetTickCount64())) {
        int pit = player_pit.load();
        // Resuming after the lock preserves the failure/request. Re-arm only
        // the approach stage; old future garage reports must not fire again.
        if (pit >= 1 && pit <= 3) {
            cancel_crew_work();
            retire_in = -1.0;
            still_for = 0;
            engine_off_standing = false;
            retired_engine_off_vehicle = -1;
            engine_off_vehicle = -1;
            engine_kill_vehicle = -1;
            pushing = pit >= 2; prepare_at_box = pit >= 2;
            pit_clearance_hold_vehicle = -1;
            garage_return_wanted = false; exit_check_at = 0;
            swofty::log("[td-probe] flashback: resumed after damage/request in pit state %d; keeping retirement and rearming approach", pit);
        }
    }
    if (car && flashback_tracker.ready(GetTickCount64())) {
        uintptr_t engine = 0, gearbox = 0;
        bool restored = guarded_read(car + 0x530, engine) && engine &&
                        guarded_read(car + 0x538, gearbox) && gearbox;
        for (int i = 0; restored && i < 7; ++i) {
            float wear = 1;
            restored = guarded_read(i < 6 ? engine + part_offsets[i] : gearbox + 0x674, wear) &&
                       std::isfinite(wear) && wear >= 0 && wear < 0.9f;
        }
        if (restored) {
            swofty::log("[td-probe] flashback: resumed before damage/request with restored parts; clearing the lock");
            reset_car_state();
        }
    }
    // Repair research: while the dead car sits in its practice garage, log its part wear (ICE, MGU-K, MGU-H, ES,
    // TC, CE, gearbox) whenever it changes, to see what a part swap or the game's repair does to it.
    if (car_dead && car) {
        static int last_parts[7] = {-1, -1, -1, -1, -1, -1, -1};
        int parts[7] = {-1, -1, -1, -1, -1, -1, -1};
        uintptr_t eng = 0, gb = 0;
        float v = 0;
        if (guarded_read(car + 0x530, eng) && eng)
            for (int i = 0; i < 6; i++)
                if (guarded_read(eng + part_offsets[i], v))
                    parts[i] = int(v * 1000.0f);
        if (guarded_read(car + 0x538, gb) && gb && guarded_read(gb + 0x674, v))
            parts[6] = int(v * 1000.0f);
        if (log_telemetry && memcmp(parts, last_parts, sizeof parts)) {
            memcpy(last_parts, parts, sizeof parts);
            swofty::log("[td-probe] repair: garage car %p parts ICE=%d MGU-K=%d MGU-H=%d ES=%d TC=%d CE=%d gearbox=%d "
                        "(per mille)",
                        reinterpret_cast<void *>(car), parts[0], parts[1], parts[2], parts[3], parts[4], parts[5],
                        parts[6]);
        }
        // Career part swap (0.31.5 tests): a fitted new engine sets the failed part back near 0 on the next send-out
        // attempt; without a swap it stays at 1000, or 999 once the car is really out (0.32.3 test). Once no part
        // is above 900 per mille, the car may leave again.
        // Crash damage and a plain retirement (no failed part) keep the car in the garage.
        bool readable = true, failed = false;
        for (int p : parts) {
            readable = readable && p >= 0;
            failed = failed || p >= 900;
        }
        if (failed) {
            garage_part_failed = true;
            for (int i = 0; i < 7; ++i)
                if (parts[i] >= 900)
                    garage_failed_types.fetch_or(1u << i);
        }
        if (readable && !failed && garage_part_failed && !(terminal_mask.load() & 0x18) && !garage_withdrawn.load()) {
            swofty::log("[td-probe] repair: the failed part was replaced, your car may leave the garage again");
            int pit=player_pit.load();
            reset_car_state();
            player_pit=pit;
            // Engine state flags at engine+0x2530 (UDP snapshot writer 0x142d00a63): bit 0 blown, bit 1 seized. The
            // swap resets the wear but keeps them, and a seized engine made the out-lap AI stop (0.32.1 test).
            uint8_t flags = 0;
            if (eng && guarded_read(eng + 0x2530, flags) && (flags & 3)) {
                *reinterpret_cast<volatile uint8_t *>(eng + 0x2530) = uint8_t(flags & ~3);
                swofty::log("[td-probe] repair: cleared engine flags 0x%02x (blown/seized)", flags);
            }
            if (swofty::hud::available())
                swofty::hud::toast("PART REPLACED", "Your car is ready again", 4);
        }
    }
    float cap = 1.0f;
    int why = 0;
    uintptr_t engine = 0, gearbox = 0;
    if (keep_control && player_broken && car) {
        float v = 0, pu = fraction(damaged_power);
        // Terminal crash damage (broken suspension): the game makes the car near undrivable, so it only rolls out.
        if (terminal_mask.load() & 0x18) {
            why = 8;
            decide_limp(car);
            if (limp_state.load() == 1)
                why = 11;
        }
        if (!why && guarded_read(car + 0x530, engine) && engine) {
            if (guarded_read(engine + part_offsets[0], v) && v >= 1.0f)
                why = 1;
            for (int i = 1; i < 6 && !why; i++)
                if (guarded_read(engine + part_offsets[i], v) && v >= 1.0f)
                    why = 2 + i;
        }
        if (!why && guarded_read(car + 0x538, gearbox) && gearbox && guarded_read(gearbox + 0x674, v) &&
            v >= 1.0f)
            why = 2;
        // The game's own (random) failures end in the same terminal message. If no part reads as failed, its mask
        // decides (bit 0 engine, 1 power unit, 2 gearbox; the corners 3/4 above). Aero or body damage only (10)
        // keeps full drive since 0.51.126 and the car is called in (heavy_damage_call).
        uint32_t mask = terminal_mask.load();
        if (!why && mask)
            why = mask & 1 ? 1 : mask & 4 ? 2 : mask & 2 ? 9 : 10;
        cap = why == 1 || why == 2 || why == 8 ? 0.0f : why == 11 ? limp_power : why && why != 10 ? pu : 1.0f;
    }
    // Terminal crash damage (suspension): the engine physics keeps idling, so the engine sound stops after a random
    // 3-6 s, as a stalled engine would.
    static uint64_t crash_off_at;
    uint64_t now = GetTickCount64();
    if (why != 8)
        crash_off_at = 0;
    else if (!crash_off_at)
        crash_off_at = now + 3000 + now % 3001;
    bool off = why == 1 || (why == 8 && now >= crash_off_at) || (player_broken && engine_off_standing) ||
               (retired_engine_off_vehicle.load() >= 0 && retired_engine_off_vehicle.load() == player_vehicle.load());
    engine_off_vehicle = off && engine_off_sound ? player_vehicle.load() : -1;
    engine_kill_vehicle = off ? player_vehicle.load() : -1;
    replay_terminal_vehicle = keep_control && player_broken && player_pit.load() == 1 && !session_ended ?
                                  player_vehicle.load() : -1;
    float t = test_cap.load();
    bool parked_retirement = retired_drive_lock_vehicle.load() >= 0 && retired_drive_lock_vehicle.load() == player_vehicle.load();
    throttle_hook.cap = parked_retirement ? 0.0f : t >= 0 ? t : std::min(cap, warning_cap.load());
    if (why != logged) {
        static const char *const names[] = {"none", "ICE", "gearbox", "MGU-K", "MGU-H", "ES", "turbo", "CE", "crash damage",
                                             "power unit (mask)", "heavy aero or body damage (mask)",
                                             "limping on a broken corner"};
        swofty::log("[td-probe] power: failed part %s, throttle cap %.2f", names[why], double(cap));
        logged = why;
    }
}

// Crash damage repair, the way the game's multiplayer damage sync (network::VehicleMechanicalManager::update,
// 0x1432495e0) mirrors a remote car: the car's F1SuspensionDamage component (car+0x5d0, owner+8 = car, vtable
// 0x145bf8bd0) packs 3 bits per corner (0x1446568e0: 0 intact, 1 puncture, 2 suspension broken, 3-5 detach stages; corners RL, RR, FL, FR), and
// 0x144656a80(component, packed) moves every corner to the given state. State 0 reattaches the wheel (part+0x38 = 0,
// slot 0xa8 removes the loose body), clears the broken suspension flags (wheel+0x20), the suspension damage
// (wheel+0x98) and a puncture. The front wing (car+0x5c8) takes Car_Damage_SetComponentDamage 0x144660cd0(wing, 0).
using wheel_state_get_fn = uint32_t (*)(void *);
using wheel_state_apply_fn = void (*)(void *, uint32_t);
using component_damage_fn = void (*)(void *, uint32_t);
wheel_state_get_fn wheel_state_get = nullptr;
wheel_state_apply_fn wheel_state_apply = nullptr;
component_damage_fn front_wing_damage_set = nullptr;
bool crash_repair_available() { return wheel_state_get && wheel_state_apply; }
void find_crash_repair()
{
    uintptr_t get = 0, apply = 0, wing = 0;
    int a = swofty::api->scan(".xtls", "56 57 48 83 EC 28 48 89 CE 48 8B 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? B8 01 00 00 00 87 05 "
                              "?? ?? ?? ?? 48 8B 46 08 8B 4E 68 33 4E 70", &get);
    int b = swofty::api->scan(".xtls", "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 98 00 00 00 C5 78 29 84 24 80 00 00 00 "
                              "C5 F8 29 7C 24 70 C5 F8 29 74 24 60 89 D6 48 89 CF 48 8B 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? "
                              "B8 01 00 00 00 87 05 ?? ?? ?? ?? 4C 8B 6F 08 48 89 F9 E8", &apply);
    int c = swofty::api->scan(".xtls", "56 57 48 83 EC 58 C5 F8 29 7C 24 40 C5 F8 29 74 24 30 89 D7 48 89 CE C5 FA 10 41 38 "
                              "C5 FA 10 49 48 C5 F2 58 71 4C", &wing);
    if (a == 1 && b == 1) {
        wheel_state_get = reinterpret_cast<wheel_state_get_fn>(get);
        wheel_state_apply = reinterpret_cast<wheel_state_apply_fn>(apply);
    }
    if (c == 1)
        front_wing_damage_set = reinterpret_cast<component_damage_fn>(wing);
    swofty::log("[td-probe] crash repair: corner state %d/%d hits, front wing %d hits", a, b, c);
}
// The car's suspension damage component (checked: its owner pointer +8 is the car), or 0.
uintptr_t suspension_damage_of(uintptr_t car)
{
    uintptr_t owner = 0, back = 0;
    if (!car || !guarded_read(car + 0x5d0, owner) || !owner || !guarded_read(owner + 8, back) || back != car)
        return 0;
    return owner;
}
// Game thread. Packed corner states of the car (3 bits per corner, 0 intact).
uint32_t corner_states(uintptr_t car, bool &ok)
{
    uintptr_t owner = suspension_damage_of(car);
    ok = owner && wheel_state_get;
    return ok ? wheel_state_get(reinterpret_cast<void *>(owner)) : 0;
}
// Debug override of a dice roll: "success" passes whenever there is a chance at all, "fail" never passes.
unsigned forced_roll(std::string_view mode, unsigned chance, unsigned roll)
{
    if (mode == "success" && chance)
        return 0;
    if (mode == "fail")
        return 100;
    return roll;
}
unsigned roll_percent()
{
    static std::mt19937 rng{static_cast<uint32_t>(GetTickCount64() ^ GetCurrentProcessId())};
    return unsigned(rng() % 100);
}
bool has_detached_corner(uint32_t packed)
{
    for (int i = 0; i < 4; ++i)
        if (((packed >> (3 * i)) & 7) >= 3)
            return true;
    return false;
}
// Game thread. Number of corners not intact, -1 unknown.
int damaged_corners(uintptr_t car)
{
    uintptr_t owner = suspension_damage_of(car);
    if (!owner || !wheel_state_get)
        return -1;
    uint32_t packed = wheel_state_get(reinterpret_cast<void *>(owner)), n = 0;
    for (int i = 0; i < 4; ++i)
        n += ((packed >> (3 * i)) & 7) >= 2;  // 1 is only a puncture
    return int(n);
}
// Game thread: the team replaces the broken corner and the front wing.
void repair_crash_damage(uintptr_t car)
{
    uintptr_t owner = suspension_damage_of(car), wing = 0;
    if (!owner || !crash_repair_available()) {
        swofty::log("[td-probe] crash repair: not possible (car %p, component %p)", reinterpret_cast<void *>(car),
                    reinterpret_cast<void *>(owner));
        return;
    }
    uint32_t before = wheel_state_get(reinterpret_cast<void *>(owner));
    wheel_state_apply(reinterpret_cast<void *>(owner), 0);
    susp::request_reset("repair");
    uint32_t after = wheel_state_get(reinterpret_cast<void *>(owner));
    bool wing_done = false;
    if ((terminal_mask.load() & 0x40) && front_wing_damage_set && guarded_read(car + 0x5c8, wing) && wing) {
        front_wing_damage_set(reinterpret_cast<void *>(wing), 0);
        wing_done = true;
    }
    swofty::log("[td-probe] crash repair: corners 0x%03x -> 0x%03x, front wing %s", before, after,
                wing_done ? "repaired" : "untouched");
}

// Game thread, right after the game updated the record. During a red flag repair wait, a record that is no longer
// terminal means the game's repairs fixed the car: clear the addon's broken state so it takes part in the restart.
void read_terminal_record(uintptr_t sys, uint8_t vehicle)
{
    uint64_t count = 0;
    uint32_t overall = 0, live = 0, states[23] = {};  // record +0x10 .. +0x6c: parts at +0x10, overall at +0x68
    uintptr_t rec = sys + 0x50 + uint64_t(vehicle) * 0x70;
    if (!sys || !guarded_read(sys + 0xc20, count) || vehicle >= count || !guarded_copy(rec + 0x10, states, sizeof states))
        return;
    overall = states[(0x68 - 0x10) / 4];
    for (int i = 0; i < 11; ++i)
        if (states[i] == 5)
            live |= 1u << i;
    if (player_broken && (int(overall) != live_terminal_state.load() || live != live_terminal_mask.load()))
        swofty::log("[td-probe] terminal record: state %u, parts at terminal 0x%x", overall, live);
    // Without the flashback hook: the record of a broken car on track turning normal again is a flashback to before
    // the damage (the red flag repair and the garage have their own paths).
    if (!flashback_hooked && player_broken && terminal_mask.load() && live_terminal_state.load() == 5 && overall != 5 &&
        !live && player_pit.load() == 1 && !red_flag_repair_wait.load())
    {
        flashback_used_at = GetTickCount64();
        flashback_count.fetch_add(1);
    }
    live_terminal_state = int(overall);
    live_terminal_mask = live;
    if (red_flag_repair_wait.load() && overall != 5 && !live && red_flag_repair_wait.exchange(false)) {
        swofty::log("[td-probe] red flag: the game repaired your car (mask was 0x%x), it takes part in the restart",
                    terminal_mask.load());
        reset_car_state(false);
        red_flag_repaired_toast = true;
    }
}

// The player's speed straight from the game, so the addon does not need the game's UDP telemetry. The game's own
// car telemetry writer (0x142cf9820) reads it like this: body = [[car+0x558]+0x340]+0x18, velocity (m/s) at body+0x220,
// speed in km/h = |velocity| * 3600 * 0.001.
std::atomic<int> game_speed{-1};
std::atomic<uint64_t> game_speed_ms{0};
// The game's last update of the player's car (on_update_vehicle, every frame while the car runs). The game stops
// updating the cars in the pause menu, the flashback view and between sessions; on_frame still gets real seconds.
std::atomic<uint64_t> player_update_ms{0};
void read_game_speed(uintptr_t car)
{
    uintptr_t physics = 0, rigid = 0, body = 0;
    float v[4] = {};
    if (!car || !guarded_read(car + 0x558, physics) || !physics || !guarded_read(physics + 0x340, rigid) || !rigid ||
        !guarded_read(rigid + 0x18, body) || !body || !guarded_copy(body + 0x220, v, sizeof v))
        return;
    float kmh = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]) * 3.6f;
    if (!std::isfinite(kmh) || kmh > 1000.0f)
        return;
    game_speed = int(kmh);
    game_speed_ms = GetTickCount64();
}

// Issue #10: an AI car with terminal damage normally stands for about 14 s, then the game's virtual session puts it in
// its garage (setVehicleStatus(v, 9) from 0x142f73486); a car one of the addon's realistic failures broke takes 15 to
// 110 s. Log what a watched car does (every second for 20 s, then every 10 s) and, when the game never removes it,
// do that removal ourselves (ai_cleanup.hpp decides when). Game thread, from updateVehicle of the watched car.
bool ai_cleanup_racing()
{
    // On the player's controller: NOT_IN_SESSION 3, SESSION_NOT_STARTED 10, SAFETY_CAR 11 (a red flag shows as the
    // same), FORMATION_LAP_REFORM_GRID 13, CINEMATIC_SAFETY_CAR 16. The session flow owns the cars then.
    constexpr uint32_t not_racing = (1u << 3) | (1u << 10) | (1u << 11) | (1u << 13) | (1u << 16);
    return !session_ended && !practice_like() && !(player_controller_flags.load() & not_racing) &&
           !red_flag_screen_open.load() && !red_flag_repair_wait.load();
}

// One look at a watched car at tick `now` (ai_watch_tick adds the once-a-second throttle; the tests step the clock).
void ai_watch_step(uintptr_t sys, uint8_t v, uint64_t now)
{
    if (v >= 32)
        return;
    const uint32_t bit = 1u << v;
    uintptr_t car = car_of(sys, v), physics = 0, rigid = 0, body = 0;
    int32_t record = -1;
    float vel[4] = {}, throttle = -1.0f, kmh = -1.0f;
    guarded_read(sys + 0x50 + uint64_t(v) * 0x70 + 0x68, record);  // 5 = terminal
    if (car)
        guarded_read(car + 0x188, throttle);
    if (car && guarded_read(car + 0x558, physics) && physics && guarded_read(physics + 0x340, rigid) && rigid &&
        guarded_read(rigid + 0x18, body) && body && guarded_copy(body + 0x220, vel, sizeof vel))
        kmh = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2] + vel[3] * vel[3]) * 3.6f;
    int pit = int(observed_pit_state[v].load()) - 1;
    double t = double(now - ai_watch_since[v].load()) / 1000.0;
    // How long the car has stood: below still_kmh since this tick (0: moving, or the speed could not be read).
    if (kmh >= 0 && kmh < aic::still_kmh) {
        if (!ai_watch_still_since[v].load())
            ai_watch_still_since[v] = now;
    } else if (kmh >= 0) {
        ai_watch_still_since[v] = 0;
    }
    uint64_t still_since = ai_watch_still_since[v].load();
    aic::Input in;
    in.since_terminal = t;
    in.standing = still_since && now >= still_since ? double(now - still_since) / 1000.0 : 0.0;
    in.record = record;
    in.pit = pit;
    in.racing = ai_cleanup_racing() && !cooldown_parked(v);  // issue #19: Cool-down's parc ferme cars
    in.enabled = ai_terminal_cleanup;
    in.already_asked = (ai_watch_asked.load() & bit) != 0;
    const aic::Action act = aic::decide(in);
    const char *end = act == aic::Action::in_garage   ? " (done)"
                      : act == aic::Action::healed    ? " (whole again)"
                      : act == aic::Action::give_up   ? " (gave up)"
                                                      : nullptr;
    bool stuck = !end && t > 25 && kmh >= 0 && kmh < aic::still_kmh;
    static uint64_t last_logged[32];
    if (t <= 20 || end || act == aic::Action::send_to_garage || now >= last_logged[v] + 10000) {
        last_logged[v] = now;
        swofty::log("[td-probe] AI watch: vehicle=%u +%.0fs pit=%d record=%d speed=%.0f throttle=%.2f flashbacks=%u%s", v,
                    t, pit, record, double(kmh), double(throttle), flashback_count.load(),
                    end ? end : stuck ? " STUCK" : "");
    }
    if (end) {
        ai_watch_mask.fetch_and(~bit);
        ai_watch_asked.fetch_and(~bit);
        return;
    }
    if (act == aic::Action::send_to_garage) {
        swofty::log("[td-probe] AI cleanup: vehicle=%u stands %.0fs after its terminal damage (it has stood %.0fs, pit %d) "
                    "and the game has not removed it: sending it to its garage", v, t, in.standing, pit);
        ai_watch_asked.fetch_or(bit);  // once per car, whether or not the call could be made
        ai_watch_asked_at[v] = now;
        if (!send_ai_car_to_garage(v))
            swofty::log("[td-probe] AI cleanup: vehicle=%u could not be sent (no pit lane system yet)", v);
        return;
    }
    if (in.already_asked && now >= ai_watch_asked_at[v].load() + uint64_t(aic::asked_wait_s * 1000.0)) {
        swofty::log("[td-probe] AI cleanup: vehicle=%u is still not in its garage %.0fs after the request, left to the game",
                    v, aic::asked_wait_s);
        ai_watch_mask.fetch_and(~bit);
        ai_watch_asked.fetch_and(~bit);
    }
}

void ai_watch_tick(uintptr_t sys, uint8_t v)
{
    static uint64_t next[32];
    uint64_t now = GetTickCount64();
    if (v >= 32 || now < next[v])
        return;
    next[v] = now + 1000;
    ai_watch_step(sys, v, now);
}
