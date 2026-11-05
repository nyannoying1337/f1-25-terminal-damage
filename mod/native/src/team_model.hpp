// Per-team failure rates (0.75.6): how much more or less often a team's cars break down than the season's average
// car, from the team's measured retirements (team_rates.hpp, made by re/tools/team_rates.py).
//
// A team's rate is pulled towards the season's mean rate with the season's prior (starts worth of average
// results), so a team with few starts is not taken at face value; a season whose teams hardly differ (2023-25: the
// differences are what chance gives) gets a large prior and is nearly flat, 2026 a small one. The relative rate w of
// each table row is scaled so that the start-weighted mean of w over the real grid is exactly 1: the season's overall
// race_chance is kept. For the grid in the game (any set of cars, teams the table does not know count as 1) the factors
// are scaled once more to a mean of 1 over those cars, so the expected number of failures in a race is always the
// season's race_chance times the number of cars.
#pragma once

#include "failure_model.hpp"
#include "team_rates.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace fm {

// Words a game team's id or name is matched against: letters and digits only, lowercase. A word of four letters or
// more is looked for anywhere in the squashed id ("game/racing_point" is "gameracingpoint"), a shorter one has to be
// a whole word of the id ("rb" must not match "barbados").
struct FamilyWords {
    const char *family;
    const char *words[5];
};
inline constexpr FamilyWords family_words[] = {
    {"mercedes", {"mercedes"}},
    {"ferrari", {"ferrari"}},
    {"redbull", {"redbull"}},
    {"rb", {"tororosso", "alphatauri", "racingbulls", "vcarb", "rb"}},
    {"mclaren", {"mclaren"}},
    {"williams", {"williams"}},
    {"aston", {"astonmartin", "racingpoint", "forceindia", "aston"}},
    {"alpine", {"alpine"}},
    {"renault", {"renault"}},
    {"lotus", {"lotus"}},
    {"sauber", {"sauber", "alfaromeo"}},
    {"audi", {"audi"}},
    {"haas", {"haas"}},
    {"cadillac", {"cadillac"}},
    {"manor", {"manor", "marussia", "virgin"}},
    {"caterham", {"caterham"}},
    {"hrt", {"hrt", "hispania"}},
};

