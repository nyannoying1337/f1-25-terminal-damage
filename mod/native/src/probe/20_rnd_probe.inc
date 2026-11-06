// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// R&D probe (0.75.6, read only).
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// ---- R&D probe (0.75.6, read only) ----
// The game's own failure model weighed teams and the player by their R&D (re/game-failure-scheduler.md); this addon's
// per-team rates ignore it. To let a team's Durability progress move its failure rate, this logs once a Career session
// what the game's R&D data looks like. Layout from Swofty's generated game API (experimental): UpgradesComponent
// +0x298/+0x2a0 = Upgrades* a team; Upgrades +0x08 mode, +0x0c team_key, +0xe0 trees[2] (0xc40 apart, UpgradeTree
// +0x10/+0x18 = UpgradeInstance begin/end), +0x1960 tree_count; UpgradeInstance +0x10 name (char *), +0x58 node_hash,
// +0x72 department (aerodynamics, chassis, durability, engine), +0xc0/+0xc8 UpgradeStateEntry (0x10 each: +0 owner,
// +4 day, +8 state, +9 adaptation, +0xc enabled, +0xd facility_locked). The instance size is not in the API: found from
// the names. The state vector is a history (Career_Push_UpgradeState 0x1435230e0 appends an entry when the state
// changes, its arguments (instance, owner, state, day); a history A, B, A collapses back to A): the current state is the
// LAST entry. The nine states (the game's names, what writes each one, which one is a finished upgrade) are listed at
// fm::rnd_state_done in team_model.hpp; research_start/end_day are +0x7e/+0x80, develop_start/end_day +0x98/+0x9a.
// On the game's flow thread (on_frame).
std::atomic<bool> rnd_probe_done{false};
std::vector<uint32_t> rnd_team_keys;  // the team keys of the upgrade trees (on_frame only)

