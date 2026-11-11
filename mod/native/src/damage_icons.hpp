// SPDX-License-Identifier: MIT
// Damage panel in Project Halo's look (the 2026 onboard halo: dark teal glass, a light rim, rounded tiles and pills): a
// header and a row of icon tiles (engine, ERS, gearbox, brakes, suspension, power) in one glass capsule, the icons drawn from vector strokes so they stay sharp at any resolution. Everything goes through a Pen, so the game overlay and the
// SVG preview (tools/damage_panel_preview.cpp) draw the same thing. Icons live on a 24 x 24 grid.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dicons {

enum icon : uint8_t { engine, ers, gearbox, brakes, suspension, power, n_icons };
enum level : uint8_t { fine, minor, problem, broken };  // white, yellow, orange, red

// Halo's label white; yellow and orange warmed to sit on the teal glass; red from the halo's brake stripe, made stronger.
constexpr uint32_t ink_fine = 0xe4f0f3ff, ink_minor = 0xf2cf3aff, ink_problem = 0xff8a3dff, ink_broken = 0xf0485cff;
inline uint32_t level_ink(int l)
{
    static const uint32_t c[4] = {ink_fine, ink_minor, ink_problem, ink_broken};
    return c[std::clamp(l, 0, 3)];
}

struct badge {
    uint8_t icon = 0, level = fine;
    uint8_t wheel[4] = {};  // suspension: corner levels RL, RR, FL, FR (the label names the worst, corner_label)
    char label[12] = {};    // short text under the tile ("-40%", "x6", "OUT", "FL"), may be empty
};

// The corners in mask m (bit 0 RL, 1 RR, 2 FL, 3 FR) as the suspension tile's label: one corner by name, a pair by its
// axle or side ("FRONT", "LEFT") or both names across ("FL RR"), three "x3", all four "ALL".
inline const char *corner_label(unsigned m)
{
    static const char *const one[4] = {"RL", "RR", "FL", "FR"};
    switch (m & 15) {
    case 1: return one[0];
    case 2: return one[1];
    case 4: return one[2];
    case 8: return one[3];
    case 12: return "FRONT";
    case 3: return "REAR";
    case 5: return "LEFT";
    case 10: return "RIGHT";
    case 6: return "FL RR";
    case 9: return "FR RL";
    case 15: return "ALL";
    case 0: return "";
    default: return "x3";
    }
}

constexpr float pi = 3.14159265f;

// Pen: line, polyline (closed or open), convex fill, filled circle, ring (stroke centred on r), arc (radians, 0 =
// right, clockwise), filled rounded rect, rounded rect outline, text. All in its own
// coordinates; the icon code maps the 24 grid through `grid`. text(x, y, size, rgba, str, align): align 0 left,
// 1 centre, 2 right of x; y is the middle of the line.
template <class Pen>
struct grid {
    Pen &p;
    float ox, oy, k;  // top-left of the 24 box and pixels per grid unit
    float x(float g) const { return ox + g * k; }
    float y(float g) const { return oy + g * k; }
    void line(float x0, float y0, float x1, float y1, float w, uint32_t c) { p.line(x(x0), y(y0), x(x1), y(y1), w * k, c); }
    template <int N>
    void poly(const float (&xy)[N], float w, uint32_t c, bool closed)
    {
        float t[N];
        for (int i = 0; i < N; i += 2) {
            t[i] = x(xy[i]);
            t[i + 1] = y(xy[i + 1]);
        }
        p.polyline(t, N / 2, w * k, c, closed);
    }
    template <int N>
    void fill(const float (&xy)[N], uint32_t c)
    {
        float t[N];
        for (int i = 0; i < N; i += 2) {
            t[i] = x(xy[i]);
            t[i + 1] = y(xy[i + 1]);
        }
        p.polygon(t, N / 2, c);
    }
    void dot(float cx, float cy, float r, uint32_t c) { p.circle(x(cx), y(cy), r * k, c); }
    void ring(float cx, float cy, float r, float w, uint32_t c) { p.ring(x(cx), y(cy), r * k, w * k, c); }
    void arc(float cx, float cy, float r, float a0, float a1, float w, uint32_t c)
    {
        p.arc(x(cx), y(cy), r * k, a0, a1, w * k, c);
    }
    void box(float bx, float by, float bw, float bh, float r, uint32_t c) { p.rrect(x(bx), y(by), bw * k, bh * k, r * k, c); }
    void frame(float bx, float by, float bw, float bh, float r, float w, uint32_t c)
    {
        p.rrect_outline(x(bx), y(by), bw * k, bh * k, r * k, w * k, c);
    }
};

