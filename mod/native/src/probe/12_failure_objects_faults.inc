// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Failure objects, engineer radio, breakdowns, game faults, brake hook.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

double uniform01();
void *(*orig_failure_register)(void *, const void *) = nullptr;
// Calls f(vehicle, obj) for every car's failure object of the current session's manager (entries mgr+0x198..+0x1a0,
// 0x10 bytes each, object at +8, vehicle at obj+0x78).
template <class F> void for_each_failure_obj(F f)
{
    uint64_t mgr = failure_mgr.load(), begin = 0, end = 0;
    if (!mgr || !guarded_read(mgr + 0x198, begin) || !guarded_read(mgr + 0x1a0, end) || !begin || end < begin ||
        end - begin > 0x10 * 64)
        return;
    for (uint64_t e = begin; e < end; e += 0x10) {
        uint64_t obj = 0;
        uint8_t idx = 0xff;
        if (guarded_read(e + 8, obj) && obj && guarded_read(obj + 0x78, idx) && idx < 32)
            f(int(idx), uintptr_t(obj));
    }
}
uintptr_t failure_obj_of(int vehicle)
{
    uintptr_t found = 0;
    for_each_failure_obj([&](int v, uintptr_t obj) {
        if (v == vehicle && !found)
            found = obj;
    });
    return found;
}
// The player's per-car failure object (obj+0x70 car, obj+0x78 vehicle), only when it belongs to the car being driven
// now. Game thread. Cached: the manager walk costs about 47 reads and ran twice on every player update (radio, slow
// shifts; 0.51.117 review: 3.8 ms/s). The cached object is checked with two reads (its car and vehicle) and looked up
// again when the manager, the vehicle or the car changes.
uintptr_t player_failure_obj(uintptr_t car)
{
    static uint64_t cached_mgr = 0;
    static int cached_vehicle = -1;
    static uintptr_t cached_car = 0, cached_obj = 0;
    if (!car)
        return 0;
    uint64_t mgr = failure_mgr.load(), obj_car = 0;
    int vehicle = player_vehicle.load();
    uint8_t idx = 0xff;
    if (cached_obj && mgr == cached_mgr && vehicle == cached_vehicle && car == cached_car &&
        guarded_read(cached_obj + 0x70, obj_car) && obj_car == car && guarded_read(cached_obj + 0x78, idx) &&
        int(idx) == vehicle)
        return cached_obj;
    uintptr_t obj = failure_obj_of(vehicle);
    cached_mgr = mgr;
    cached_vehicle = vehicle;
    cached_car = car;
    cached_obj = obj && guarded_read(obj + 0x70, obj_car) && obj_car == car ? obj : 0;
    return cached_obj;
}

// Game thread: play the next pending engineer line. Once per session it logs the planner's own radio names (cfg =
// [[obj+0x98]+0xb8]: +0x1c8.. fault imminent/occurred/fixed, +0x2a0.. failure suspected .. retirement destination),
// which shows the name format the speech interface takes.
void radio_tick(uintptr_t car)
{
    static uint64_t last_play = 0;
    static uintptr_t logged_for = 0;
    // Nothing to say, or the last line less than 6 s ago: no reads (this runs on every player update).
    uint32_t pending = radio_pending.load();
    if (!pending || GetTickCount64() < last_play + 6000)
        return;
    uintptr_t obj = player_failure_obj(car), ctx = 0, speech = 0, vtable = 0, fn = 0;
    if (!obj || !guarded_read(obj + 0x98, ctx) || !ctx || !guarded_read(ctx + 8, speech) || !speech ||
        !guarded_read(speech, vtable) || !vtable || !guarded_read(vtable + 0x98, fn) || !fn)
        return;
    if (logged_for != obj) {
        logged_for = obj;
        uintptr_t cfg = 0;
        if (guarded_read(ctx + 0xb8, cfg) && cfg) {
            static const uint32_t offsets[] = {0x1c8, 0x1d0, 0x1e8, 0x1f0, 0x1f8, 0x200, 0x208, 0x2a0, 0x2a8, 0x2b0,
                                               0x2b8, 0x2d0, 0x2e0};
            for (uint32_t off : offsets) {
                uintptr_t str = 0;
                char text[64] = {};
                if (guarded_read(cfg + off, str) && str && guarded_copy(str, text, sizeof text - 1))
                    swofty::log("[td-probe] radio: the game's line at cfg+0x%x is \"%.*s\"", off,
                                int(strnlen(text, sizeof text - 1)), text);
            }
        }
    }
    int line = 0;
    while (!(pending & (1u << line)))
        ++line;
    radio_pending.fetch_and(~(1u << line));
    last_play = GetTickCount64();
    using play_fn = uint64_t (*)(void *, const char *, uint64_t, uint64_t);
    uint64_t r = reinterpret_cast<play_fn>(fn)(reinterpret_cast<void *>(speech), radio_names[line], 0, 0);
    swofty::log("[td-probe] radio: played %s (result 0x%llx)", radio_names[line], (unsigned long long)r);
}

