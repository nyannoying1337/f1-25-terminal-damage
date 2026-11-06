#pragma once
// Suspension damage from hits (0.51.88): a hit on a wheel can bend that corner (toe: the car pulls) and weaken it; a
// weak corner can collapse later under heavy load. A hit that breaks a corner lets it go at once or, since 0.51.132,
// only later while driving (failing). Plus the debug suspension test (Ctrl+Shift+F8).
// AI cars (suspension_damage_ai, issue #18): hits bend, weaken and collapse their corners the same way (no kerb
// fatigue); a collapsed corner retires the car through the game's own terminal damage handling, or, if the game sends
// no terminal damage message within 15 s, through a retirement status request.
//
// Per-wheel geometry step 0x1446b4610 (rcx = the car's suspension system car+0x580, xmm1 dt), about once per frame
// while driving, not while paused or in the garage:
//   camber wheel+0x154 = travel wheel+0x168 * gain (axle+0x15f0 / +0x15fc) + axle camber axle+0x1650
//   toe    wheel+0x160 = axle toe axle+0x1654 + bump steer spline
// The toe stays in a register: angle = side (+-1) * toe + steering (wheel+0x178) + wheel+0x148 terms, and the step
// stores the wheel heading as sin at wheel+0x1f8 and cos at wheel+0x1fc. The physics uses those (0.51.87 test: the
// car pulls), so a bent corner is the heading rotated after the step. Writing wheel+0x160 afterwards does nothing.
// Axles: sys+0x90 rear (axle+0x1598 = -2), sys+0x98 front; axle+0x1630 / +0x1638 its two wheels. Wheels
// sys+0x70+i*8: RL, RR, FL, FR (the corner state order, 3 bits each from bit 0). Units: radians.
//
// Hits: the collision handler 0x144654d10 (rcx damage component, [rcx+8] car, rdx contact; other contacts whose
// normal points up, i.e. the ground and flat kerbs, are ignored; needs the damage settings [car+0x130]&1, &8 and
// [car+0x133]&2) finds the wheel of the hit zone by its side/axle ids (wheel+0x2b4/+0x2b8) and stores |velocity at
// the contact point| (m/s, sliding included): car contacts set wheel+0x20 bit 2 and wheel+0x24, others bit 3 and
// wheel+0x2c. The addon takes the corner from that write and measures the contact itself (re/collision-impact.md):
//   contact +0x00 car, +0x10 other object's wrapper ([+0x10] entity, [+0x18] shape: group byte +0x164, rigid body
//   +0x108 -> actor +0x458 -> body +0x18), +0x18 -> point, +0x20 -> normal (into this car), +0x28 -> own material,
//   +0x30 -> other material (4cc, first char in the low byte), +0x38 impulse J on this car (kg m/s), +0x3c dt,
//   +0x40 other vehicle (0xff none). Chassis [car+0x558]: +0x154 1/mass, +0x170 velocity at the start of the step,
//   +0x180 angular velocity, +0x190/+0x1a0/+0x1b0 sideways/up/forward, +0x1c0 position.
// The hit size is the normal speed into the object (the game's own measure: velocity at the point minus the other
// body's, along the normal) as far as the impulse confirms it (J/mass, the speed change it gave the car). Up to
// 0.51.113 the addon took |velocity . normal| without the impulse: brushing advertising boards, edges and kerb faces
// read as 25-85 m/s hits (0.51.113 log: "RR hit 71.2 m/s") and broke corners.
// Body contacts (0.51.116): the nose, the body, the wings and the floor are no wheel zone, so the game's handler adds
// no suspension damage for them; only the front wing's own callback passes its overflow on to FL/FR. The 0.51.115 log
// had every crash there: no wheel contact above J 50, the game's FL damage jumped to 57.6 with no wheel contact. The
// addon logs them, one line per hit, with where on the car and how hard, to calibrate a body-hit model; they do not
// count yet.
// Kerbs and landings: the rate of the wheel travel wheel+0x168, measured in the geometry step.
// Collapse: the corner state apply 0x144656a80 with the corner at state 2 (suspension broken), on the game thread.
namespace susp {

// ---- Model (pure, unit tested) ----
constexpr float deg = 0.0174533f;
// Hit sizes (the normal speed into the object, m/s) from the real cases (re/failure-realism-research.md sections 2
// and 5, a wheel against concrete or Armco): below 1.5 nothing (brushes and taps: Russell Monaco 2022, Verstappen
// Monaco 2023 "clipped a few barriers", Norris Singapore 2024); 1.5-4 bent by 0.2-1 deg (Norris Singapore 2023:
// steering off-centre, finished 2nd), about 1 in 10 break; 4-8 break 30-70 %, otherwise bent by more than 1 deg;
// 8-12 over 80 %; from 12 always (the corner collapses or the wheel hangs on its tethers). Engineering check: about
// 8.5 kN per m/s on the wheel, parts fail at 18-28 kN (safety factor 1.5-2), so 2-3.5 m/s for a square hit.
constexpr float hit_min = 1.5f, hit_always = 12.0f;
constexpr float bend_max = 3.5f * deg;
constexpr float weak_health = 0.4f;
constexpr float heavy_fatigue = 0.0005f;
// A healthy corner never fails from kerbs alone: every real kerb failure we found had new or raised kerbs or a part
// damaged before (re/failure-realism-research.md section 2.2).
constexpr float base_failure = 0.0f;
constexpr float weak_failure = 0.03f;  // per heavy load, times how far below weak_health the corner is

struct Corner {
    float bend = 0;    // toe change in radians
    float health = 1;  // 1 new, 0 gone
    bool told_bend = false, told_weak = false, collapsed = false;
    uint64_t crack_at = 0;  // hidden crack: fails at the first heavy load after this driving time (ms), 0 = none
};
struct Model {
    Corner c[4];
};

struct HitResult {
    float severity = 0, delta = 0, chance = 0;
    // failing: the hit broke the corner, but it holds for now and lets go later (failing_delay_ms)
    bool collapse = false, newly_bent = false, newly_weak = false, cracked = false, failing = false;
};

inline float severity_of(float speed) { return std::clamp((speed - hit_min) / (hit_always - hit_min), 0.0f, 1.0f); }

// Chance that a hit of this size breaks a healthy corner at once.
inline float break_chance(float v)
{
    if (!(v >= hit_min))
        return 0.0f;
    if (v < 4.0f)
        return 0.25f * (v - hit_min) / (4.0f - hit_min);
    if (v < 8.0f)
        return 0.25f + 0.45f * (v - 4.0f) / 4.0f;
    if (v < hit_always)
        return 0.70f + 0.30f * (v - 8.0f) / (hit_always - 8.0f);
    return 1.0f;
}

// Toe change of a hit that bends but does not break (degrees): 0.2-1 below 4 m/s, 1-2.5 above, r in [0, 1) scales
// it by 0.6-1.4.
inline float bend_deg(float v, float r)
{
    float base = v < 4.0f ? 0.2f + 0.8f * (v - hit_min) / (4.0f - hit_min) : 1.0f + 1.5f * std::min(1.0f, (v - 4.0f) / 8.0f);
    return base * (0.6f + 0.8f * r);
}

// Delayed failure: 10-20 % of the hits that do not break a corner leave a hidden crack, which lets go under a heavy
// load from moments to about 25 laps later (Montoya Canada 2006 "no visible damage", stopped moments later; Leclerc
// Monaco 2021, a crack missed in inspection; Alonso Monza 2025, a pushrod failed 23 laps after lap-1 kerb damage).
// Since 0.51.132 the due time is front-loaded (r squared, the same range): half within about 10 minutes of driving,
// so more of them come in one session (uniform, half came after 19 minutes and none in the test logs).
inline float crack_chance(float severity) { return 0.10f + 0.10f * severity; }
inline uint64_t crack_delay_ms(float r)
{
    float x = std::clamp(r, 0.0f, 1.0f);
    return 5000 + uint64_t(x * x * 2245000.0f);
}
// A due crack also lets go under ordinary loads (braking, cornering) when no heavy load came within a minute.
constexpr uint64_t crack_grace_ms = 60000;

// Of the solid hits that break a corner, this share lets go at once: 30 % up to 4 m/s, rising to all from 12 m/s.
// The rest break the corner without letting go yet (failing): it holds for now and fails later, while driving, like a
// part cracked through (Montoya Canada 2006). A report after 0.51.131: 'i get suspension damage but not to a point where
// its damaged hard enough to limp and or that the suspension might collaps while driving. the car is always dead'.
// A kerb or road strike that breaks a damaged corner, a corner with a crack in it and one with no health left go at
// once.
inline float break_now_share(float v)
{
    return 0.3f + 0.7f * std::clamp((v - 4.0f) / (hit_always - 4.0f), 0.0f, 1.0f);
}
// A failing corner's crack is due after 5 s to 3 min of driving, more than half within the first minute (r squared);
// then it lets go at the next heavy load, or crack_grace_ms later anyway. Its health drops to failing_health: the
// weakened warning, and heavy loads can finish it before.
inline uint64_t failing_delay_ms(float r)
{
    float x = std::clamp(r, 0.0f, 1.0f);
    return 5000 + uint64_t(x * x * 175000.0f);
}
constexpr float failing_health = 0.25f;
inline bool crack_gives(const Corner &c, uint64_t now_ms)
{
    return !c.collapsed && c.crack_at && now_ms >= c.crack_at + crack_grace_ms;
}

// Uniform random numbers in [0, 1) for one hit. now: a break below break_now_share lets go at once (the default 0:
// every break at once); when: the due time of a crack or of a failing corner.
struct Rolls {
    float size = 0.5f, sign = 0.5f, brk = 0.99f, crack = 0.99f, when = 0.5f, now = 0.0f;
};

// A hit of size v on corner i. surface: only the road or a kerb (their tilted faces), which never breaks a healthy
// corner but can finish a damaged one. now_ms: the driving clock, for a crack's due time.
inline HitResult apply_hit(Model &m, int i, float v, bool surface, const Rolls &r, uint64_t now_ms)
{
    HitResult h;
    Corner &c = m.c[i];
    if (c.collapsed || !(v >= hit_min))
        return h;
    float s = h.severity = std::max(severity_of(v), 1e-6f);
    // A damaged corner breaks more easily (heavy kerb strikes finish off a bent one).
    float chance = surface ? break_chance(v) * std::min(1.0f, 2.0f * (1.0f - c.health)) : break_chance(v) * (2.0f - c.health);
    h.chance = std::min(1.0f, chance);
    c.health = std::max(0.0f, c.health - (0.05f + 0.55f * s));
    bool breaks = r.brk < h.chance;
    if (c.health <= 0 || (breaks && (surface || c.crack_at || r.now < break_now_share(v)))) {
        h.collapse = c.collapsed = true;
        return h;
    }
    if (breaks) {
        h.failing = true;
        c.health = std::min(c.health, failing_health);
        c.crack_at = now_ms + failing_delay_ms(r.when);
    }
    h.delta = bend_deg(v, r.size) * deg * (r.sign < 0.5f ? -1.0f : 1.0f);
    c.bend = std::clamp(c.bend + h.delta, -bend_max, bend_max);
    if (!c.told_bend && std::fabs(c.bend) >= 0.4f * deg)
        c.told_bend = h.newly_bent = true;
    if (!c.told_weak && c.health < weak_health)
        c.told_weak = h.newly_weak = true;
    if (!c.crack_at && r.crack < crack_chance(s)) {
        c.crack_at = now_ms + crack_delay_ms(r.when);
        h.cracked = true;
    }
    return h;
}

// A heavy kerb strike or landing on corner i. Returns true when the corner collapses: a due crack lets go, a weak
// corner can fail.
inline bool heavy_load(Model &m, int i, float r, uint64_t now_ms)
{
    Corner &c = m.c[i];
    if (c.collapsed)
        return false;
    if (c.crack_at && now_ms >= c.crack_at)
        return c.collapsed = true;
    c.health = std::max(0.0f, c.health - heavy_fatigue);
    float p = base_failure + (c.health < weak_health ? (weak_health - c.health) * weak_failure : 0.0f);
    if (r < p || c.health <= 0)
        return c.collapsed = true;
    return false;
}

// Garage service (practice and qualifying, 0.51.135). A report after 0.51.134: 'i literally went into the pits with a
// broken corner, but the repair instantly clears up upon entering pits'. A corner that holds but took a hit (bent,
// weakened or cracked) is repaired in the garage, and that takes time: 4-8 minutes for a light bend (toe, a track
// rod), 8-14 for a weakened corner (health below 0.8) or one with a crack, 14-20 for one close to breaking (health
// below 0.5: wishbones, pushrod, upright). The crews work side by side: the longest corner plus half of each other
// one. A broken corner is the crash repair's (20-45 minutes). Kerb wear alone (heavy_fatigue, 0.0005 a heavy load)
// needs 200 heavy loads to reach service_health.
constexpr float service_bend = 0.1f * deg, service_health = 0.9f;
inline bool needs_service(const Corner &c)
{
    return !c.collapsed && (std::fabs(c.bend) >= service_bend || c.health < service_health || c.crack_at);
}
// Minutes for one corner, r in [0, 1).
inline float service_minutes(const Corner &c, float r)
{
    float x = std::clamp(r, 0.0f, 1.0f);
    if (c.health < 0.5f)
        return 14.0f + 6.0f * x;
    if (c.health < 0.8f || c.crack_at)
        return 8.0f + 6.0f * x;
    return 4.0f + 4.0f * x;
}
struct ServicePlan {
    float seconds = 0;     // 0: nothing to repair
    uint32_t corners = 0;  // bit i = corner i
    bool crack = false;    // the team finds a crack
};
inline ServicePlan service_plan(const Model &m, const float r[4])
{
    ServicePlan p;
    float longest = 0, sum = 0;
    for (int i = 0; i < 4; ++i) {
        const Corner &c = m.c[i];
        if (!needs_service(c))
            continue;
        float t = service_minutes(c, r[i]);
        p.corners |= 1u << i;
        p.crack |= c.crack_at != 0;
        longest = std::max(longest, t);
        sum += t;
    }
    p.seconds = (longest + 0.5f * (sum - longest)) * 60.0f;
    return p;
}

// The model before each change, on the driving clock (ms of driving, paused time does not count), so a flashback can
// undo the changes of the rewound stretch.
struct History {
    struct Entry {
        uint64_t at;
        Model before;
    };
    std::vector<Entry> e;
    void remember(uint64_t at, const Model &m)
    {
        e.push_back({at, m});
        while (!e.empty() && at - e.front().at > 60000)
            e.erase(e.begin());
    }
    // Undo every change made at or after `since`. True if something was undone.
    bool undo_since(uint64_t since, Model &m)
    {
        size_t k = 0;
        while (k < e.size() && e[k].at < since)
            ++k;
        if (k == e.size())
            return false;
        m = e[k].before;
        e.resize(k);
        return true;
    }
    void clear() { e.clear(); }
};

// ---- Contacts, the damage page value and the corner names (pure) ----
// A contact counts when the impulse changed the car's speed by at least dv_min and it came in at vn_min or more (the
// game's own body damage gate is 1.0/1.0). A scrape along a wall pushes the car only a little per physics step and
// barely moves into it; a polystyrene board breaks at its activation impulse (re/collision-impact.md section 4).
constexpr float dv_min = 0.5f, vn_min = 1.0f;
// The hit size: the normal speed, but only as far as the impulse confirms it. A solid hit takes about 0.4
// (glancing) to 0.9 (square) of the normal speed out of the car (re/collision-impact.md section 8).
inline float contact_speed(float vn, float sum_dv) { return std::min(vn, 2.0f * sum_dv); }

constexpr uint32_t tag4(const char *t)
{
    return uint32_t(uint8_t(t[0])) | uint32_t(uint8_t(t[1])) << 8 | uint32_t(uint8_t(t[2])) << 16 |
           uint32_t(uint8_t(t[3])) << 24;
}
enum ObjectClass : int { obj_solid, obj_soft, obj_surface, obj_harmless };
inline const char *class_name(int c)
{
    return c == obj_soft ? "soft" : c == obj_surface ? "surface" : c == obj_harmless ? "harmless" : "solid";
}
// What the other object is, from its material tag and collision group (re/collision-impact.md section 4):
// - harmless: boards, signs, banners, polystyrene and cones break away ("lightweight, harmless pieces", FIA Appendix
//   O), never suspension damage. Also the Breakables collision groups 0x14 (polystyrene, plastic), 0x15 (broken,
//   debris) and 0x18 (signs).
// - surface: road, kerbs, grass, gravel, sand: their tilted faces pass the game's floor filter.
// - soft: tyre walls and TecPro give way (0.6 of the hit), tyre against tyre as well.
// - solid: walls, Armco, concrete, everything else.
inline int classify(uint32_t other, uint32_t own, uint8_t group)
{
    for (const char *t : {"SIGN", "POLY", "PLAS", "CONE"})
        if (other == tag4(t))
            return obj_harmless;
    if (group == 0x14 || group == 0x15 || group == 0x18)
        return obj_harmless;
    uint32_t three = other & 0xffffff;
    for (const char *t : {"AST", "DRT", "DDT", "PDT", "RDT", "MDG", "HDG", "GRS", "SDT", "RMB", "LAT", "MKN", "BUM"})
        if (three == (tag4(t) & 0xffffff))
            return obj_surface;
    if (other == tag4("CON+") || other == tag4("CON*"))
        return obj_surface;
    for (const char *t : {"TYRW", "DTYR", "DRUB", "RUBB"})
        if (other == tag4(t))
            return obj_soft;
    if (other == tag4("TYRE") && own == tag4("TYRE"))
        return obj_soft;
    return obj_solid;
}
inline float class_scale(int c) { return c == obj_soft ? 0.6f : c == obj_surface ? 0.35f : c == obj_harmless ? 0.0f : 1.0f; }
// A hit on the front or rear face of the tyre (normal within 45 deg of the car's long axis: wall ends, barrier
// corners, another car's wheel) loads the toe link or track rod in its weak direction: it counts twice (Albon and
// Bearman Baku 2025, Hamilton Baku 2016, Verstappen Monaco 2018: small clips that broke the corner at once).
constexpr float face_cos = 0.7071f;

// The tag as text (4 characters).
inline void tag_text(uint32_t t, char out[5])
{
    for (int k = 0; k < 4; ++k) {
        char ch = char((t >> (8 * k)) & 0xff);
        out[k] = ch >= 32 && ch < 127 ? ch : '?';
    }
    out[4] = 0;
}

// One hit comes as several contacts over a few physics steps. A hit sums their speed change and keeps their highest
// normal speed within `window` ms; after it, contacts count as a new hit only after `quiet` ms without contact or when
// clearly harder than the last hit.
struct HitGate {
    static constexpr uint64_t window = 250, quiet = 750;
    static constexpr float harder = 3.0f;
    struct Hit {
        float v = 0, sum_dv = 0, peak_vn = 0;
        uint32_t tag = 0;
        int contacts = 0;
        bool solid = false, face = false;
    };
    Hit cur;
    float last_hit = 0;
    uint64_t open_until = 0, last_contact = 0;
    void contact(uint64_t now, float dv, float vn, uint32_t tag = 0, bool solid = true, int contacts = 1, bool face = false)
    {
        float v = contact_speed(vn, dv);
        bool fresh = !last_contact || now - last_contact > quiet || v > last_hit + harder;
        last_contact = now;
        if (!open_until) {
            if (!fresh)
                return;
            open_until = now + window;
        }
        if (vn >= cur.peak_vn)
            cur.tag = tag;
        cur.sum_dv += dv;
        cur.peak_vn = std::max(cur.peak_vn, vn);
        cur.contacts += contacts;
        cur.solid |= solid;
        cur.face |= face;
    }
    // A finished hit (v > 0), or none (v == 0).
    Hit take(uint64_t now)
    {
        if (!open_until || now < open_until)
            return {};
        Hit h = cur;
        h.v = contact_speed(cur.peak_vn, cur.sum_dv);
        open_until = 0;
        cur = {};
        last_hit = h.v;
        return h;
    }
    void clear() { *this = HitGate{}; }
};

// What the damage MFD page shows for a corner (0..1, the game shows max(its own value, this)): the lost health, or
// half the bend range for a corner that is only bent; a collapsed corner shows 100 % through the game itself.
inline float mfd_value(const Corner &c)
{
    if (c.collapsed)
        return 1.0f;
    return std::clamp(std::max(1.0f - c.health, std::fabs(c.bend) / bend_max * 0.5f), 0.0f, 1.0f);
}

inline const char *corner_name(int i)
{
    static const char *names[4] = {"Rear-left", "Rear-right", "Front-left", "Front-right"};
    return i >= 0 && i < 4 ? names[i] : "?";
}
inline const char *corner_tag(int i)
{
    static const char *tags[4] = {"RL", "RR", "FL", "FR"};
    return i >= 0 && i < 4 ? tags[i] : "?";
}

// ---- Runtime ----
bool enabled = false;       // suspension_damage at game start and both hooks in place
bool test_enabled = false;  // debug_suspension_test not off at game start
bool step_hooked = false, hit_hooked = false;
std::atomic<uintptr_t> player_sys{0};  // [player car + 0x580], refreshed every frame
// For the damage panel (render thread): 2 bits per corner RL, RR, FL, FR: 0 fine, 1 bent, 2 weakened, 3 collapsed.
std::atomic<uint32_t> corner_view{0};
std::atomic<uintptr_t> hit_car{0};     // the player car while the feature runs
std::atomic<float> toe_offset[4];      // radians, applied in the geometry step
// Contacts that counted since the last frame, per corner (physics thread adds, frame thread takes): summed speed
// change and peak normal speed (both already scaled by the object class), the other object's tag at the peak.
struct Pending {
    std::atomic<float> dv{0}, vn{0};
    std::atomic<uint32_t> tag{0}, contacts{0}, solid{0}, face{0};
    void clear()
    {
        dv = vn = 0;
        tag = contacts = solid = face = 0;
    }
};
Pending pending[4];
std::atomic<float> rate_peak[4];       // highest |travel rate| since the last frame
std::atomic<uint32_t> collapse_request{0};  // corner bits, applied on the game thread
std::atomic<uint64_t> drive_us{0};          // driving clock: sum of the geometry step dt
std::atomic<uint64_t> steps{0};
// Contact hook calls (every car, your car), for the 30 s stats: its cost is known, its rate not (0.51.117 review).
std::atomic<uint32_t> hook_calls{0}, hook_calls_own{0};
std::atomic<const char *> reset_request{nullptr};

// Physics thread only.
float written_sin[4] = {2, 2, 2, 2};
float last_travel[4] = {};
bool travel_known = false;

// Frame thread only.
Model model;
History history;
HitGate gate[4];
uint64_t heavy_after[4] = {};
uint32_t flashback_seen = 0;
bool garage_reset_done = false;
float stat_rate[4] = {}, stat_hit[4] = {};
int stat_heavy[4] = {}, stat_hits[4] = {};
float stat_out[4] = {};  // the game's damage from outside its suspension handler
uint64_t stat_next = 0;

// AI cars, indexed by vehicle. car/sys are registered from the game's vehicle update; 0 = not known or the player.
namespace ai {
constexpr int n = 32;
bool enabled = false;
std::atomic<uintptr_t> car[n], sys[n];
std::atomic<float> toe[n][4];             // radians, applied in the geometry step
Pending pending[n][4];                    // contacts that counted since the last frame
std::atomic<uint32_t> collapse_req[n];    // corner bits, applied on the game thread
std::atomic<uint32_t> terminal_seen{0};   // vehicles the game reported terminally damaged
float written_sin[n][4];                  // physics thread only
Model model[n];                           // frame thread only
History history[n];
HitGate gate[n][4];
uint64_t retire_at[n];                    // fallback retirement tick, 0 = none
int index_of_sys(uintptr_t s)
{
    for (int i = 0; i < n; ++i)
        if (sys[i].load(std::memory_order_relaxed) == s)
            return i;
    return -1;
}
int index_of_car(uintptr_t c)
{
    for (int i = 0; i < n; ++i)
        if (car[i].load(std::memory_order_relaxed) == c)
            return i;
    return -1;
}
}  // namespace ai

// Kerb strikes and landings: |travel rate| above this, travel units per second. Placeholder until the 30 s load stats
// in the log show the range.
constexpr float heavy_rate = 1.5f;

void store_max(std::atomic<float> &a, float v)
{
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}
void add_float(std::atomic<float> &a, float v)
{
    float cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
    }
}

