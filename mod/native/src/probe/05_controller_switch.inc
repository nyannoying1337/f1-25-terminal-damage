// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Controller hand-over tracking, set-controller hook, participants scan.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

std::atomic<void *> controller_system{nullptr};
void reset_car_state(bool preserve_retirement_audio = false, bool preserve_request_timeline = false, bool preserve_damage_timeline = false);
std::atomic<uint32_t> player_controller_flags{0};

// The retired car's engine stays off through the DNF camera. Once the race is over for it (NOT_IN_SESSION) and the game
// moves on, the silence ends: kept any longer it mutes your car in the replay (test reports 0.51.124, 2026-11-07).
void release_retired_engine_sound(uint32_t id, const char *why)
{
    if (!(player_controller_flags.load() & (1u << 3)) || retired_engine_off_vehicle.load() != int(id))
        return;
    retired_engine_off_vehicle = -1;
    engine_off_vehicle = -1;
    engine_kill_vehicle = -1;
    swofty::log("[td-probe] engine: sound back for vehicle %u (%s)", id, why);
}

// Defined in 31_garage_return_failure_helpers.inc.
void trigger_failure(int forced_part = -1);
// Part the game's own failure planner picked for the player's car (0 ICE .. 5 CE, 6 gearbox), -1 if none yet.
std::atomic<int> game_planned_part{-1};
// Part the addon broke for the game's own failure (after dropping its RETIRED hand-over), -1 none. A flashback to
// before it hands it back to game_planned_part: the game replays the failure and its hand-over, without the planner.
std::atomic<int> game_failure_part{-1};
// After the game's own failure or heavy damage: the pit window, open since this tick (0 = closed). It ends with the
// next lap (the game's lap, else your line crossings), or after 10 minutes of driving (gp_failure_tick).
std::atomic<uint64_t> game_failure_deadline{0};
std::atomic<int> game_failure_lap{-1};          // game lap (0-based) the failure was announced in, -1 unknown
std::atomic<int> game_failure_crossing{-1};     // player_crossings when the window opened, -1 unknown
std::atomic<int> pit_call_reason{0};            // what opened the window: 0 the game's failure, 1 heavy damage
std::atomic<bool> pit_call_ended{false};        // the window ran out (disqualified or retired): no new call
std::atomic<uintptr_t> failure_planner_obj{0};  // the player's failure object, seen by the planner
// Your car's line crossings this session (session.lap), logged at the session end to check the game's lap count.
std::atomic<int> player_crossings{0};
int game_lap();
int game_lap_of(uintptr_t obj);
const char *pit_call_name() { return pit_call_reason.load() ? "heavy damage" : "game failure"; }

// Aero or body damage the game calls terminal (mask bits 5-10 only: tyres?, front wing / nose, sidepods, floor,
// rear wing, diffuser; no power unit, gearbox or corner part): the power unit still drives the car. Until 0.51.125
// it lost all drive with no word why (test, Shanghai race: a side hit gave 0x40 and only the steering was left).
// Race: like a black flag with an orange disc (FIA Appendix H 2.5.4.1 e), pit by the end of the next lap or retire;
// in the pit lane the crew pushes the car into the garage. Practice and qualifying: back to the garage, where crash
// damage keeps the car out for the session (garage_exit_locked). In the pit lane of a race the call waits until
// the car is on track again (heavy_damage_tick).
void heavy_damage_call(uint32_t mask)
{
    bool race = !practice_like();
    bool window = race && player_pit.load() == 1 && !session_ended;
    if (race && !window) {
        swofty::log("[td-probe] heavy damage 0x%x (aero or body only): the car keeps its drive; not on track, the "
                    "pit call comes once it is", mask);
        return;
    }
    if (window) {
        pit_call_reason = 1;
        game_failure_lap = game_lap();
        game_failure_crossing = player_crossings.load();
        game_failure_deadline = GetTickCount64() | 1;
    }
    swofty::log("[td-probe] heavy damage 0x%x (aero or body only, no power unit, gearbox or corner part): the car "
                "keeps its drive; %s", mask, window ? "pit by the end of the next lap or retire" :
                "back to the garage, out for this session");
    if (swofty::hud::available())
        swofty::hud::toast("HEAVY DAMAGE", window ? "Pit by the end of the next lap or retire"
                                                  : "Bring the car back to the garage. No repairs this session.", 6);
    radio_say(radio_damage_strategy);
}

