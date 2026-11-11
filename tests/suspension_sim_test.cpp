// Offline simulator for the suspension model v2 (0.76): race scenarios run through the pure model (apply_hit,
// heavy_load) for each strength, many times with fresh parts and random kerb strikes, so the thresholds can be tuned
// without the game. It prints the outcome table into the test log and checks what the model promises.
//
// A 50-lap race. The heavy loads a lap (a kerb strike or landing past heavy_rate), from the 2026-11-10 kerb test
// (travel rate peaks: clean laps 0.25-0.76, laps attacking every kerb 1.0-1.64): 0 for a clean driver, 1 for a normal
// one (q 1.0-1.3), 3 for one who attacks the kerbs (q 1.0-1.65), each on a random corner (approximation: the counts
// are from 30 s peaks, not every load). The incidents come on lap 1, all on the front-left.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include <cstdio>
#include "test_util.hpp"

namespace {
std::mt19937_64 rng{20261110};
float u() { return float(rng() >> 40) / float(1u << 24); }

struct Hit {
    float v;
    bool surface;
};
struct Scenario {
    const char *name;
    std::vector<Hit> hits;
    int kerbs;  // heavy loads a lap
    float q_hi;
};
struct Outcome {
    double intact = 0, collapsed = 0, off = 0, median_lap = -1;  // shares of the runs; the median collapse lap
};

constexpr int laps = 50, runs = 4000;

Outcome run(const Scenario &s, float brk)
{
    Outcome o;
    std::vector<int> when;
    for (int k = 0; k < runs; ++k) {
        float r[4] = {u(), u(), u(), u()};
        susp::Model m = susp::fresh_model(brk, r);
        int lap_down = -1;
        for (const Hit &h : s.hits)
            susp::apply_hit(m, 2, h.v, h.surface, susp::Rolls{u(), u()});
        if (m.c[2].collapsed)
            lap_down = 1;
        for (int lap = 1; lap <= laps && lap_down < 0; ++lap)
            for (int j = 0; j < s.kerbs && lap_down < 0; ++j) {
                float q = 1.0f + (s.q_hi - 1.0f) * u();
                int i = int(u() * 4.0f) & 3;
                if (susp::heavy_load(m, i, q) == susp::Load::collapse)
                    lap_down = lap;
            }
        bool off = false, collapsed = false;
        for (const susp::Corner &c : m.c) {
            off |= c.off;
            collapsed |= c.collapsed;
        }
        o.off += off;
        o.collapsed += collapsed && !off;
        o.intact += !collapsed;
        if (lap_down > 0)
            when.push_back(lap_down);
    }
    o.intact /= runs;
    o.collapsed /= runs;
    o.off /= runs;
    if (!when.empty()) {
        std::sort(when.begin(), when.end());
        o.median_lap = when[when.size() / 2];
    }
    return o;
}
}  // namespace

int main()
{
    SwoftyApi api{};
    api.log = tu::quiet_log;
    swofty::api = &api;
    using namespace susp;

    const std::vector<Scenario> scenarios = {
        {"brushes: 10 wall brushes 1.5-3 m/s, normal kerbs", {{1.6f, false}, {2.0f, false}, {2.4f, false}, {3.0f, false},
          {1.8f, false}, {2.2f, false}, {2.6f, false}, {1.5f, false}, {2.0f, false}, {2.8f, false}}, 1, 1.3f},
        {"clip: 4 m/s, normal kerbs", {{4.0f, false}}, 1, 1.3f},
        {"wall: 6 m/s, clean", {{6.0f, false}}, 0, 1.0f},
        {"wall: 6 m/s, normal kerbs", {{6.0f, false}}, 1, 1.3f},
        {"wall: 6 m/s, attacks the kerbs", {{6.0f, false}}, 3, 1.65f},
        {"two hits: 5 + 4.5 m/s", {{5.0f, false}, {4.5f, false}}, 1, 1.3f},
        {"kerb faces: 5 x 8 m/s", {{8.0f, true}, {8.0f, true}, {8.0f, true}, {8.0f, true}, {8.0f, true}}, 1, 1.3f},
        {"crash: 9 m/s", {{9.0f, false}}, 1, 1.3f},
        {"big crash: 14 m/s", {{14.0f, false}}, 1, 1.3f},
    };
    const float strengths[3] = {strength_forgiving, strength_realistic, strength_brutal};
    const char *names[3] = {"Forgiving", "Realistic", "Brutal"};
    Outcome out[16][3];
    std::printf("suspension model v2: %d runs of a %d-lap race per cell; intact / collapsed (limps) / wheel off, "
                "median collapse lap\n", runs, laps);
    for (size_t s = 0; s < scenarios.size(); ++s) {
        std::printf("%-50s", scenarios[s].name);
        for (int k = 0; k < 3; ++k) {
            out[s][k] = run(scenarios[s], strengths[k]);
            const Outcome &o = out[s][k];
            std::printf(" | %s %3.0f/%3.0f/%3.0f %%", names[k], o.intact * 100, o.collapsed * 100, o.off * 100);
            if (o.median_lap > 0)
                std::printf(" lap %2.0f", o.median_lap);
            else
                std::printf("       ");
        }
        std::printf("\n");
    }
    std::fflush(stdout);

    // What the model promises, on Realistic (index 1).
    assert(out[0][1].intact == 1.0);                                         // brushes bend, never break
    assert(out[1][1].intact == 1.0);                                         // a clip never breaks
    assert(out[2][1].intact > 0.95);  // a 6 m/s hit, driven clean: holds (a weak part can give)
    // Weakened by it: normal kerbs wear it over the race, attacking them breaks it much sooner, never at once.
    assert(out[3][1].median_lap > 8 && out[4][1].median_lap > 1 && out[4][1].median_lap < out[3][1].median_lap);
    assert(out[6][1].intact == 1.0 || out[6][1].off == 0.0);                 // kerb faces never tear a wheel off
    assert(out[7][1].collapsed == 1.0 && out[7][1].median_lap == 1);         // 9 m/s: collapses, limps
    assert(out[8][1].off == 1.0);                                            // 14 m/s: the wheel comes off
    // The strengths keep their order everywhere.
    for (size_t s = 0; s < scenarios.size(); ++s)
        assert(out[s][0].intact + 0.02 >= out[s][1].intact && out[s][1].intact + 0.02 >= out[s][2].intact);
    assert(out[7][0].collapsed < 1.0);  // Forgiving: a 9 m/s hit can hold
    assert(out[5][2].intact < out[5][1].intact);
    return 0;
}
