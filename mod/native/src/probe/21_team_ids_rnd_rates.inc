// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Team ids from game memory, Durability R&D shares, team rate log.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// The team of every car from the game's own entrant records (0.75.6). The session context's participants list is empty
// in a session (Career test: "[]"), but the failure object of each car keeps a pointer at +0x80 whose record has the
// entrant's team stat id at +0x58 (swofty/game/entrants.hpp: EntrantRecord::team_stat_id; the same ids as the keys of the
// R&D trees): (476 + row) << 16, see fm::row_family. The ids are stored as "slot:<row>" and read as a team family when a
// plan is made. At most once a second until it works; the cars are there as soon as the failure scheduler has run.
bool team_ids_from_memory()
{
    if (team_ids_ready.load())
        return true;
    static uint64_t next = 0;
    uint64_t now = GetTickCount64();
    if (session_ended || !failure_mgr.load() || now < next)
        return false;
    next = now + 1000;
    std::vector<std::string> ids(32);
    std::vector<uint32_t> keys(32, 0);
    std::string line;
    int n = 0;
    for_each_failure_obj([&](int v, uintptr_t obj) {
        uintptr_t rec = 0;
        uint32_t key = 0;
        if (v < 0 || v >= 32 || !guarded_read(obj + 0x80, rec) || !rec || !guarded_read(rec + 0x58, key) || (key & 0xffff))
            return;
        int row = int(key >> 16) - 476;
        if (row < 0 || row > 15)
            return;
        ids[v] = "slot:" + std::to_string(row);
        keys[v] = key;
        ++n;
        char one[40];
        snprintf(one, sizeof one, "%s%d:%d", line.empty() ? "" : " ", v, row);
        line += one;
    });
    if (n < 2) {
        // Not readable (0.76.1: every car was "?" in Grand Prix with a 2026 entrant list): after 10 tries in a session
        // log once what the records hold, so the layout can be checked from a log.
        static uint64_t diag_mgr = 0;
        static int tries = 0;
        if (diag_mgr != failure_mgr.load()) {
            diag_mgr = failure_mgr.load();
            tries = 0;
        }
        if (++tries == 10) {
            std::string what;
            int objs = 0, recs = 0;
            for_each_failure_obj([&](int v, uintptr_t obj) {
                ++objs;
                uintptr_t rec = 0;
                uint32_t w[8] = {};
                if (!guarded_read(obj + 0x80, rec) || !rec)
                    return;
                ++recs;
                if (recs > 4)
                    return;
                for (int i = 0; i < 8; ++i)
                    guarded_read(rec + 0x50 + 4 * i, w[i]);
                char one[160];
                snprintf(one, sizeof one, " [car %d: %08x %08x %08x %08x %08x %08x %08x %08x]", v, w[0], w[1], w[2], w[3],
                         w[4], w[5], w[6], w[7]);
                what += one;
            });
            swofty::log("[td-probe] teams: the entrant records give no team after 10 tries: %d car object(s), %d with a "
                        "record, %d readable; record +0x50..+0x6c of the first:%s", objs, recs, n, what.c_str());
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(team_mu);
        team_of_car = ids;
        team_key_of_car = keys;
    }
    team_ids_ready = true;
    team_pending = false;
    team_epoch.fetch_add(1);
    swofty::log("[td-probe] teams: from the game's entrant records, %d car(s) (vehicle:team slot, 0 Mercedes 1 Ferrari 2 Red Bull "
                "3 Williams 4 Aston Martin 5 Alpine 6 RB 7 Haas 8 McLaren 9 Sauber/Audi 10 Cadillac): %s", n, line.c_str());
    return true;
}

// The size of one node record in the instance list [ib, ie): the first candidate size that divides the span and whose
// first nodes all have plausible names, 0 if none fits.
size_t rnd_node_stride(uint64_t ib, uint64_t ie)
{
    uint64_t span = ie - ib;
    for (size_t cand : {0xd0u, 0xd8u, 0xe0u, 0xe8u, 0xf0u, 0xf8u, 0x100u, 0x108u, 0x110u, 0x120u, 0x130u, 0x140u}) {
        if (span % cand || span / cand < 2 || span / cand > 400)
            continue;
        size_t n = span / cand, ok = 0, look = std::min<size_t>(n, 6);
        for (size_t k = 0; k < look; ++k)
            ok += rnd_plausible_name(ib + k * cand + 0x10);
        if (ok >= look)
            return cand;
    }
    return 0;
}

// The share of finished Durability upgrades of every team (department 2 of the R&D data, see rnd_probe_tick for the
// layout), once a Career session: all nodes of the department (not only the enabled ones: 39 of 52 on day 27, and that
// number grows with the season) and how many of them are finished (fm::rnd_state_done on the last state entry: Completed;
// a failed research does not count). Only a team's first tree is read (the upgrade modes 0 and 1 have one, mode 2 one a
// driver). Takes effect through team_grid_now. On the game's flow thread (on_frame).
void read_rnd_durability()
{
    static uint64_t next = 0;
    if (rnd_shares_read.load() || session_ended)
        return;
    uint64_t now = GetTickCount64();
    if (now < next)
        return;
    next = now + 2000;
    swofty::career::State st{};
    rnd_shares_read = true;
    if (swofty::career::state(&st) != swofty::career::Result::ok || !st.loaded)
        return;  // not a Career
    uintptr_t comp = reinterpret_cast<uintptr_t>(swofty::career::component("UpgradesComponent"));
    uint64_t b = 0, e = 0;
    if (!comp || !guarded_read(comp + 0x298, b) || !guarded_read(comp + 0x2a0, e) || !b || e < b) {
        swofty::log("[td-probe] rd: no UpgradesComponent, the teams' R&D does not change the failure rates");
        return;
    }
    std::vector<RndShare> out;
    std::string line;
    size_t teams = std::min<size_t>((e - b) / 8, 32);
    for (size_t i = 0; i < teams; ++i) {
        uintptr_t up = 0, tree = 0;
        uint32_t key = 0;
        uint64_t ib = 0, ie = 0;
        if (!guarded_read(b + 8 * i, up) || !up || !guarded_read(up + 0xc, key))
            continue;
        tree = up + 0xe0;
        if (!guarded_read(tree + 0x10, ib) || !guarded_read(tree + 0x18, ie) || !ib || ie <= ib)
            continue;
        size_t stride = rnd_node_stride(ib, ie);
        if (!stride)
            continue;
        RndShare s;
        s.key = key;
        int failed = 0, history_nodes = 0;
        std::string running;  // what is being researched: the node, the day it ends, whether the game rolled a failure
        std::string histories;  // the state history of nodes that went beyond the usual 0/1, 2, 3 (a failed research)
        for (size_t k = 0; k < (ie - ib) / stride; ++k) {
            uintptr_t inst = ib + k * stride;
            uint8_t dept = 0xff, state = 0xff;
            uint64_t sb = 0, se = 0;
            if (!guarded_read(inst + 0x72, dept) || dept != 2)
                continue;
            if (!guarded_read(inst + 0xc0, sb) || !guarded_read(inst + 0xc8, se) || !sb || se < sb + 0x10 ||
                !guarded_read(se - 0x10 + 8, state))
                continue;
            ++s.total;
            s.done += fm::rnd_state_done(state);
            failed += state == 4;
            // The whole history when it holds a state beyond 0/1, 2, 3 (a failed research and the restart after it): the
            // log prints state@day, so the numbers of the game's states show (the 'rnd:' dump only has the last one).
            if (size_t entries = size_t((se - sb) / 0x10); entries >= 3 && entries <= 40 && history_nodes < 8) {
                std::vector<fm::RndEntry> hist;
                for (size_t j = 0; j < entries; ++j) {
                    uint16_t entry_day = 0;
                    uint8_t entry_state = 0xff;
                    if (!guarded_read(uintptr_t(sb) + j * 0x10 + 4, entry_day) ||
                        !guarded_read(uintptr_t(sb) + j * 0x10 + 8, entry_state))
                        break;
                    hist.push_back({int(entry_state), int(entry_day)});
                }
                if (fm::rnd_history_notable(hist)) {
                    std::string name = rnd_name(inst + 0x10);
                    if (name.rfind("om_xm_", 0) == 0)
                        name.erase(0, 6);
                    histories += (histories.empty() ? "" : "; ") + name + ": " + fm::rnd_history_text(hist);
                    ++history_nodes;
                }
            }
            if (state == 2) {
                // Career_Start_UpgradeResearch (0x1435168d0) keeps the end day at +0x80 and the roll for the research
                // failing at +0x88; on the end day 0x143523a40 pushes Failed (4) when that roll said fail, else
                // Completed (3). So a research the game rolled a failure for should show state 4 after its end day.
                uint16_t end_day = 0;
                uint8_t will_fail = 0;
                guarded_read(inst + 0x80, end_day);
                guarded_read(inst + 0x88, will_fail);
                std::string name = rnd_name(inst + 0x10);
                if (name.rfind("om_xm_", 0) == 0)
                    name.erase(0, 6);
                char when[64];
                snprintf(when, sizeof when, " until day %u%s", unsigned(end_day),
                         will_fail ? ", the game rolled a failure" : "");
                running += (running.empty() ? "" : "; ") + name + when;
            }
        }
        if (s.total <= 0)
            continue;
        out.push_back(s);
        char one[64];
        snprintf(one, sizeof one, "%s%u: %d/%d", line.empty() ? "" : "; ", unsigned(key), s.done, s.total);
        line += one;
        if (!running.empty() || failed) {
            line += " (";
            if (!running.empty())
                line += "researching " + running;
            if (failed) {
                snprintf(one, sizeof one, "%s%d failed", running.empty() ? "" : ", ", failed);
                line += one;
            }
            line += ")";
        }
        if (!histories.empty())
            swofty::log("[td-probe] rd: state history of team %u (state@day; 1 Available, 2 InProgress, 3 Completed, 4 Failed): "
                        "%s", unsigned(key), histories.c_str());
    }
    if (out.empty()) {
        swofty::log("[td-probe] rd: no Durability upgrades found, the teams' R&D does not change the failure rates");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(team_mu);
        rnd_shares = out;
    }
    team_epoch.fetch_add(1);  // a plan made before this is made again
    swofty::log("[td-probe] rd: finished Durability upgrades by team (team key: done/total); every team starts with %d "
                "done, the first one more takes about %.1f %% off that team's failure rate and later ones less, %.0f %% with all "
                "done%s: %s",
                fm::rnd_start_done,
                (1.0 - fm::rnd_factor(1.0 / std::max(1, out.front().total - fm::rnd_start_done))) * 100.0,
                fm::rnd_max_reduction * 100.0, rnd_reliability ? "" : " (the option is off, no effect)", line.c_str());
}

// Once a session, when the teams are known (in a Career also once its R&D data has been read): the factor of every team as
// the plans use it, R&D included. The failure plans log this for a race only; this shows it in practice and qualifying
// too (0.75.6: the "(R&D 0.99)" tag of a team that finished a Durability upgrade). On the game's flow thread (on_frame).
void team_rates_log_tick()
{
    if (rates_logged.load() || session_ended || !team_ids_ready.load())
        return;
    swofty::career::State st{};
    if (!rnd_shares_read.load() && swofty::career::state(&st) == swofty::career::Result::ok && st.loaded)
        return;  // the Career's R&D data comes first
    rates_logged = true;
    const fm::Season &season = fm::season_of(failure_era);
    int matched = 0;
    uint32_t present = 0;
    team_factors_now(season, &matched, &present);
    int cars = 0;
    for (uint32_t m = present; m; m &= m - 1)
        ++cars;
    swofty::log("[td-probe] teams: rates now for %.*s (%d of %d cars with a known team, R&D included): %s",
                int(season.id.size()), season.id.data(), matched, cars, team_rates_text(season).c_str());
}
