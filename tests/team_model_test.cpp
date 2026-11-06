// Standalone test of the per-team failure rates (team_model.hpp and the data in team_rates.hpp). No game, no Swofty.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/team_model.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "test_util.hpp"

namespace {
using tu::close_to;

struct Total {
    int year;
    double starts, mech;
};
// Starts and mechanical retirements per season (re/out/failure-stats.json for 2012-2022; 2023-25 pooled from
// re/failure-statistics-research.md; 2026 rounds 1-19), the totals the team rows must add up to.
constexpr Total totals[] = {
    {2012, 477, 34}, {2013, 417, 31}, {2014, 405, 51}, {2015, 373, 40}, {2016, 460, 42}, {2017, 399, 60},
    {2018, 420, 45}, {2019, 419, 28}, {2020, 339, 28}, {2021, 439, 22}, {2022, 439, 41}, {2023, 1387, 56},
    {2024, 1387, 56}, {2025, 1387, 56}, {2026, 411, 53},
};

std::string year_id(int year) { return std::to_string(year); }
}  // namespace

int main()
{
    // ---- The team rows add up to every season's totals, and to the season's race_chance ----
    for (const Total &t : totals) {
        const fm::Season &s = fm::season_of(year_id(t.year));
        assert(fm::season_year(s) == t.year);
        const tr::Table &table = fm::team_table(s);
        double starts = 0, mech = 0;
        for (int i = 0; i < table.n; ++i) {
            starts += table.rows[i].starts;
            mech += table.rows[i].mech;
        }
        assert(close_to(starts, t.starts, 1e-9) && close_to(mech, t.mech, 1e-9));
        if (t.year < 2023 || t.year == 2026)  // 2023-25 are one pooled table, the seasons' own rates differ
            assert(close_to(mech / starts, s.race_chance, 0.0006));
        assert(table.prior >= 10.0 && table.prior <= 400.0);

        // The weights average to 1 over the real grid (weighted by starts): the season's overall rate is kept.
        double weighted = 0;
        for (int i = 0; i < table.n; ++i) {
            double w = fm::family_weight(s, table.rows[i].family);
            assert(w > 0);
            weighted += table.rows[i].starts * w;
        }
        assert(close_to(weighted / starts, 1.0, 1e-9));

        // The real grid, two cars a team: the factors average to 1, so a race has the season's expected failures.
        std::vector<std::string> ids;
        for (int i = 0; i < table.n; ++i) {
            ids.push_back(std::string("game/") + table.rows[i].family);
            ids.push_back(std::string("game/") + table.rows[i].family);
        }
        int matched = 0;
        std::vector<double> f = fm::grid_factors(s, ids, &matched);
        assert(matched == int(ids.size()) && f.size() == ids.size());
        double sum = 0;
        for (double x : f)
            sum += x;
        assert(close_to(sum, double(ids.size()), 1e-9));
        double expected = 0;
        for (double x : f)
            expected += std::fmin(0.95, s.race_chance * x);
        assert(close_to(expected, s.race_chance * double(ids.size()), 1e-9));
    }

    // ---- 2026: the spread between the teams ----
    {
        const fm::Season &s = fm::season_of("2026");
        double aston = fm::family_weight(s, "aston"), cadillac = fm::family_weight(s, "cadillac"),
               audi = fm::family_weight(s, "audi"), redbull = fm::family_weight(s, "redbull"),
               mercedes = fm::family_weight(s, "mercedes");
        assert(aston > cadillac && cadillac > audi && audi > mercedes && mercedes > redbull);
        assert(cadillac > 1.9 && cadillac < 2.4);  // about 1 race in 3.6 a car, not every race
        assert(s.race_chance * cadillac > 0.25 && s.race_chance * cadillac < 0.31);
        assert(s.race_chance * aston < 0.45);
        // A family the season does not have: none.
        assert(fm::family_weight(s, "lotus") < 0);
        assert(fm::family_weight(fm::season_of("2018"), "cadillac") < 0);
    }

    // ---- A grid without Cadillac (the stock 10 teams) or with a team nobody knows ----
    {
        const fm::Season &s = fm::season_of("2026");
        std::vector<std::string> full, stock, odd;
        for (const char *t : {"mercedes", "ferrari", "redbull", "mclaren", "williams", "aston", "alpine", "rb", "audi",
                              "haas", "cadillac"}) {
            full.push_back(std::string("game/") + t);
            full.push_back(std::string("game/") + t);
            if (std::strcmp(t, "cadillac")) {
                stock.push_back(std::string("game/") + t);
                stock.push_back(std::string("game/") + t);
            }
        }
        std::vector<double> a = fm::grid_factors(s, full), b = fm::grid_factors(s, stock);
        // Without Cadillac everybody else is rated up a little, so the grid still adds up.
        double sum = 0;
        for (double x : b)
            sum += x;
        assert(close_to(sum, double(b.size()), 1e-9));
        assert(b[0] > a[0]);  // Mercedes
        // An unknown team counts as an average car before the scaling, and is told apart from the known ones.
        odd = stock;
        odd.push_back("mods/zeta_racing");
        int matched = 0;
        std::vector<double> c = fm::grid_factors(s, odd, &matched);
        assert(matched == int(stock.size()) && c.size() == odd.size());
        sum = 0;
        for (double x : c)
            sum += x;
        assert(close_to(sum, double(c.size()), 1e-9));
        assert(c.back() > b[0]);  // unknown (1.0) beats the 2026 Mercedes (a good team)
        // No cars, no factors; nothing known gives 1 for everybody.
        assert(fm::grid_factors(s, {}).empty());
        for (double x : fm::grid_factors(s, {"x", "y", "z"}))
            assert(close_to(x, 1.0, 1e-12));
    }

    // ---- Seasons where the teams hardly differ are nearly flat; a bad year has real differences ----
    {
        double lo = 9, hi = 0;
        for (const char *t : {"williams", "aston", "mercedes", "haas", "alpine", "sauber", "ferrari", "rb", "redbull", "mclaren"}) {
            double w = fm::family_weight(fm::season_of("2025"), t);
            assert(w > 0);
            lo = std::fmin(lo, w);
            hi = std::fmax(hi, w);
        }
        assert(lo > 0.8 && hi < 1.2);
        double lo17 = 9, hi17 = 0;
        for (const char *t : {"mclaren", "renault", "redbull", "rb", "haas", "williams", "sauber", "ferrari", "aston", "mercedes"}) {
            double w = fm::family_weight(fm::season_of("2017"), t);
            assert(w > 0);
            lo17 = std::fmin(lo17, w);
            hi17 = std::fmax(hi17, w);
        }
        assert(hi17 > 1.5 && lo17 < 0.6);  // McLaren-Honda and Renault 2017 against Mercedes
    }

    // ---- Matching a game team id or name to a family ----
    {
        struct Case {
            const char *id;
            const char *family;
        };
        const Case cases[] = {
            {"game/ferrari", "ferrari"},           {"Scuderia Ferrari HP", "ferrari"},
            {"Cadillac F1 Team", "cadillac"},      {"mods/cadillac_2026", "cadillac"},
            {"game/racing_point", "aston"},        {"Aston Martin Aramco", "aston"},
            {"game/force_india", "aston"},         {"RB F1 Team", "rb"},
            {"Visa Cash App RB", "rb"},            {"Scuderia AlphaTauri", "rb"},
            {"game/toro_rosso", "rb"},             {"game/redbull", "redbull"},
            {"Oracle Red Bull Racing", "redbull"}, {"Kick Sauber", "sauber"},
            {"Alfa Romeo Racing", "sauber"},       {"Audi F1 Team", "audi"},
            {"MoneyGram Haas F1 Team", "haas"},    {"Mercedes-AMG Petronas", "mercedes"},
            {"McLaren Formula 1 Team", "mclaren"}, {"Williams Racing", "williams"},
            {"BWT Alpine F1 Team", "alpine"},      {"Renault Sport", "renault"},
            {"Lotus F1 Team", "lotus"},            {"Manor Marussia", "manor"},
            {"HRT", "hrt"},                        {"Caterham", "caterham"},
        };
        for (const Case &c : cases) {
            const char *f = fm::family_of(c.id);
            assert(f && !std::strcmp(f, c.family));
        }
        // A short word has to be a whole word: nothing here is an RB or an HRT.
        for (const char *id : {"Barbados GP", "mods/zeta_racing", "game/safety_car", "Thrt Racing", "", "game/"})
            assert(!fm::family_of(id));
    }

    // ---- Team slots: the game's team list rows (the 2026 Career test: vehicles 0-1 McLaren, 2-3 Mercedes, 4-5 Red Bull ...) ----
    {
        const fm::Season &s26 = fm::season_of("2026"), &s25 = fm::season_of("2025"), &s18 = fm::season_of("2018"),
                         &s13 = fm::season_of("2013"), &s22 = fm::season_of("2022");
        auto fam = [](const fm::Season &s, const char *id) {
            const char *f = fm::family_for(s, id);
            return std::string(f ? f : "(none)");
        };
        assert(fam(s26, "slot:8") == "mclaren" && fam(s26, "slot:0") == "mercedes" && fam(s26, "slot:2") == "redbull");
        assert(fam(s26, "slot:9") == "audi" && fam(s25, "slot:9") == "sauber" && fam(s26, "slot:10") == "cadillac");
        assert(fam(s26, "slot:5") == "alpine" && fam(s22, "slot:5") == "alpine" && fam(s18, "slot:5") == "renault" &&
               fam(s13, "slot:5") == "lotus");
        assert(fam(s26, "slot:4") == "aston" && fam(s26, "slot:6") == "rb" && fam(s26, "slot:7") == "haas");
        assert(fam(s26, "slot:11") == "(none)" && fam(s26, "slot:-1") == "(none)" && fam(s26, "slot:x") == "(none)");
        assert(fam(s26, "game/ferrari") == "ferrari");  // a registry id still works
        // The grid of that test: 22 cars, two a team, the rows in the order of the 2025 standings. Cadillac is slot 10.
        const int rows[22] = {8, 8, 0, 0, 2, 2, 1, 1, 3, 3, 6, 6, 4, 4, 7, 7, 9, 9, 5, 5, 10, 10};
        std::vector<std::string> ids;
        for (int r : rows)
            ids.push_back("slot:" + std::to_string(r));
        int matched = 0;
        std::vector<double> f = fm::grid_factors(s26, ids, &matched);
        assert(matched == 22 && f.size() == 22);
        double sum = 0;
        for (double x : f)
            sum += x;
        assert(close_to(sum, 22.0, 1e-9));
        assert(f[20] > 1.9 && f[20] < 2.4);               // Cadillac
        assert(f[12] > f[20] && f[12] > 2.5);             // Aston Martin is the worst
        assert(f[0] < 0.7 && f[4] < f[0] && f[2] < 0.75);  // McLaren, Red Bull and Mercedes are the best
        assert(close_to(f[0], f[1], 1e-12) && close_to(f[20], f[21], 1e-12));
        // The same grid by registry-style ids gives the same factors.
        const char *names[11] = {"game/mclaren", "game/mercedes", "game/redbull", "game/ferrari", "game/williams",
                                 "game/tororosso", "game/forceindia", "game/haas", "game/sauber", "game/renault", "game/cadillac"};
        std::vector<std::string> by_name;
        for (const char *nm : names) {
            by_name.push_back(nm);
            by_name.push_back(nm);
        }
        std::vector<double> g = fm::grid_factors(s26, by_name);
        for (int i = 0; i < 22; ++i)
            assert(close_to(f[i], g[i], 1e-9));
    }

    // ---- R&D durability: a team's finished upgrades beyond the start state take up to half of its rate off ----
    {
        assert(close_to(fm::rnd_factor(0.0), 1.0, 1e-12) && close_to(fm::rnd_factor(1.0), 0.5, 1e-12) &&
               close_to(fm::rnd_factor(0.25), 0.875, 1e-12));
        assert(close_to(fm::rnd_factor(-1.0), 1.0, 1e-12) && close_to(fm::rnd_factor(9.0), 0.5, 1e-12));  // clamped
        // Progress beyond the start state (5 of the 52 Durability upgrades are done in every new Career): 1 % a node.
        assert(close_to(fm::rnd_progress(5, 52), 0.0, 1e-12) && close_to(fm::rnd_progress(52, 52), 1.0, 1e-12) &&
               close_to(fm::rnd_progress(0, 52), 0.0, 1e-12) && close_to(fm::rnd_progress(28, 52), 23.0 / 47.0, 1e-12) &&
               close_to(fm::rnd_progress(5, 0), 0.0, 1e-12) && close_to(fm::rnd_progress(5, 5), 0.0, 1e-12) &&
               close_to(fm::rnd_progress(99, 52), 1.0, 1e-12));
        assert(close_to(fm::rnd_factor(fm::rnd_progress(6, 52)), 1.0 - 0.5 / 47.0, 1e-12));
        // Only Completed (3) is a finished upgrade: not Failed (4), the Invalid placeholder (5) nor the mode 2 stages 6-8.
        for (int s = -1; s <= 9; ++s)
            assert(fm::rnd_state_done(s) == (s == 3));
        assert(!fm::rnd_state_done(255));
        assert(std::strcmp(fm::rnd_state_name(0), "Unavailable") == 0 && std::strcmp(fm::rnd_state_name(3), "Completed") == 0 &&
               std::strcmp(fm::rnd_state_name(4), "Failed") == 0 && std::strcmp(fm::rnd_state_name(5), "Invalid") == 0 &&
               std::strcmp(fm::rnd_state_name(6), "PendingDevelopment") == 0 &&
               std::strcmp(fm::rnd_state_name(8), "PendingFitting") == 0 && std::strcmp(fm::rnd_state_name(9), "?") == 0 &&
               std::strcmp(fm::rnd_state_name(-1), "?") == 0);
        // The history of a node: only one with a state beyond 0/1, 2, 3 is notable, printed as state@day.
        {
            std::vector<fm::RndEntry> usual = {{0, 0}, {2, 27}, {3, 27}};
            std::vector<fm::RndEntry> failed_then_restarted = {{1, 0}, {2, 30}, {4, 50}, {2, 55}};
            assert(!fm::rnd_history_notable(usual) && fm::rnd_history_notable(failed_then_restarted));
            assert(fm::rnd_history_text(failed_then_restarted) == "1@0 2@30 4@50 2@55" && fm::rnd_history_text({}).empty());
            assert(fm::rnd_history_notable({{0, 0}, {5, 65535}}) && fm::rnd_history_notable({{2, 1}, {7, 2}}));
            assert(!fm::rnd_history_notable({}) && !fm::rnd_history_notable({{1, 0}, {2, 3}}));
        }
        const fm::Season &s = fm::season_of("2026");
        std::vector<std::string> ids;
        for (int r : {8, 8, 0, 0, 2, 2, 1, 1})
            ids.push_back("slot:" + std::to_string(r));
        std::vector<double> plain = fm::grid_factors(s, ids);
        std::vector<double> extra(ids.size(), 1.0);
        extra[0] = extra[1] = fm::rnd_factor(1.0);  // McLaren has every Durability upgrade
        std::vector<double> with = fm::grid_factors(s, ids, nullptr, &extra);
        // McLaren fails half as often as before; the others are neither made worse nor better.
        assert(close_to(with[0], 0.5 * plain[0], 1e-12) && close_to(with[1], 0.5 * plain[1], 1e-12));
        for (size_t i = 2; i < ids.size(); ++i)
            assert(close_to(with[i], plain[i], 1e-12));
        // The whole grid researches (the AI teams do): everybody fails less, nothing is shared out.
        std::vector<double> all(ids.size(), fm::rnd_factor(0.5));
        std::vector<double> every = fm::grid_factors(s, ids, nullptr, &all);
        for (size_t i = 0; i < ids.size(); ++i)
            assert(close_to(every[i], 0.75 * plain[i], 1e-12));
        std::vector<double> flat(ids.size(), 1.0);
        std::vector<double> same = fm::grid_factors(s, ids, nullptr, &flat);
        for (size_t i = 0; i < ids.size(); ++i)
            assert(close_to(same[i], plain[i], 1e-12));
    }

    // ---- A season outside the table uses the nearest one ----
    {
        fm::Season s{"2040", 4, 0.129};
        assert(fm::season_year(s) == 2040);
        assert(fm::family_weight(s, "cadillac") > 1.9);
    }
    std::puts("team model ok");
}