constexpr float stroke = 1.8f;

template <class Pen>
void draw_icon(grid<Pen> g, const badge &b, uint32_t ink)
{
    switch (b.icon) {
    case engine: {  // engine block with the cylinder head on top and the gearbox stub on the left
        const float block[] = {5, 9, 8, 9, 9, 7, 15, 7, 16, 9, 18, 9, 18, 11, 20.5f, 11, 20.5f, 16, 18, 16, 18, 18.5f,
                               10, 18.5f, 8, 16, 5, 16};
        g.poly(block, stroke, ink, true);
        g.line(9.5f, 4.5f, 14.5f, 4.5f, stroke, ink);
        g.line(12, 4.5f, 12, 7, stroke, ink);
        g.line(2.5f, 10.5f, 2.5f, 14.5f, stroke, ink);
        g.line(2.5f, 12.5f, 5, 12.5f, stroke, ink);
        break;
    }
    case ers: {  // battery with a bolt
        g.frame(3, 7, 16, 10, 2, stroke, ink);
        g.box(19.6f, 10, 2, 4, 0.8f, ink);
        const float up[] = {13, 8.6f, 8.2f, 12.9f, 12.2f, 12.9f};
        const float down[] = {11.8f, 11.1f, 15.8f, 11.1f, 10.8f, 15.4f};
        g.fill(up, ink);
        g.fill(down, ink);
        break;
    }
    case gearbox: {  // H shift pattern
        for (float gx : {5.5f, 12.0f, 18.5f})
            g.line(gx, 5.5f, gx, 18.5f, stroke, ink);
        g.line(5.5f, 12, 18.5f, 12, stroke, ink);
        for (float gx : {5.5f, 12.0f, 18.5f}) {
            g.dot(gx, 5, 2.1f, ink);
            g.dot(gx, 19, 2.1f, ink);
        }
        break;
    }
    case brakes: {  // drilled disc with the caliper on the right
        g.ring(10.5f, 12, 7.3f, stroke, ink);
        g.dot(10.5f, 12, 2.2f, ink);
        for (int i = 0; i < 6; ++i) {
            float a = i * pi / 3 + pi / 6;
            g.dot(10.5f + 4.6f * std::cos(a), 12 + 4.6f * std::sin(a), 0.85f, ink);
        }
        g.arc(10.5f, 12, 9.4f, -0.95f, 0.95f, 3.2f, ink);
        break;
    }
    case suspension: {  // coil spring between its two seats (0.51.120: the car with coloured wheels read as a puncture)
        g.line(6, 3.6f, 18, 3.6f, 2.2f, ink);
        g.line(6, 20.4f, 18, 20.4f, 2.2f, ink);
        const float coil[] = {12, 3.6f, 17.5f, 6, 6.5f, 9, 17.5f, 12, 6.5f, 15, 17.5f, 18, 12, 20.4f};
        g.poly(coil, stroke, ink, false);
        break;
    }
    case power: {  // gauge, needle low
        g.arc(12, 13.5f, 8.5f, 0.75f * pi, 2.25f * pi, stroke, ink);
        float a = 1.12f * pi;
        g.line(12, 13.5f, 12 + 6.2f * std::cos(a), 13.5f + 6.2f * std::sin(a), stroke, ink);
        g.dot(12, 13.5f, 1.9f, ink);
        g.line(9, 20.5f, 15, 20.5f, stroke, ink);
        break;
    }
    default:
        break;
    }
}

// Project Halo's palette (tools/halo.js, theme authentic): the band glass, its rim and glow, the stripe panel behind
// the halo's slats (the tiles), the label white and the dim grey-blue.
constexpr uint32_t glass = 0x081a28cc, glass_top = 0x183e4e66, rim = 0x96d4de8c, rim_glow = 0x5ac8d22e,
                   tile_fill = 0x121e28cc, tile_line = 0x424e58c0, label_ink = 0xe4f0f3ff, dim_ink = 0x93a9bbff,
                   pill_text_dark = 0x06141fff;