// One controller switch (ai: 1 hands the car to the AI, reason: the controller flag). Called from the
// setController hook and from the session.player.control event (Swofty 1.7); both see the same switch,
// so everything here is idempotent. Returns true to drop the switch.
bool note_controller_switch(uint8_t id, uint32_t ai, int32_t reason)
{
    if (time_trial)
        return false;  // every switch passes, your car stays unknown to the addon
    if (id < max_vehicles) {
        if (!ai) {
            human[id] = true;
            player_vehicle = id;
        }
        if (last_ai[id] != int32_t(ai) || last_reason[id] != reason) {
            last_ai[id] = int32_t(ai);
            last_reason[id] = reason;
            swofty::log("[td-probe] setController vehicle=%u known_human=%d -> %s reason=%d %s", id,
                        human[id], ai ? "AI" : "HUMAN", reason, label(reasons, reason));
        }
    }
    // Controller flags are not restart evidence. The actual garage restart
    // callback resets lifecycle state before stock crews can move grid cars.
    bool mine = id < max_vehicles && int(id) == player_vehicle.load();
    // A red flag starts as SAFETY_CAR for every car (Sess_ScRedFlagDeploy_Update): remember where the car was then.
    if (mine && ai && reason == 11 && !(player_controller_flags.load() & (1u << 11))) {
        pit_at_suspension = player_pit.load();
        swofty::log("[td-probe] suspension/safety car: your car was in pit state %d", pit_at_suspension.load());
    }
    if (mine && reason>=0 && reason<32) {
        if (ai) player_controller_flags.fetch_or(1u<<reason);
        else player_controller_flags.fetch_and(~(1u<<reason));
    }
    if (mine && reason == 3 && ai && (player_broken || car_dead || terminal_mask.load())) {
        swofty::log("[td-probe] session end (NOT_IN_SESSION set): clearing gameplay, retaining DNF audio");
        reset_car_state(true);
    }
    // After the race (NOT_IN_SESSION) the game can put your retired car back to SESSION_NOT_STARTED when a replay
    // or a restart follows (0.51.124 logs 18:47:38 and 19:14:09). The DNF camera is over by then.
    if (mine && ai && reason == 10)
        release_retired_engine_sound(id, "SESSION_NOT_STARTED");
    // The game's own scripted mechanical failure (career, My Team) pulls the car over and hands it to the AI with
    // RETIRED, with no terminal-damage message first. Keep the car yours unless you or the addon retire it.
    // Only while racing on track with a failure the game planned for this car (planner hook): a red flag, safety car or
    // garage hand-over must pass through, or the game's flow can wait for a car that never retires.
    constexpr uint32_t not_racing = (1u << 3) | (1u << 10) | (1u << 11);
    if (mine && reason == 4 && ai && keep_control && keep_game_failure && !session_ended &&
        game_planned_part.load() >= 0 && player_pit.load() == 1 && !(player_controller_flags.load() & not_racing) &&
        GetTickCount64() >= own_retire_until.load() && GetTickCount64() >= status7_at.load() + 3000 && !player_broken && !retire_requested &&
        retire_in.load() == -1.0 && confirmed_retired_vehicle.load() < 0) {
        swofty::log("[td-probe] keep_control: dropped the game's RETIRED hand-over of vehicle %u (not requested by you)", id);
        // The flag was recorded above, but the hand-over never happens; a stale RETIRED bit blocks the standstill retire.
        player_controller_flags.fetch_and(~(1u << 4));
        // The game already broke a part (planner). Run the addon's own broken-car flow for that part: throttle cap,
        // retire after standing still, pause menu / hotkey retire.
        int part = game_planned_part.exchange(-1);
        if (part > 6)
            part = 0;
        game_failure_part = part;
        swofty::log("[td-probe] game failure: breaking part %d for the addon's broken-car handling", part);
        trigger_failure(part);
        // Like a black flag with an orange disc (FIA Appendix H 2.5.4.1 e): shown at the line, the driver must stop at
        // his pit on the next lap. So: pit by the end of the next lap, or retire.
        pit_call_reason = 0;
        game_failure_lap = game_lap();
        game_failure_crossing = player_crossings.load();
        game_failure_deadline = player_pit.load() == 1 ? GetTickCount64() | 1 : 0;
        if (game_failure_deadline.load() && swofty::hud::available())
            swofty::hud::toast("MECHANICAL FAILURE", "Pit by the end of the next lap or retire", 6);
        return true;
    }
    if (reason == reason_terminal_damage && ai && is_player(id)) {
        if (keep_control) {
            swofty::log("[td-probe] keep_control: dropped TERMINAL_DAMAGE hand-over of vehicle %u", id);
            return true;
        }
    }
    return false;
}

