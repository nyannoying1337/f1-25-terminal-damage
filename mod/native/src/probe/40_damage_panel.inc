// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// The damage panel overlay.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Damage panel (issue #15): a header and a row of icon tiles at the top right (damage_icons.hpp), in the 2026 halo
// look (Project Halo's glass capsule): engine, ERS, gearbox, brakes, suspension, power. Bar, icon and label pill in yellow, orange or red
// by how bad it is. "auto" shows the badges with a problem, "always" all six while on track, "off" none.
// Render thread: reads atomics only.
dicons::icon icon_of_outcome(int o)
{
    switch (o) {
    case fm::mguk:
    case fm::es:
    case fm::ce:
        return dicons::ers;
    case fm::gearbox:
        return dicons::gearbox;
    case fm::suspension:
        return dicons::suspension;
    case fm::brakes:
        return dicons::brakes;
    default:
        return dicons::engine;  // ICE, turbo, MGU-H
    }
}

// The badges the panel shows now, in icon order (also used by the test). all: the fine ones too.
int damage_badges(dicons::badge *out, bool all)
{
    using namespace dicons;
    badge b[n_icons];
    for (int i = 0; i < n_icons; ++i)
        b[i].icon = uint8_t(i);
    // Raise a badge to level l; the label of the worst problem wins, the first one on a tie.
    auto raise = [&](int i, int l, const char *fmt = nullptr, double v = 0) {
        badge &x = b[i];
        if (l < x.level || (l == x.level && (x.label[0] || !fmt)))
            return;
        if (l > x.level)
            x.label[0] = 0;
        x.level = uint8_t(l);
        if (fmt)
            snprintf(x.label, sizeof x.label, fmt, v);
    };
    uint32_t mask = terminal_mask.load() | live_terminal_mask.load();
    if (mask & 1)
        raise(engine, broken, "OUT");
    if (mask & 2)
        raise(ers, broken, "OUT");
    if (mask & 4)
        raise(gearbox, broken, "OUT");
    if (int part = game_failure_part.load(); part >= 0 && !(mask & 7))
        raise(icon_of_outcome(part), broken, "OUT");
    if (int w = shown_warning.load(); w >= 0)
        raise(icon_of_outcome(w), problem);
    if (shown_power_cuts.load())
        raise(power, problem, "CUTS");
    else if (float loss = shown_power_loss.load(); loss >= 0.02f)
        raise(power, problem, "-%.0f%%", double(loss) * 100.0);
    if (int k = shown_power_kind.load(); k != pl::none && (shown_power_loss.load() >= 0.02f || shown_power_cuts.load()))
        raise(k == pl::mguk_loss || k == pl::battery_derate || k == pl::mguh ? ers : engine, problem);
    if (int st = shown_decline_stage.load(); st >= 0)
        raise(gearbox, st == 0 ? minor : st == 1 ? problem : broken);
    if (float sc = shown_shift_scale.load(); sc > 1.0f)
        raise(gearbox, problem, "x%.0f", double(sc));
    if (int g = shown_missing_gear.load(); g >= 2 && g <= 10)
        raise(gearbox, problem, "NO %.0f", double(g - 1));
    if (float r = rear_brake_loss_target.load(); r != 0)
        raise(brakes, problem, r > 0 ? "-%.0f%%" : "+%.0f%%", std::fabs(double(r)) * 100.0);
    // Corners RL, RR, FL, FR (2 bits each: fine, bent, weakened, collapsed) from the addon's model; a corner the
    // crash broke (broken_corners of crash_corners) is red too, limping or not (0.51.127: the game broke the FR, the
    // model had only bent the RR, the tile said RR; 0.51.128: a wheel the game only damaged, a flat tyre, was red).
    // The suspension tile names the worst corners ("FL", "FRONT", "x3"), limping says "LIMP".
    uint32_t corners = susp::corner_view.load(), collapsed = 0;
    for (int i = 0; i < 4; ++i)
        if (((corners >> (2 * i)) & 3) == 3)
            collapsed |= 1u << i;
    bool limping = limp_state.load() == 1;
    uint32_t crash = broken_corners(crash_corners.load(), collapsed);
    int worst = fine;
    for (int i = 0; i < 4; ++i) {
        uint8_t st = uint8_t((corners >> (2 * i)) & 3);
        if ((crash >> i) & 1)
            st = broken;
        b[suspension].wheel[i] = st;
        worst = std::max(worst, int(st));
    }
    if (limping)
        raise(suspension, broken, "LIMP");
    if (worst != fine) {
        unsigned at_worst = 0;
        for (int i = 0; i < 4; ++i)
            if (b[suspension].wheel[i] == worst)
                at_worst |= 1u << i;
        raise(suspension, worst, corner_label(at_worst));
    }
    int n = 0;
    for (int i = 0; i < n_icons; ++i)
        if (all || b[i].level != fine)
            out[n++] = b[i];
    return n;
}