// Game thread: arm the game's own breakdown sequence (re/game-failure-scheduler.md section 4) for the requested cars:
// part +0xa0, state +0xa1 = 0, lap +0xa4 = the car's lap now, fraction +0xa8 = 0 (due at once), +0xac time, +0xb0,
// +0xb1 = 0 (a real failure, not the false alarm). AI cars: the part breaks at once and the AI retires the car the
// stock way. The player: the "failure suspected" radio, a few seconds later the part breaks ("confirmed"), then the
// RETIRED hand-over the addon handles as a game failure. A player car whose sequence cannot be armed gets the
// addon's own failure instead.
void arm_breakdowns()
{
    uint32_t want = breakdown_request.exchange(0);
    if (!want)
        return;
    int player = player_vehicle.load();
    for (int v = 0; v < 32; ++v) {
        if (!(want & (1u << v)))
            continue;
        uint8_t part = breakdown_part[v].load();
        uintptr_t obj = failure_obj_of(v);
        // Lap 0, fraction 0 is due in any lap (the trigger fires on `lap < current lap` or the same lap with the
        // fraction reached), so the lap getter is not needed (it gave -1 in a Grand Prix race).
        int lap = obj ? game_lap_of(obj) : -1;
        uint8_t state = 0xff, old_part = 0xff;
        bool ok = obj && guarded_read(obj + 0xa1, state) && guarded_read(obj + 0xa0, old_part) &&
                  // a sequence already running (states 1-4) is left alone
                  (old_part >= 7 || state == 0 || state >= 5);
        if (ok) {
            auto *o = reinterpret_cast<volatile uint8_t *>(obj);
            uint32_t l = 0;
            float f = 0.0f;
            o[0xa0] = part;
            o[0xa1] = 0;
            memcpy(const_cast<uint8_t *>(o) + 0xa4, &l, 4);
            memcpy(const_cast<uint8_t *>(o) + 0xa8, &f, 4);
            memset(const_cast<uint8_t *>(o) + 0xac, 0, 4);
            o[0xb0] = 0;
            o[0xb1] = 0;
            if (v == player && !failure_planner_obj.load())
                failure_planner_obj = obj;
            if (v == player)
                breakdown_watch = {obj, part, GetTickCount64()};
        }
        swofty::log("[td-probe] breakdown: %s car %d part %u at lap %d%s", ok ? "armed" : "could not arm",
                    v, unsigned(part), lap, v == player ? " (yours)" : "");
        if (!ok && v == player)
            trigger_failure(part);
    }
}

void breakdown_watch_tick()
{
    // Work on a copy: reset_race_faults clears breakdown_watch on the frame thread while this runs on the game thread,
    // and the write below must never go through an object pointer that was zeroed in between.
    const breakdown_watch_state b = breakdown_watch;
    if (!b.obj)
        return;
    uint8_t state = 0xff, part = 0xff;
    if (player_broken || !guarded_read(b.obj + 0xa1, state) || !guarded_read(b.obj + 0xa0, part) || part != b.part ||
        state != 0) {
        if (!player_broken)
            swofty::log("[td-probe] breakdown: the game took it (part %u, state %u)", unsigned(part), unsigned(state));
        breakdown_watch = {};
        return;
    }
    if (GetTickCount64() < b.at + 15000)
        return;
    swofty::log("[td-probe] breakdown: the game did not start part %u in 15 s, the addon breaks it",
                unsigned(b.part));
    reinterpret_cast<volatile uint8_t *>(b.obj)[0xa0] = 7;  // disarmed (7 = no part)
    breakdown_watch = {};
    trigger_failure(b.part);
}

