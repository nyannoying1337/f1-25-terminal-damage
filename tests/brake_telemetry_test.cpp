// SPDX-License-Identifier: MIT
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/brake_telemetry.hpp"

#include <cassert>
#include <cstring>
#include <cstdio>

namespace {

bool close_to(double a, double b, double eps) { return a > b - eps && a < b + eps; }

// Brake from v0 to v1 at decel m/s^2 in 10 ms steps; rear wheels get rear_share of the torque, a rear wheel locks
// (rim speed 0) when lock_rear, the car slides sideways by lat m/s.
int brake(btel::tracker &t, float v0, float v1, float decel, float rear_share, bool lock_rear, float lat = 0)
{
    int events = 0;
    btel::sample s;
    s.dt = 0.01;
    for (float v = v0; v > v1; v -= decel * 0.01f) {
        s.v_long = v;
        s.v_lat = lat;
        for (int w = 0; w < 4; ++w) {
            s.rim[w] = (lock_rear && w < 2) ? 0.0f : v;
            s.torque[w] = w < 2 ? 1000 * rear_share : 1000 * (1 - rear_share);
        }
        events |= t.feed(s);
    }
    // brakes off, rolling on
    for (int i = 0; i < 50; ++i) {
        for (int w = 0; w < 4; ++w) {
            s.rim[w] = v1;
            s.torque[w] = 0;
        }
        s.v_long = v1;
        s.v_lat = 0;
        events |= t.feed(s);
    }
    return events;
}

}

int main()
{
    using namespace btel;
    tracker t;
    // Cruise: no zone.
    sample cruise;
    cruise.dt = 0.01;
    cruise.v_long = 80;
    for (float &r : cruise.rim)
        r = 80;
    for (int i = 0; i < 100; ++i)
        assert(t.feed(cruise) == tracker::none);

    // A clean stop from 288 to 90 km/h at 4 g, 45 % rear.
    int ev = brake(t, 80, 25, 4 * g, 0.45f, false);
    assert(ev & tracker::zone_done);
    assert(close_to(t.z.v_in, 80, 0.5) && t.z.v_min < 25.5f);
    assert(close_to(t.z.peak_decel / g, 4, 0.3));
    assert(close_to(t.z.rear_share(), 0.45, 0.01));
    assert(!t.z.locked(0) && !t.z.locked(2));
    assert(t.st.zones == 1 && t.st.locks_rear == 0 && t.st.slides == 0);

    // Rear brakes cut to 22 %, the rears lock and the car slides 20 deg.
    ev = brake(t, 80, 25, 3 * g, 0.22f, true, 12);
    assert(ev & tracker::zone_done);
    assert(ev & tracker::slide_started);
    assert(close_to(t.z.rear_share(), 0.22, 0.01));
    assert(t.z.locked(0) && t.z.locked(1) && !t.z.locked(2));
    assert(t.z.max_slip_deg > 8);
    assert(t.st.zones == 2 && t.st.locks_rear == 1 && t.st.locks_front == 0 && t.st.slides == 1);

    // Slow corner below 60 km/h: no zone.
    ev = brake(t, 15, 8, 2 * g, 0.45f, false);
    assert(!(ev & tracker::zone_done) && t.st.zones == 2);

    // Power check: full throttle on a straight from 200 to 330 km/h at 6 m/s^2 counts in every band; part throttle,
    // a corner (yaw) and braking do not.
    {
        tracker p;
        sample a;
        a.dt = 0.01;
        a.throttle = 1;
        for (float v = 200 / 3.6f; v < 330 / 3.6f; v += 6 * 0.01f) {
            a.v_long = v;
            for (float &r : a.rim)
                r = v;
            p.feed(a);
        }
        for (int b = 0; b < n_power_bands; ++b)
            assert(p.st.power.has(b) && close_to(p.st.power.accel(b), 6, 0.05));
        assert(close_to(p.st.power.t[0], (240 - 200) / 3.6 / 6, 0.05));
        tracker q;
        sample c = a;
        c.throttle = 0.6f;  // part throttle
        for (float v = 220 / 3.6f; v < 260 / 3.6f; v += 0.03f) {
            c.v_long = v;
            q.feed(c);
        }
        c.throttle = 1;
        c.yaw_rate = 0.3f;  // a fast corner
        for (float v = 220 / 3.6f; v < 260 / 3.6f; v += 0.03f) {
            c.v_long = v;
            q.feed(c);
        }
        for (int b = 0; b < n_power_bands; ++b)
            assert(!q.st.power.has(b));
        power_bands sum;
        sum.add(p.st.power);
        sum.add(p.st.power);
        assert(close_to(sum.accel(1), 6, 0.05) && close_to(sum.t[1], 2 * p.st.power.t[1], 1e-9));

        // The band text: a band without enough time is "-", the change against a reference where both have one.
        power_bands slow = p.st.power, ref = p.st.power;
        for (int k = 0; k < n_power_bands; ++k)
            slow.dv[k] *= 0.8;  // 20 % less acceleration
        slow.t[3] = 0.2;        // too short for a value
        char text[320];
        format_bands(text, sizeof text, slow, &ref);
        assert(strstr(text, "200-240 km/h 4.80 (") && strstr(text, " -20 %") && strstr(text, "300-400 km/h -"));
        format_bands(text, sizeof text, ref, nullptr);
        assert(!strstr(text, "%") && strstr(text, "200-240 km/h 6.00 ("));

        // The run summary: a step that ran twice adds up; the reference is every step without a power fault.
        power_run run;
        const char *base = "baseline", *mguk = "MGU-K lost", *later = "rear brakes weak";
        power_bands fast = ref;
        for (int k = 0; k < n_power_bands; ++k)
            fast.dv[k] *= 1.3;
        run.add(base, false, fast);
        run.add(mguk, true, slow);
        run.add(mguk, true, slow);
        run.add(later, false, ref);
        assert(run.n == 3 && run.e[1].power && close_to(run.e[1].p.t[0], 2 * slow.t[0], 1e-9));
        power_bands all = run.reference();
        assert(close_to(all.accel(0), 6 * 1.15, 0.05) && close_to(all.t[0], 2 * ref.t[0], 1e-9));
        for (int k = 0; k < 20; ++k)
            run.add(k % 2 ? base : later, false, ref);  // more steps than the table: the same names add up
        assert(run.n == 3);
    }

    // A bad dt does not count.
    sample bad = cruise;
    bad.dt = 2;
    assert(t.feed(bad) == tracker::none);
    puts("brake telemetry ok");
    return 0;
}
