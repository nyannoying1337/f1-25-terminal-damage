// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Team ids from the session, per-team rates, session events.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// The team of every car of the session, from the session context's participants: the typed list first, then the raw
// text read by hand (a record that does not parse into an Entrant still has its car and team). Called once per
// session, at the first player control change (attach_session_events). The list was empty in the 0.75.6 test logs (raw
// participants "[]"), so the ids come from team_ids_from_memory; this stays as the first try. The 900 ms throttle only
// matters if another caller is added. Logged in one line so a test log shows the ids the game uses for the teams; when
// it fails, the start of the raw text is logged (three times) to show why.
bool read_session_teams(const swofty::events::Context &ctx, const char *where)
{
    if (team_ids_ready.load())
        return true;
    static std::atomic<uint64_t> last_try{0};
    static std::atomic<int> fails_logged{0};
    uint64_t now = GetTickCount64();
    if (now < last_try.load() + 900)
        return false;
    last_try = now;
    std::vector<std::string> ids(32);
    std::string line;
    int n = 0, known = 0;
    auto note = [&](int car, std::string id, bool human) {
        if (car < 0 || car >= 32)
            return;
        while (!id.empty() && id.back() == '?')
            id.pop_back();
        ids[car] = id;
        ++n;
        known += !id.empty();
        char one[112];
        snprintf(one, sizeof one, "%s%d %s%s", line.empty() ? "" : "; ", car, id.empty() ? "?" : id.c_str(),
                 human ? " (you)" : "");
        if (line.size() < 1000)
            line += one;
    };
    for (const auto &e : ctx.participants())
        note(int(e.car), std::string(e.team.id()), e.human);
    std::string rawp;
    if (!n) {
        rawp = context_raw(ctx, "participants");
        scan_participants_json(rawp, note);
    }
    if (!n) {
        if (fails_logged.fetch_add(1) < 3) {
            std::string player = context_raw(ctx, "player");
            swofty::log("[td-probe] teams: no participants at %s (typed list empty, raw participants %zu bytes: %.300s; raw "
                        "player %zu bytes: %.200s)", where, rawp.size(), rawp.c_str(), player.size(), player.c_str());
        }
        return false;
    }
    if (known < 2) {
        // The list has the cars but no team ids (0.76.1: 23 participants, every id empty, in Grand Prix with a 2026
        // entrant list). It used to count as done, which kept the entrant records (team_ids_from_memory) from being
        // read: every car got the season's average rate.
        if (fails_logged.fetch_add(1) < 3)
            swofty::log("[td-probe] teams: %d participant(s) at %s without a team id, the entrant records are read instead",
                        n, where);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(team_mu);
        team_of_car = ids;
    }
    team_ids_ready = true;
    team_pending = false;
    team_epoch.fetch_add(1);
    swofty::log("[td-probe] teams: %d participant(s) at %s (vehicle, team id): %s", n, where, line.c_str());
    return true;
}

// The cars with a known team: team ids, vehicle ids and the R&D factor of each car's team (1 without R&D data or with
// the option off).
struct TeamGrid {
    std::vector<std::string> ids;
    std::vector<int> cars;
    std::vector<double> extra;
};

TeamGrid team_grid_now()
{
    TeamGrid g;
    bool rnd = rnd_reliability;
    std::lock_guard<std::mutex> lock(team_mu);
    for (int v = 0; v < 32; ++v) {
        if (team_of_car[v].empty())
            continue;
        g.ids.push_back(team_of_car[v]);
        g.cars.push_back(v);
        double x = 1.0;
        if (rnd)
            for (const RndShare &s : rnd_shares)
                if (s.key == team_key_of_car[v] && s.total > 0) {
                    x = fm::rnd_factor(fm::rnd_progress(s.done, s.total));
                    break;
                }
        g.extra.push_back(x);
    }
    return g;
}

// Failure factors of the cars in the Failure season: each car's team's relative rate (mean 1 over the grid) times its
// team's R&D durability factor (absolute, see fm::rnd_factor), 1 for every car when the teams are not known. matched: how
// many cars have a team the season's table knows.
std::array<double, 32> team_factors_now(const fm::Season &season, int *matched = nullptr, uint32_t *present = nullptr)
{
    std::array<double, 32> out;
    out.fill(1.0);
    TeamGrid g = team_grid_now();
    uint32_t mask = 0;
    for (int v : g.cars)
        mask |= 1u << v;
    if (matched)
        *matched = 0;
    if (present)
        *present = mask;
    if (g.ids.empty())
        return out;
    std::vector<double> f = fm::grid_factors(season, g.ids, matched, &g.extra);
    for (size_t i = 0; i < g.cars.size(); ++i)
        out[g.cars[i]] = f[i];
    return out;
}

