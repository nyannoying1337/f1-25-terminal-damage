// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Team id probe (0.75.6, read only).
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// ---- Team id probe (0.75.6, read only) ----
// Asks the registry which stat id a team's record has (SwoftyRegistry.stat_id), for the ids the stock teams may have
// ("game/<name>"), and logs the answers once a run. The other half of the first probe, the team of each car from the
// game's memory (the failure object keeps a pointer at +0x80 whose record has the team stat id at +0x58, the same ids
// as the R&D probe's team keys), is now team_ids_from_memory: it is not read only, it fills the team ids that the
// failure plans use.
// Once a Career session, after the R&D probe. On the game's flow thread (on_frame).
std::atomic<bool> team_probe_done{false};

void team_id_probe_tick()
{
    static uint64_t next = 0;
    if (team_probe_done.load() || session_ended || !rnd_probe_done.load() || rnd_team_keys.empty())
        return;  // not a Career (no R&D trees)
    uint64_t now = GetTickCount64();
    if (now < next)
        return;
    next = now + 3000;
    team_probe_done = true;
    static bool registry_logged = false;  // the stat ids of the stock teams do not change: once a run
    if (registry_logged)
        return;
    registry_logged = true;
    const SwoftyRegistry *reg = swofty::detail::registry_table();
    if (!reg || !reg->stat_id) {
        swofty::log("[td-probe] teams: no registry stat ids on this core");
        return;
    }
    std::string found;
    int tried = 0;
    for (const char *name : {"mercedes", "ferrari", "redbull", "red_bull", "mclaren", "williams", "astonmartin", "aston_martin",
                             "alpine", "rb", "racingbulls", "racing_bulls", "alphatauri", "tororosso", "audi", "sauber",
                             "kicksauber", "haas", "cadillac", "gm_cadillac", "renault", "forceindia", "racingpoint",
                             "alfaromeo", "lotus", "user_defined"}) {
        for (const char *prefix : {"game/", ""}) {
            std::string id = std::string(prefix) + name;
            int64_t stat = -1;
            ++tried;
            if (reg->stat_id("team.identity", id.c_str(), &stat) == SWOFTY_ASSET_OK) {
                char one[96];
                snprintf(one, sizeof one, "%s%s = %lld", found.empty() ? "" : "; ", id.c_str(), (long long)stat);
                found += one;
            }
        }
    }
    swofty::log("[td-probe] teams: registry stat ids of %d guessed team ids: %s", tried,
                found.empty() ? "none resolved" : found.c_str());
}