// What a contact was, as read on the physics thread.
struct Contact {
    float j = 0, dv = 0, vn = 0, vn_cm = 0, dt = 0, face = 0;  // impulse, J/mass, game's normal speed, |v_cm . n|
    uint32_t tag = 0, own = 0;
    uint8_t group = 0xff, veh = 0xff;
    bool entity = false, dynamic = false;
    // The other body is this car's own (0.51.114 log: a broken wheel touching its own car, hundreds a second with
    // no impulse, hid every other contact in the log).
    bool self = false;
    // The game's own suspension damage from this contact (F1SuspensionDamage +0x130.. before and after it; 1.0 is the
    // game's wheel detach): the total and the corner that took the most. Player only.
    float dd = 0;
    int8_t dd_corner = -1;
    // Where on the car: m from its centre along the forward and the left axis; the normal's up part (above floor_up:
    // the floor on the ground).
    float at_fwd = 0, at_left = 0, n_up = 0;
    bool body = false;  // on no wheel the game named: the nose, the body, a wing, the floor
    // The own shape is a wheel proxy all the same (1 front, 2 rear): the game names no wheel for a contact within
    // 0.5 m of the centre line, one whose normal points up, or with its puncture bits off.
    int8_t proxy = 0;
};
// The player's contacts for the calibration log: written on the physics thread, read on the frame thread (seqlock
// slots, the newest 32 kept).
struct ContactRecord {
    Contact c;
    int8_t corner = -1;
    uint8_t cls = 0;
    bool counted = false;
};
struct ContactLog {
    static constexpr uint32_t n = 32;
    struct Slot {
        std::atomic<uint32_t> seq{0};
        ContactRecord r;
    };
    Slot slot[n];
    std::atomic<uint32_t> head{0};
    void put(const ContactRecord &r)
    {
        uint32_t i = head.fetch_add(1, std::memory_order_relaxed);
        Slot &sl = slot[i % n];
        sl.seq.store(2 * i + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        sl.r = r;
        sl.seq.store(2 * i + 2, std::memory_order_release);
    }
    bool get(uint32_t i, ContactRecord &out)
    {
        Slot &sl = slot[i % n];
        uint32_t a = sl.seq.load(std::memory_order_acquire);
        if (a != 2 * i + 2)
            return false;
        out = sl.r;
        std::atomic_thread_fence(std::memory_order_acquire);
        return sl.seq.load(std::memory_order_relaxed) == a;
    }
};
ContactLog contact_log;
uint32_t contact_log_read = 0;  // frame thread
// Contacts that did not count, since the last 30 s stats: harmless objects (the peak normal speed, with its tag),
// the rest (too little impulse or normal speed: the one with the most impulse, its normal speed and tag) and
// contacts with the car's own body.
std::atomic<uint32_t> ignored_n{0}, light_n{0}, self_n{0}, ignored_tag{0}, light_tag{0};
std::atomic<float> ignored_vn{0}, light_vn{0}, light_dv{-1};
// Body contacts since the last 30 s stats: off the ground (the one with the most impulse: its normal speed, own and
// other tag) and on it (the normal up: the floor, or a wheel on a kerb top; the most impulse).
std::atomic<uint32_t> body_n{0}, floor_n{0}, body_tag{0}, body_own{0};
std::atomic<float> body_dv{-1}, body_vn{0}, floor_dv{-1};
// The game's suspension damage that rose outside its suspension handler (physics thread adds, frame thread takes).
std::atomic<float> out_pending[4];

float random01()
{
    static std::mt19937 rng{static_cast<uint32_t>(GetTickCount64() * 2654435761u) ^ GetCurrentProcessId()};
    return float(rng() >> 8) / float(1u << 24);
}

uint64_t drive_ms() { return drive_us.load() / 1000; }

void publish()
{
    for (int i = 0; i < 4; ++i)
        toe_offset[i] = enabled ? model.c[i].bend : 0.0f;
}

// Any thread: the car was repaired or the session restarted.
void request_reset(const char *why) { reset_request = why; }

void reset(const char *why)
{
    bool damaged = false;
    for (const Corner &c : model.c)
        damaged |= c.bend != 0 || c.health < 1 || c.collapsed;
    model = Model{};
    history.clear();
    for (int i = 0; i < 4; ++i) {
        gate[i].clear();
        pending[i].clear();
        rate_peak[i] = 0;
    }
    collapse_request = 0;
    publish();
    if (damaged)
        swofty::log("[td-probe] suspension: all corners repaired (%s)", why);
}

// A new session or a race restart: forget the AI cars and their damage.
void ai_reset()
{
    for (int v = 0; v < ai::n; ++v) {
        ai::model[v] = Model{};
        ai::history[v].clear();
        ai::retire_at[v] = 0;
        ai::collapse_req[v] = 0;
        for (int i = 0; i < 4; ++i) {
            ai::gate[v][i].clear();
            ai::pending[v][i].clear();
            ai::toe[v][i] = 0;
        }
    }
    ai::terminal_seen = 0;
}

// ---- Debug test (Ctrl+Shift+F8, front-left wheel) ----
namespace test {
std::atomic<int> mode{0};      // 1 toe, 2 camber, 3 damage
std::atomic<float> offset{0};  // radians, or the damage target 0..1
float written_camber = 0;      // physics thread only
float damage_before = -1;      // physics thread only
std::atomic<bool> restore_damage{false};
int mode_of(std::string_view s) { return s == "toe" ? 1 : s == "camber" ? 2 : s == "damage" ? 3 : 0; }
constexpr float toe_stages[] = {0, 0.25f, 1.0f, 3.0f};
constexpr float camber_stages[] = {0, 1.5f, 4.0f};
constexpr float damage_stages[] = {0, 0.3f, 0.6f, 0.79f};
}  // namespace test

// ---- Geometry step hook (physics thread) ----
using step_fn = uint64_t (*)(void *, float);
step_fn orig_step = nullptr;
constexpr uint8_t step_prologue[] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00};
constexpr const char *step_sig =
    "41 57 41 56 56 57 53 48 81 EC F0 00 00 00 C5 78 29 BC 24 E0 00 00 00 C5 78 29 B4 24 D0 00 00 00 C5 78 29 AC 24 "
    "C0 00 00 00 C5 78 29 A4 24 B0 00 00 00 C5 78 29 9C 24 A0 00 00 00 C5 78 29 94 24 90 00 00 00 C5 78 29 8C 24 80 "
    "00 00 00 C5 78 29 44 24 70 C5 F8 29 7C 24 60 C5 F8 29 74 24 50 48 89 CE 48 8B 81 90 00 00 00 48 8D 88 F0 15 00 00";

