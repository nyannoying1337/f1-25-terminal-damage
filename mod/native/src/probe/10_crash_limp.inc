// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Corner damage names, limp chance and the crash limp decision.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

void return_to_garage();
void game_failure_dsq_now();

// The corners a crash broke, bit i = corner i (RL RR FL FR): the game's state 3-4 (wheel coming off) and 5 (wheel
// lost), and state 2 where the addon's model collapsed the corner (`collapsed`, bit i). State 2 is wheel+0x20 bit 0
// (getter 0x1446568e0; bit 1 alone is state 1, a puncture), and the setter 0x144656a80 adds a puncture for it
// (Car_Puncture_Add): the corner shows a flat tyre. The game sets it itself too, without suspension damage, and
// drives on with it: it sends no terminal message for a state 2 corner (0.51.89: not for the addon's collapse until
// more broke; 0.51.127 test 20:27:46: FL state 2 at 0.00 suspension damage, the car drove on). 0.51.128 test
// 20:41:10: the game broke the FR (state 4) and set the RL to state 2 (0.02 damage; in the test 'a puncture at least');
// the RL counted as a broken rear corner, so the car could not limp and the panel showed FR RL. A wheel the game
// only damaged counts only when no other corner broke.
uint32_t broken_corners(uint32_t packed, uint32_t collapsed)
{
    uint32_t mask = 0, damaged = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned s = (packed >> (3 * i)) & 7;
        if (s >= 3 || (s == 2 && ((collapsed >> i) & 1)))
            mask |= 1u << i;
        else if (s == 2)
            damaged |= 1u << i;
    }
    return mask ? mask : damaged;
}

// What broke at corner i, by the game's corner state (state 2: the addon's collapse, or a wheel the game damaged).
const char *corner_damage_name(uint32_t packed, uint32_t collapsed, int i)
{
    unsigned s = (packed >> (3 * i)) & 7;
    if (s >= 5)
        return "wheel lost";
    if (s >= 3)
        return "wheel coming off";
    return (collapsed >> i) & 1 ? "suspension collapsed" : "wheel damaged";
}

// The message for a car a crash left undrivable: the broken corners (broken_corners) by the game's own state,
// whoever broke them: the addon's suspension model or the game's own damage. 0.51.127 test (20:24:36-39): the
// addon's model bent the RR, then the game broke the FR itself (contacts at 10 m/s that were too light for the
// model); no message named the FR and the panel said RR. Two corners are both named, the worst first. nullptr: no
// corner broken.
const char *undrivable_message(uint32_t packed, uint32_t collapsed, char *text, size_t n)
{
    auto what = [packed, collapsed](int i) { return corner_damage_name(packed, collapsed, i); };
    uint32_t mask = broken_corners(packed, collapsed);
    int broken = 0, at = -1, other = -1;
    unsigned worst = 0;
    bool wheels_only = true;  // only wheels the game damaged: no other corner broke
    for (int i = 0; i < 4; ++i) {
        if (!((mask >> i) & 1))
            continue;
        unsigned s = (packed >> (3 * i)) & 7;
        ++broken;
        wheels_only = wheels_only && s == 2 && !((collapsed >> i) & 1);
        if (s >= worst) {
            other = at;
            worst = s;
            at = i;
        } else {
            other = i;
        }
    }
    if (!broken)
        return nullptr;
    if (broken > 2) {
        snprintf(text, n, "%d corners broken. The car cannot be driven.", broken);
    } else if (broken == 2) {
        char second[16];
        snprintf(second, sizeof second, "%s", susp::corner_name(other));
        second[0] = char(tolower(uint8_t(second[0])));
        snprintf(text, n, "%s %s, %s %s. The car cannot be driven.", susp::corner_name(at), what(at), second,
                 what(other));
    } else {
        snprintf(text, n, "%s %s. The car cannot be driven.", susp::corner_name(at), what(at));
    }
    return worst >= 5 ? "WHEEL LOST" : wheels_only ? "WHEEL DAMAGE" : "SUSPENSION FAILURE";
}