uint64_t on_set_controller(void *system, uint64_t vehicle, uint32_t ai, int32_t reason)
{
    controller_system = system;
    uint8_t id = uint8_t(vehicle);
    if (note_controller_switch(id, ai, reason))
        return 0;
    uint64_t result=orig_set_controller(system, vehicle, ai, reason);
    if (id < max_vehicles && int(id) == player_vehicle.load() && reason==4 && ai) confirmed_retired_vehicle=int(id);
    return result;
}

// Issue #19: Cool-down Lap (vmknolle) holds back your car's NOT_IN_SESSION hand-over at the chequered flag and says so
// in a shared block (cooldown_link.hpp). Mapped read-only once it exists; without Cool-down nothing changes.
std::atomic<const cdl::State *> cooldown_state{nullptr};
uint64_t cooldown_open_at = 0;     // next try to open the block
uint64_t cooldown_next_poll = 0;   // polled four times a second from your car's update
bool cooldown_applied = false;     // the held hand-over was taken for this finish (until Cool-down clears the bit)

const cdl::State *cooldown_map(uint64_t now)
{
    if (const cdl::State *s = cooldown_state.load())
        return s;
    if (now < cooldown_open_at)
        return nullptr;
    cooldown_open_at = now + 2000;
    wchar_t name[64];
    swprintf(name, 64, cdl::name_format, static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!h)
        return nullptr;
    void *view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(cdl::State));
    CloseHandle(h);  // the view keeps the mapping
    if (!view)
        return nullptr;
    const auto *s = static_cast<const cdl::State *>(view);
    cdl::View v = cdl::read(*s);
    static bool logged_bad = false;
    if (!cdl::valid(v)) {
        if (!logged_bad)
            swofty::log("[td-probe] Cool-down Lap: shared state has magic 0x%08x version %u, not one this version "
                        "reads; ignored", v.magic, v.version);
        logged_bad = true;
        UnmapViewOfFile(view);
        return nullptr;
    }
    swofty::log("[td-probe] Cool-down Lap: shared state found (version %u): its held finish hand-over counts as yours",
                v.version);
    cooldown_state = s;
    return s;
}

// For the tests: forget the block and the poll state.
[[maybe_unused]] void cooldown_forget()
{
    if (const cdl::State *s = cooldown_state.exchange(nullptr))
        UnmapViewOfFile(const_cast<cdl::State *>(s));
    cooldown_open_at = 0;
    cooldown_next_poll = 0;
    cooldown_applied = false;
}