// Game thread: register the requested temporary fault for the player's car the way the game's own player fault
// planning does (0x1409b37a0(obj, record), record template of 0x1409bb078). It starts at once: the game plays the
// "fault imminent" radio, applies it a few seconds later ("fault occurred") and removes it after the duration
// ("fault fixed"). Flashback safe (the game re-applies its records on a rewind).
void register_fault(uintptr_t car)
{
    // 5, 15, 30 and 60 s after registering: log how far the game got with the newest record (obj+0xc8..+0xd0):
    // +4/+8 start lap and fraction (rewritten when it starts), +0x10 start time, +0x14 warning time (the "fault
    // imminent" radio, -1 before), +0x18 active, +0x19 finished, +0x1a allowed (needs the vehicle's faults component).
    // 0.51.93 log 20261103-094201: allowed 1 but not active after 12 s.
    static uint64_t registered_at = 0;
    static int checks_done = 0;
    static const uint64_t check_after[] = {5000, 15000, 30000, 60000};
    if (registered_at && checks_done < 4 && GetTickCount64() >= registered_at + check_after[checks_done]) {
        uint64_t obj = player_failure_obj(car), b = 0, e = 0;
        uint8_t rec[0x34] = {};
        uint8_t flag88 = 0;
        if (obj && guarded_read(obj + 0xc8, b) && guarded_read(obj + 0xd0, e) && e >= b + 0x34 &&
            guarded_copy(e - 0x34, rec, sizeof rec)) {
            guarded_read(obj + 0x88, flag88);
            uint32_t type = 0, lap0 = 0;
            float frac0 = 0, dur = 0, t0 = 0, warn = 0;
            memcpy(&type, rec, 4);
            memcpy(&lap0, rec + 4, 4);
            memcpy(&frac0, rec + 8, 4);
            memcpy(&dur, rec + 0xc, 4);
            memcpy(&t0, rec + 0x10, 4);
            memcpy(&warn, rec + 0x14, 4);
            // Effect size: record +0x2c (rolled from the game's database for types 0/1), and what the car got:
            // engine+0x2534 (engine fault) and gearbox+0x67c (shift fault), both added to a drivetrain term in
            // 0x144678360. In the 0.51.94 test slow shifts did not feel slower.
            float magnitude = 0, engine_fault = -1, shift_fault = -1;
            memcpy(&magnitude, rec + 0x2c, 4);
            uintptr_t engine = 0, gearbox = 0;
            if (guarded_read(car + 0x530, engine) && engine)
                guarded_read(engine + 0x2534, engine_fault);
            uint32_t gear_missing = 99, gear = 99, target = 99;
            float shift_scale = -1, up_time = -1, down_time = -1;
            auto xor_float = [](uintptr_t a, uintptr_t k) {
                uint32_t x = 0, y = 0;
                float f = -1;
                if (guarded_read(a, x) && guarded_read(k, y)) {
                    x ^= y;
                    memcpy(&f, &x, 4);
                }
                return f;
            };
            if (guarded_read(car + 0x538, gearbox) && gearbox) {
                guarded_read(gearbox + 0x67c, shift_fault);
                guarded_read(gearbox + 0x664, gear_missing);
                guarded_read(gearbox + 0x66c, shift_scale);
                guarded_read(gearbox + 0x680, gear);
                guarded_read(gearbox + 0x684, target);
                up_time = xor_float(gearbox + 0x170, gearbox + 0x178);
                down_time = xor_float(gearbox + 0x180, gearbox + 0x188);
            }
            swofty::log("[td-probe] fault: after %llu s type %u start lap %u fraction %.2f duration %.1f, start time "
                        "%.1f, warning time %.1f, active %u finished %u allowed %u, obj+0x88 %u, lap now %d, "
                        "magnitude %.4f, engine fault %.4f, shift fault %.4f, missing gear index %u (gearbox %u), "
                        "shift time scale %.2f (up %.3f s, down %.3f s), gear %u target %u (%llu records)",
                        (unsigned long long)(check_after[checks_done] / 1000), type, lap0, double(frac0), double(dur),
                        double(t0), double(warn), rec[0x18], rec[0x19], rec[0x1a], flag88, game_lap_of(obj),
                        double(magnitude), double(engine_fault), double(shift_fault), rec[0x30],
                        gear_missing, double(shift_scale), double(up_time), double(down_time), gear, target,
                        (unsigned long long)((e - b) / 0x34));
        } else {
            swofty::log("[td-probe] fault: the record could not be read");
        }
        ++checks_done;
    }
    uint64_t req = fault_request.load();
    if (!req || !fault_request.compare_exchange_strong(req, 0))
        return;
    uintptr_t obj = player_failure_obj(car);
    if (!obj || !orig_failure_register) {
        swofty::log("[td-probe] fault: cannot register (failure object %p)", reinterpret_cast<void *>(obj));
        return;
    }
    // Start lap 0, fraction 0: due at once in any lap (the game starts a record when `lap < current lap` or the
    // fraction is reached in the same lap) and writes the real lap and fraction when it starts it. 0.51.92 log
    // 20261103-093048: the lap getter gave -1 in a Grand Prix race, so the fault never registered.
    int lap = game_lap_of(obj);
    // Make game_lap() work from now on (the planner sets it only once the game plans a failure itself).
    if (!failure_planner_obj.load())
        failure_planner_obj = obj;
    alignas(8) uint8_t rec[0x34] = {};
    uint32_t type = uint32_t(req & 0xff), start = 0;
    float frac = 0.0f, duration = float((req >> 8) & 0xffffffff) / 10.0f, unset = -1.0f;
    memcpy(rec + 0x00, &type, 4);
    memcpy(rec + 0x04, &start, 4);
    memcpy(rec + 0x08, &frac, 4);
    memcpy(rec + 0x0c, &duration, 4);
    memcpy(rec + 0x10, &unset, 4);
    memcpy(rec + 0x14, &unset, 4);
    rec[0x1d] = uint8_t((req >> 40) & 1);
    // Missing gear: the gearbox's gear index (0 reverse, 1 neutral, 2 first gear ...: the shift code 0x144742557 skips
    // a requested gear equal to gearbox+0x664 and never checks 0 and 1). 0.51.96 took the AI list at mgr+0x150, whose
    // gear the tester did not notice; the addon now names the gear itself.
    rec[0x30] = uint8_t((req >> 48) & 0xff);
    rec[0x31] = 0x63;
    // Effect size for slow shifts (0 = the game rolls it from its database: 36.6 in log 20261103-104709, not
    // noticeable): bits 56-63 times 10.
    float magnitude = float((req >> 56) & 0xff) * 10.0f;
    if (magnitude > 0)
        memcpy(rec + 0x2c, &magnitude, 4);
    orig_failure_register(reinterpret_cast<void *>(obj), rec);
    registered_at = GetTickCount64();
    checks_done = 0;
    swofty::log("[td-probe] fault: registered %s (type %u) at lap %d for %.0f %s, gear index %u, magnitude %.0f",
                fm::fault_name(int(type)), type, lap, double(duration), rec[0x1d] ? "s" : "laps", rec[0x30],
                double(magnitude));
}

