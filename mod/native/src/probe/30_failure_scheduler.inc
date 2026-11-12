// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Failure planner, scheduler and register hooks, game failure DSQ.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// The game's AI failure planner (0x1409b2940, `this` in rcx) announces a planned breakdown (radio message) and writes
// the part to fail into the car's failure object (+0xa0). Remember the player's part: when the game then hands the car
// to the AI (RETIRED), the addon breaks that part itself instead.
void (*orig_failure_planner)(void *) = nullptr;
void on_failure_planner(void *self)
{
    uintptr_t a = reinterpret_cast<uintptr_t>(self);
    uint64_t car = 0;
    uint8_t part = 0xff;
    if (a && guarded_read(a + 0x70, car) && car && car == uint64_t(player_car()) && guarded_read(a + 0xa0, part) &&
        (failure_planner_obj = a, game_planned_part.exchange(int(part)) != int(part)))
        swofty::log("[td-probe] player failure planned by the game: part %u (0 ICE,1 MGU-K,2 MGU-H,3 ES,4 TC,5 CE,6 gearbox)", part);
    orig_failure_planner(self);
}
bool hook_failure_planner()
{
    constexpr uint8_t prologue[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x40, 0xC5, 0xF9, 0x29, 0x74, 0x24, 0x30, 0x48, 0x89, 0xCE};
    return detour("AI failure planner", "56 57 53 48 83 EC 40 C5 F9 29 74 24 30 48 89 CE 48 8B 81 98 00 00 00 48 8B 78 58 80 BF 98 00 00 00 00 74",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_failure_planner),
                  reinterpret_cast<void **>(&orig_failure_planner));
}

// The failure scheduler (0x1409b9aa0, `this` in rcx) runs once per session and rolls, per car, whether and which part
// fails. Every mode stores the failure manager (failure_mgr) and logs the race settings. "native" patches the roll and
// the chance check before the scheduler runs, "realistic" (the default) clears the failures it rolled afterwards,
// "breakdown" arms the player's.
void (*orig_failure_scheduler)(void *) = nullptr;
int choice_int(std::string_view v, int fallback);
// The game's race length (re/game-failure-scheduler.md, code 0x1409b9d86): settings = [[[mgr+8]+0x10]+0x1a8], byte +8
// the race-length index (0-12, the scheduler's tables are indexed by it), dword +0xdf0 ^ +0xdf8 the lap count N the
// game plans its failures over (laps 2..N). Logged at each session start. 0.51.122 log: N 5 in a 5-lap race (the
// car crossed the line 5 times), so race_total_laps uses it when Swofty's telemetry is off and its session context
// has 0 laps for a race (0.51.120 log). The session end clears it; the next session's scheduler call sets it again.
std::atomic<int> game_race_laps{-1}, game_race_length{-1};
void log_race_settings(uint64_t mgr)
{
    uint64_t a = 0, b = 0, settings = 0;
    uint8_t length = 0xff;
    uint32_t n0 = 0, n1 = 0;
    if (!mgr || !guarded_read(mgr + 8, a) || !a || !guarded_read(a + 0x10, b) || !b ||
        !guarded_read(b + 0x1a8, settings) || !settings || !guarded_read(settings + 8, length) ||
        !guarded_read(settings + 0xdf0, n0) || !guarded_read(settings + 0xdf8, n1)) {
        swofty::log("[td-probe] race settings: not readable (manager %p)", reinterpret_cast<void *>(mgr));
        return;
    }
    game_race_laps = int(n0 ^ n1);
    game_race_length = int(length);
    swofty::log("[td-probe] race settings: length index %u, the game's lap count %u (the laps it plans failures over)",
                unsigned(length), n0 ^ n1);
}