// Rotate the wheel heading the step just computed by `off` radians, never the same heading twice.
void rotate_heading(uintptr_t w, float off, float &written)
{
    float sin_h = 0, cos_h = 1;
    if (off == 0 || !guarded_read(w + 0x1f8, sin_h) || !guarded_read(w + 0x1fc, cos_h) || sin_h == written)
        return;
    float c = std::cos(off), sn = std::sin(off);
    written = sin_h * c + cos_h * sn;
    *reinterpret_cast<volatile float *>(w + 0x1fc) = cos_h * c - sin_h * sn;
    *reinterpret_cast<volatile float *>(w + 0x1f8) = written;
}

void ai_step(uintptr_t s)
{
    int v = ai::index_of_sys(s);
    if (v < 0)
        return;
    for (int i = 0; i < 4; ++i) {
        float off = ai::toe[v][i].load(std::memory_order_relaxed);
        uintptr_t w = 0;
        if (off != 0 && guarded_read(s + 0x70 + i * 8, w) && w)
            rotate_heading(w, off, ai::written_sin[v][i]);
    }
}

uint64_t on_step(void *sys, float dt)
{
    uint64_t r = orig_step(sys, dt);
    uintptr_t s = reinterpret_cast<uintptr_t>(sys);
    if (!s)
        return r;
    if (s != player_sys.load(std::memory_order_relaxed)) {
        if (ai::enabled)
            ai_step(s);
        return r;
    }
    steps.fetch_add(1, std::memory_order_relaxed);
    bool sane_dt = dt > 0.0001f && dt < 0.2f;
    if (sane_dt)
        drive_us.fetch_add(uint64_t(dt * 1e6f), std::memory_order_relaxed);
    int tm = test::mode.load(std::memory_order_relaxed);
    float toff = test::offset.load(std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) {
        uintptr_t w = 0;
        float travel = 0, sin_h = 0, cos_h = 1;
        if (!guarded_read(s + 0x70 + i * 8, w) || !w || !guarded_read(w + 0x168, travel) ||
            !guarded_read(w + 0x1f8, sin_h) || !guarded_read(w + 0x1fc, cos_h))
            continue;
        if (travel_known && sane_dt)
            store_max(rate_peak[i], std::fabs(travel - last_travel[i]) / dt);
        last_travel[i] = travel;
        float off = toe_offset[i].load(std::memory_order_relaxed) + (i == 2 && tm == 1 ? toff : 0.0f);
        // Rotate only a heading the game computed in this step, never the same one twice.
        if (off != 0 && sin_h != written_sin[i]) {
            float c = std::cos(off), sn = std::sin(off);
            written_sin[i] = sin_h * c + cos_h * sn;
            *reinterpret_cast<volatile float *>(w + 0x1fc) = cos_h * c - sin_h * sn;
            *reinterpret_cast<volatile float *>(w + 0x1f8) = written_sin[i];
        }
        if (i != 2 || !tm)
            continue;
        float camber = 0, damage = 0;
        if (tm == 2 && toff != 0 && guarded_read(w + 0x154, camber) && camber != test::written_camber) {
            test::written_camber = camber + toff;
            *reinterpret_cast<volatile float *>(w + 0x154) = test::written_camber;
        }
        if (guarded_read(w + 0x98, damage)) {
            if (tm == 3 && toff > 0 && damage < toff) {
                if (test::damage_before < 0)
                    test::damage_before = damage;
                *reinterpret_cast<volatile float *>(w + 0x98) = toff;
            }
            if (test::restore_damage.exchange(false) && test::damage_before >= 0) {
                *reinterpret_cast<volatile float *>(w + 0x98) = test::damage_before;
                test::damage_before = -1;
            }
        }
    }
    travel_known = true;
    return r;
}

