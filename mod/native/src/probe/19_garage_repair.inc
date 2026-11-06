// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Garage crash repair jobs, service text, garage exit lock.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Practice and qualifying: a car that limped into its garage on one broken corner gets the game's own
// "detached wheel" repair job (kind 16, lng_ft_detached_wheel) with a long time: 20-30 min for a broken suspension,
// 30-45 min for a lost wheel. The game shows it as its repair timer. RepairTimes (update 0x142bf50a0, `this` rcx,
// dt xmm1) keeps per-vehicle records at +0x48..+0x50 (stride 0x58: +0x10 active, +0x16 vehicle, +0x20..+0x28 jobs
// {u32 kind, float seconds}) and counts the jobs of active records down. A finished job calls 0x142bf03e0(record,
// kind) and is erased; for kinds 3..16 that resets the game's terminal damage record. When our job finishes, the
// addon rebuilds the corner (repair_crash_damage) and the car may leave again. A car that could not be driven
// (abandoned on track, FIA B1.8.9) gets no job and stays in its garage for the session.
using repair_update_fn = void (*)(void *, float);
repair_update_fn orig_repair_update;
using repair_done_fn = void (*)(void *, uint32_t);
repair_done_fn orig_repair_done;
bool repair_update_hooked = false;
constexpr uint8_t repair_update_prologue[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x70, 0xC5, 0x78, 0x29,
                                              0x4C, 0x24, 0x60, 0xC5, 0x78, 0x29, 0x44, 0x24, 0x50};
constexpr uint8_t repair_done_prologue[] = {0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x41, 0x08,
                                            0x4C, 0x8B, 0x40, 0x30, 0x0F, 0xB6, 0x71, 0x16};

float garage_repair_time(uint32_t corners)
{
    if (debug_garage_repair == std::string_view("fast"))
        return 60.0f;
    if (garage_service_corners.load())
        return std::max(60.0f, garage_service_seconds.load());
    bool wheel = has_detached_corner(corners);
    float lo = wheel ? 1800.0f : 1200.0f, hi = wheel ? 2700.0f : 1800.0f;
    return lo + (hi - lo) * float(roll_percent()) / 99.0f;
}

// The serviced corners for a message: "front-left suspension", "front-left and rear-right suspension",
// "suspension on three corners". Front corners first.
void service_corners_text(uint32_t corners, char *out, size_t n)
{
    int count = 0, pick[2] = {-1, -1};
    for (int i : {2, 3, 0, 1})
        if (corners & (1u << i)) {
            if (count < 2)
                pick[count] = i;
            ++count;
        }
    char a[16] = "", b[16] = "";
    snprintf(a, sizeof a, "%s", susp::corner_name(pick[0]));
    snprintf(b, sizeof b, "%s", susp::corner_name(pick[1]));
    a[0] = char(tolower(uint8_t(a[0])));
    b[0] = char(tolower(uint8_t(b[0])));
    if (count <= 1)
        snprintf(out, n, "%s suspension", a);
    else if (count == 2)
        snprintf(out, n, "%s and %s suspension", a, b);
    else
        snprintf(out, n, "suspension on %s corners", count == 4 ? "all four" : "three");
}

// The game's session kind (1..4 practice, 5..14 qualifying and shootout, 15..17 races, 18 time trial), 0 unknown.
uint32_t game_session_kind()
{
    uint32_t session = 0;
    return repair_session_kind && guarded_read(repair_session_kind, session) ? session : 0;
}

// The player's RepairTimes record, or 0.
uintptr_t player_repair_record(uintptr_t manager)
{
    uintptr_t first = 0, last = 0;
    int vehicle = player_vehicle.load();
    if (vehicle < 0 || !guarded_read(manager + 0x48, first) || !guarded_read(manager + 0x50, last) ||
        !first || last < first || (last - first) % 0x58 || (last - first) / 0x58 > 64)
        return 0;
    for (uintptr_t at = first; at < last; at += 0x58) {
        uint8_t id = 0xff;
        if (guarded_read(at + 0x16, id) && int(id) == vehicle)
            return at;
    }
    return 0;
}