void on_failure_scheduler(void *self)
{
    if (time_trial) {
        orig_failure_scheduler(self);
        return;
    }
    std::string_view mode = gp_failures;
    {
        // Experimental "native" mode: the scheduler rolls three Mersenne Twister numbers per car at session start; the
        // first one (stored at entry+8) is the threshold the failure chance has to beat. Storing 0.0 instead of the
        // roll (vmovss [rbx+8],xmm0 -> xmm8, which is zero) makes the game's own scheduler register a failure.
        // Both sites are fixed addresses of this build: check the whole instruction (C5 FA 11 43 08 vmovss [rbx+8],
        // xmm0; 0F 86 C0 FE FF FF jbe), so another build's code is never written.
        static uint8_t *patch = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x9b9d27);
        bool want = mode == "native";
        DWORD old = 0;
        bool vmovss = patch[-1] == 0xC5 && patch[1] == 0x11 && patch[2] == 0x43 && patch[3] == 0x08;
        if (vmovss && patch[0] == (want ? 0xFA : 0x7A) && VirtualProtect(patch, 1, PAGE_EXECUTE_READWRITE, &old)) {
            patch[0] = want ? 0x7A : 0xFA;
            VirtualProtect(patch, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), patch, 1);
            swofty::log("[td-probe] failure scheduler: threshold roll %s", want ? "forced to 0 (native test)" : "restored");
        }
        // Also skip the "chance > threshold" branch (jbe 0x1409bb3bf) so a record is registered even when the chance is 0
        // at race start (no race-length data in Grand Prix).
        static uint8_t *branch = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x9bb4f9);
        static uint8_t saved[6] = {};
        static bool have_saved = false;
        constexpr uint8_t jbe[6] = {0x0F, 0x86, 0xC0, 0xFE, 0xFF, 0xFF};
        if (!have_saved && !memcmp(branch, jbe, sizeof jbe)) {
            memcpy(saved, branch, 6);
            have_saved = true;
        }
        if (have_saved && want != (branch[0] == 0x90) && VirtualProtect(branch, 6, PAGE_EXECUTE_READWRITE, &old)) {
            if (want) memset(branch, 0x90, 6); else memcpy(branch, saved, 6);
            FlushInstructionCache(GetCurrentProcess(), branch, 6);
            VirtualProtect(branch, 6, old, &old);
            swofty::log("[td-probe] failure scheduler: chance check %s", want ? "skipped (native test)" : "restored");
        }
    }
    orig_failure_scheduler(self);
    {
        uint64_t mgr = 0;
        if (guarded_read(reinterpret_cast<uintptr_t>(self) + 0xf0, mgr) && mgr)
            failure_mgr = mgr;
        log_race_settings(mgr);
        // Realistic failures (0.75.5: Career too): the season's rates replace the failures the game rolled from its own
        // database for every car (part 7 = none); gp_ai_failure_tick plans the AI's instead. A fixed chance plans only
        // your car, so in Career the game's rolls stay for the AI (and add to yours there).
        if (mode == "realistic") {
            int cleared = 0;
            for_each_failure_obj([&](int, uintptr_t obj) {
                uint8_t part = 7, state = 0;
                if (guarded_read(obj + 0xa0, part) && part < 7 && guarded_read(obj + 0xa1, state) && state == 0) {
                    *reinterpret_cast<volatile uint8_t *>(obj + 0xa0) = 7;
                    ++cleared;
                }
            });
            swofty::log("[td-probe] failure scheduler: realistic failures, %d failure(s) the game rolled removed",
                        cleared);
        }
    }
    if (self && mode == "breakdown") {
        // Experimental: arm the game's own breakdown state machine for the player's car. The scheduler normally
        // fills these per-car object fields (+0xa0 part, +0xa1 state, +0xa4 lap, +0xa8 lap fraction, +0xb1 branch)
        // when a breakdown was rolled; in Grand Prix none is, so write them here and let the game run its planner
        // (state 0 -> 1 -> 2: announce, pull over, hand the car to the AI).
        uintptr_t a = reinterpret_cast<uintptr_t>(self);
        uint64_t mgr = 0, begin = 0, end = 0;
        guarded_read(a + 0xf0, mgr);
        if (mgr) { guarded_read(mgr + 0x198, begin); guarded_read(mgr + 0x1a0, end); }
        if (end < begin || end - begin > 0x10 * 64)  // the same bound for_each_failure_obj applies: no garbage range
            begin = end = 0;
        int player = player_vehicle.load();
        for (uint64_t e = begin; e && e < end && player >= 0; e += 0x10) {
            uint64_t obj = 0;
            uint8_t idx = 0xff;
            guarded_read(e + 8, obj);
            if (!obj || !guarded_read(obj + 0x78, idx) || idx != uint8_t(player))
                continue;
            uint8_t part = 0;
            std::string_view ft = gp_fault_type;
            part = ft == "random" ? uint8_t((GetTickCount64() / 17) % 6) : uint8_t(choice_int(ft, 0) % 6);
            uint32_t lap = 1;
            float frac = 0.30f;
            uint8_t flag88 = 0xff, oa0 = 0xff, oa1 = 0xff;
            uint32_t oa4 = 0;
            guarded_read(obj + 0x88, flag88);
            guarded_read(obj + 0xa0, oa0);
            guarded_read(obj + 0xa1, oa1);
            guarded_read(obj + 0xa4, oa4);
            auto *o = reinterpret_cast<uint8_t *>(obj);
            DWORD old = 0;
            if (VirtualProtect(o + 0xa0, 0x20, PAGE_READWRITE, &old)) {
                o[0xa0] = part;
                o[0xa1] = 0;
                memcpy(o + 0xa4, &lap, 4);
                memcpy(o + 0xa8, &frac, 4);
                memset(o + 0xac, 0, 4);
                o[0xb0] = 0;
                o[0xb1] = 0;
                VirtualProtect(o + 0xa0, 0x20, old, &old);
            }
            swofty::log("[td-probe] breakdown armed: object %llx car %u, +88 %u, part %u -> %u, state %u -> 0, lap %u -> 1, fraction 0.30",
                        (unsigned long long)obj, unsigned(idx), unsigned(flag88), unsigned(oa0), unsigned(part), unsigned(oa1), oa4);
        }
    }
}
bool hook_failure_scheduler()
{
    constexpr uint8_t prologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0x28, 0x02, 0x00, 0x00};
    return detour("AI failure scheduler", "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 28 02 00 00 C5 78 29 BC 24 10 02 00 00 C5 78 29 B4 24 00 02 00 00 C5 78 29 AC 24 F0 01 00 00 C5 78 29 A4 24 E0 01 00 00 C5 78 29 9C 24 D0 01 00 00 C5 78 29 94 24 C0 01 00 00 C5 78 29 8C 24 B0 01 00 00 C5 78 29 84 24 A0 01 00 00 C5 F8 29 BC 24 90 01 00 00 C5 F8 29 B4 24 80 01 00 00 80 B9 09 01 00 00 00 0F 84 63 21 00 00 48 89 CF 48 8B 41",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_failure_scheduler),
                  reinterpret_cast<void **>(&orig_failure_scheduler));
}