// "cadillac x2.14, aston x2.95, ..." for the log: the factor of every team in the grid (its R&D durability included,
// shown as "(R&D 0.78)" once it differs from the start state), highest first, and the number of cars whose team the
// season's table does not know.
std::string team_rates_text(const fm::Season &season)
{
    TeamGrid g = team_grid_now();
    std::vector<double> f = fm::grid_factors(season, g.ids, nullptr, &g.extra);
    std::vector<std::pair<double, std::string>> by_team;
    int unknown = 0;
    for (size_t i = 0; i < g.ids.size(); ++i) {
        const char *fam = fm::family_for(season, g.ids[i]);
        if (!fam || fm::family_weight(season, fam) <= 0) {
            ++unknown;
            continue;
        }
        std::string name = fam;
        if (g.extra[i] != 1.0) {
            char tag[24];
            snprintf(tag, sizeof tag, " (R&D %.2f)", g.extra[i]);
            name += tag;
        }
        bool seen = false;
        for (const auto &p : by_team)
            seen = seen || p.second == name;
        if (!seen)
            by_team.emplace_back(f[i], name);
    }
    std::sort(by_team.begin(), by_team.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::string out;
    char one[96];
    for (const auto &p : by_team) {
        snprintf(one, sizeof one, "%s%s x%.2f", out.empty() ? "" : ", ", p.second.c_str(), p.first);
        out += one;
    }
    if (unknown) {
        snprintf(one, sizeof one, "%s%d car(s) of an unknown team x1.00 before scaling", out.empty() ? "" : "; ", unknown);
        out += one;
    }
    return out;
}

// The factor of one car (1 without a team).
double team_scale(const fm::Season &season, int vehicle)
{
    if (vehicle < 0 || vehicle >= 32)
        return 1.0;
    return team_factors_now(season)[vehicle];
}

void reset_session(bool preserve_retirement_audio);

void attach_session_events()
{
    namespace sess = swofty::sessions;
    namespace cars = swofty::cars;
    auto control = swofty::events::on<sess::events::PlayerControl>().replace(
        [](sess::events::PlayerControl &c, const swofty::events::Context &ctx) {
            if (!session_laps_read.exchange(true)) {
                if (const auto &v = ctx.session()) {
                    bool race = v->type == swofty::events::SessionType::race ||
                                v->type == swofty::events::SessionType::sprint;
                    session_laps = race && v->laps > 0 && v->laps < 200 ? v->laps : 0;
                    std::string_view track = v->track.id();
                    swofty::log("[td-probe] session: type %d '%s', track '%.*s', %d laps (session context)",
                                int(v->type), v->session.c_str(), int(track.size()), track.data(), v->laps);
                    if (v->type == swofty::events::SessionType::time_trial) {
                        time_trial = true;
                        session_ended = true;
                        reset_session(true);
                        team_pending = false;
                        swofty::log("[td-probe] time trial: the addon does nothing in this session (no damage model, "
                                    "no failures, no retirement)");
                    }
                } else {
                    swofty::log("[td-probe] session: no session context");
                }
                if (!time_trial)
                    read_session_teams(ctx, "the first control change");
            }
            if (time_trial)
                return false;
            if (control_events.fetch_add(1) == 0)
                swofty::log("[td-probe] session.player.control fires (first: vehicle %d %d -> %d reason %d)",
                            int(c.car.index()), int(c.from), int(c.to), int(c.reason));
            return note_controller_switch(uint8_t(c.car.index()), c.to == sess::Control::ai ? 1u : 0u, c.reason);
        });
    auto lap = swofty::events::on<sess::events::Lap>().after(
        [](const sess::events::Lap &l, const swofty::events::Context &) {
            if (int(l.car.index()) == player_vehicle.load())
                player_crossings.fetch_add(1);
        });
    auto rebuilt = swofty::events::on<cars::events::ControllersRebuilt>().after(
        [](const cars::events::ControllersRebuilt &r, const swofty::events::Context &) {
            if (int(r.car.index()) == player_vehicle.load() || rebuild_events.fetch_add(1) == 0)
                swofty::log("[td-probe] car.controllers.rebuilt: vehicle %d (flashback or return to garage)",
                            int(r.car.index()));
        });
    // Red flag camera reports: log when the spectator director's HUD comes and goes while your car is broken, with
    // the controller flags, so a test log shows what switched the camera.
    auto spectator = swofty::events::on<swofty::spectator::events::HudVisible>().after(
        [](const swofty::spectator::events::HudVisible &h, const swofty::events::Context &) {
            if (player_broken || confirmed_retired_vehicle.load() >= 0)
                swofty::log("[td-probe] spectator HUD %s (following vehicle %d; your car %d broken %d retired %d pit "
                            "state %d controller flags 0x%x)",
                            h.visible ? "on" : "off", int(h.following.index()), player_vehicle.load(),
                            int(player_broken.load()), confirmed_retired_vehicle.load() >= 0, player_pit.load(),
                            player_controller_flags.load());
        });
    swofty::log("[td-probe] session events: player.control %s, lap %s, controllers.rebuilt %s, spectator HUD %s",
                control == swofty::events::Result::ok ? "attached" : "unavailable",
                lap == swofty::events::Result::ok ? "attached" : "unavailable",
                rebuilt == swofty::events::Result::ok ? "attached" : "unavailable",
                spectator == swofty::events::Result::ok ? "attached" : "unavailable");
}