// Slow shifts. The game's Gearbox Fault (type 1) writes its size to gearbox+0x67c, which only feeds a drivetrain
// inertia sum (0x144678360): nothing to feel at 36.6 or 150 (logs 20261103-104709, -110541). The shift time itself
// is the base time (up: +0x170 ^ +0x178, down: +0x180 ^ +0x188) times gearbox+0x66c (state "shiftfaultscale", getter
// 0x1446a6040; used in 0x1446a6b60: the gear changes from +0x680 to the target +0x684 once the time in the shift
// +0x690 reaches it). The gearbox's own damage path raises +0x66c (ShiftTimeFaultScaleMin/Max at +0x4e0/+0x4f0) until
// the clock [gearbox+8]+0x9db0 passes +0x670, then puts it back to 1 (0x1446a6128). So while a slow-shift record of
// the player is active, the addon holds +0x66c at the record's size / 10 and keeps +0x670 one second ahead: it ends
// by itself a second after the fault (or a flashback before it). Base times 0.015 s up, 0.030 s down (log
// 20261103-113432). The addon asks for 3-15 times (fm::slow_shift_severity), the request carries scale x 10.
inline float slow_shift_scale(float magnitude) { return std::clamp(magnitude / 10.0f, 1.0f, 20.0f); }

void slow_shift_tick(uintptr_t car)
{
    static bool was_active = false;
    static uint64_t next_scan = 0;
    static float record_scale = 0;
    // The game's records only change when a fault starts or ends: read them 4 times a second while no slow shift is
    // active, every update while one is (this runs on every player update).
    if (uint64_t now = GetTickCount64(); was_active || now >= next_scan) {
        next_scan = now + 250;
        record_scale = 0;
        uint64_t obj = car ? player_failure_obj(car) : 0, b = 0, e = 0;
        if (obj && guarded_read(obj + 0xc8, b) && guarded_read(obj + 0xd0, e) && e > b && (e - b) % 0x34 == 0) {
            size_t n = std::min<size_t>((e - b) / 0x34, 64);
            uint8_t recs[64 * 0x34];
            if (guarded_copy(e - n * 0x34, recs, n * 0x34))
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t *rec = recs + i * 0x34;
                    uint32_t type = 0;
                    float magnitude = 0;
                    memcpy(&type, rec, 4);
                    memcpy(&magnitude, rec + 0x2c, 4);
                    if (type == fm::f_slow_shift && rec[0x18] && !rec[0x19])
                        record_scale = std::max(record_scale, slow_shift_scale(magnitude));
                }
        }
        uintptr_t gb = 0;
        int missing = 11;
        if (car && guarded_read(car + 0x538, gb) && gb)
            guarded_read(gb + 0x664, missing);
        shown_missing_gear = missing;
    }
    float scale = std::max(record_scale, decline_shift_scale.load());
    uintptr_t gearbox = 0, vehicle = 0;
    float current = 0, clock = 0;
    if (scale > 0 && guarded_read(car + 0x538, gearbox) && gearbox && guarded_read(gearbox + 0x66c, current) &&
        guarded_read(gearbox + 8, vehicle) && vehicle && guarded_read(vehicle + 0x9db0, clock)) {
        auto *g = reinterpret_cast<volatile float *>(gearbox + 0x66c);
        if (current != scale)
            g[0] = scale;  // also lowers it when a fault eases off
        g[1] = clock + 1.0f;  // +0x670
    }
    bool active = scale > 0;
    shown_shift_scale = scale;
    if (active != was_active)
        swofty::log("[td-probe] fault: slow shifts %s (shift time x%.1f)", active ? "on" : "off", double(scale));
    was_active = active;
}