// ---- Collision hook (physics thread) ----
using hit_fn = uint64_t (*)(void *, void *);
hit_fn orig_hit = nullptr;
constexpr uint8_t hit_prologue[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x40, 0xC5, 0xF8, 0x29,
                                    0x74, 0x24, 0x30, 0x48, 0x8B, 0x41, 0x08};
constexpr const char *hit_sig = "56 57 53 48 83 EC 40 C5 F8 29 74 24 30 48 8B 41 08 F6 80 30 01 00 00 01 75 0E C5 F8 28 "
                                "74 24 30 48 83 C4 40 5B 5F 5E C3";

// Read a contact the game just handled for `car`. All reads guarded: the other object's chain may end anywhere.
bool read_contact(uintptr_t car, uintptr_t contact, Contact &ci)
{
    uint8_t c[0x48];
    uintptr_t chassis = 0;
    float ch[0x80 / 4];  // chassis +0x150..+0x1d0
    if (!guarded_copy(contact, c, sizeof c) || !guarded_read(car + 0x558, chassis) || !chassis ||
        !guarded_copy(chassis + 0x150, ch, sizeof ch))
        return false;
    auto ptr = [&](int off) {
        uintptr_t p;
        memcpy(&p, c + off, 8);
        return p;
    };
    float j = 0, p[4] = {}, nrm[4] = {};
    memcpy(&j, c + 0x38, 4);
    memcpy(&ci.dt, c + 0x3c, 4);
    ci.veh = c[0x40];
    if (!guarded_copy(ptr(0x18), p, sizeof p) || !guarded_copy(ptr(0x20), nrm, sizeof nrm))
        return false;
    uintptr_t mat = ptr(0x30), own = ptr(0x28);
    if (mat)
        guarded_read(mat, ci.tag);
    if (own)
        guarded_read(own, ci.own);
    const float invm = ch[(0x154 - 0x150) / 4], *v = ch + (0x170 - 0x150) / 4, *w = ch + (0x180 - 0x150) / 4,
                *left = ch + (0x190 - 0x150) / 4, *up = ch + (0x1a0 - 0x150) / 4, *fwd = ch + (0x1b0 - 0x150) / 4,
                *pos = ch + (0x1c0 - 0x150) / 4;
    auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto at_point = [&](const float *lin, const float *ang, const float *centre, float out[3]) {
        float r[3] = {p[0] - centre[0], p[1] - centre[1], p[2] - centre[2]};
        out[0] = lin[0] + ang[1] * r[2] - ang[2] * r[1];
        out[1] = lin[1] + ang[2] * r[0] - ang[0] * r[2];
        out[2] = lin[2] + ang[0] * r[1] - ang[1] * r[0];
    };
    float vc[3], vo[3] = {};
    at_point(v, w, pos, vc);
    // The other object's velocity at the point, if it is a moving body (another car, a prop knocked loose). The car's
    // own actor is [Ch+0x340] (the game's listener compares a record body's actor with it to pick the side).
    uintptr_t other = ptr(0x10), entity = 0, shape = 0, body = 0, actor = 0, ob = 0, com = 0, own_actor = 0;
    guarded_read(chassis + 0x340, own_actor);
    if (other && guarded_read(other + 0x10, entity) && guarded_read(other + 0x18, shape) && shape) {
        guarded_read(shape + 0x164, ci.group);
        if (guarded_read(shape + 0x108, body) && body && guarded_read(body + 0x458, actor) && actor &&
            guarded_read(actor + 0x18, ob) && ob) {
            ci.self = own_actor && actor == own_actor;
            float ov[8], oc[4];
            if (guarded_copy(ob + 0x210, ov, sizeof ov) && guarded_read(ob + 0x378, com) && com &&
                guarded_copy(com, oc, sizeof oc)) {
                at_point(ov + 4, ov, oc, vo);
                ci.dynamic = true;
            }
        }
    }
    ci.entity = entity != 0;
    float rel[3] = {vc[0] - vo[0], vc[1] - vo[1], vc[2] - vo[2]};
    ci.j = std::fabs(j);
    ci.dv = ci.j * invm;
    ci.vn = std::fabs(dot(rel, nrm));
    ci.vn_cm = std::fabs(dot(v, nrm));
    ci.face = std::fabs(dot(fwd, nrm));
    const float from_centre[3] = {p[0] - pos[0], p[1] - pos[1], p[2] - pos[2]};
    ci.at_fwd = dot(from_centre, fwd);
    ci.at_left = dot(from_centre, left);
    ci.n_up = dot(nrm, up);
    for (float x : {ci.dv, ci.vn, ci.vn_cm, ci.face, ci.at_fwd, ci.at_left, ci.n_up})
        if (!std::isfinite(x) || x > 500 || x < -500)
            return false;
    return true;
}

