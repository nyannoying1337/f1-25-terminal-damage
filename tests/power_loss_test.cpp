// Power loss over time (power_loss.hpp): each course against the real cases in re/failure-realism-research.md.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/failure_model.hpp"
#include <cassert>
#include <cstdio>
#include <random>

namespace {
std::mt19937_64 rng{12345};
double u() { return double(rng() >> 11) * (1.0 / 9007199254740992.0); }
struct Fixed {
    double v;
    double operator()() const { return v; }
};
// km/h lost at the end of a 320 km/h straight for a power share p (drag-limited: power^(1/3)).
double kmh_lost(double p, double top = 320.0) { return top * (1.0 - pl::top_speed_share(p)); }
// Run a course for `seconds` in steps of dt and return the lowest and the mean power at speed v.
struct Run {
    float lo = 1, hi = 0;
    double mean = 0;
    int dips = 0;
};
Run run(pl::State &s, double seconds, float v, double dt = 0.05)
{
    Run r;
    int n = 0;
    bool in_dip = false;
    for (double t = 0; t < seconds; t += dt) {
        pl::advance(s, dt, u);
        float p = pl::power(s, v);
        r.lo = std::min(r.lo, p);
        r.hi = std::max(r.hi, p);
        r.mean += p;
        ++n;
        bool dip = s.t < s.cut_end;
        r.dips += dip && !in_dip;
        in_dip = dip;
    }
    r.mean /= std::max(1, n);
    return r;
}
}  // namespace

