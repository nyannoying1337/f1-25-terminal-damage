// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Session events, player speed.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

void on_event(const SwoftyEvent *ev, void *)
{
    if (!strcmp(ev->name, SWOFTY_EVENT_SESSION_START)) {
        session_laps = 0;
        session_laps_read = false;
        player_crossings = 0;
        reset_session();
        session_ended = false;
        {
            std::lock_guard<std::mutex> lock(team_mu);
            team_of_car.assign(32, std::string());
            team_key_of_car.assign(32, 0);
            rnd_shares.clear();
        }
        rnd_shares_read = false;
        rates_logged = false;
        team_ids_ready = false;
        team_pending_at = GetTickCount64();
        team_pending = true;  // the participants come with the first control change; the AI plan waits for them
        rnd_probe_done = false;
        team_probe_done = false;
    } else if (!strcmp(ev->name, SWOFTY_EVENT_SESSION_END)) {
        if (int n = player_crossings.load(); n > 0)
            swofty::log("[td-probe] session end: your car crossed the line %d times (the game's lap count %d, length "
                        "index %d)", n, game_race_laps.load(), game_race_length.load());
        game_race_laps = -1;  // the next session's failure scheduler call reads them again
        game_race_length = -1;
        session_ended = true;
        reset_session(true);
        pit_lane_system = 0;
        controller_system = nullptr;
        {
            std::lock_guard<std::mutex> lock(team_mu);
            team_of_car.assign(32, std::string());
            team_key_of_car.assign(32, 0);
            rnd_shares.clear();
        }
        rnd_shares_read = false;
        rates_logged = false;
        team_ids_ready = false;
        team_pending = false;
    }
    swofty::log("[td-probe] event %s a=%lld b=%lld", ev->name, (long long)ev->a, (long long)ev->b);
    if (!strcmp(ev->name, SWOFTY_EVENT_RACE_START)) {
        // What the fixed-chance failures can spread over (gp_point_tick).
        const char *source = "";
        if (int laps = race_total_laps(&source); laps > 0)
            swofty::log("[td-probe] race: %d laps (%s)", laps, source);
        else
            swofty::log("[td-probe] race: lap count unknown (no Swofty telemetry, no session context, the game's race "
                        "settings say %d laps, length index %d); 10-50 %% failures come within 15 minutes of driving",
                        game_race_laps.load(), game_race_length.load());
    }
}

// SpawnGarageCrew(module, slot) uses slot entry [[module+0x60]+0x838+slot*8];
// the crew instance is per vehicle (module+0x68+v*8). A vehicle's slot comes from the module's roster
// (current_garage_assignment, garage_assignment.hpp). This diagnostic (crew_asset_diagnostics) logs the non-null
// slots (and their first bytes) when they change.
void probe_crew_entry(double dt)
{
    static double since;
    static uintptr_t last[33];
    since += dt;
    if (!crew_module || !crew_asset_diagnostics || since < 2.0)
        return;
    since = 0;
    auto module = reinterpret_cast<uintptr_t>(*crew_module);
    if (!module)
        return;
    uintptr_t now[33] = {};
    guarded_read(module + 0x60, now[32]);
    for (int i = 0; i < 32 && now[32]; i++)
        guarded_read(now[32] + 0x838 + uintptr_t(i) * 8, now[i]);
    if (!memcmp(now, last, sizeof now))
        return;
    memcpy(last, now, sizeof now);
    int v = player_vehicle.load();
    uintptr_t instance = 0;
    if (v >= 0 && v < 0x20)
        guarded_read(module + 0x68 + uintptr_t(v) * 8, instance);
    swofty::log("[td-probe] garage: crew slots: table %p, player vehicle %d instance %p",
                reinterpret_cast<void *>(now[32]), v, reinterpret_cast<void *>(instance));
    for (int i = 0; i < 32; i++) {
        if (!now[i])
            continue;
        uint64_t q[12] = {};
        for (int k = 0; k < 12; k++)
            guarded_read(now[i] + k * 8, q[k]);
        swofty::log("[td-probe] garage: crew slot %d entry %p: %016llx %016llx %016llx %016llx %016llx %016llx "
                    "%016llx %016llx %016llx %016llx %016llx %016llx", i, reinterpret_cast<void *>(now[i]),
                    q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
    }
}

// The game's UDP telemetry was a second speed source and a debug log source until 0.76.2, read by a listener thread
// behind the debug option "UDP telemetry port" (off by default). The listener is gone: the DLL has no network code
// (Swofty's review holds back every addon with a socket), and the speed comes from the game's car (read_game_speed).
// Nothing in the game feeds these values any more: they stay at "no data" and the readers below take that path. The
// tests still set them to check those readers.
constexpr int udp_cars = 24, udp_car_size = 59;
std::atomic<int> udp_speed[udp_cars], udp_rpm[udp_cars], udp_gear[udp_cars];
std::atomic<float> udp_throttle[udp_cars];
std::atomic<int> udp_player{-1};
std::atomic<uint64_t> udp_packets{0}, udp_last_ms{0};
// CarDamage (id 10) = 29 + 24 x 46 bytes (2025 car layout): u8 gearbox @36, engine damage % @37, ICE wear @41,
// engine blown @44, engine seized @45. Packed as gearbox | engine << 8 | ice << 16 | blown << 24 | seized << 25.
constexpr int udp_damage_size = 46;
std::atomic<uint32_t> udp_engine_state{0xffffffff};

// Player speed in km/h, read from the game's car every frame (read_game_speed). -1 when it is not fresh (paused,
// between sessions).
int player_speed()
{
    if (session_ended)
        return -1;
    uint64_t now = GetTickCount64(), game = game_speed_ms.load();
    if (game && game > telemetry_not_before.load() && now - game <= 500)
        return game_speed.load();
    int p = udp_player.load();
    uint64_t last = udp_last_ms.load();
    if (p < 0 || p >= udp_cars || !last || last <= telemetry_not_before.load() || now - last > 500)
        return -1;
    return udp_speed[p].load();
}

// True while the game updates the player's car: not in the pause menu, the flashback view or between sessions. Not the
// speed read: that runs only when a feature needs the speed (suspension damage, a broken car), so with suspension
// damage off 0.75.3's speed-based check kept every failure clock at a standstill.
bool player_car_running()
{
    if (session_ended)
        return false;
    uint64_t last = player_update_ms.load();
    return last && last > telemetry_not_before.load() && GetTickCount64() - last <= 500;
}

// The seconds the car drove in this frame: dt while the game updates the player's car, 0 in the pause menu and other
// frozen screens (on_frame gets real seconds on every rendered frame) and between sessions.
double drive_seconds(double dt) { return player_car_running() ? dt : 0.0; }