// dicons pen on the Swofty overlay.
struct overlay_pen {
    SwoftyPaint *p;
    void line(float x0, float y0, float x1, float y1, float w, uint32_t c)
    {
        swofty::overlay::line(p, x0, y0, x1, y1, w, c);
    }
    void polyline(const float *xy, int n, float w, uint32_t c, bool closed)
    {
        swofty::overlay::polyline(p, xy, uint32_t(n), w, c, closed);
    }
    void polygon(const float *xy, int n, uint32_t c) { swofty::overlay::polygon(p, xy, uint32_t(n), c); }
    void circle(float cx, float cy, float r, uint32_t c) { swofty::overlay::circle(p, cx, cy, r, c); }
    // dicons rings are centred on r; the overlay draws its outline inside the radius.
    void ring(float cx, float cy, float r, float w, uint32_t c)
    {
        swofty::overlay::circle_outline(p, cx, cy, r + w / 2, w, c);
    }
    void arc(float cx, float cy, float r, float a0, float a1, float w, uint32_t c)
    {
        swofty::overlay::arc(p, cx, cy, r, a0, a1, w, c);
    }
    void rrect(float x, float y, float w, float h, float r, uint32_t c)
    {
        swofty::overlay::rect_rounded(p, x, y, w, h, r, c);
    }
    void rrect_outline(float x, float y, float w, float h, float r, float t, uint32_t c)
    {
        swofty::overlay::rect_rounded_outline(p, x, y, w, h, r, t, c);
    }
    void text(float x, float y, float size, uint32_t c, const char *s, int align)
    {
        namespace ov = swofty::overlay;
        ov::TextStyle st;
        st.size = size;
        st.rgba = c;
        st.tracking = size * 0.04f;
        st.align = (align == 0 ? ov::align::left : align == 1 ? ov::align::center : ov::align::right) |
                   ov::align::middle;
        ov::text(p, x, y, st, s);
    }
};

void paint_damage_panel(SwoftyPaint *paint, void *)
{
    namespace ov = swofty::overlay;
    // One log line whenever the panel changes between drawing and not drawing, and why (0.75.5: a Career test showed
    // no panel after an engine failure and the log could not say why). Render thread only.
    static int last_state = -1;
    auto note = [&](int state, const char *what, int tiles = 0, float x = 0, float y = 0, float scale = 0) {
        if (state == last_state)
            return;
        last_state = state;
        if (state == 5)
            swofty::log("[td-probe] damage panel: drawing %d tile(s) at x %.0f y %.0f, scale %.2f", tiles, double(x),
                        double(y), double(scale));
        else
            swofty::log("[td-probe] damage panel: not drawing (%s)", what);
    };
    std::string_view mode = damage_panel;
    if (mode == "off") {
        note(0, "the option is off");
        return;
    }
    if (session_ended || player_vehicle.load() < 0) {
        note(1, session_ended ? "no session" : "your car is not known yet");
        return;
    }
    // The session is over for you (NOT_IN_SESSION: finished or retired, the results and the replays after them):
    // no panel (0.51.124: it stayed up after the finish and over the replay).
    if (player_controller_flags.load() & (1u << 3)) {
        note(2, "your session is over: NOT_IN_SESSION");
        return;
    }
    dicons::badge b[dicons::n_icons];
    int n = damage_badges(b, mode == "always");
    if (n == 0) {
        note(3, "nothing wrong with the car");
        return;
    }
    // Top right, under the lap timing box (21 % down the safe area).
    ov::Rect safe = ov::safe_area(paint);
    overlay_pen pen{paint};
    float x = safe.x + safe.w, y = safe.y + safe.h * 0.21f, scale = ov::ui_scale(paint);
    note(5, "", n, x, y, scale);
    dicons::draw_panel(pen, x, y, scale, b, n);
}