inline const char *level_word(int l)
{
    static const char *const w[4] = {"OK", "CHECK", "WARNING", "FAILURE"};
    return w[std::clamp(l, 0, 3)];
}

// One tile, top-left (x, y), t wide and tall: a rounded stripe-panel square with the icon (Halo white when fine, in the
// level's colour otherwise) and a short pill of that colour along its bottom; below it, the label on a pill.
template <class Pen>
void draw_tile(Pen &p, float x, float y, float t, float s, const badge &b, float lh, float lgap)
{
    uint32_t col = level_ink(b.level);
    p.rrect(x, y, t, t, 6 * s, tile_fill);
    p.rrect_outline(x, y, t, t, 6 * s, 1 * s, b.level == fine ? tile_line : (col & 0xffffff00u) | 0xa0u);
    if (b.level != fine)
        p.rrect(x + 8 * s, y + t - 6 * s, t - 16 * s, 3 * s, 1.5f * s, col);
    float k = t * 0.6f / 24;
    draw_icon(grid<Pen>{p, x + t / 2 - 12 * k, y + t / 2 - 2 * s - 12 * k, k}, b, b.level == fine ? dim_ink : col);
    if (b.label[0]) {
        float ly = y + t + lgap;
        p.rrect(x, ly, t, lh, lh / 2, col);
        float size = strlen(b.label) > 4 ? 11 * s : 13 * s;  // "FRONT", "FL RR" fit the tile
        p.text(x + t / 2, ly + lh / 2, size, pill_text_dark, b.label, 1);
    }
}

// Tile size, gap, header height, the capsule's padding, the label pill's height and the capsule's least width at UI
// scale s (1 at 1080p).
struct panel_metrics {
    float t, gap, head, pad, lh, lgap, min_w;
    explicit panel_metrics(float s)
        : t(46 * s), gap(5 * s), head(20 * s), pad(9 * s), lh(17 * s), lgap(3 * s), min_w(150 * s) {}
    // The capsule's height, with or without a row of labels.
    float height(bool labels) const { return pad + head + gap + t + (labels ? lgap + lh : 0) + pad; }
};

inline bool any_label(const badge *b, int n)
{
    for (int i = 0; i < n; ++i)
        if (b[i].label[0])
            return true;
    return false;
}

// The panel, right edge at `right`, top at `top`: a Halo glass capsule (soft glow, light rim, a sheen along the top)
// holding the header ("CAR" and the worst state in its colour) and the row of tiles, right-aligned.
template <class Pen>
void draw_panel(Pen &p, float right, float top, float s, const badge *b, int n)
{
    if (n <= 0)
        return;
    panel_metrics m(s);
    float row = n * m.t + (n - 1) * m.gap, inner = std::max(row, m.min_w), w = inner + 2 * m.pad, x0 = right - w;
    float h = m.height(any_label(b, n)), r = 14 * s;
    int worst = 0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, int(b[i].level));
    uint32_t accent = worst ? level_ink(worst) : label_ink;
    // glow: two soft outlines just outside the rim, in the worst state's colour when something is wrong
    uint32_t glow = worst >= problem ? (accent & 0xffffff00u) | 0x30u : rim_glow;
    p.rrect_outline(x0 - 3 * s, top - 3 * s, w + 6 * s, h + 6 * s, r + 3 * s, 2 * s, (glow & 0xffffff00u) | ((glow & 255) / 2));
    p.rrect_outline(x0 - 1.5f * s, top - 1.5f * s, w + 3 * s, h + 3 * s, r + 1.5f * s, 2 * s, glow);
    p.rrect(x0, top, w, h, r, glass);
    p.rrect(x0 + r * 0.6f, top + 1.5f * s, w - 1.2f * r, m.head * 0.9f, m.head * 0.45f, glass_top);  // the top sheen
    p.rrect_outline(x0, top, w, h, r, 1.4f * s, rim);
    float mid = top + m.pad + m.head / 2;
    p.text(x0 + m.pad + 4 * s, mid, 13 * s, label_ink, "CAR", 0);
    p.text(right - m.pad - 4 * s, mid, 13 * s, worst ? accent : dim_ink, level_word(worst), 2);
    float ty = top + m.pad + m.head + m.gap;
    for (int i = 0; i < n; ++i)
        draw_tile(p, right - m.pad - row + i * (m.t + m.gap), ty, m.t, s, b[i], m.lh, m.lgap);
}

}