// The chance that a car with one broken corner can still crawl on, by the game's corner state. A collapsed
// suspension (state 2: the wheel still on, it drives like a flat tyre; 0.51.125 race 17:18:46 drove 20 s on a
// collapsed FL) 85 % at the front and, since 0.51.132, 60 % at the rear (test report: 'the car is always dead'; a rear
// corner never limped before, 6 of the 12 crashes the model decided in 0.51.127-0.51.130). A front wheel coming
// loose (3: D past the game's wheel detach limit W, no terminal message yet) 35 %. Never once the game tears the
// wheel off itself (4: D past its parts detach limit P, the game's terminal crash damage, which switches the engine
// off: 0.51.132 test 23:05:15, 5918 rpm, 611 at the impact, then 0 while the car rolled 75 -> 0 km/h in 5th gear;
// 0 of 7 such cars drove in the logs), once the wheel is gone (5), nor with a rear wheel coming loose.
unsigned limp_chance(unsigned state, int corner)
{
    bool front = corner >= 2;
    return state >= 4 ? 0 : state == 3 ? (front ? 35 : 0) : front ? 85 : 60;
}

// With the addon's suspension model on (0.76) no dice decide: the game adds no suspension damage of its own (its
// damage scales are 0), so a broken corner is the model's. A collapsed corner (state 2) keeps its wheel and the car
// limps; a wheel off (3 and up, the model tore it off, the tether stops the car: lab run 4) cannot be driven.
unsigned limp_chance_model(unsigned state) { return state <= 2 ? 100 : 0; }

