#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include "test_util.hpp"

int main()
{
    SwoftyApi api{};
    api.log = tu::quiet_log;
    swofty::api = &api;
    using namespace susp;

    // ---- Model v2 (0.76): the stages follow from the hit and what is left of the corner ----
    assert(strength_of("realistic") == 7.0f && strength_of("forgiving") == 9.5f && strength_of("brutal") == 5.0f);
    assert(strength_of("anything") == strength_realistic);
    {
        float r0[4] = {0, 0.5f, 1, 2};
        Model m = fresh_model(5.0f, r0);
        assert(m.brk == 5.0f && m.c[0].part == 0.85f && std::fabs(m.c[1].part - 1.0f) < 1e-6f);
        assert(std::fabs(m.c[2].part - 1.15f) < 1e-6f && std::fabs(m.c[3].part - 1.15f) < 1e-6f);
    }
    // Below 1.5 m/s nothing happens.
    {
        Model m;
        HitResult h = apply_hit(m, 2, 1.4f, false, Rolls{});
        assert(h.severity == 0 && m.c[2].health == 1 && m.c[2].bend == 0 && !h.collapse);
    }
    // A clip at 2.5 m/s (below the elastic limit, 2.8 on Realistic): bent by 0.2-1 deg, no health lost. A hit past
    // it loses health by the square of the part past it. The same hit, the same result.
    {
        Model a, b;
        HitResult h = apply_hit(a, 2, 2.5f, false, Rolls{1.0f, 1.0f});
        assert(!h.collapse && h.delta > 0 && h.delta / deg <= 1.0f && h.delta / deg >= 0.2f && h.newly_bent);
        assert(a.c[2].health == 1.0f && h.loss == 0.0f);
        HitResult h4 = apply_hit(a, 1, 4.0f, false, Rolls{});
        apply_hit(b, 1, 4.0f, false, Rolls{});
        float past = (4.0f - 2.8f) / 4.2f;
        assert(std::fabs(h4.loss - hit_loss * past * past) < 1e-5f && a.c[1].health == b.c[1].health);
    }
    // One solid hit: holds below the strength, collapses from it, the wheel comes off from 1.7x. No dice.
    {
        for (float v = 1.5f; v < 7.0f; v += 0.25f) {
            Model m;
            HitResult h = apply_hit(m, 0, v, false, Rolls{});
            assert(!h.collapse && !h.wheel_off && !m.c[0].collapsed && m.c[0].health > 0);
        }
        for (float v : {7.0f, 9.0f, 11.8f}) {
            Model m;
            HitResult h = apply_hit(m, 0, v, false, Rolls{});
            assert(h.collapse && !h.wheel_off && m.c[0].collapsed && !m.c[0].off && m.c[0].health == 0);
        }
        for (float v : {12.0f, 20.0f, 35.0f}) {
            Model m;
            HitResult h = apply_hit(m, 0, v, false, Rolls{});
            assert(h.collapse && h.wheel_off && m.c[0].collapsed && m.c[0].off);
            assert(apply_hit(m, 0, v, false, Rolls{}).severity == 0);  // nothing more after the wheel is off
            assert(heavy_load(m, 0, 5.0f) == Load::none);
        }
        // A corner's own part: a weaker one (0.85) breaks at 5.95, a stronger one (1.15) holds at 8.
        Model m;
        m.c[1].part = 0.85f;
        m.c[2].part = 1.15f;
        assert(apply_hit(m, 1, 6.0f, false, Rolls{}).collapse && !apply_hit(m, 2, 8.0f, false, Rolls{}).collapse);
    }
    // Two medium hits on one corner break it where one does not: the strength falls with the health.
    {
        Model m;
        HitResult first = apply_hit(m, 3, 6.0f, false, Rolls{});
        assert(!first.collapse && m.c[3].health < weak_health && first.newly_weak && m.c[3].told_weak);
        assert(strength_now(m, m.c[3]) < 5.0f);
        assert(first.newly_damaged && m.c[3].told_damaged);
        HitResult second = apply_hit(m, 3, 5.0f, false, Rolls{});
        assert(second.collapse && m.c[3].collapsed && !m.c[3].off);
        // Light clips add up too, and warn once each.
        Model n;
        int bent = 0, weak = 0, k = 0;
        for (; k < 40 && !n.c[1].collapsed; ++k) {
            HitResult h = apply_hit(n, 1, 3.5f, false, Rolls{1.0f, 0.0f});
            bent += h.newly_bent;
            weak += h.newly_weak;
            assert(n.c[1].bend >= -bend_max - 1e-6f);
        }
        assert(bent == 1 && weak == 1 && n.c[1].collapsed && k > 4 && n.c[1].bend < 0);
        // A bent corner keeps bending its way whatever the roll says; one hit past warn_health warns once.
        Model s;
        apply_hit(s, 0, 4.6f, false, Rolls{0.5f, 0.9f});
        float b1 = s.c[0].bend;
        HitResult h2 = apply_hit(s, 0, 4.0f, false, Rolls{0.5f, 0.1f});
        assert(b1 > 0 && s.c[0].bend > b1 && h2.delta > 0);
        assert(s.c[0].health < warn_health && s.c[0].told_damaged);
        assert(!apply_hit(s, 0, 4.0f, false, Rolls{}).newly_damaged);
    }
    // Kerbs and the road: never break a corner that is not weakened, half the health of a solid hit.
    {
        for (float v = 1.5f; v < 25.0f; v += 0.5f) {
            Model m;
            HitResult h = apply_hit(m, 1, v, true, Rolls{});
            assert(!h.collapse && !h.wheel_off && (v < 3.0f || m.c[1].health < 1));
        }
        Model a, b;
        float la = apply_hit(a, 2, 4.0f, true, Rolls{}).loss, lb = apply_hit(b, 2, 4.0f, false, Rolls{}).loss;
        assert(std::fabs(2.0f * la - lb) < 1e-6f);
        Model w;
        w.c[1].health = 0.3f;
        assert(apply_hit(w, 1, 5.0f, true, Rolls{}).collapse);
    }
    // Heavy loads: a corner that is not weakened never wears, a weakened one faster the deeper the crack, and it
    // collapses after some hard strikes, never at once.
    {
        Model m;
        for (int k = 0; k < 10000; ++k)
            assert(heavy_load(m, 2, 3.0f) == Load::none);
        assert(m.c[2].health == 1.0f && heavy_load(m, 2, 0.9f) == Load::none);
        auto strikes = [](float health, float q) {
            Model x;
            x.c[0].health = health;
            int n = 0;
            while (!x.c[0].collapsed && n < 100000) {
                heavy_load(x, 0, q);
                ++n;
            }
            return n;
        };
        Model light;
        light.c[1].health = 0.75f;
        for (int k = 0; k < 10000; ++k)
            assert(heavy_load(light, 1, 3.0f) == Load::none);
        assert(light.c[1].health == 0.75f);
        assert(strikes(0.5f, 3.0f) == 100000);  // not weakened yet
        int at45 = strikes(0.45f, 1.3f), at30 = strikes(0.3f, 1.3f), at10 = strikes(0.1f, 1.3f);
        assert(at45 > at30 && at30 > at10 && at10 >= 2);
        assert(at45 >= 20 && at45 <= 70);  // just weakened: a few laps of hard kerbs
        assert(strikes(0.45f, 2.0f) < at45);
    }
    // A collapsed corner: a gentle limp keeps the wheel on, a hard kerb or a solid hit lets it go.
    {
        Model m;
        apply_hit(m, 2, 8.0f, false, Rolls{});
        assert(m.c[2].collapsed && !m.c[2].off);
        assert(heavy_load(m, 2, 1.9f) == Load::none && !m.c[2].off);
        assert(apply_hit(m, 2, 3.0f, false, Rolls{}).severity > 0 && !m.c[2].off);  // below half its strength
        assert(!apply_hit(m, 2, 5.0f, true, Rolls{}).wheel_off);                    // a kerb face
        Model a = m;
        assert(heavy_load(a, 2, 2.0f) == Load::wheel_off && a.c[2].off);
        Model b = m;
        assert(apply_hit(b, 2, 3.5f, false, Rolls{}).wheel_off && b.c[2].off);
    }
    // The strength option: the same hit breaks a Brutal corner, holds on Forgiving.
    {
        float mid[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        Model f = fresh_model(strength_forgiving, mid), r = fresh_model(strength_realistic, mid),
              b = fresh_model(strength_brutal, mid);
        assert(!apply_hit(f, 0, 6.0f, false, Rolls{}).collapse && !apply_hit(r, 0, 6.0f, false, Rolls{}).collapse);
        assert(apply_hit(b, 0, 6.0f, false, Rolls{}).collapse);
        assert(!apply_hit(f, 1, 12.0f, false, Rolls{}).wheel_off && apply_hit(r, 1, 12.0f, false, Rolls{}).wheel_off);
    }
    // Flashback undo: changes at or after the cut go, earlier ones stay.
    {
        Model m;
        History hist;
        Rolls hold{1.0f, 1.0f};
        hist.remember(1000, m);
        apply_hit(m, 2, 3.0f, false, hold);
        Model after_first = m;
        hist.remember(30000, m);
        apply_hit(m, 2, 3.0f, false, hold);
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
        // What of a body hit reaches the corners (0.76.0, the 70 km/h tyre-wall hit front-right): a square nose hit
        // loads both front corners, one by the wheel mostly that corner, a sidepod hit none, a landing none.
        {
            float v[4];
            body_to_corners(19.4f, 2.8f, -0.53f, v);
            assert(v[0] == 0 && v[1] == 0 && v[3] > v[2] && v[3] > 10.0f && v[2] > 4.0f);
            body_to_corners(10.0f, 2.5f, 0.0f, v);
            assert(std::fabs(v[2] - v[3]) < 1e-5f && std::fabs(v[2] - 6.0f * std::sqrt(0.5f)) < 1e-4f);
            body_to_corners(8.7f, 2.8f, 0.9f, v);
            assert(std::fabs(v[2] - 0.6f * 8.7f) < 1e-4f && v[3] == 0);
            body_to_corners(10.0f, -1.8f, -0.8f, v);
            assert(v[1] > 5.9f && v[0] == 0 && v[2] == 0 && v[3] == 0);
            body_to_corners(10.0f, 0.2f, 0.9f, v);
            assert(v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0);
            BodyBurst hb;
            ContactRecord w, f, t;
            w.c.dv = 2.0f, w.c.vn = 9.0f, w.c.at_fwd = 2.8f, w.c.at_left = 0.9f, w.cls = obj_solid;
            f.c.dv = 3.0f, f.c.vn = 9.0f, f.c.n_up = 1.0f, f.c.at_fwd = 2.8f, f.cls = obj_solid;  // the floor: not counted
            t.c.dv = 2.0f, t.c.vn = 10.0f, t.c.at_fwd = 2.8f, t.c.at_left = -0.9f, t.cls = obj_soft;  // a tyre wall: 0.6
            hb.add(w, 0);
            hb.add(f, 0);
            hb.add(t, 0);
            assert(std::fabs(hb.hdv - 3.2f) < 1e-5f && hb.hvn == 9.0f && hb.hit_v() == 6.4f);
            assert(std::fabs(hb.hit_left() - (2.0f * 0.9f - 1.2f * 0.9f) / 3.2f) < 1e-5f);
        }
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
    // Damage lab: the layout guard, the detached flag and what each step writes (fake suspension damage component).
    {
        alignas(16) static uint8_t comp[0x200] = {}, part[4][0x80] = {};
        auto keyed = [](size_t at, float v, uint32_t key) {
            tu::put(comp, at, std::bit_cast<uint32_t>(v) ^ key);
            tu::put(comp, at + 8, key);
        };
        keyed(0x58, 1.0f, 0x5a5a1234u);  // W
        keyed(0x68, 1.5f, 0x0badf00du);  // P
        for (int i = 0; i < 4; ++i)
            tu::put(comp, 0xe8 + 8 * i, tu::addr(part[i]));
        part[2][0x38] = 1;
        float w = 0, p = 0;
        assert(lab::limits(tu::addr(comp), w, p) && w == 1.0f && p == 1.5f);
        assert(lab::detached(tu::addr(comp), 2) == 1 && lab::detached(tu::addr(comp), 0) == 0);
        static uint32_t applied = 0xffffffff;
        wheel_state_get = +[](void *) -> uint32_t { return 0x100; };
        wheel_state_apply = +[](void *, uint32_t s) { applied = s; };
        auto d_fl = [] { float v; std::memcpy(&v, comp + 0x130 + 8, 4); return v; };
        lab::run(lab::steps[3], tu::addr(comp));  // just past W
        assert(std::fabs(d_fl() - 1.02f) < 1e-6f && !lab::unsupported);
        lab::run(lab::steps[6], tu::addr(comp));  // past twice P
        assert(std::fabs(d_fl() - 3.075f) < 1e-5f);
        lab::run(lab::steps[4], tu::addr(comp));  // the game's repair
        assert(applied == 0);
        lab::run(lab::steps[1], tu::addr(comp));  // camber +2 deg, cleared by the next step
        assert(std::fabs(lab::camber.load() - 2.0f * deg) < 1e-6f);
        lab::run(lab::steps[2], tu::addr(comp));
        assert(lab::camber.load() == 0 && std::fabs(d_fl() - 0.5f) < 1e-6f);
        // A layout that does not decode (P below W): no write, the lab stops writing.
        keyed(0x68, 0.5f, 0x0badf00du);
        assert(!lab::limits(tu::addr(comp), w, p));
        lab::run(lab::steps[5], tu::addr(comp));
        assert(lab::unsupported && std::fabs(d_fl() - 0.5f) < 1e-6f);
        lab::unsupported = false;
        wheel_state_get = nullptr;
        wheel_state_apply = nullptr;
    }
    std::puts("suspension damage ok");
}