void on_repair_update(void *manager, float dt)
{
    int state = garage_repair.load();
    if (corner_toast_at.load() && GetTickCount64() >= corner_toast_at.load()) {
        corner_toast_at = 0;
        if (state == 2 && swofty::hud::available()) {
            char text[160], time[32];
            int minutes = corner_toast_minutes.load();
            if (minutes > 1)
                snprintf(time, sizeof time, "%d minutes", minutes);
            else
                snprintf(time, sizeof time, "about a minute");
            if (uint32_t corners = garage_service_corners.load()) {
                char where[64];
                service_corners_text(corners, where, sizeof where);
                if (garage_service_crack.load())
                    snprintf(text, sizeof text, "The team found a crack in the %s. The repair takes %s.", where, time);
                else
                    snprintf(text, sizeof text, "The team repairs the damaged %s. This takes %s.", where, time);
                swofty::hud::toast("SUSPENSION REPAIR", text, 8);
            } else {
                snprintf(text, sizeof text, "The team rebuilds the broken corner. This takes %s.", time);
                swofty::hud::toast("CORNER REPAIR", text, 8);
            }
        }
    }
    // A withdrawn car is not repaired: drop the game's own repair jobs (their timer and Skip Repair Time). A failed
    // power unit or gearbox keeps them, the part change runs through them.
    static bool cleared_logged = false;
    if (state == 0 && garage_exit_locked()) {
        uintptr_t record = player_repair_record(reinterpret_cast<uintptr_t>(manager)), first = 0, last = 0;
        if (record && guarded_read(record + 0x20, first) && guarded_read(record + 0x28, last) && first &&
            last > first && (last - first) % 8 == 0 && (last - first) / 8 <= 32) {
            *reinterpret_cast<volatile uintptr_t *>(record + 0x28) = first;
            if (!cleared_logged) {
                cleared_logged = true;
                swofty::log("[td-probe] garage: your car is %s, removed %d repair jobs of the game",
                            garage_withdrawn.load() ? "withdrawn" : "out for this session", int((last - first) / 8));
            }
            // 0.51.123 test: the engine change's timer showed for a moment and went, with no word why.
            static uint64_t toast_after = 0;
            if (!part_lock.load() && swofty::hud::available() && GetTickCount64() >= toast_after) {
                toast_after = GetTickCount64() + 5000;
                bool withdrawn = garage_withdrawn.load();
                swofty::hud::toast(withdrawn ? "CAR WITHDRAWN" : "NO REPAIRS",
                                   withdrawn ? "Your car stopped on track: no repairs in this session. Skip Session to go on."
                                             : "Your car cannot be repaired in this session. Skip Session to go on.",
                                   6);
            }
        }
    } else {
        cleared_logged = false;
    }
    if ((state == 1 || state == 2) && !session_ended && orig_add_repair) {
        uintptr_t record = player_repair_record(reinterpret_cast<uintptr_t>(manager)), item = 0;
        float seconds = 0;
        if (record && repair_item(record, 16, item)) {
            if (state == 1) {
                if (!item)
                    orig_add_repair(reinterpret_cast<void *>(record), 16);
                uint8_t active = 0;
                if (repair_item(record, 16, item) && item && guarded_read(item + 4, seconds) &&
                    std::isfinite(seconds) && guarded_read(record + 0x10, active)) {
                    float t = garage_repair_time(crash_corners.load());
                    *reinterpret_cast<volatile float *>(item + 4) = t;
                    garage_repair_left = t;
                    if (!active)
                        *reinterpret_cast<volatile uint8_t *>(record + 0x10) = 1;
                    garage_repair = 2;
                    swofty::log("[td-probe] garage repair: detached wheel job %.0f -> %.0f s (record %p, active %u, %s)",
                                double(seconds), double(t), reinterpret_cast<void *>(record), unsigned(active),
                                garage_service_corners.load()                ? "damaged suspension"
                                : has_detached_corner(crash_corners.load()) ? "lost wheel"
                                                                             : "broken suspension");
                    // Shown 3 s later: right now the garage camera is still fading in.
                    corner_toast_minutes = int(t / 60.0f + 0.5f);
                    corner_toast_at = GetTickCount64() + 3000;
                }
            } else if (item && guarded_read(item + 4, seconds)) {
                // The job's +4 is its length, not the time left (0.51.133 test 23:37:43-23:38:59: '60 s left' three
                // times, 30 s apart, the job ended after 76 s), so 'minutes left' never went down: the addon counts
                // down itself with the repair update's dt from the time it gave the job.
                float step = std::isfinite(dt) && dt > 0.0f && dt < 1.0f ? dt : 0.0f;
                float left = std::max(0.0f, garage_repair_left.load() - step);
                garage_repair_left = left;
                static uint64_t next_log = 0;
                if (GetTickCount64() >= next_log) {
                    next_log = GetTickCount64() + 30000;
                    swofty::log("[td-probe] garage repair: about %.0f s left (job %.0f s)", double(left),
                                double(seconds));
                }
            }
        }
    }
    orig_repair_update(manager, dt);
}

void on_repair_done(void *record, uint32_t kind)
{
    uint8_t id = 0xff;
    bool ours = kind == 16 && garage_repair.load() == 2 &&
                guarded_read(reinterpret_cast<uintptr_t>(record) + 0x16, id) && int(id) == player_vehicle.load();
    orig_repair_done(record, kind);
    if (ours) {
        garage_repair = 3;
        swofty::log("[td-probe] garage repair: the detached wheel job finished");
    }
}