// Game thread. A crash broke a corner: decide once whether the car can still be driven. Only a single broken corner
// can limp (limp_chance). While it limps, a wheel that comes off, another broken corner or the limping rear wheel
// coming off stops it. A wheel the game only damaged (state 2, a flat tyre) is no broken corner (broken_corners)
// unless no other corner broke. Corner order is RL, RR, FL, FR like the UDP format (visual_wheel_fl reads
// owner+0x138 = corner 2), so the fronts are 2 and 3.
void decide_limp(uintptr_t car)
{
    if (limp_state.load() == 1 && car) {
        static uint64_t next_check = 0;
        uint64_t now = GetTickCount64();
        if (now < next_check)
            return;
        next_check = now + 500;
        bool ok = false;
        uint32_t packed = corner_states(car, ok), collapsed = susp::collapsed_corners();
        uint32_t mask = broken_corners(packed, collapsed);
        int at = limp_corner.load(), broken = 0, gone = 0;
        for (int i = 0; ok && i < 4; ++i) {
            broken += (mask >> i) & 1;
            gone += ((packed >> (3 * i)) & 7) >= 5;
        }
        // Another corner broke: one that is not the limping one (a wheel the game only damaged does not count
        // next to it, broken_corners); a limping rear corner whose wheel comes off stops the car too.
        bool more = at >= 0 && at < 4 ? (mask & ~(1u << at)) != 0 : broken > 1;
        // With the model on, the limping corner's wheel coming off (a hit or a hard kerb on the collapsed corner)
        // ends it at the front too.
        bool loose_rear = at >= 0 && (at < 2 || susp::enabled) && ((packed >> (3 * at)) & 7) >= 3;
        if (ok && (more || gone || loose_rear)) {
            crash_corners = packed;
            limp_state = 0;
            limp_corner = -1;
            swofty::log("[td-probe] crash: corners 0x%03x, %s, the car cannot be driven any more", packed,
                        more ? "another corner broke" : "the wheel came off");
            char text[120];
            const char *title = undrivable_message(packed, collapsed, text, sizeof text);
            if (title && swofty::hud::available())
                swofty::hud::toast(title, text, 6);
            susp::hold_minor_toasts(6000);
        }
        return;
    }
    if (limp_state.load() != -1 || !car)
        return;
    bool ok = false;
    uint32_t packed = corner_states(car, ok);
    if (!ok)
        return;  // retried next update; the component may not be readable this frame
    uint32_t collapsed = susp::collapsed_corners(), mask = broken_corners(packed, collapsed), wheels = 0;
    int broken = 0, corner = -1;
    for (int i = 0; i < 4; ++i) {
        if ((mask >> i) & 1) {
            ++broken;
            corner = i;
        } else if (((packed >> (3 * i)) & 7) == 2) {
            wheels |= 1u << i;
        }
    }
    unsigned chance = 0, roll = roll_percent();
    if (broken == 1)
        chance = susp::enabled ? limp_chance_model((packed >> (3 * corner)) & 7)
                               : limp_chance((packed >> (3 * corner)) & 7, corner);
    roll = forced_roll(debug_limp_roll, chance, roll);
    crash_corners = packed;
    limp_state = roll < chance ? 1 : 0;
    limp_corner = limp_state.load() == 1 ? corner : -1;
    float gd[4];
    susp::game_damage(gd);
    swofty::log("[td-probe] crash: corners 0x%03x, %d broken (last %d), limp chance %u%%, roll %u -> %s; the game's "
                "suspension damage RL %.2f RR %.2f FL %.2f FR %.2f", packed, broken, corner, chance, roll,
                limp_state.load() ? "the car can limp on" : "the car cannot be driven", double(gd[0]), double(gd[1]),
                double(gd[2]), double(gd[3]));
    if (wheels)
        swofty::log("[td-probe] crash: corners 0x%x only have a wheel the game damaged (state 2, a flat tyre): not "
                    "counted as broken", wheels);
    limp_watch_left = 30.0;
    // The limping corner and what broke there, e.g. "Rear-left suspension collapsed".
    char what[64] = "";
    if (limp_state.load() == 1)
        snprintf(what, sizeof what, "%s %s", susp::corner_name(corner), corner_damage_name(packed, collapsed, corner));
    if (limp_state.load() == 1 && !practice_like() && susp::enabled) {
        // The model's collapsed corner (0.76): no 20 s stop. Like the officials' black-and-orange flag the car must
        // come in: into the pit lane before it completes the next lap (two crossings of the line would retire it).
        // The game's lap is not always readable (its failure object comes later): then the line crossings count.
        race_stop_left = 1.0;  // race_stop_tick runs; it checks the lap
        if (int lap = game_lap(); lap >= 0) {
            race_stop_lap = lap + 2;
            swofty::log("[td-probe] crash: race, the car must be in the pit lane before lap %d ends (now lap %d)",
                        race_stop_lap.load(), lap + 1);
        } else {
            race_stop_crossing = player_crossings.load() + 2;
            swofty::log("[td-probe] crash: race, the game's lap is unknown; the car must be in the pit lane before it "
                        "crosses the line twice more (crossings now %d)", player_crossings.load());
        }
        char text[120];
        snprintf(text, sizeof text, "%s broken. Drive slowly into the pits, this lap or next.",
                 susp::corner_name(corner));
        if (swofty::hud::available())
            swofty::hud::toast("BOX, BOX", text, 6);
        radio_say(radio_box);
    } else if (limp_state.load() == 1 && !practice_like()) {
        race_stop_left = race_stop_grace;
        swofty::log("[td-probe] crash: race, the car must stop as soon as it is safe (%.0f s of driving)",
                    race_stop_grace);
        char text[120];
        snprintf(text, sizeof text, "%s: pull over as soon as it is safe.", what);
        if (swofty::hud::available())
            swofty::hud::toast("STOP THE CAR", text, 6);
        radio_say(radio_mech_dnf);
    } else if (limp_state.load() == 1) {
        char text[120];
        snprintf(text, sizeof text, "%s broken. Drive slowly back to the garage.", susp::corner_name(corner));
        if (swofty::hud::available())
            swofty::hud::toast("BROKEN SUSPENSION", text, 6);
        radio_say(radio_box);
    } else {
        char text[120];
        const char *title = undrivable_message(packed, collapsed, text, sizeof text);
        if (title && swofty::hud::available())
            swofty::hud::toast(title, text, 6);
    }
    // The central notification shows one message: a corner bent a moment later must not replace this one.
    susp::hold_minor_toasts(6000);
}