int main()
{
    // ---- MGU-K lost: 0.78-0.84, -15 to -26 km/h at 320 km/h; half effect first in about half the cases ----
    int half = 0, intermittent = 0;
    for (int i = 0; i < 2000; ++i) {
        pl::State s = pl::start(pl::mguk_loss, 0, u);
        assert(s.depth >= 0.16f && s.depth <= 0.22f);
        double lost = kmh_lost(1.0 - s.depth);
        assert(lost >= 15.0 && lost <= 26.5);
        half += s.build_s > 0;
        intermittent += s.intermittent;
        if (!s.intermittent) {
            pl::State c = s;
            if (c.build_s > 0) {
                assert(std::fabs(pl::power(c, 300) - (1.0f - c.depth / 2)) < 1e-6f);
                run(c, c.build_s + 1, 300);
            }
            assert(std::fabs(pl::power(c, 300) - (1.0f - c.depth)) < 1e-6f);
            assert(std::fabs(pl::power(c, 80) - (1.0f - c.depth)) < 1e-6f);  // at every speed
        }
    }
    assert(half > 900 && half < 1100);
    assert(intermittent > 200 && intermittent < 400);  // about 15 %
    {
        // Intermittent: full power most of the time, the loss for a few seconds now and then.
        pl::State s = pl::start(pl::mguk_loss, 0, Fixed{0.1});
        assert(s.intermittent && !s.phase_on);
        Run r = run(s, 600, 300);
        assert(r.hi == 1.0f && r.lo < 0.85f && r.mean > 0.9);
        // Quick (test run): always the half-effect lap of 10 s, never intermittent.
        pl::State q = pl::start(pl::mguk_loss, 38, Fixed{0.1}, false, true);
        assert(q.build_s == 10 && !q.intermittent);
        // A warning gives at most a third of its lead to the half effect.
        pl::State w = pl::start(pl::mguk_loss, 120, Fixed{0.1}, true);
        assert(w.build_s == 40 && !w.intermittent);
    }

    // ---- Battery derate: only on the straights, builds over 2-10 laps, worst to about lap 15, gone by lap 40 ----
    for (int i = 0; i < 200; ++i) {
        pl::State s = pl::start(pl::battery_derate, 2400 + 1200 * u(), u);
        assert(s.depth >= 0.10f && s.depth <= 0.16f);
        assert(s.build_s >= 180 && s.build_s <= 900 && s.hold_s >= 1350);
        // -11 to -18 km/h at the top of a 320 km/h straight (Bahrain 2024: -11 at the speed trap).
        double lost = kmh_lost(1.0 - s.depth);
        assert(lost >= 10.5 && lost <= 18.5);
        s.t = s.build_s + 1;
        assert(pl::power(s, 200) == 1.0f && pl::power(s, 230) == 1.0f);
        assert(std::fabs(pl::power(s, 300) - (1.0f - s.depth)) < 1e-6f);
        assert(pl::power(s, 245) > pl::power(s, 300) && pl::power(s, 245) < 1.0f);
        s.t = s.build_s / 2;
        assert(std::fabs(pl::steady_loss(s) - s.depth / 2) < 1e-4f);
        s.t = (s.hold_s + s.length) / 2;
        assert(std::fabs(pl::steady_loss(s) - s.depth / 2) < 1e-3f);
        s.t = s.length;
        assert(pl::steady_loss(s) == 0.0f);
    }
    {
        // As the warning of a battery failure: worse and worse up to the failure.
        pl::State w = pl::start(pl::battery_derate, 300, u, true);
        w.t = 150;
        assert(std::fabs(pl::steady_loss(w) - w.depth / 2) < 1e-4f);
        w.t = 299.9;
        assert(pl::steady_loss(w) > w.depth * 0.99f);
    }

    // ---- Misfire: about 80 hp, -4 to -11 km/h, rough dips every 8-25 s ----
    for (int i = 0; i < 200; ++i) {
        pl::State s = pl::start(pl::misfire, 900 + 900 * u(), u);
        double lost = kmh_lost(1.0 - s.depth);
        assert(lost >= 4.0 && lost <= 11.5);
        Run r = run(s, 300, 300);
        assert(r.dips >= 300 / 25 - 1 && r.dips <= 300 / 8 + 1);
        assert(r.lo >= 1.0f - s.depth - pl::rough_drop - 1e-6f && r.hi == 1.0f - s.depth);
    }

    // ---- Dead cylinder: the loss grows over 2-3 laps, then -17 to -23 km/h (Bahrain 2019: "25 kph") ----
    for (int i = 0; i < 200; ++i) {
        pl::State s = pl::start(pl::dead_cylinder, 900, u, true);
        assert(s.build_s >= 180 && s.build_s <= 270);
        double lost = kmh_lost(1.0 - s.depth);
        assert(lost >= 16.0 && lost <= 24.0);
        s.t = s.build_s / 3;
        assert(pl::steady_loss(s) < s.depth / 2);
        s.t = s.build_s;
        assert(std::fabs(pl::steady_loss(s) - s.depth) < 1e-6f);
        // A short lead: the ramp takes at most half of it.
        pl::State q = pl::start(pl::dead_cylinder, 200, u, true);
        assert(q.build_s <= 100);
    }

    // ---- Leak: nothing in the first half, a turned-down engine, cut-outs in the last fifth ----
    {
        pl::State s = pl::start(pl::leak, 500, u, true);
        Run a = run(s, 249.5, 300);
        assert(a.lo == 1.0f && a.dips == 0);
        run(s, 1, 300);
        Run b = run(s, 149, 300);
        assert(b.hi == 1.0f - s.depth && b.dips == 0 && s.depth >= 0.06f && s.depth <= 0.10f);
        Run c = run(s, 100, 300);
        assert(c.lo == pl::cut_power && c.dips >= 3);  // 100 s left: a cut every 12-26 s at first
        assert(pl::cutting(s));
    }

    // ---- Turbo: worse and worse to 0.70-0.80, surges in the last 30 %; MGU-H 0.90 from half the lead ----
    {
        pl::State s = pl::start(pl::turbo, 60, u, true);
        Run a = run(s, 41, 300);
        assert(a.dips == 0 && a.lo < 1.0f);
        s.t = 59.9;
        assert(pl::steady_loss(s) >= 0.199f && pl::steady_loss(s) <= 0.30f);
        pl::State h = pl::start(pl::mguh, 40, u, true);
        h.t = 10;
        assert(std::fabs(pl::steady_loss(h) - 0.05f) < 1e-5f);
        h.t = 25;
        assert(std::fabs(pl::steady_loss(h) - 0.10f) < 1e-5f);
        pl::State e = pl::start(pl::electrical, 20, u, true);
        Run r = run(e, 20, 300);
        assert(r.dips >= 2 && r.lo == pl::cut_power && pl::steady_loss(e) == 0.0f);
    }

    // ---- The failure model's kinds and faults ----
    assert(fm::warning_kind(fm::ice, 0.1) == pl::leak && fm::warning_kind(fm::ice, 0.9) == pl::dead_cylinder);
    assert(fm::warning_kind(fm::mguk, 0.5) == pl::mguk_loss && fm::warning_kind(fm::es, 0.5) == pl::battery_derate);
    assert(fm::warning_kind(fm::tc, 0.5) == pl::turbo && fm::warning_kind(fm::mguh, 0.5) == pl::mguh);
    assert(fm::warning_kind(fm::ce, 0.5) == pl::electrical);
    for (int o : {int(fm::gearbox), int(fm::brakes), int(fm::suspension)})
        assert(fm::warning_kind(o, 0.5) == pl::none);
    assert(fm::power_kind_of_fault(fm::f_mguk_loss) == pl::mguk_loss &&
           fm::power_kind_of_fault(fm::f_battery_derate) == pl::battery_derate &&
           fm::power_kind_of_fault(fm::f_misfire) == pl::misfire && fm::power_kind_of_fault(fm::f_drs) == pl::none);
    assert(std::fabs(fm::brake_fade(0.5) - 0.225) < 1e-12 && fm::brake_fade(2) == fm::brake_fade_max);
    // Hybrid eras: MGU-K loss 20 %, battery derate 8 %, misfire 7 %; the game's ERS faults 5 % each.
    const fm::FaultEra &fe = fm::fault_eras[3];
    assert(fe.share[fm::f_mguk_loss] == 200 && fe.share[fm::f_battery_derate] == 80 && fe.share[fm::f_misfire] == 70 &&
           fe.share[fm::f_ers_modes] == 50 && fe.share[fm::f_ers_glitch] == 50);
    assert(fm::fault_eras[0].share[fm::f_mguk_loss] == 0 && fm::fault_eras[0].share[fm::f_misfire] == 150);

    std::puts("power loss ok");
}