// Rear brake failure (re/brake-torque-path.md). Every physics tick the car update 0x14473fd80 splits the brake torque
// by the bias into each wheel's maximum brake torque wheel+0xf8 (float, XOR-keyed with the dword at +0x100) for
// every car, then runs the brake step 0x1446dc480 (VehicleSim vt+8: rcx = sim, [sim+0x18] = car, xmm1 = dt), which
// stores pedal x that torque x the wheel's brake factor (+0xf0) as the applied torque +0x108 that the wheels and
// the powertrain use. The addon scales the rear wheels' +0xf8 (sys = car+0x580, +0x70 RL, +0x78 RR, skipped when
// byte +0x138 is set, as the game does) right before that step and puts the game's value back after it: no state
// and nothing accumulates, the next tick's split starts fresh. 0.51.106-0.51.113 held +0xf8 on the game thread and
// after the AI's split 0x144676250; the next tick's split overwrote both before the brake step, so the rear share
// stayed at 47 % (0.51.113 telemetry) and nothing could be felt.
// The hook runs for every car, maybe on job threads: atomics only, no log, no lock.
using brake_step_fn = void (*)(void *, float);
brake_step_fn orig_brake_step = nullptr;
constexpr uint8_t brake_step_prologue[] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00};
constexpr const char *brake_step_sig =
    "41 57 41 56 56 57 53 48 81 EC 10 01 00 00 C5 78 29 BC 24 00 01 00 00 C5 78 29 B4 24 F0 00 00 00 C5 78 29 AC 24 "
    "E0 00 00 00 C5 78 29 A4 24 D0 00 00 00 C5 78 29 9C 24 C0 00 00 00 C5 78 29 94 24 B0 00 00 00 C5 78 29 8C 24 A0 "
    "00 00 00 C5 78 29 84 24 90 00 00 00 C5 F8 29 BC 24 80 00 00 00 C5 F8 29 74 24 70 48 89 CE 48 8D 0D ?? ?? ?? ?? "
    "48 8B 46 18 83 B8 24 01 00 00 01";