// Physics thread: count a contact on the corners it hit (corner[i]) into dest, or note why not. vehicle < 0: the
// player (statistics and the calibration log).
void note_contact(const Contact &ci, Pending *dest, const bool corner[4], int vehicle)
{
    if (ci.self) {
        if (vehicle < 0)
            self_n.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    int cls = classify(ci.tag, ci.own, ci.group);
    bool face = cls != obj_surface && ci.face > face_cos;
    float k = class_scale(cls) * (face ? 2.0f : 1.0f);
    bool counted = cls != obj_harmless && ci.dv >= dv_min && ci.vn >= vn_min;
    if (counted)
        for (int i = 0; i < 4; ++i) {
            if (!corner[i])
                continue;
            Pending &p = dest[i];
            add_float(p.dv, ci.dv * k);
            if (ci.vn * k >= p.vn.load(std::memory_order_relaxed))
                p.tag.store(ci.tag, std::memory_order_relaxed);
            store_max(p.vn, ci.vn * k);
            p.contacts.fetch_add(1, std::memory_order_relaxed);
            if (cls != obj_surface)
                p.solid.store(1, std::memory_order_relaxed);
            if (face)
                p.face.store(1, std::memory_order_relaxed);
        }
    if (vehicle >= 0)
        return;
    if (!counted && cls == obj_harmless) {
        ignored_n.fetch_add(1, std::memory_order_relaxed);
        if (ci.vn > ignored_vn.load(std::memory_order_relaxed)) {
            ignored_tag.store(ci.tag, std::memory_order_relaxed);
            store_max(ignored_vn, ci.vn);
        }
    } else if (!counted) {
        light_n.fetch_add(1, std::memory_order_relaxed);
        if (ci.dv > light_dv.load(std::memory_order_relaxed)) {
            light_tag.store(ci.tag, std::memory_order_relaxed);
            light_vn.store(ci.vn, std::memory_order_relaxed);
            store_max(light_dv, ci.dv);
        }
    }
    // The log ring: counted contacts, any the game itself took damage from, and the ones with some impulse or a fast
    // normal speed (boards).
    if (counted || ci.dd > 0 || ci.dv >= 0.05f || ci.vn >= 3.0f) {
        ContactRecord r;
        r.c = ci;
        for (int i = 0; i < 4; ++i)
            if (corner[i]) {
                r.corner = int8_t(i);
                break;
            }
        r.cls = uint8_t(cls);
        r.counted = counted;
        contact_log.put(r);
    }
}

// Contacts on no wheel, player only (0.51.116, for calibration; they do not count yet). Read from body_j_min up: 150
// kg m/s is 0.19 m/s for an 800 kg car, below any hit. The log ring gets the ones from body_dv_log up off the ground
// and from floor_dv_log up on it (the floor on a kerb or a landing).
constexpr float body_j_min = 150.0f, body_dv_log = 0.3f, floor_dv_log = 1.0f, floor_up = 0.8f;
void note_body_contact(const Contact &ci)
{
    if (ci.self) {
        self_n.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    bool floor = ci.n_up > floor_up;
    if (floor) {
        floor_n.fetch_add(1, std::memory_order_relaxed);
        store_max(floor_dv, ci.dv);
    } else {
        body_n.fetch_add(1, std::memory_order_relaxed);
        if (ci.dv > body_dv.load(std::memory_order_relaxed)) {
            body_tag.store(ci.tag, std::memory_order_relaxed);
            body_own.store(ci.own, std::memory_order_relaxed);
            body_vn.store(ci.vn, std::memory_order_relaxed);
            store_max(body_dv, ci.dv);
        }
    }
    if (ci.dv < (floor ? floor_dv_log : body_dv_log))
        return;
    ContactRecord r;
    r.c = ci;
    r.c.body = true;
    r.cls = uint8_t(classify(ci.tag, ci.own, ci.group));
    contact_log.put(r);
}

// Is the contact's own shape a wheel proxy ([car+0x5b8] +0x40..+0x78, re/collision-impact.md section 6: front
// +0x50/+0x58/+0x70/+0x78, rear +0x40/+0x48/+0x60/+0x68)? 1 front, 2 rear, 0 not or unreadable.
int wheel_proxy(uintptr_t car, uintptr_t contact)
{
    uintptr_t wp = 0, own = 0, slots[8] = {};
    if (!guarded_read(car + 0x5b8, wp) || !wp || !guarded_read(contact + 8, own) || !own ||
        !guarded_copy(wp + 0x40, slots, sizeof slots))
        return 0;
    for (int k = 0; k < 8; ++k)
        if (slots[k] == own)
            return k & 2 ? 1 : 2;
    return 0;
}

// Physics thread, player only: the game's suspension damage that rose between two calls of its handler came from
// outside it: the front wing's overflow into FL/FR (re/collision-impact.md section 6), which the 0.51.115 log
// showed as FL 57.6 after a nose hit with no wheel contact. d1 null: unreadable after the call.
uintptr_t out_comp = 0;  // physics thread
float out_last[4] = {};
void note_outside_damage(uintptr_t comp, const float d0[4], const float *d1)
{
    if (comp == out_comp)
        for (int i = 0; i < 4; ++i) {
            float x = d0[i] - out_last[i];
            if (std::isfinite(x) && x > 0.001f && x < 1000)
                add_float(out_pending[i], x);
        }
    out_comp = d1 ? comp : 0;
    if (d1)
        memcpy(out_last, d1, sizeof out_last);
}

uint64_t on_hit(void *component, void *contact)
{
    uintptr_t car = hit_car.load(std::memory_order_relaxed), sys = player_sys.load(std::memory_order_relaxed),
              owner = 0;
    uintptr_t w[4] = {};
    float impact[4] = {}, scrape[4] = {};
    guarded_read(reinterpret_cast<uintptr_t>(component) + 8, owner);
    bool mine = car && sys && owner == car;
    hook_calls.fetch_add(1, std::memory_order_relaxed);
    if (mine)
        hook_calls_own.fetch_add(1, std::memory_order_relaxed);
    Pending *dest = pending;
    int vehicle = -1;
    if (!mine && ai::enabled && owner) {
        int v = ai::index_of_car(owner);
        if (v >= 0 && (sys = ai::sys[v].load(std::memory_order_relaxed)) != 0) {
            car = owner;
            dest = ai::pending[v];
            vehicle = v;
            mine = true;
        }
    }
    if (mine)
        for (int i = 0; i < 4; ++i)
            if (guarded_read(sys + 0x70 + i * 8, w[i]) && w[i]) {
                guarded_read(w[i] + 0x24, impact[i]);
                guarded_read(w[i] + 0x2c, scrape[i]);
            }
    // The game's own suspension damage per corner (RL RR FL FR at comp+0x130, plain floats) around the call.
    uintptr_t comp = reinterpret_cast<uintptr_t>(component);
    float d0[4] = {};
    bool have_d = mine && vehicle < 0 && guarded_copy(comp + 0x130, d0, sizeof d0);
    uint64_t r = orig_hit(component, contact);
    if (!mine)
        return r;
    // The corner: the wheel whose contact speed the game just wrote (+0x24 another car, +0x2c anything else).
    bool corner[4] = {};
    bool any = false;
    for (int i = 0; i < 4; ++i) {
        float a = 0, b = 0;
        if (!w[i] || !guarded_read(w[i] + 0x24, a) || !guarded_read(w[i] + 0x2c, b))
            continue;
        if ((a != impact[i] && a > 0) || (b != scrape[i] && b > 0))
            corner[i] = any = true;
    }
    float dd[4] = {}, d1[4] = {}, dsum = 0;
    int dmax = -1;
    bool have_d1 = have_d && guarded_copy(comp + 0x130, d1, sizeof d1);
    if (have_d)
        note_outside_damage(comp, d0, have_d1 ? d1 : nullptr);
    if (have_d1)
        for (int i = 0; i < 4; ++i) {
            float x = d1[i] - d0[i];
            if (!std::isfinite(x) || x <= 0 || x > 100)
                continue;
            dd[i] = x;
            dsum += x;
            if (dmax < 0 || x > dd[dmax])
                dmax = i;
        }
    if (!any && dmax >= 0) {  // the game damaged a corner but wrote no contact speed (its damage bits): take that one
        corner[dmax] = true;
        any = true;
    }
    Contact ci;
    if (any) {
        if (read_contact(car, reinterpret_cast<uintptr_t>(contact), ci)) {
            ci.dd = dsum;
            ci.dd_corner = int8_t(dmax);
            note_contact(ci, dest, corner, vehicle);
        }
    } else if (vehicle < 0) {
        // On no wheel. The contact is the dispatcher's stack local for this call: J read directly first, so the
        // light ones (most of them) cost no further reads.
        float j = std::fabs(*reinterpret_cast<const float *>(reinterpret_cast<uintptr_t>(contact) + 0x38));
        if (j >= body_j_min && read_contact(car, reinterpret_cast<uintptr_t>(contact), ci)) {
            ci.proxy = int8_t(wheel_proxy(car, reinterpret_cast<uintptr_t>(contact)));
            note_body_contact(ci);
        }
    }
    return r;
}

// The game's own suspension damage of the player's car per corner (RL RR FL FR), -1 if unreadable. Frame thread.
bool game_damage(float d[4])
{
    uintptr_t car = hit_car.load(), comp = 0, back = 0;
    for (int i = 0; i < 4; ++i)
        d[i] = -1;
    return car && guarded_read(car + 0x5d0, comp) && comp && guarded_read(comp + 8, back) && back == car &&
           guarded_copy(comp + 0x130, d, 4 * sizeof(float));
}

// The game's stock front and rear suspension damage scales: the shipping values (ferrari.vtf; every logged session
// without another damage addon has them), as Heavier Damage's "Stock (EA)" set has them too. While the model runs,
// the addon sets the game's own scales back to these on the cars the model covers (apply_game_suspension_layers).
constexpr float stock_front_scale = 0.001f, stock_rear_scale = 0.0001f;

// Once per session: the game's suspension damage settings (XOR-keyed floats, re/collision-impact.md section 6), to
// compare its damage with the addon's hits.
void log_game_settings()
{
    static uintptr_t logged = 0;
    uintptr_t car = hit_car.load(), comp = 0, back = 0;
    if (!car || car == logged || !guarded_read(car + 0x5d0, comp) || !comp || !guarded_read(comp + 8, back) ||
        back != car)
        return;
    float v[5] = {};
    for (int k = 0; k < 5; ++k) {
        uint32_t a = 0, b = 0;
        if (!guarded_read(comp + 0x28 + 0x10 * k, a) || !guarded_read(comp + 0x30 + 0x10 * k, b))
            return;
        v[k] = std::bit_cast<float>(a ^ b);
    }
    logged = car;
    char note[256] = "";
    if (v[1] > 2 * stock_front_scale || v[2] > 2 * stock_rear_scale)
        snprintf(note, sizeof note, "; %.0fx (front) and %.0fx (rear) the game's stock 0.001 and 0.0001: another addon "
                 "raised them (Heavier Damage: Suspension & wheel damage) and the stock layers are not on, so scrapes "
                 "and light car contacts add up to broken corners", double(v[1] / stock_front_scale),
                 double(v[2] / stock_rear_scale));
    swofty::log("[td-probe] suspension: the game's own suspension damage: speed threshold %.2f m/s, front scale %.5f, "
                "rear scale %.5f, wheel detach at %.2f, parts detach at %.2f%s",
                double(v[0]), double(v[1]), double(v[2]), double(v[3]), double(v[4]), note);
}

// ---- Damage MFD page ----
// The car damage panel (mfd.attacher.car_damage_panel, vtable 0x1456ed810, update 0x141530970) shows one value per
// corner, wheels sys+0x70+i*8 in the order RL, RR, FL, FR (loop index r14, wheel in rax, car = [wheel]):
//   1.0 if the wheel is detached or its suspension broken (wheel+0x20 & 3), else min(wheel+0x88, 1.0)
// times 100 as the corner's percentage, above a threshold with the warning icon. At 0x141530c6a the read
// `vmovss xmm0,[rax+0x88]; vminss xmm0,xmm0,xmm10` (13 bytes, no jump lands inside) becomes a jump to a stub that
// also takes the addon's value for the player's car: max(wheel+0x88, value[r14]). rcx is free there (next written
// at 0x141530c8d before any read) and the flags are set again at 0x141530c7d.
constexpr const char *mfd_sig = "80 79 38 00 C5 78 29 D0 75 1B 0F 1F 40 00 F6 40 20 03 C5 78 29 D0 75 0D C5 FA 10 80 88 "
                                "00 00 00 C4 C1 7A 5D C2";
constexpr size_t mfd_patch_at = 24;
constexpr uint8_t mfd_original[13] = {0xC5, 0xFA, 0x10, 0x80, 0x88, 0x00, 0x00, 0x00, 0xC4, 0xC1, 0x7A, 0x5D, 0xC2};
volatile uintptr_t *mfd_car = nullptr;  // the player's car, 0 = show nothing
volatile float *mfd_values = nullptr;   // per corner

uint8_t *alloc_near(uintptr_t near_to, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t step = si.dwAllocationGranularity;
    uintptr_t lo = near_to > 0x7ff00000 ? near_to - 0x7ff00000 : step;
    for (uintptr_t at = (near_to & ~(step - 1)); at > lo; at -= step)
        if (auto *p = static_cast<uint8_t *>(VirtualAlloc(reinterpret_cast<void *>(at), size, MEM_COMMIT | MEM_RESERVE,
                                                          PAGE_EXECUTE_READWRITE)))
            return p;
    return nullptr;
}

bool install_mfd()
{
    uintptr_t at = 0;
    int n = swofty::api->scan(".xtls", mfd_sig, &at);
    auto *site = reinterpret_cast<uint8_t *>(at + mfd_patch_at);
    if (n != 1 || memcmp(site, mfd_original, sizeof mfd_original) != 0) {
        swofty::log("[td-probe] suspension: damage MFD page signature has %d hits, not shown there", n);
        return false;
    }
    uint8_t *stub = alloc_near(reinterpret_cast<uintptr_t>(site), 4096);
    if (!stub) {
        swofty::log("[td-probe] suspension: no memory near the damage MFD page code, not shown there");
        return false;
    }
    uint8_t *data = stub + 64;  // car pointer, then 4 floats
    uint8_t code[] = {
        0xC5, 0xFA, 0x10, 0x80, 0x88, 0x00, 0x00, 0x00,  // vmovss xmm0,[rax+0x88]
        0x48, 0x8B, 0x08,                                // mov rcx,[rax]          (the car)
        0x48, 0x3B, 0x0D, 0, 0, 0, 0,                    // cmp rcx,[rip+car]
        0x75, 0x0D,                                      // jne done
        0x48, 0x8D, 0x0D, 0, 0, 0, 0,                    // lea rcx,[rip+values]
        0xC4, 0xA1, 0x7A, 0x5F, 0x04, 0xB1,              // vmaxss xmm0,xmm0,[rcx+r14*4]
        0xC4, 0xC1, 0x7A, 0x5D, 0xC2,                    // done: vminss xmm0,xmm0,xmm10
        0xE9, 0, 0, 0, 0,                                // jmp back
    };
    auto rel = [&](size_t field, const uint8_t *target) {
        int32_t d = int32_t(target - (stub + field + 4));
        memcpy(code + field, &d, 4);
    };
    rel(14, data);
    rel(23, data + 8);
    rel(39, site + sizeof mfd_original);
    memset(data, 0, 24);
    memcpy(stub, code, sizeof code);
    mfd_car = reinterpret_cast<volatile uintptr_t *>(data);
    mfd_values = reinterpret_cast<volatile float *>(data + 8);
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof code);
    uint8_t patch[13] = {0xE9, 0, 0, 0, 0, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};  // jmp stub; 8-byte nop
    int32_t to_stub = int32_t(stub - (site + 5));
    memcpy(patch + 1, &to_stub, 4);
    DWORD old;
    if (!VirtualProtect(site, sizeof patch, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(site, patch, sizeof patch);
    VirtualProtect(site, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof patch);
    swofty::log("[td-probe] suspension: damage MFD page shows the suspension damage (patched %p, stub %p)", site, stub);
    return true;
}

void publish_mfd(uintptr_t car)
{
    if (!mfd_values)
        return;
    for (int i = 0; i < 4; ++i)
        mfd_values[i] = enabled && car ? mfd_value(model.c[i]) : 0.0f;
    *mfd_car = enabled ? car : 0;
}

void install(bool feature, std::string_view test_setting, bool ai_feature = false)
{
    test_enabled = test::mode_of(test_setting) != 0;
    if (!feature && !test_enabled)
        return;
    step_hooked = detour("suspension: wheel geometry step", step_sig, step_prologue, sizeof step_prologue,
                         reinterpret_cast<void *>(&on_step), reinterpret_cast<void **>(&orig_step));
    if (feature)
        hit_hooked = detour("suspension: wheel collision", hit_sig, hit_prologue, sizeof hit_prologue,
                            reinterpret_cast<void *>(&on_hit), reinterpret_cast<void **>(&orig_hit));
    enabled = feature && step_hooked && hit_hooked;
    ai::enabled = enabled && ai_feature;
    if (enabled)
        install_mfd();
    if (ai_feature)
        swofty::log("[td-probe] suspension damage for AI cars: %s", ai::enabled ? "on" : "OFF");
    if (feature)
        swofty::log("[td-probe] suspension damage: %s (geometry hook %d, collision hook %d)", enabled ? "on" : "OFF",
                    int(step_hooked), int(hit_hooked));
}

bool speed_wanted() { return enabled; }

// Game thread (player update): collapse the requested corners.
void game_thread(uintptr_t car)
{
    uint32_t want = collapse_request.exchange(0);
    if (!want)
        return;
    uintptr_t owner = suspension_damage_of(car);
    if (!owner || !crash_repair_available()) {
        swofty::log("[td-probe] suspension: collapse not possible (component %p)", reinterpret_cast<void *>(owner));
        return;
    }
    uint32_t before = wheel_state_get(reinterpret_cast<void *>(owner)), after = before;
    for (int i = 0; i < 4; ++i)
        if (((want >> i) & 1) && ((after >> (3 * i)) & 7) < 2)
            after = (after & ~(7u << (3 * i))) | (2u << (3 * i));
    if (after != before)
        wheel_state_apply(reinterpret_cast<void *>(owner), after);
    swofty::log("[td-probe] suspension: collapse corners 0x%x: state 0x%03x -> 0x%03x (now 0x%03x)", want, before, after,
                wheel_state_get(reinterpret_cast<void *>(owner)));
    // The game sends its terminal damage message for a corner state set this way only once more breaks (0.51.89
    // log 20261103-030336: 24-40 s later), so the car drove on with a collapsed corner. Take it as the game's
    // terminal suspension damage now: limp roll for one broken corner, otherwise the car rolls out. A corner the game
    // had already damaged (state 2, a flat tyre; 3, a wheel coming loose) keeps its state but collapses all the same:
    // before 0.51.133 such a collapse changed nothing and the car drove on as if the corner held.
    if (keep_control) {
        uint64_t now = GetTickCount64();
        terminal_mask.fetch_or(1u << 4);
        flashback_tracker.mark(now);
        damage_flashback_tracker.record_damage(1u << 4, now);
        player_broken = true;
        decide_limp(car);  // undecided: the limp roll; already limping: a second broken corner ends it
        swofty::log("[td-probe] suspension: your car counts as terminally damaged (mask 0x%x)", terminal_mask.load());
    }
}

bool ai_wanted() { return ai::enabled; }

// Game thread, the update of an AI car (vehicle != the player's): register it and collapse requested corners.
void ai_game_thread(int v, uintptr_t car)
{
    if (v < 0 || v >= ai::n)
        return;
    uintptr_t s = 0;
    if (car && !guarded_read(car + 0x580, s))
        s = 0;
    ai::car[v] = car;
    ai::sys[v] = s;
    uint32_t want = ai::collapse_req[v].exchange(0);
    if (!want || !car)
        return;
    uintptr_t owner = suspension_damage_of(car);
    if (!owner || !crash_repair_available())
        return;
    uint32_t before = wheel_state_get(reinterpret_cast<void *>(owner)), after = before;
    for (int i = 0; i < 4; ++i)
        if (((want >> i) & 1) && ((after >> (3 * i)) & 7) < 2)
            after = (after & ~(7u << (3 * i))) | (2u << (3 * i));
    if (after != before)
        wheel_state_apply(reinterpret_cast<void *>(owner), after);
    swofty::log("[td-probe] suspension: AI car %d collapse corners 0x%x: state 0x%03x -> 0x%03x", v, want, before, after);
}

// The player's slot is not an AI car (the player can change, e.g. after a restart in another car).
void ai_forget(int v)
{
    if (v >= 0 && v < ai::n && ai::car[v].load()) {
        ai::car[v] = 0;
        ai::sys[v] = 0;
    }
}

// Message bus: the game reported an AI car terminally damaged.
void ai_terminal(int v)
{
    if (v >= 0 && v < ai::n)
        ai::terminal_seen.fetch_or(1u << v);
}

// The race HUD shows one central notification and a newer toast replaces it: a bent corner 50 ms after a broken
// one hid the failure (0.51.127 test 20:20:39). After a failure message, bent/weakened messages are skipped.
std::atomic<uint64_t> minor_toast_until{0};
void hold_minor_toasts(uint32_t ms)
{
    minor_toast_until = GetTickCount64() + ms;
}

void toast(const char *title, const char *what, int i, int seconds, bool minor = false)
{
    if (!swofty::hud::available() || (minor && GetTickCount64() < minor_toast_until.load()))
        return;
    char text[120];
    snprintf(text, sizeof text, what, corner_name(i));
    swofty::hud::toast(title, text, seconds);
}

// The corners the addon's model collapsed (bit i = corner i), read now: corner_view is taken at the start of the
// frame, and game_thread() collapses a corner and decides the limp later in the same frame. The hit thread sets
// `collapsed` before collapse_request, which game_thread() takes first.
uint32_t collapsed_corners()
{
    uint32_t mask = 0;
    for (int i = 0; i < 4; ++i)
        if (model.c[i].collapsed)
            mask |= 1u << i;
    return mask;
}

void collapse(int i, const char *why)
{
    collapse_request.fetch_or(1u << i);
    swofty::log("[td-probe] suspension: %s collapses (%s, health %.2f, bend %+.2f deg)", corner_tag(i), why,
                double(model.c[i].health), double(model.c[i].bend / deg));
    toast("SUSPENSION FAILURE", "%s suspension collapsed", i, 6);
    hold_minor_toasts(6000);
}

void process_hit(int i, const HitGate::Hit &hit)
{
    ++stat_hits[i];
    stat_hit[i] = std::max(stat_hit[i], hit.v);
    char tag[5];
    tag_text(hit.tag, tag);
    Model before = model;
    Rolls r{random01(), random01(), random01(), random01(), random01(), random01()};
    uint64_t at = drive_ms();
    HitResult h = apply_hit(model, i, hit.v, !hit.solid, r, at);
    if (h.severity <= 0) {
        if (model.c[i].collapsed)
            return;
        swofty::log("[td-probe] suspension: %s contact %.1f m/s with %s (sum dv %.1f, peak vn %.1f, %d contacts%s): "
                    "no damage", corner_tag(i), double(hit.v), tag, double(hit.sum_dv), double(hit.peak_vn),
                    hit.contacts, hit.face ? ", tyre face" : "");
        return;
    }
    history.remember(at, before);
    char extra[64] = "";
    if (h.failing)
        snprintf(extra, sizeof extra, ", lets go after %.0f s of driving",
                 double(model.c[i].crack_at > at ? model.c[i].crack_at - at : 0) / 1000.0);
    else if (h.cracked)
        snprintf(extra, sizeof extra, ", hidden crack");
    swofty::log("[td-probe] suspension: %s hit %.1f m/s with %s (sum dv %.1f, peak vn %.1f, %d contacts%s%s), break "
                "chance %.0f %%: %s, bend %+.2f deg (now %+.2f), health %.2f -> %.2f%s",
                corner_tag(i), double(hit.v), tag, double(hit.sum_dv), double(hit.peak_vn), hit.contacts,
                hit.face ? ", tyre face" : "", hit.solid ? "" : ", kerb or road", double(h.chance) * 100.0,
                h.collapse ? "BREAKS" : h.failing ? "BREAKS LATER" : "holds", double(h.delta / deg),
                double(model.c[i].bend / deg), double(before.c[i].health), double(model.c[i].health), extra);
    if (h.collapse) {
        collapse(i, "hit");
    } else if (h.failing) {
        toast("SUSPENSION DAMAGE", "%s suspension badly damaged. It can fail at any moment.", i, 6);
        hold_minor_toasts(6000);
    } else if (h.newly_weak)
        toast("SUSPENSION DAMAGE", "%s suspension weakened. It may fail.", i, 6, true);
    else if (h.newly_bent)
        toast("SUSPENSION DAMAGE", "%s corner bent: the car pulls", i, 5, true);
}

// Frame thread: the player's new contacts for calibrating the thresholds. Every counted one (a real hit) and every
// one the game took suspension damage from (up to 4 a frame), and of the rest the one with the most impulse at most
// every 5 s.
void log_one_contact(const ContactRecord &r, int seen)
{
    char tag[5], own[5], game[48] = "";
    tag_text(r.c.tag, tag);
    tag_text(r.c.own, own);
    if (r.c.dd > 0)
        snprintf(game, sizeof game, ", game damage +%.3f %s", double(r.c.dd),
                 r.c.dd_corner >= 0 ? corner_tag(r.c.dd_corner) : "-");
    swofty::log("[td-probe] suspension contact: %s %s with %s (%s, own %s, group 0x%02x%s%s%s): J %.0f, dv %.2f, vn %.2f, "
                "|v.n| %.1f, face %.2f, dt %.4f%s (%d new)",
                r.corner >= 0 ? corner_tag(r.corner) : "-", r.counted ? "counts" : "ignored", tag, class_name(r.cls),
                own, r.c.group, r.c.entity ? ", entity" : "", r.c.dynamic ? ", moving" : "", r.c.veh != 0xff ? ", car" : "",
                double(r.c.j), double(r.c.dv), double(r.c.vn), double(r.c.vn_cm), double(r.c.face), double(r.c.dt), game,
                seen);
}

// A body hit comes as a burst of contacts over a few physics steps: one line for it, body_quiet_ms after its last
// contact, with the game's suspension damage from outside its handler in that time.
constexpr uint64_t body_quiet_ms = 300;
struct BodyBurst {
    uint64_t first = 0, last = 0;  // ms
    int n = 0, wheels = 0;  // contacts, of them on a wheel proxy
    float dv = 0, vn = 0, fwd = 0, left = 0, up_lo = 2, up_hi = -2;  // fwd, left: dv-weighted sums
    float out[4] = {};
    Contact peak{};
    uint8_t peak_cls = 0;
    void touch(uint64_t now)
    {
        if (!first)
            first = now;
        last = now;
    }
    void add(const ContactRecord &r, uint64_t now)
    {
        touch(now);
        if (n++ == 0 || r.c.dv > peak.dv) {
            peak = r.c;
            peak_cls = r.cls;
        }
        wheels += r.c.proxy != 0;
        dv += r.c.dv;
        vn = std::max(vn, r.c.vn);
        fwd += r.c.dv * r.c.at_fwd;
        left += r.c.dv * r.c.at_left;
        up_lo = std::min(up_lo, r.c.n_up);
        up_hi = std::max(up_hi, r.c.n_up);
    }
    void add_out(int i, float x, uint64_t now)
    {
        touch(now);
        out[i] += x;
    }
    bool due(uint64_t now) const { return first && now >= last + body_quiet_ms; }
    float at_fwd() const { return dv > 0 ? fwd / dv : 0.0f; }
    float at_left() const { return dv > 0 ? left / dv : 0.0f; }
};

// Where on the car, from the centre of mass: the front axle is about 2 m ahead, the rear axle about 1.6 m behind
// (3.6 m wheelbase, 45 % on the front), the wheels about 0.8 m out.
inline const char *body_region(float fwd, float left)
{
    bool l = left > 0.3f, r = left < -0.3f;
    if (fwd > 1.0f)
        return l ? "front left" : r ? "front right" : "front";
    if (fwd < -1.0f)
        return l ? "rear left" : r ? "rear right" : "rear";
    return l ? "left side" : r ? "right side" : "middle";
}

void log_burst(const BodyBurst &b)
{
    char out[64] = "";
    for (int i = 0, at = 0; i < 4; ++i)
        if (b.out[i] > 0)
            at += snprintf(out + at, sizeof out - at, " %s +%.2f", corner_tag(i), double(b.out[i]));
    if (!b.n) {
        swofty::log("[td-probe] suspension: the game's own damage rose outside its suspension handler with no body hit "
                    "(front wing overflow?):%s", out);
        return;
    }
    char tag[5], own[5];
    tag_text(b.peak.tag, tag);
    tag_text(b.peak.own, own);
    swofty::log("[td-probe] suspension: body hit, %d contacts (%d of them on a wheel) in %.2f s: "
                "sum dv %.2f m/s, peak vn %.1f m/s, at %+.2f m forward %+.2f m left (%s), normal up %.2f..%.2f; "
                "strongest: own %s%s with %s (%s, group 0x%02x%s%s%s), J %.0f, dv %.2f, vn %.1f, face %.2f%s%s",
                b.n, b.wheels, double(b.last - b.first) / 1000.0, double(b.dv), double(b.vn), double(b.at_fwd()),
                double(b.at_left()), body_region(b.at_fwd(), b.at_left()), double(b.up_lo), double(b.up_hi), own,
                b.peak.proxy == 1 ? " (a front wheel)" : b.peak.proxy == 2 ? " (a rear wheel)" : "", tag,
                class_name(b.peak_cls), b.peak.group, b.peak.entity ? ", entity" : "", b.peak.dynamic ? ", moving" : "",
                b.peak.veh != 0xff ? ", car" : "", double(b.peak.j), double(b.peak.dv), double(b.peak.vn),
                double(b.peak.face), out[0] ? "; the game's suspension damage outside its handler (front wing overflow?)" : "",
                out);
}

// Which ignored contact the 5 s log shows: the most impulse, then the fastest normal speed.
inline float ignored_score(const Contact &c) { return c.dv * 1000.0f + std::min(c.vn, 999.0f); }

void log_contacts(uint64_t now)
{
    static uint64_t next_ignored = 0;
    static ContactRecord best_ignored{};
    static float best_score = -1;
    static int ignored_seen = 0;
    static BodyBurst burst;
    uint32_t head = contact_log.head.load(std::memory_order_acquire);
    if (head - contact_log_read > ContactLog::n)
        contact_log_read = head - ContactLog::n;
    int counted = 0;
    ContactRecord r;
    for (; contact_log_read != head; ++contact_log_read) {
        if (!contact_log.get(contact_log_read, r))
            continue;
        if (r.c.body) {
            burst.add(r, now);
            continue;
        }
        if (r.counted || r.c.dd > 0) {
            if (counted++ < 4)
                log_one_contact(r, 1);
        } else {
            ++ignored_seen;
            if (ignored_score(r.c) > best_score) {
                best_score = ignored_score(r.c);
                best_ignored = r;
            }
        }
    }
    for (int i = 0; i < 4; ++i)
        if (float x = out_pending[i].exchange(0); x > 0) {
            burst.add_out(i, x, now);
            stat_out[i] += x;
        }
    if (burst.due(now)) {
        log_burst(burst);
        burst = {};
    }
    if (best_score >= 0 && now >= next_ignored) {
        log_one_contact(best_ignored, ignored_seen);
        best_score = -1;
        ignored_seen = 0;
        next_ignored = now + 5000;
    }
}

void log_stats()
{
    const Corner *c = model.c;
    char it[5], lt[5];
    tag_text(ignored_tag.exchange(0), it);
    tag_text(light_tag.exchange(0), lt);
    float gd[4];
    bool have_gd = game_damage(gd);
    log_game_settings();
    swofty::log("[td-probe] suspension: last 30 s contacts not counted: %u harmless (fastest vn %.1f m/s, %s), %u too "
                "light (most impulse dv %.2f m/s at vn %.1f, %s), %u with the own car; the game's own damage RL %.2f RR "
                "%.2f FL %.2f FR %.2f%s",
                ignored_n.exchange(0), double(ignored_vn.exchange(0)), it, light_n.exchange(0),
                double(std::max(0.0f, light_dv.exchange(-1))), double(light_vn.exchange(0)), lt, self_n.exchange(0),
                double(gd[0]), double(gd[1]), double(gd[2]), double(gd[3]),
                have_gd ? " (1.0 = the game's wheel detach)" : " (unreadable)");
    uint32_t bn = body_n.exchange(0), fn = floor_n.exchange(0);
    float bdv = std::max(0.0f, body_dv.exchange(-1)), bvn = body_vn.exchange(0), fdv = std::max(0.0f, floor_dv.exchange(-1));
    char bt[5], bo[5];
    tag_text(body_tag.exchange(0), bt);
    tag_text(body_own.exchange(0), bo);
    if (bn || fn || stat_out[0] > 0 || stat_out[1] > 0 || stat_out[2] > 0 || stat_out[3] > 0)
        swofty::log("[td-probe] suspension: last 30 s body contacts from J %.0f: %u off the ground (most impulse dv %.2f "
                    "m/s at vn %.1f, own %s with %s), %u on the ground (normal up, most impulse dv %.2f m/s); the "
                    "game's damage from outside its suspension handler RL %.2f RR %.2f FL %.2f FR %.2f",
                    double(body_j_min), bn, double(bdv), double(bvn), bo, bt, fn, double(fdv), double(stat_out[0]),
                    double(stat_out[1]), double(stat_out[2]), double(stat_out[3]));
    for (float &x : stat_out)
        x = 0;
    swofty::log("[td-probe] suspension: last 30 s RL/RR/FL/FR travel rate peak %.2f %.2f %.2f %.2f, heavy loads %d %d "
                "%d %d, hits %d %d %d %d (peak %.1f %.1f %.1f %.1f m/s), health %.2f %.2f %.2f %.2f, bend %+.2f "
                "%+.2f %+.2f %+.2f deg, steps %llu, contact hook calls %u (yours %u)",
                double(stat_rate[0]), double(stat_rate[1]), double(stat_rate[2]), double(stat_rate[3]), stat_heavy[0],
                stat_heavy[1], stat_heavy[2], stat_heavy[3], stat_hits[0], stat_hits[1], stat_hits[2], stat_hits[3],
                double(stat_hit[0]), double(stat_hit[1]), double(stat_hit[2]), double(stat_hit[3]),
                double(c[0].health), double(c[1].health), double(c[2].health), double(c[3].health),
                double(c[0].bend / deg), double(c[1].bend / deg), double(c[2].bend / deg), double(c[3].bend / deg),
                (unsigned long long)steps.exchange(0), hook_calls.exchange(0), hook_calls_own.exchange(0));
    for (int i = 0; i < 4; ++i) {
        stat_rate[i] = stat_hit[i] = 0;
        stat_heavy[i] = stat_hits[i] = 0;
    }
}

void log_test_state(const char *why)
{
    uintptr_t s = player_sys.load(), w = 0;
    float toe = 0, camber = 0, damage = 0, travel = 0, sin_h = 0, cos_h = 1;
    if (!s || !guarded_read(s + 0x70 + 2 * 8, w) || !w)
        return;
    guarded_read(w + 0x160, toe);
    guarded_read(w + 0x154, camber);
    guarded_read(w + 0x98, damage);
    guarded_read(w + 0x168, travel);
    guarded_read(w + 0x1f8, sin_h);
    guarded_read(w + 0x1fc, cos_h);
    swofty::log("[td-probe] suspension test (%s): mode %d offset %.4f, FL toe %.5f heading %.5f camber %.5f damage "
                "%.3f travel %.4f",
                why, test::mode.load(), double(test::offset.load()), double(toe), double(std::atan2(sin_h, cos_h)),
                double(camber), double(damage), double(travel));
}

void test_tick(std::string_view setting)
{
    static bool was_down = false;
    static int stage = 0;
    static uint64_t next_log = 0;
    int m = test::mode_of(setting);
    if (m != test::mode.load()) {
        if (test::mode.load() == 3)
            test::restore_damage = true;
        test::mode = m;
        stage = 0;
        test::offset = 0;
    }
    bool down = m && ctrl_shift_down(VK_F8);
    if (down && !was_down) {
        const float *stages = m == 1 ? test::toe_stages : m == 2 ? test::camber_stages : test::damage_stages;
        stage = (stage + 1) % (m == 2 ? 3 : 4);
        float value = stages[stage];
        if (m == 3 && !value)
            test::restore_damage = true;
        test::offset = m == 3 ? value : value * deg;
        char line[120];
        if (!value)
            snprintf(line, sizeof line, "Front-left %s: back to normal", m == 1 ? "toe" : m == 2 ? "camber" : "damage");
        else if (m == 3)
            snprintf(line, sizeof line, "Front-left suspension damage %.0f %%", double(value * 100));
        else
            snprintf(line, sizeof line, "Front-left %s %+.2f degrees", m == 1 ? "toe" : "camber", double(value));
        if (swofty::hud::available())
            swofty::hud::toast("SUSPENSION TEST", line, 4);
        swofty::log("[td-probe] suspension test: %s", line);
        log_test_state("step");
    }
    was_down = down;
    uint64_t now = GetTickCount64();
    if (m && player_sys.load() && now >= next_log) {
        next_log = now + 2000;
        log_test_state("2 s");
    }
}

// Frame thread: move a corner's counted contacts since the last frame into its hit gate.
void take_pending(Pending &p, HitGate &g, uint64_t now)
{
    float dv = p.dv.exchange(0, std::memory_order_relaxed);
    if (dv <= 0)
        return;
    float vn = p.vn.exchange(0, std::memory_order_relaxed);
    int contacts = int(p.contacts.exchange(0, std::memory_order_relaxed));
    bool solid = p.solid.exchange(0, std::memory_order_relaxed) != 0, face = p.face.exchange(0, std::memory_order_relaxed) != 0;
    g.contact(now, dv, vn, p.tag.load(std::memory_order_relaxed), solid, std::max(1, contacts), face);
}

void ai_collapse(int v, int i, uint64_t now);

void ai_process_hit(int v, int i, const HitGate::Hit &hit, uint64_t now)
{
    Model &m = ai::model[v];
    Model before = m;
    Rolls r{random01(), random01(), random01(), random01(), random01(), random01()};
    HitResult h = apply_hit(m, i, hit.v, !hit.solid, r, drive_ms());
    if (h.severity <= 0)
        return;
    ai::history[v].remember(drive_ms(), before);
    char tag[5];
    tag_text(hit.tag, tag);
    swofty::log("[td-probe] suspension: AI car %d %s hit %.1f m/s with %s (sum dv %.1f, peak vn %.1f), break chance "
                "%.0f %%: bend %+.2f deg, health %.2f%s%s",
                v, corner_tag(i), double(hit.v), tag, double(hit.sum_dv), double(hit.peak_vn), double(h.chance) * 100.0,
                double(m.c[i].bend / deg), double(m.c[i].health),
                h.collapse ? ", collapses" : h.failing ? ", fails later" : "", h.cracked ? ", hidden crack" : "");
    if (h.collapse)
        ai_collapse(v, i, now);
}

void ai_collapse(int v, int i, uint64_t now)
{
    ai::collapse_req[v].fetch_or(1u << i);
    if (!ai::retire_at[v])
        ai::retire_at[v] = now + 15000;
}

void ai_tick(uint64_t now, bool flashback)
{
    int player = player_vehicle.load();
    for (int v = 0; v < ai::n; ++v) {
        Model &m = ai::model[v];
        if (flashback) {
            // The rewind restores the game's own damage of every car; undo the addon's last 20 s here too.
            uint64_t at = drive_ms();
            if (ai::history[v].undo_since(at > 20000 ? at - 20000 : 0, m)) {
                swofty::log("[td-probe] suspension: flashback undoes AI car %d's damage of the last 20 s", v);
                bool collapsed = false;
                for (const Corner &c : m.c)
                    collapsed |= c.collapsed;
                if (!collapsed)
                    ai::retire_at[v] = 0;
            }
            for (int i = 0; i < 4; ++i) {
                ai::gate[v][i].clear();
                ai::pending[v][i].clear();
            }
        }
        if (v == player || !ai::car[v].load()) {
            for (int i = 0; i < 4; ++i)
                ai::toe[v][i] = 0;
            continue;
        }
        for (int i = 0; i < 4; ++i) {
            take_pending(ai::pending[v][i], ai::gate[v][i], now);
            if (HitGate::Hit hit = ai::gate[v][i].take(now); hit.v > 0)
                ai_process_hit(v, i, hit, now);
            if (crack_gives(m.c[i], drive_ms())) {
                Model before = m;
                m.c[i].collapsed = true;
                ai::history[v].remember(drive_ms(), before);
                swofty::log("[td-probe] suspension: AI car %d %s collapses (hidden crack)", v, corner_tag(i));
                ai_collapse(v, i, now);
            }
            ai::toe[v][i] = m.c[i].collapsed ? 0.0f : m.c[i].bend;
        }
        if (ai::retire_at[v] && now >= ai::retire_at[v]) {
            ai::retire_at[v] = 0;
            if (ai::terminal_seen.load() & (1u << v)) {
                swofty::log("[td-probe] suspension: AI car %d is handled by the game's terminal damage", v);
            } else {
                swofty::log("[td-probe] suspension: AI car %d got no terminal damage message, retiring it", v);
                post_vehicle_status(v, 7, 3);
            }
        }
    }
}

// on_frame. `flashbacks` counts the flashbacks used so far.
void tick(std::string_view test_setting, uint32_t flashbacks)
{
    uintptr_t car = player_car(), sys = 0;
    if ((!enabled && !test_enabled) || !step_hooked || !car || !guarded_read(car + 0x580, sys))
        sys = 0;
    player_sys = sys;
    hit_car = enabled && sys ? car : 0;
    if (test_enabled)
        test_tick(test_setting);
    if (!enabled) {
        corner_view = 0;
        return;
    }
    {
        uint32_t view = 0;
        for (int i = 0; i < 4; ++i) {
            const Corner &c = model.c[i];
            uint32_t s = c.collapsed ? 3 : c.told_weak ? 2 : c.bend != 0 ? 1 : 0;
            view |= s << (2 * i);
        }
        corner_view = view;
    }
    bool new_session = false;
    if (const char *why = reset_request.exchange(nullptr)) {
        reset(why);
        new_session = !strcmp(why, "new session") || !strcmp(why, "restart");
    }
    if (ai::enabled) {
        if (new_session)
            ai_reset();
        ai_tick(GetTickCount64(), flashbacks != flashback_seen);
    }
    if (flashbacks != flashback_seen) {
        // The rewind restores the game's own damage; undo the addon's changes of the last 20 s of driving.
        flashback_seen = flashbacks;
        uint64_t at = drive_ms();
        if (history.undo_since(at > 20000 ? at - 20000 : 0, model))
            swofty::log("[td-probe] suspension: flashback undoes the damage of the last 20 s");
        for (int i = 0; i < 4; ++i) {
            gate[i].clear();
            pending[i].clear();
        }
    }
    // Practice and qualifying: the team repairs the suspension in the garage. Since 0.51.135 that takes time (the
    // garage service, start_garage_service): the model keeps the damage until the job is done (request_reset
    // "repair"). Without the service (the addon does not keep control, no repair hooks, a crash repair) the corners
    // are repaired at once, as before. A drive out attempt during the service leaves the garage for 300 ms: that
    // return starts nothing new.
    bool in_garage = player_pit.load() == 9;
    if (in_garage && practice_like()) {
        if (!garage_reset_done && !garage_service_corners.load()) {
            float r[4] = {random01(), random01(), random01(), random01()};
            ServicePlan plan = service_plan(model, r);
            if (!plan.corners || !start_garage_service(plan.seconds, plan.corners, plan.crack))
                reset("garage");
        }
        garage_reset_done = true;
    } else {
        garage_reset_done = false;
    }
    // A car in its garage takes no loads: no hits, no heavy loads, and a crack does not let go there.
    if (!sys || in_garage) {
        if (in_garage) {
            for (int i = 0; i < 4; ++i) {
                gate[i].clear();
                pending[i].clear();
                rate_peak[i] = 0;
            }
        }
        publish();
        publish_mfd(car);
        return;
    }
    uint64_t now = GetTickCount64();
    int speed = player_speed();
    for (int i = 0; i < 4; ++i) {
        take_pending(pending[i], gate[i], now);
        if (HitGate::Hit hit = gate[i].take(now); hit.v > 0)
            process_hit(i, hit);
        float rate = rate_peak[i].exchange(0);
        stat_rate[i] = std::max(stat_rate[i], rate);
        if (rate > heavy_rate && speed > 50 && now >= heavy_after[i] && !model.c[i].collapsed) {
            heavy_after[i] = now + 500;
            ++stat_heavy[i];
            Model before = model;
            bool due = model.c[i].crack_at && drive_ms() >= model.c[i].crack_at;
            if (heavy_load(model, i, random01(), drive_ms())) {
                history.remember(drive_ms(), before);
                collapse(i, due ? "hidden crack, heavy load" : "heavy load");
            }
        }
        if (crack_gives(model.c[i], drive_ms())) {
            Model before = model;
            model.c[i].collapsed = true;
            history.remember(drive_ms(), before);
            collapse(i, "hidden crack");
        }
    }
    log_contacts(now);
    publish();
    publish_mfd(car);
    if (now >= stat_next) {
        if (stat_next)
            log_stats();
        stat_next = now + 30000;
    }
}

}  // namespace susp