// Game thread (updateVehicle of the player's car).
void garage_repair_finish(uintptr_t car)
{
    if (garage_repair.load() != 3 || !car)
        return;
    if (uint32_t corners = garage_service_corners.load()) {
        // The service: the model forgets the damage, nothing else changes (the car was never broken). car_dead goes
        // first: at garage_repair 0 the repair update drops the game's jobs of a car that may not leave.
        car_dead = false;
        garage_service_corners = 0;
        garage_service_seconds = 0;
        garage_service_crack = false;
        garage_repair = 0;
        repair_label(false);
        susp::request_reset("repair");
        swofty::log("[td-probe] garage repair: the suspension is repaired (corners 0x%x), your car may leave the "
                    "garage", corners);
        if (swofty::hud::available())
            swofty::hud::toast("CAR REPAIRED", "The suspension is repaired. Your car may leave the garage.", 6);
        return;
    }
    repair_crash_damage(car);
    int pit = player_pit.load();
    reset_car_state();
    player_pit = pit;
    swofty::log("[td-probe] garage repair: your car is rebuilt and may leave the garage");
    if (swofty::hud::available())
        swofty::hud::toast("CAR REPAIRED", "The broken corner is rebuilt. Your car may leave the garage.", 6);
}

// Frame thread (susp::tick): the car reached its practice or qualifying garage with corners that took damage but
// hold. The team services them with the crash repair's job (the detached wheel kind) for `seconds`; the car may not
// leave until it is done (car_dead: garage_exit_locked, the drive out buttons, on_set_vehicle_status). False when
// that cannot run (the addon does not keep control, the repair hooks are missing, another repair runs, time trial):
// the model is then repaired at once, as before 0.51.135.
bool start_garage_service(float seconds, uint32_t corners, bool crack)
{
    if (!keep_control || session_ended || player_broken || car_dead || garage_repair.load() != 0 || !corners ||
        !(seconds > 0) || !repair_update_hooked || !orig_add_repair || game_session_kind() > 14)
        return false;
    garage_service_seconds = seconds;
    garage_service_crack = crack;
    garage_service_corners = corners;
    // garage_repair first: at garage_repair 0 the repair update drops the game's jobs of a car that may not leave.
    garage_repair = 1;
    car_dead = true;
    repair_label(true);
    swofty::log("[td-probe] garage repair: your car is in its garage with damaged suspension (corners 0x%x%s), the "
                "team repairs it: %.0f s", corners, crack ? ", a crack" : "", double(seconds));
    return true;
}

// The garage repair job's name: the game's "detached wheel" job (lng_ft_detached_wheel) also carries the suspension
// repairs, so it reads "Suspension" while one runs (core v8 text overrides; without them the game's name stays).
void repair_label(bool suspension)
{
    static std::atomic<bool> shown{false};
    if (shown.exchange(suspension) == suspension || !swofty::lang::available())
        return;
    auto r = suspension ? swofty::lang::set("lng_ft_detached_wheel", "Suspension")
                        : swofty::lang::clear("lng_ft_detached_wheel");
    swofty::log("[td-probe] garage repair: the repair job's name %s (%s)",
                suspension ? "reads Suspension" : "is the game's again", swofty::lang::name(r));
}

void hook_garage_crash_repair()
{
    repair_update_hooked =
        detour("RepairTimes update",
               "56 57 53 48 83 EC 70 C5 78 29 4C 24 60 C5 78 29 44 24 50 C5 F8 29 7C 24 40 C5 F8 29 74 24 30 48 89 CE "
               "48 8B 41 70 48 8B 48 20 48 8B 89 F0 00 00",
               repair_update_prologue, sizeof repair_update_prologue, reinterpret_cast<void *>(&on_repair_update),
               reinterpret_cast<void **>(&orig_repair_update)) &&
        detour("RepairTimes job finished",
               "56 48 83 EC 30 48 8B 41 08 4C 8B 40 30 0F B6 71 16 49 39 B0 80 01 00 00 0F 86 84 00 00 00 49 83 BC F0 "
               "B0 00 00 00 00 74 79 8D 4A FD 83 F9 0E 73",
               repair_done_prologue, sizeof repair_done_prologue, reinterpret_cast<void *>(&on_repair_done),
               reinterpret_cast<void **>(&orig_repair_done));
    if (!repair_update_hooked)
        swofty::log("[td-probe] garage repair: not available, a limping car stays in its garage");
}

// Practice/qualifying: a car that cannot be repaired in this session (crash damage, a plain retirement) or whose
// corner is still being rebuilt may not leave its garage at all. The garage menu switches its drive out buttons
// off (on_garage_vm_update); a drive out request that still comes (a practice programme start, 0.51.125) is passed
// on and the car comes straight back (on_exit_garage). A failed power unit or gearbox keeps the old path: the part
// swap only shows on a send-out attempt.
bool garage_exit_locked()
{
    return car_dead && keep_control && !session_ended &&
           (garage_withdrawn.load() || (!(terminal_mask.load() & 0x7) && !garage_part_failed) || part_lock.load());
}