inline std::string squash_id(std::string_view s)
{
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline bool has_word(std::string_view id, std::string_view word)
{
    size_t i = 0;
    while (i < id.size()) {
        while (i < id.size() && !std::isalnum(static_cast<unsigned char>(id[i])))
            ++i;
        size_t j = i;
        while (j < id.size() && std::isalnum(static_cast<unsigned char>(id[j])))
            ++j;
        if (j > i) {
            std::string w = squash_id(id.substr(i, j - i));
            if (w == word)
                return true;
        }
        i = j;
    }
    return false;
}

// The family of a game team id or name (nullptr: none of the known teams).
inline const char *family_of(std::string_view id)
{
    std::string sq = squash_id(id);
    for (const FamilyWords &f : family_words)
        for (const char *w : f.words) {
            if (!w)
                break;
            size_t len = std::strlen(w);
            if (len >= 4 ? sq.find(w) != std::string::npos : has_word(id, w))
                return f.family;
        }
    return nullptr;
}

inline int season_year(const Season &s)
{
    int year = 2026;
    std::from_chars(s.id.data(), s.id.data() + s.id.size(), year);
    return year;
}

// A team slot of the game's team list (0.75.6): the game's entrant record gives each car's team as a stat id, (476 + row)
// << 16, and the rows of the ten stock teams are the registry's stat ids of game/mercedes 0, ferrari 1, redbull 2,
// williams 3, forceindia 4, renault 5, tororosso 6, haas 7, mclaren 8, sauber 9; row 10 is the eleventh team (Cadillac in
// a 2026 grid). Each row is a team lineage, so the family it stands for depends on the season. Seen in a Career test
// (2026 content): vehicles 0 and 1 had row 8 (McLaren), 2 and 3 row 0 (Mercedes), 4 and 5 row 2 (Red Bull), the order of
// the 2025 standings. A game id of the form "slot:<row>" stands for such a row.
inline const char *row_family(const Season &s, int row)
{
    int year = season_year(s);
    switch (row) {
    case 0: return "mercedes";
    case 1: return "ferrari";
    case 2: return "redbull";
    case 3: return "williams";
    case 4: return "aston";  // Force India, Racing Point, Aston Martin
    case 5: return year <= 2015 ? "lotus" : year <= 2020 ? "renault" : "alpine";
    case 6: return "rb";  // Toro Rosso, AlphaTauri, RB
    case 7: return "haas";
    case 8: return "mclaren";
    case 9: return year >= 2026 ? "audi" : "sauber";
    case 10: return "cadillac";
    default: return nullptr;
    }
}

// The slot row of a team lineage (-1: a team without a stock slot).
inline int family_row(std::string_view family)
{
    struct Row {
        const char *family;
        int row;
    };
    static constexpr Row rows[] = {{"mercedes", 0}, {"ferrari", 1}, {"redbull", 2}, {"williams", 3}, {"aston", 4},
                                   {"lotus", 5},    {"renault", 5}, {"alpine", 5},  {"rb", 6},       {"haas", 7},
                                   {"mclaren", 8},  {"sauber", 9},  {"audi", 9},    {"cadillac", 10}};
    for (const Row &r : rows)
        if (family == r.family)
            return r.row;
    return -1;
}

// The family of a game team id or name, or of a "slot:<row>" id, as the Season's table spells it: a team's lineage keeps
// its slot, so the Sauber of 2025 is the Audi of 2026 and the Renault of 2018 the Alpine of 2022 (nullptr: unknown).
inline const char *family_for(const Season &s, std::string_view id)
{
    if (id.substr(0, 5) == "slot:") {
        int row = -1;
        std::from_chars(id.data() + 5, id.data() + id.size(), row);
        return row_family(s, row);
    }
    const char *f = family_of(id);
    int row = f ? family_row(f) : -1;
    return row >= 0 ? row_family(s, row) : f;
}

inline const tr::Table &team_table(const Season &s)
{
    int n = int(sizeof tr::tables / sizeof tr::tables[0]);
    return tr::tables[std::clamp(season_year(s) - 2012, 0, n - 1)];
}

// Relative failure rate of a family in a season (mean 1 over the real grid, weighted by starts); -1 if the season's
// table has no such team.
inline double family_weight(const Season &s, std::string_view family)
{
    const tr::Table &t = team_table(s);
    double starts = 0, mech = 0;
    for (int i = 0; i < t.n; ++i) {
        starts += t.rows[i].starts;
        mech += t.rows[i].mech;
    }
    double p = starts > 0 ? mech / starts : 0.0, k = t.prior, mean = 0, mine = -1;
    for (int i = 0; i < t.n; ++i) {
        const tr::Row &r = t.rows[i];
        double rate = (r.mech + k * p) / (r.starts + k);
        mean += r.starts * rate;
        if (family == r.family)
            mine = rate;
    }
    mean /= starts;
    return mine < 0 || mean <= 0 ? -1.0 : mine / mean;
}

// Factors for the cars of a grid, given each car's team id or name: the weight of the car's family (1 for an unknown
// team), scaled to a mean of 1 over the grid. matched, if given, gets the number of cars whose team is known.
// extra, if given, is one more factor a car (the R&D durability of its team, rnd_factor) that multiplies the factor AFTER
// the scaling: it is not shared out. A team that researches more fails less and the others are not made worse, and when
// the whole grid researches (the AI teams do), the whole grid fails less as a season goes on.
inline std::vector<double> grid_factors(const Season &s, const std::vector<std::string> &team_ids, int *matched = nullptr,
                                        const std::vector<double> *extra = nullptr)
{
    std::vector<double> w(team_ids.size(), 1.0);
    int known = 0;
    double sum = 0;
    for (size_t i = 0; i < team_ids.size(); ++i) {
        const char *f = family_for(s, team_ids[i]);
        double x = f ? family_weight(s, f) : -1.0;
        if (x > 0) {
            w[i] = x;
            ++known;
        }
        sum += w[i];
    }
    if (sum > 0)
        for (double &x : w)
            x *= double(w.size()) / sum;
    if (extra)
        for (size_t i = 0; i < w.size() && i < extra->size(); ++i)
            if ((*extra)[i] > 0)
                w[i] *= (*extra)[i];
    if (matched)
        *matched = known;
    return w;
}

}  // namespace fm

