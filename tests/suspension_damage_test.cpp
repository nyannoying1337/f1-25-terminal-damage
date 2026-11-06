#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include "test_util.hpp"

namespace {
std::mt19937_64 rng{777};
float u() { return float(rng() >> 40) / float(1u << 24); }
susp::Rolls rolls() { return {u(), u(), u(), u(), u(), u()}; }
}  // namespace

int main()
{
    SwoftyApi api{};
    api.log = tu::quiet_log;
    swofty::api = &api;
    using namespace susp;

    // ---- Thresholds (re/failure-realism-research.md section 5) ----
    assert(break_chance(1.4f) == 0 && break_chance(1.5f) == 0);
    assert(std::fabs(break_chance(4.0f) - 0.25f) < 1e-6f && std::fabs(break_chance(8.0f) - 0.70f) < 1e-6f);
    assert(break_chance(12.0f) == 1 && break_chance(35.0f) == 1);
    for (float v = 1.5f; v < 13; v += 0.25f)
        assert(break_chance(v + 0.25f) >= break_chance(v));
    // Below 1.5 m/s nothing happens.
    {
        Model m;
        HitResult h = apply_hit(m, 2, 1.4f, false, Rolls{0, 0, 0, 0, 0}, 1000);
        assert(h.severity == 0 && m.c[2].health == 1 && m.c[2].bend == 0 && !h.collapse && !m.c[2].crack_at);
    }
    // A clip at 2.5 m/s that holds: bent by 0.2-1 deg, a little health lost.
    {
        Model m;
        HitResult h = apply_hit(m, 2, 2.5f, false, Rolls{1.0f, 1.0f, 0.99f, 0.99f, 0.5f}, 1000);
        assert(!h.collapse && h.delta > 0 && h.delta / deg <= 1.0f && h.delta / deg >= 0.2f);
        assert(m.c[2].health < 1 && m.c[2].health > 0.85f && !m.c[2].crack_at);
        assert(h.chance > 0.05f && h.chance < 0.15f);
    }
    // A square hit at 12 m/s or more always breaks (the 0.51.88 "35 m/s wheel hit" case too).
    for (float v : {12.0f, 20.0f, 35.0f}) {
        Model m;
        HitResult h = apply_hit(m, 0, v, false, Rolls{0.5f, 0.5f, 0.999f, 0.5f, 0.5f}, 1000);
        assert(h.collapse && m.c[0].collapsed && h.chance == 1);
        // A collapsed corner takes no more hits or loads.
        HitResult again = apply_hit(m, 0, v, false, Rolls{}, 2000);
        assert(again.severity == 0 && !again.collapse);
        assert(!heavy_load(m, 0, 0.0f, 3000));
    }
    // Monte Carlo against the table: break share per hit size on a healthy corner (at once or failing), and the
    // share that lets go at once (0.51.132: break_now_share, 30 % up to 4 m/s, all from 12 m/s).
    {
        struct Shares {
            double broke, now;
        };
        auto share = [](float v) {
            int broke = 0, now = 0, n = 20000;
            for (int k = 0; k < n; ++k) {
                Model m;
                HitResult h = apply_hit(m, 1, v, false, rolls(), 0);
                broke += h.collapse || h.failing;
                now += h.collapse;
                assert(!(h.collapse && h.failing));
            }
            return Shares{double(broke) / n, double(now) / n};
        };
        Shares s2 = share(2.5f), s6 = share(6.0f), s10 = share(10.0f), s14 = share(14.0f);
        assert(s2.broke > 0.05 && s2.broke < 0.15);   // 1.5-4: about 1 in 10
        assert(s6.broke > 0.40 && s6.broke < 0.55);   // 4-8: 30-70 %
        assert(s10.broke > 0.80 && s10.broke < 0.90); // 8-12: over 80 %
        assert(s14.broke == 1.0 && s14.now == 1.0);   // 12 and more: always, at once
        assert(s2.now > 0.015 && s2.now < 0.045);     // 0.10 * 0.30
        assert(s6.now > 0.20 && s6.now < 0.25);       // 0.475 * 0.475
        assert(s10.now > 0.67 && s10.now < 0.73);     // 0.85 * 0.825
        assert(std::fabs(break_now_share(3.0f) - 0.3f) < 1e-6f && std::fabs(break_now_share(8.0f) - 0.65f) < 1e-6f);
        assert(break_now_share(12.0f) == 1.0f && break_now_share(30.0f) == 1.0f);
    }
    // Failing corners (0.51.132): a solid hit that breaks the corner but does not let go yet. It holds, bent, its
    // health down to failing_health, and its crack is due after 5 s to 3 min of driving, mostly within a minute.
    {
        Model m;
        HitResult h = apply_hit(m, 2, 6.0f, false, Rolls{0.5f, 0.9f, 0.0f, 0.0f, 0.5f, 0.99f}, 100000);
        assert(h.failing && !h.collapse && !h.cracked && !m.c[2].collapsed);
        assert(m.c[2].health == failing_health && h.newly_weak && m.c[2].told_weak);
        assert(h.delta > 0 && m.c[2].bend == h.delta && h.newly_bent);
        assert(m.c[2].crack_at == 100000 + failing_delay_ms(0.5f));
        assert(failing_delay_ms(0.0f) == 5000 && failing_delay_ms(1.0f) == 180000 && failing_delay_ms(0.5f) == 48750);
        // It lets go at the first heavy load once due, or crack_grace_ms later under ordinary loads.
        Model a = m;
        assert(!heavy_load(a, 2, 0.99f, m.c[2].crack_at - 1) && !a.c[2].collapsed);
        assert(heavy_load(a, 2, 0.99f, m.c[2].crack_at) && a.c[2].collapsed);
        assert(!crack_gives(m.c[2], m.c[2].crack_at + crack_grace_ms - 1) &&
               crack_gives(m.c[2], m.c[2].crack_at + crack_grace_ms));
        // Hit again while failing: a break lets go at once (it has a crack); a hit that holds keeps the due time.
        Model b = m;
        HitResult again = apply_hit(b, 2, 3.0f, false, Rolls{0.5f, 0.5f, 0.0f, 0.0f, 0.5f, 0.99f}, 110000);
        assert(again.collapse && b.c[2].collapsed);
        Model c = m;
        HitResult held = apply_hit(c, 2, 2.0f, false, Rolls{0.5f, 0.5f, 0.999f, 0.0f, 0.0f, 0.99f}, 110000);
        assert(!held.collapse && !held.failing && !held.cracked && c.c[2].crack_at == m.c[2].crack_at);
        // A kerb or road strike that breaks a damaged corner lets go at once, never later.
        Model d;
        d.c[1].health = 0.3f;
        HitResult kerb = apply_hit(d, 1, 8.0f, true, Rolls{0.5f, 0.5f, 0.0f, 0.99f, 0.5f, 0.99f}, 0);
        assert(kerb.collapse && !kerb.failing);
        // A corner with an older hidden crack breaks at once too.
        Model e;
        e.c[3].crack_at = 900000;
        assert(apply_hit(e, 3, 6.0f, false, Rolls{0.5f, 0.5f, 0.0f, 0.99f, 0.5f, 0.99f}, 0).collapse);
        // The failing delays: 5 s to 3 min, more than half within the first minute of driving.
        int within_minute = 0, n = 20000;
        for (int k = 0; k < n; ++k) {
            uint64_t ms = failing_delay_ms(u());
            assert(ms >= 5000 && ms <= 180000);
            within_minute += ms <= 60000;
        }
        assert(within_minute > n / 2 && within_minute < n * 6 / 10);
        // A flashback undoes it like any other hit.
        History hist;
        Model f;
        hist.remember(1000, f);
        apply_hit(f, 0, 6.0f, false, Rolls{0.5f, 0.5f, 0.0f, 0.0f, 0.5f, 0.99f}, 1000);
        assert(f.c[0].crack_at && hist.undo_since(0, f) && !f.c[0].crack_at && f.c[0].health == 1);
    }
    // Hidden cracks: 10-20 % of the hits that hold; due between 5 s and about 25 laps later.
    {
        int held = 0, cracked = 0;
        uint64_t lo = ~0ull, hi = 0;
        int soon = 0;
        for (int k = 0; k < 20000; ++k) {
            Model m;
            HitResult h = apply_hit(m, 3, 1.5f + 6.0f * u(), false, rolls(), 100000);
            if (h.collapse || h.failing)
                continue;
            ++held;
            if (h.cracked) {
                ++cracked;
                lo = std::min(lo, m.c[3].crack_at - 100000);
                hi = std::max(hi, m.c[3].crack_at - 100000);
                soon += m.c[3].crack_at - 100000 <= 600000;
            }
        }
        double share = double(cracked) / held;
        assert(share > 0.10 && share < 0.20);
        assert(lo >= 5000 && hi <= 2250000 && hi > 2000000);
        // 0.51.132: front-loaded, about half within 10 minutes of driving (uniform: about a quarter).
        double early = double(soon) / cracked;
        assert(early > 0.45 && early < 0.58);
        // A due crack lets go at the next heavy load, or a minute later anyway.
        Model m;
        apply_hit(m, 3, 3.0f, false, Rolls{0.5f, 0.5f, 0.99f, 0.0f, 0.0f}, 0);
        assert(m.c[3].crack_at == 5000);
        assert(!heavy_load(m, 3, 0.99f, 4000) && !m.c[3].collapsed);
        assert(heavy_load(m, 3, 0.99f, 5000) && m.c[3].collapsed);
        Model m2;
        apply_hit(m2, 2, 3.0f, false, Rolls{0.5f, 0.5f, 0.99f, 0.0f, 0.0f}, 0);
        assert(!crack_gives(m2.c[2], 5000 + crack_grace_ms - 1) && crack_gives(m2.c[2], 5000 + crack_grace_ms));
    }
    // Kerbs and the road: never break a healthy corner, can finish a damaged one.
    {
        for (int k = 0; k < 1000; ++k) {
            Model m;
            assert(!apply_hit(m, 1, 1.5f + 10.0f * u(), true, rolls(), 0).collapse);
        }
        Model m;
        m.c[1].health = 0.3f;
        HitResult h = apply_hit(m, 1, 8.0f, true, Rolls{0.5f, 0.5f, 0.5f, 0.99f, 0.5f}, 0);
        assert(h.chance > 0.69f && h.collapse);
    }
    // A damaged corner breaks more easily.
    {
        Model a, b;
        b.c[2].health = 0.5f;
        float ca = apply_hit(a, 2, 3.0f, false, Rolls{}, 0).chance, cb = apply_hit(b, 2, 3.0f, false, Rolls{}, 0).chance;
        assert(std::fabs(cb - 1.5f * ca) < 1e-6f);
    }
    // Repeated clips: the bent warning once, the weak warning once, the bend capped.
    {
        Model m;
        int bent = 0, weak = 0;
        for (int k = 0; k < 8 && !m.c[3].collapsed; ++k) {
            HitResult h = apply_hit(m, 3, 3.5f, false, Rolls{1.0f, 0.0f, 0.999f, 0.999f, 0.5f}, 0);
            bent += h.newly_bent;
            weak += h.newly_weak;
            assert(m.c[3].bend >= -bend_max - 1e-6f);
        }
        assert(bent == 1 && weak == 1 && m.c[3].bend < 0);
    }
    // Heavy loads: a healthy corner never fails, a weakened one can.
    {
        Model m;
        for (int k = 0; k < 1000; ++k)
            assert(!heavy_load(m, 2, 0.0f, uint64_t(k) * 1000));
        assert(m.c[2].health < 1 && m.c[2].health > 0.4f);
        m.c[2].health = 0.2f;
        assert(!heavy_load(m, 2, 0.5f, 0));
        assert(heavy_load(m, 2, 0.001f, 0) && m.c[2].collapsed);
    }
    // Flashback undo: changes at or after the cut go, earlier ones stay.
    {
        Model m;
        History hist;
        Rolls hold{1.0f, 1.0f, 0.999f, 0.999f, 0.5f};
        hist.remember(1000, m);
        apply_hit(m, 2, 3.0f, false, hold, 1000);
        Model after_first = m;
        hist.remember(30000, m);
        apply_hit(m, 2, 3.0f, false, hold, 30000);
        assert(m.c[2].bend > after_first.c[2].bend);
        assert(hist.undo_since(25000, m));
        assert(m.c[2].bend == after_first.c[2].bend && m.c[2].health == after_first.c[2].health);
        assert(!hist.undo_since(25000, m));
        assert(hist.undo_since(0, m) && m.c[2].bend == 0 && m.c[2].health == 1);
    }

    // ---- Contacts: what counts ----
    assert(tag4("PLAS") == 0x53414c50u && tag4("SIGN") == 0x4e474953u);  // as the game compares them
    assert(classify(tag4("SIGN"), 0, 0x13) == obj_harmless && classify(tag4("POLY"), 0, 0) == obj_harmless);
    assert(classify(tag4("PLAS"), 0, 0) == obj_harmless && classify(tag4("CONE"), 0, 0) == obj_harmless);
    assert(classify(tag4("WALL"), 0, 0x18) == obj_harmless && classify(tag4("WALL"), 0, 0x15) == obj_harmless);
    assert(classify(tag4("WALL"), 0, 0x13) == obj_solid && classify(tag4("FMET"), 0, 0) == obj_solid);
    assert(classify(tag4("GRS+"), 0, 0) == obj_surface && classify(tag4("RMBL"), 0, 0) == obj_surface);
    assert(classify(tag4("CON+"), 0, 0) == obj_surface && classify(tag4("DDT "), 0, 0) == obj_surface);
    assert(classify(tag4("TYRW"), 0, 0) == obj_soft && classify(tag4("DRUB"), 0, 0) == obj_soft);
    assert(classify(tag4("TYRE"), tag4("TYRE"), 0) == obj_soft && classify(tag4("CAR_"), tag4("TYRE"), 0) == obj_solid);
    {
        char t[5];
        tag_text(tag4("WALL"), t);
        assert(!strcmp(t, "WALL"));
    }
    // note_contact: a 0.51.113 board contact (|v.n| 71 m/s, almost no impulse) and a brush do not count; a clip does,
    // a tyre wall gives 0.6 of it, a hit on the tyre's face counts twice.
    {
        Pending p[4];
        bool corner[4] = {false, true, false, false};
        Contact board;
        board.tag = tag4("WALL");
        board.dv = 0.08f;
        board.vn = 30.0f;
        board.vn_cm = 71.2f;
        board.face = 0.3f;
        light_n = 0;
        light_dv = -1;
        uint32_t ring = contact_log.head.load();
        note_contact(board, p, corner, -1);
        // The stats keep the light contact with the most impulse, with its normal speed; the ring keeps it (vn >= 3).
        assert(p[1].dv.load() == 0 && light_n.load() == 1 && light_dv.load() == 0.08f && light_vn.load() == 30.0f);
        assert(contact_log.head.load() == ring + 1);
        Contact lighter = board;
        lighter.dv = 0.02f;
        lighter.vn = 0.5f;
        note_contact(lighter, p, corner, -1);
        assert(light_n.load() == 2 && light_dv.load() == 0.08f && contact_log.head.load() == ring + 1);
        // One the game itself took suspension damage from is always in the ring, counted or not.
        Contact damaged = lighter;
        damaged.dd = 0.2f;
        damaged.dd_corner = 1;
        note_contact(damaged, p, corner, -1);
        assert(contact_log.head.load() == ring + 2);
        ContactRecord rec;
        assert(contact_log.get(ring + 1, rec) && !rec.counted && rec.c.dd == 0.2f && rec.corner == 1);
        // A contact with the car's own body (a broken wheel against the car): never counted, only counted as such.
        Contact own = board;
        own.self = true;
        own.dv = 3.0f;
        own.vn = 9.0f;
        self_n = 0;
        note_contact(own, p, corner, -1);
        assert(p[1].dv.load() == 0 && self_n.load() == 1 && light_n.load() == 3);
        assert(contact_log.head.load() == ring + 2);
        Contact sign = board;
        sign.tag = tag4("SIGN");
        sign.dv = 3.0f;
        ignored_n = 0;
        note_contact(sign, p, corner, -1);
        assert(p[1].dv.load() == 0 && ignored_n.load() == 1);
        Contact brush = board;
        brush.dv = 0.9f;
        brush.vn = 0.6f;
        note_contact(brush, p, corner, -1);
        assert(p[1].dv.load() == 0);
        Contact clip = board;
        clip.dv = 1.4f;
        clip.vn = 3.0f;
        note_contact(clip, p, corner, -1);
        assert(p[1].dv.load() == 1.4f && p[1].vn.load() == 3.0f && p[1].solid.load() && p[1].tag.load() == tag4("WALL"));
        assert(p[0].dv.load() == 0);
        Pending q[4];
        Contact tyres = clip;
        tyres.tag = tag4("TYRW");
        note_contact(tyres, q, corner, 5);
        assert(std::fabs(q[1].dv.load() - 0.84f) < 1e-5f && std::fabs(q[1].vn.load() - 1.8f) < 1e-5f);
        Pending f[4];
        Contact face = clip;
        face.face = 0.9f;
        note_contact(face, f, corner, 5);
        assert(f[1].dv.load() == 2.8f && f[1].vn.load() == 6.0f && f[1].face.load());
        Pending k[4];
        Contact kerb = clip;
        kerb.tag = tag4("RMBL");
        kerb.face = 0.9f;  // no face rule for the road
        note_contact(kerb, k, corner, 5);
        assert(std::fabs(k[1].vn.load() - 1.05f) < 1e-5f && !k[1].solid.load() && !k[1].face.load());
    }
    // Body contacts (no wheel): counted in the stats; the ring gets them from 0.3 m/s off the ground and 1.0 m/s on it.
    {
        body_n = floor_n = 0;
        body_dv = -1;
        floor_dv = -1;
        uint32_t ring = contact_log.head.load();
        Contact nose;
        nose.tag = tag4("SCON");
        nose.own = tag4("CARB");
        nose.j = 2400;
        nose.dv = 3.0f;
        nose.vn = 14.0f;
        nose.at_fwd = 2.6f;
        nose.at_left = 0.5f;
        nose.n_up = 0.05f;
        note_body_contact(nose);
        assert(body_n.load() == 1 && body_dv.load() == 3.0f && body_tag.load() == tag4("SCON") &&
               body_own.load() == tag4("CARB") && contact_log.head.load() == ring + 1);
        ContactRecord rec;
        assert(contact_log.get(ring, rec) && rec.c.body && !rec.counted && rec.corner == -1);
        Contact light = nose;
        light.dv = 0.2f;
        note_body_contact(light);
        assert(body_n.load() == 2 && body_dv.load() == 3.0f && contact_log.head.load() == ring + 1);
        Contact floor = nose;
        floor.n_up = 0.95f;
        floor.dv = 0.6f;
        note_body_contact(floor);
        assert(floor_n.load() == 1 && floor_dv.load() == 0.6f && contact_log.head.load() == ring + 1);
        floor.dv = 1.2f;
        note_body_contact(floor);
        assert(floor_n.load() == 2 && contact_log.head.load() == ring + 2);
        Contact own = nose;
        own.self = true;
        self_n = 0;
        note_body_contact(own);
        assert(self_n.load() == 1 && body_n.load() == 2 && contact_log.head.load() == ring + 2);

        // One line per hit: the contacts of a burst add up, the place is weighted by their impulse.
        BodyBurst bb;
        ContactRecord a, c;
        a.c = nose;
        c.c = nose;
        c.c.dv = 1.0f;
        c.c.vn = 20.0f;
        c.c.at_fwd = 1.8f;
        c.c.at_left = -0.3f;
        c.c.proxy = 1;
        bb.add(a, 1000);
        bb.add(c, 1050);
        bb.add_out(2, 57.6f, 1060);
        assert(bb.n == 2 && bb.wheels == 1 && bb.dv == 4.0f && bb.vn == 20.0f && bb.peak.dv == 3.0f && bb.out[2] == 57.6f);
        assert(std::fabs(bb.at_fwd() - 2.4f) < 1e-5f && std::fabs(bb.at_left() - 0.3f) < 1e-5f);
        assert(!bb.due(1300) && bb.due(1360));
        assert(!strcmp(body_region(bb.at_fwd(), bb.at_left()), "front"));
        assert(!strcmp(body_region(2.0f, 0.6f), "front left") && !strcmp(body_region(-1.7f, -0.8f), "rear right") &&
               !strcmp(body_region(0.2f, 0.9f), "left side") && !strcmp(body_region(0, 0), "middle"));
        {
            alignas(16) static uint8_t fcar[0x600]{}, wp[0x80]{}, cont[0x48]{}, wrap[8][16]{}, hull[16]{};
            uintptr_t wpa = reinterpret_cast<uintptr_t>(wp);
            memcpy(fcar + 0x5b8, &wpa, 8);
            for (int k = 0; k < 8; ++k) {
                uintptr_t w = reinterpret_cast<uintptr_t>(wrap[k]);
                memcpy(wp + 0x40 + 8 * k, &w, 8);
            }
            uintptr_t fc = reinterpret_cast<uintptr_t>(fcar), cc = reinterpret_cast<uintptr_t>(cont);
            auto own_is = [&](const void *w) {
                uintptr_t x = reinterpret_cast<uintptr_t>(w);
                memcpy(cont + 8, &x, 8);
            };
            own_is(wrap[2]);  // +0x50: front
            assert(wheel_proxy(fc, cc) == 1);
            own_is(wrap[7]);  // +0x78: front
            assert(wheel_proxy(fc, cc) == 1);
            own_is(wrap[0]);  // +0x40: rear
            assert(wheel_proxy(fc, cc) == 2);
            own_is(wrap[5]);  // +0x68: rear
            assert(wheel_proxy(fc, cc) == 2);
            own_is(hull);
            assert(wheel_proxy(fc, cc) == 0);
        }

        // The game's damage between two calls of its handler came from outside it (the front wing's overflow).
        for (auto &x : out_pending)
            x = 0;
        out_comp = 0;
        float d0[4] = {0, 0, 0, 0}, d1[4] = {0, 0, 0.1f, 0};
        note_outside_damage(0x1000, d0, d1);  // the first call: nothing to compare
        assert(out_pending[2].load() == 0);
        float d2[4] = {0, 0, 57.7f, 0}, d3[4] = {0, 0, 57.8f, 0};
        note_outside_damage(0x1000, d2, d3);
        assert(std::fabs(out_pending[2].load() - 57.6f) < 1e-4f && out_pending[3].load() == 0);
        float back[4] = {0, 0, 0, 0};  // a flashback: lower, nothing
        note_outside_damage(0x1000, back, back);
        assert(std::fabs(out_pending[2].load() - 57.6f) < 1e-4f);
        float other[4] = {5, 5, 5, 5};  // another car's component (a new session): nothing
        note_outside_damage(0x2000, other, other);
        assert(std::fabs(out_pending[2].load() - 57.6f) < 1e-4f && out_pending[0].load() == 0);
        note_outside_damage(0x2000, other, nullptr);  // unreadable after the call: the next one is not compared
        float more[4] = {9, 9, 9, 9};
        note_outside_damage(0x2000, more, more);
        assert(out_pending[0].load() == 0);
    }
    // Hit gate: the hit size is the peak normal speed as far as the summed impulse confirms it; one hit per burst, a
    // harder hit or a new contact after a pause counts again.
    {
        HitGate g;
        g.contact(1000, 1.0f, 5.0f, tag4("WALL"));
        g.contact(1100, 1.5f, 9.0f, tag4("FMET"));
        assert(g.take(1200).v == 0);
        g.contact(1240, 0.5f, 7.0f);
        HitGate::Hit h = g.take(1250);
        assert(h.v == 6.0f && h.sum_dv == 3.0f && h.peak_vn == 9.0f && h.tag == tag4("FMET") && h.contacts == 3);
        for (uint64_t t = 1300; t < 3000; t += 100) {
            g.contact(t, 2.0f, 4.0f);
            assert(g.take(t).v == 0);
        }
        g.contact(3000, 6.0f, 13.0f);
        assert(g.take(3250).v == 12.0f);
        g.contact(5000, 3.0f, 6.0f);
        assert(g.take(5250).v == 6.0f);
        assert(contact_speed(30.0f, 0.4f) == 0.8f && contact_speed(3.0f, 4.0f) == 3.0f);
    }
    // Damage MFD value: nothing for a new corner, lost health, half the range for a fully bent corner, 100 % collapsed.
    {
        Corner c;
        assert(mfd_value(c) == 0);
        c.health = 0.48f;
        assert(std::fabs(mfd_value(c) - 0.52f) < 1e-5f);
        c.health = 1;
        c.bend = -bend_max;
        assert(std::fabs(mfd_value(c) - 0.5f) < 1e-5f);
        c.collapsed = true;
        assert(mfd_value(c) == 1);
    }
    std::puts("suspension damage ok");
}