std::atomic<bool> brake_step_hooked{false};

void on_brake_step(void *sim, float dt)
{
    float f = rear_brake_factor.load(std::memory_order_relaxed);
    uintptr_t car = 0, sys = 0, wheel[2] = {};
    uint32_t old[2] = {}, put[2] = {};
    bool changed = false;
    // [sim+0x18] read directly: the step itself reads it first thing, so it is valid whenever the game calls it.
    if (f != 1.0f && sim && (car = *reinterpret_cast<const volatile uintptr_t *>(reinterpret_cast<uintptr_t>(sim) + 0x18)) &&
        car == brake_car.load(std::memory_order_relaxed) && guarded_read(car + 0x580, sys) && sys) {
        for (int w = 0; w < 2; ++w) {
            uint32_t enc = 0, key = 0;
            uint8_t skip = 1;
            if (!guarded_read(sys + 0x70 + 8 * w, wheel[w]) || !wheel[w] || !guarded_read(wheel[w] + 0x138, skip) ||
                skip || !guarded_read(wheel[w] + 0xf8, enc) || !guarded_read(wheel[w] + 0x100, key)) {
                wheel[w] = 0;
                continue;
            }
            float torque = std::bit_cast<float>(enc ^ key) * f;
            if (!std::isfinite(torque) || torque < 0) {
                wheel[w] = 0;
                continue;
            }
            old[w] = enc;
            put[w] = std::bit_cast<uint32_t>(torque) ^ key;
            *reinterpret_cast<volatile uint32_t *>(wheel[w] + 0xf8) = put[w];
            changed = true;
        }
    }
    orig_brake_step(sim, dt);
    if (!changed)
        return;
    for (int w = 0; w < 2; ++w) {
        if (!wheel[w])
            continue;
        auto *enc = reinterpret_cast<volatile uint32_t *>(wheel[w] + 0xf8);
        if (*enc == put[w])  // still ours (not re-keyed): the game's value back for every other reader
            *enc = old[w];
    }
    brake_steps_scaled.fetch_add(1, std::memory_order_relaxed);
}

void hook_brake_step()
{
    brake_step_hooked = detour("brake step (rear brake failure)", brake_step_sig, brake_step_prologue,
                               sizeof brake_step_prologue, reinterpret_cast<void *>(&on_brake_step),
                               reinterpret_cast<void **>(&orig_brake_step));
}

// Game thread: the player's car and the rear brake factor for the hook; a timed fault (test, debug) ends here.
void brake_fault_tick(uintptr_t car)
{
    brake_car.store(car, std::memory_order_relaxed);
    float loss = rear_brake_loss_target.load();
    uint64_t until = rear_brake_until.load();
    if (loss != 0 && until && GetTickCount64() >= until) {
        rear_brake_loss_target = 0.0f;
        rear_brake_until = 0;
        loss = 0;
        swofty::log("[td-probe] fault: rear brakes back");
    }
    float factor = (1.0f - loss) * (1.0f - brake_fade.load());
    float was = rear_brake_factor.exchange(factor);
    // Once a second while it acts: how many brake steps the hook scaled (the proof it reaches the brakes).
    static uint64_t next_log = 0;
    uint64_t now = GetTickCount64();
    if (factor != 1.0f && (was == 1.0f || now >= next_log)) {
        uint32_t n = brake_steps_scaled.exchange(0);
        if (was == 1.0f)
            swofty::log("[td-probe] brakes: rear brake torque x%.2f from now (brake step hook %s)", double(factor),
                        brake_step_hooked.load() ? "on" : "MISSING, no effect");
        else if (now >= next_log && next_log)
            swofty::log("[td-probe] brakes: rear brake torque x%.2f, %u brake steps scaled in the last 10 s",
                        double(factor), n);
        next_log = now + 10000;
    } else if (factor == 1.0f && was != 1.0f) {
        swofty::log("[td-probe] brakes: rear brake torque back to normal (%u brake steps scaled)",
                    brake_steps_scaled.exchange(0));
        next_log = 0;
    }
}