// Failure record registration (0x1409b37a0, rcx = the car's failure object, rdx = 0x34-byte record). The "native" test
// mode rewrites the record (the 0.51.38 probe); Realistic drops the game's own fault for a human driver in Career
// (0.75.5).
void *on_failure_register(void *list, const void *rec)
{
    if (std::string_view(gp_failures) == "native" && rec) {
        uint32_t w[2] = {};
        for (int i = 0; i < 2; i++) guarded_read(reinterpret_cast<uintptr_t>(rec) + i * 4, w[i]);
        // Record layout (0x34 bytes): +0 type, +4 lap the fault starts in (rolled up to the track's full race length,
        // so 7 or 24 in a short Grand Prix), +8 fraction of that lap, +0xc duration. Pull it to lap 1 so a short test
        // race reaches it.
        uint32_t lap = 1;
        float frac = 0.30f;
        DWORD old = 0;
        auto *dst = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(rec));
        if (VirtualProtect(dst, 12, PAGE_READWRITE, &old)) {
            // The game only ever rolls type 1 (gearbox) here; pick one of the fault types 0-6 at random instead.
            uint32_t type = uint32_t(GetTickCount64() / 11) % 7;
            if (std::string_view(gp_fault_type) != "random")
                type = uint32_t(choice_int(std::string_view(gp_fault_type), int(type)));
            memcpy(dst, &type, 4);
            swofty::log("[td-probe] failure RECORD: type %u -> %u (native test)", w[0], type);
            memcpy(dst + 4, &lap, 4);
            memcpy(dst + 8, &frac, 4);
            VirtualProtect(dst, 12, old, &old);
            swofty::log("[td-probe] failure RECORD: lap %u -> lap 1 fraction 0.30 (native test)", w[1]);
        }
    }
    // 0.75.5: Realistic replaces the game's own system in Career too. The game plans one temporary fault for a human
    // driver (phase D, record template word 0x63ff at +0x30: wheel 0x63) and writes it into the career save; yours come
    // from gp_fault_tick instead (registered through orig_failure_register, not through this hook). The game's
    // record is not registered: its caller gets a scratch record to fill in. AI cars (phase B) keep the game's faults,
    // the addon plans no partial faults for them.
    if (rec && std::string_view(gp_failures) == "realistic") {
        uint8_t wheel = 0;
        swofty::career::State st{};
        if (guarded_read(reinterpret_cast<uintptr_t>(rec) + 0x31, wheel) && wheel == 0x63 &&
            swofty::career::state(&st) == swofty::career::Result::ok && st.loaded) {
            alignas(8) static uint8_t scratch[0x40];
            memset(scratch, 0, sizeof scratch);
            static bool logged = false;
            if (!logged) {
                logged = true;
                uint32_t type = 99;
                guarded_read(reinterpret_cast<uintptr_t>(rec), type);
                swofty::log("[td-probe] failure RECORD: the game's own fault (type %u) for your car dropped (Realistic "
                            "in Career: the addon plans them)", type);
            }
            return scratch;
        }
    }
    return orig_failure_register(list, rec);
}
// RuleManager (0x1435676a0 onSessionEnter, rcx = rules). Rules_Disqualify(rules, vehicle) at 0x143571440 marks the
// result DSQ and hands the car to the AI with DISQUALIFIED, as the formation-lap timer does.
std::atomic<uintptr_t> rule_manager{0};
std::atomic<bool> dsq_pending{false};
uint64_t (*orig_rules_enter)(void *, void *, void *) = nullptr;
uint64_t on_rules_enter(void *rules, void *systems, void *components)
{
    rule_manager = reinterpret_cast<uintptr_t>(rules);
    swofty::log("[td-probe] rule manager %p (session enter)", rules);
    return orig_rules_enter(rules, systems, components);
}
bool hook_rules_enter()
{
    constexpr uint8_t prologue[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x4C, 0x89, 0xC7, 0x48, 0x89, 0xD3, 0x48, 0x89, 0xCE};
    return detour("rule manager session enter", "56 57 53 48 83 EC 50 4C 89 C7 48 89 D3 48 89 CE 48 8B 02 48 8D 15 ?? ?? ?? ?? 48 89 D9 FF 50 28 48 89 86 00 1C 00 00",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_rules_enter),
                  reinterpret_cast<void **>(&orig_rules_enter));
}
// On the game thread (updateVehicle of the player's car).
void game_failure_dsq_now()
{
    if (!dsq_pending.exchange(false))
        return;
    uintptr_t rules = rule_manager.load();
    int id = player_vehicle.load();
    static uintptr_t at = 0;
    static int hits = swofty::api->scan(".xtls", "41 56 56 57 53 48 83 EC 48 C5 F9 29 74 24 30 80 FA FF 0F 84", &at);
    void *fn = hits == 1 ? reinterpret_cast<void *>(at) : nullptr;
    if (!rules || id < 0 || !fn) {
        swofty::log("[td-probe] game failure: DSQ not possible (rules %p, car %d, fn %p), retiring instead",
                    reinterpret_cast<void *>(rules), id, fn);
        player_broken = true;
        retire_in = 0.0;
        return;
    }
    swofty::log("[td-probe] game failure: disqualifying car %d (rules %p)", id, reinterpret_cast<void *>(rules));
    reinterpret_cast<void (*)(void *, uint32_t)>(fn)(reinterpret_cast<void *>(rules), uint32_t(id));
    // Rules_IssueDisqualification (0x143572f90) also sets the car's bit in the rule manager's DSQ masks +0x1d08 and
    // +0x1d0c. The session end (ruleset finalisation 0x142d2f440) reads +0x1d08: with the bit the car is classified
    // disqualified (black flagged), without it a car that has not finished becomes a DNF.
    for (uintptr_t off : {uintptr_t(0x1d08), uintptr_t(0x1d0c)}) {
        uint32_t mask = 0;
        if (id < 32 && guarded_read(rules + off, mask))
            *reinterpret_cast<volatile uint32_t *>(rules + off) = mask | (1u << id);
    }
    // Rules_Disqualify only marks the car and hands it to the AI, which stops it on track (DISQUALIFIED sends the AI
    // to its PreLights state). The result status is what takes a car out of the session: the pause menu's Retire
    // requests status 7 (retired). Request 5 (disqualified, black flagged) the same way.
    post_vehicle_status(id, 5, 6);
}

bool hook_failure_register()
{
    constexpr uint8_t prologue[] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCE};
    return detour("failure record register", "41 57 41 56 56 57 53 48 83 EC 20 48 89 D7 48 89 CE 48 8B 81 D0 00 00 00",
                  prologue, sizeof prologue, reinterpret_cast<void *>(&on_failure_register),
                  reinterpret_cast<void **>(&orig_failure_register));
}