// Game thread, from your car's update. Cool-down marked your car finished while it holds the hand-over back: take it
// as the NOT_IN_SESSION hand-over (once per finish; the real one later changes nothing).
void cooldown_tick(uint64_t now)
{
    if (now < cooldown_next_poll)
        return;
    cooldown_next_poll = now + 250;
    int player = player_vehicle.load();
    const cdl::State *s = player >= 0 ? cooldown_map(now) : nullptr;
    if (!s)
        return;
    cdl::View v = cdl::read(*s);
    if (!cdl::bit(v.finished, player)) {
        cooldown_applied = false;
        return;
    }
    if (cooldown_applied || !cdl::finished(v, player, uint32_t(now)))
        return;
    cooldown_applied = true;
    if (player_controller_flags.load() & (1u << 3))
        return;
    swofty::log("[td-probe] Cool-down Lap holds your car's NOT_IN_SESSION hand-over for its cool-down lap: your race "
                "is over");
    note_controller_switch(uint8_t(player), 1, 3);
}

// An AI car Cool-down parks in parc ferme now: the stopped-car cleanup leaves it alone.
bool cooldown_parked(int vehicle)
{
    const cdl::State *s = cooldown_state.load();
    return s && cdl::parked(cdl::read(*s), vehicle, uint32_t(GetTickCount64()));
}

// Swofty 1.7 hook points (attach_session_events). They run next to the setController hook above and call the same
// note_controller_switch; both are installed. The log lines "session.player.control fires" and
// "car.controllers.rebuilt" show that the events arrive (they do in the 0.75.6 test logs).
std::atomic<unsigned> control_events{0}, rebuild_events{0};
// The session's laps from the event context (Swofty's telemetry is off on this core): read at the first player
// control change after session.start; 0 outside a race or sprint.
std::atomic<int> session_laps{0};
std::atomic<bool> session_laps_read{false};

// The raw text of one part of the event context ("participants", "player", ...).
std::string context_raw(const swofty::events::Context &ctx, const char *name)
{
    auto t = swofty::detail::hooks_table();
    std::string out;
    if (!t || !t->context)
        return out;
    std::string n(name);
    if (swofty::detail::fill(out, [&](char *o, size_t cap, size_t *len) { return t->context(ctx.handle(), n.c_str(), o, cap, len); }) !=
        SWOFTY_ASSET_OK)
        out.clear();
    return out;
}

// The entries of a participants JSON text, read by hand (the JSON library would add 200 KB to the DLL): each "car"
// starts an entry, its "team" is a string or an object with an "id". note(car, team id, human) per entry.
template <class F> int scan_participants_json(const std::string &rawp, F &&note)
{
    int count = 0;
    size_t pos = 0;
    while ((pos = rawp.find("\"car\"", pos)) != std::string::npos) {
        size_t colon = rawp.find(':', pos), next = rawp.find("\"car\"", pos + 5);
        if (colon == std::string::npos)
            break;
        if (next == std::string::npos)
            next = rawp.size();
        int car = atoi(rawp.c_str() + colon + 1);
        std::string team;
        size_t tk = rawp.find("\"team\"", pos);
        if (tk != std::string::npos && tk < next) {
            size_t c2 = rawp.find(':', tk), from = c2 == std::string::npos ? c2 : c2 + 1;
            while (from != std::string::npos && from < next && (rawp[from] == ' ' || rawp[from] == '{'))
                ++from;
            if (from != std::string::npos && from < next && rawp[from] == '"') {
                size_t q = rawp.find('"', from + 1);
                team = rawp.substr(from + 1, q == std::string::npos ? 0 : q - from - 1);
                if (team == "id" && q != std::string::npos) {  // an object: the value after "id":
                    size_t q1 = rawp.find('"', rawp.find(':', q) + 1);
                    size_t q2 = q1 == std::string::npos ? q1 : rawp.find('"', q1 + 1);
                    team = q2 == std::string::npos ? std::string() : rawp.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
        bool human = false;
        size_t hk = rawp.find("\"human\"", pos);
        if (hk != std::string::npos && hk < next) {
            size_t at = hk + 7;
            while (at < next && (rawp[at] == ' ' || rawp[at] == ':'))
                ++at;
            human = at < next && rawp[at] == 't';
        }
        note(car, team, human);
        ++count;
        pos = next;
    }
    return count;
}