std::string rnd_name(uintptr_t at)
{
    char buf[48] = {};
    uintptr_t s = 0;
    if (!guarded_read(at, s) || !s || !guarded_copy(s, buf, sizeof buf - 1))
        return "?";
    std::string out;
    for (size_t i = 0; i < sizeof buf - 1 && buf[i]; ++i)
        out += (buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.';
    return out.empty() ? "?" : out;
}

bool rnd_plausible_name(uintptr_t at)
{
    std::string n = rnd_name(at);
    if (n.size() < 3 || n == "?")
        return false;
    for (char c : n)
        if (c == '.')
            return false;
    return true;
}

void rnd_probe_tick()
{
    static uint64_t next = 0;
    if (rnd_probe_done.load() || session_ended)
        return;
    uint64_t now = GetTickCount64();
    if (now < next)
        return;
    next = now + 2000;
    swofty::career::State st{};
    if (swofty::career::state(&st) != swofty::career::Result::ok || !st.loaded) {
        rnd_probe_done = true;
        return;  // not a Career: nothing to read
    }
    rnd_probe_done = true;
    uintptr_t comp = reinterpret_cast<uintptr_t>(swofty::career::component("UpgradesComponent"));
    uint64_t b = 0, e = 0;
    if (!comp || !guarded_read(comp + 0x298, b) || !guarded_read(comp + 0x2a0, e) || !b || e < b) {
        swofty::log("[td-probe] rnd: no UpgradesComponent (%p) or its team list is unreadable",
                    reinterpret_cast<void *>(comp));
        return;
    }
    size_t teams = std::min<size_t>((e - b) / 8, 32);
    swofty::log("[td-probe] rnd: UpgradesComponent %p, %zu team(s) with upgrade trees, your team %u",
                reinterpret_cast<void *>(comp), teams, unsigned(st.player_team));
    bool detailed = false;
    for (size_t i = 0; i < teams; ++i) {
        uintptr_t up = 0;
        uint32_t key = 0;
        uint8_t mode = 0;
        if (!guarded_read(b + 8 * i, up) || !up || !guarded_read(up + 0xc, key))
            continue;
        if (std::find(rnd_team_keys.begin(), rnd_team_keys.end(), key) == rnd_team_keys.end())
            rnd_team_keys.push_back(key);
        guarded_read(up + 8, mode);
        for (int tr = 0; tr < 2; ++tr) {
            uintptr_t tree = up + 0xe0 + uintptr_t(tr) * 0xc40;
            uint64_t ib = 0, ie = 0;
            if (!guarded_read(tree + 0x10, ib) || !guarded_read(tree + 0x18, ie) || !ib || ie <= ib)
                continue;
            uint64_t span = ie - ib;
            size_t stride = 0;
            for (size_t cand : {0xd0u, 0xd8u, 0xe0u, 0xe8u, 0xf0u, 0xf8u, 0x100u, 0x108u, 0x110u, 0x120u, 0x130u, 0x140u}) {
                if (span % cand || span / cand < 2 || span / cand > 400)
                    continue;
                size_t n = span / cand, ok = 0, look = std::min<size_t>(n, 6);
                for (size_t k = 0; k < look; ++k)
                    ok += rnd_plausible_name(ib + k * cand + 0x10);
                if (ok >= look) {
                    stride = cand;
                    break;
                }
            }
            if (!stride) {
                swofty::log("[td-probe] rnd: team %u tree %d: %llu bytes of nodes, no instance size fits the names",
                            unsigned(key), tr, (unsigned long long)span);
                continue;
            }
            size_t n = span / stride;
            int dept_count[8] = {};
            int state_count[16] = {}, dur_state_count[16] = {}, many_entries = 0;
            for (size_t k = 0; k < n; ++k) {
                uintptr_t inst = ib + k * stride;
                uint8_t dept = 0xff, state = 0xff;
                uint64_t sb = 0, se = 0;
                guarded_read(inst + 0x72, dept);
                if (dept < 8)
                    ++dept_count[dept];
                if (guarded_read(inst + 0xc0, sb) && guarded_read(inst + 0xc8, se) && sb && se >= sb + 0x10 &&
                    guarded_read(se - 0x10 + 8, state) && state < 16) {
                    ++state_count[state];
                    if (dept == 2)
                        ++dur_state_count[state];
                    many_entries += se - sb > 0x10;
                }
            }
            char dur_text[320] = "";
            for (int s = 0; s < 16; ++s)
                if (dur_state_count[s])
                    snprintf(dur_text + strlen(dur_text), sizeof dur_text - strlen(dur_text), "%sstate %d (%s): %d",
                             *dur_text ? ", " : "", s, fm::rnd_state_name(s), dur_state_count[s]);
            char dept_text[96] = "", state_text[320] = "";
            for (int d = 0; d < 8; ++d)
                if (dept_count[d])
                    snprintf(dept_text + strlen(dept_text), sizeof dept_text - strlen(dept_text), "%sdepartment %d: %d",
                             *dept_text ? ", " : "", d, dept_count[d]);
            for (int s = 0; s < 16; ++s)
                if (state_count[s])
                    snprintf(state_text + strlen(state_text), sizeof state_text - strlen(state_text), "%sstate %d (%s): %d",
                             *state_text ? ", " : "", s, fm::rnd_state_name(s), state_count[s]);
            swofty::log("[td-probe] rnd: team %u (mode %u) tree %d: %zu nodes (size 0x%zx); %s; current state of each node: "
                        "%s; durability (department 2) nodes: %s; nodes with more than one state entry: %d",
                        unsigned(key), unsigned(mode), tr, n, stride, dept_text, state_text, dur_text, many_entries);
            // One team's tree in detail (the player's team when its key is known, else the last one): every node's
            // name, department and first state entry, 12 a line, so the Durability nodes and what "done" looks like
            // show.
            if (!detailed && (key == st.player_team || i + 1 == teams)) {
                detailed = true;
                std::string line;
                int on_line = 0;
                for (size_t k = 0; k < n && k < 200; ++k) {
                    uintptr_t inst = ib + k * stride;
                    uint8_t dept = 0xff, state = 0xff, adapt = 0xff, enabled = 0xff;
                    uint16_t day = 0, r_start = 0, r_end = 0, d_start = 0, d_end = 0;
                    uint64_t sb = 0, se = 0, entries = 0;
                    guarded_read(inst + 0x72, dept);
                    guarded_read(inst + 0x7e, r_start);
                    guarded_read(inst + 0x80, r_end);
                    guarded_read(inst + 0x98, d_start);
                    guarded_read(inst + 0x9a, d_end);
                    if (guarded_read(inst + 0xc0, sb) && guarded_read(inst + 0xc8, se) && sb && se >= sb + 0x10) {
                        entries = (se - sb) / 0x10;
                        guarded_read(se - 0x10 + 4, day);
                        guarded_read(se - 0x10 + 8, state);
                        guarded_read(se - 0x10 + 9, adapt);
                        guarded_read(se - 0x10 + 0xc, enabled);
                    }
                    char one[160];
                    snprintf(one, sizeof one, "%s%s[d%u s%u a%u e%u x%llu day %u research %u-%u develop %u-%u]",
                             line.empty() ? "" : "; ", rnd_name(inst + 0x10).c_str(), unsigned(dept), unsigned(state),
                             unsigned(adapt), unsigned(enabled), (unsigned long long)entries, unsigned(day),
                             unsigned(r_start), unsigned(r_end), unsigned(d_start), unsigned(d_end));
                    line += one;
                    if (++on_line == 8 || k + 1 == n || k + 1 == 200) {
                        swofty::log("[td-probe] rnd: team %u tree %d nodes: %s", unsigned(key), tr, line.c_str());
                        line.clear();
                        on_line = 0;
                    }
                }
            }
        }
    }
}