namespace fm {

// How much the R&D durability of a team changes its failure rate. Every team starts a Career with the same Durability
// upgrades done (rnd_start_done: seen in a 2026 Career, day 27, all 11 teams in upgrade mode 0: 5 of the 52 nodes, the
// game researches them for everybody), and that start state is the rate of the season's statistics, factor 1. The
// progress of a team is its finished upgrades beyond that start, as a share of the rest (0..1); at progress 1 the rate is
// rnd_max_reduction lower than at the start. The factor is absolute: it belongs to the team that did the research (the AI
// teams research too), nothing is taken from or given to the others.
inline constexpr double rnd_max_reduction = 0.5;
inline constexpr int rnd_start_done = 5;

// The factor of a team with this progress (1 at the start state, rnd_max_reduction off at all done).
inline double rnd_factor(double progress) { return 1.0 - rnd_max_reduction * std::clamp(progress, 0.0, 1.0); }

// The progress of a team with `done` finished upgrades of `total` in the department: (done - start) / (total - start),
// 0 below the start state or without a department.
inline double rnd_progress(int done, int total)
{
    int rest = total - rnd_start_done;
    return rest > 0 ? std::clamp(double(done - rnd_start_done) / double(rest), 0.0, 1.0) : 0.0;
}

// The states of the game's R&D data (UpgradeStateEntry.state). The names are the game's own (k_Career_UpgradeStateNameTable
// 0x14565cee0), what writes each one was read from the code (build d873a1c8; every change goes through
// Career_Push_UpgradeState 0x1435230e0). The upgrade mode is the Career's setting (career +0x3e0, copied into every team's
// Upgrades +8 and into the global 0x1460fc310): 0 one shared tree, 1 one tree a team, 2 one tree a driver.
//   0 Unavailable       locked: every instance starts here; the availability refresh (0x14352f300) moves it to 1 and back
//                       by whether the node can be reached from the finished ones
//   1 Available         can be researched
//   2 InProgress        Career_Start_UpgradeResearch (0x1435168d0). The roll for the research failing is made HERE
//                       (chanceOfFailure against the team's failure modifier) and kept in the instance (+0x88); the end
//                       day is the instance's +0x80
//   3 Completed         research finished and not failed: the daily research tick (0x143530f00) calls 0x143523a40 on the end
//                       day, which pushes 3 in modes 0 and 1. In mode 2 the part has to be developed and fitted first
//                       and Car_Upgrades_FitUpgradeState (0x143517c50) pushes 3. The game's own counts (the R&D tiles'
//                       "fitted", the AI's planner Car_Fit_UpgradesToRating 0x143514670) take 3 as the finished state
//   4 Failed            the same routine when the roll said fail (modes 0 and 1), in mode 2 also a development that failed
//                       (daily development tick 0x1435314d0). It can be started again and the restart cannot fail (both
//                       start routines skip the roll when the last state is 4)
//   5 Invalid           the placeholder entry {owner 0x3fff0000, day 0xffff} of an instance that was never set (every
//                       department 4 node); the first push overwrites it, it is no real state
//   6 PendingDevelopment  mode 2 only: research finished, waits for its development (0x143523a40 pushes 6 instead of 3)
//   7 InDevelopment     Career_Start_UpgradeDevelopment (0x1435173e0), develop_end_day is +0x9a
//   8 PendingFitting    mode 2 only: development finished (0x1435314d0), waits to be fitted
// Only 3 is a finished upgrade: a failed research (4) and the stages of mode 2 (6-8) are not in effect yet. In the mode 0
// Career of the tests only 0 to 3 were seen so far (day 27); 4 is expected as the AI teams' researches end.
inline bool rnd_state_done(int state) { return state == 3; }

// The game's name of a state (for the log), "?" for a value it does not use.
inline const char *rnd_state_name(int state)
{
    static constexpr const char *names[9] = {"Unavailable", "Available", "InProgress",    "Completed",      "Failed",
                                             "Invalid",     "PendingDevelopment", "InDevelopment", "PendingFitting"};
    return state >= 0 && state < 9 ? names[state] : "?";
}

// One entry of an upgrade's state history (UpgradeStateEntry: the state at +8, the day it began at +4).
struct RndEntry {
    int state = 0;
    int day = 0;
};

// Whether a history holds a state beyond the usual 0 or 1, 2, 3: a Failed research (and the restart after it), the
// Invalid placeholder or a stage of upgrade mode 2. Those are the histories worth printing.
inline bool rnd_history_notable(const std::vector<RndEntry> &h)
{
    for (const RndEntry &e : h)
        if (e.state < 0 || e.state > 3)
            return true;
    return false;
}

// "1@0 2@30 4@50 2@55": every state with the day it began.
inline std::string rnd_history_text(const std::vector<RndEntry> &h)
{
    std::string out;
    for (const RndEntry &e : h) {
        if (!out.empty())
            out += ' ';
        out += std::to_string(e.state) + "@" + std::to_string(e.day);
    }
    return out;
}

}  // namespace fm
