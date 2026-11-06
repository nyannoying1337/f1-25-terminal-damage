// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Damage rating, flashback rule and flashback used-count hooks.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// The game's damage rating 0x142f168b0 (rcx the system the addon calls VehicleTerminalDamageSystem, type descriptor
// 0x146096948, its replay recordable "VehicleDamageMonitor" at +0x20; rdx a record: +8 the vehicle wrapper, +0x10..+0x38
// the part levels 0-5 from the threshold tables at +0xc30.., +0x68 the worst). Three callers: updateVehicle (the
// stored record system+0x50+0x70*vehicle; a worst of 5 while system+0xf90 is set starts the timer at record+0x6c
// and then sends MessageVehicleTerminallyDamaged, 0x142f16881), a re-rating of every stored record (0x14f6a4050),
// and the recordable's canJumpIn (vtable slot 9, thunk 0x142f16d70 -> 0x14f696e10), which rates the player's car into
// a record on its stack and refuses the flashback jump-in at worst 5; the game's REPLAY channel then logs
// "canJumpIn() for recordable 'VehicleDamageMonitor' returned false!" (Career practice 0.51.129, 21:20:02). Without
// system+0xf90 every 5 reads 2 (the front wing 2 + 3 * flag). While keep_control handles terminal damage, only that
// stack record is rated as without the flag, so the jump-in is allowed; the stored records, and with them the
// terminal message, stay the game's. The stored record of the player's car feeds the limp watch. The game's pointers
// are valid inside its own call, so they are read directly.
constexpr const char *sig_damage_rating =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 28 48 89 D6 48 89 CF 48 8B 42 08 48 8B 48 58 48 8B 89 30 05 00 00 "
    "F6 81 30 25 00 00 03";
constexpr uint8_t damage_rating_prologue[16] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x28};
uint64_t (*orig_damage_rating)(void *, void *) = nullptr;
std::atomic<int> player_rating{-1};       // the worst level of the player's stored record (limp watch)
std::atomic<uint64_t> jump_in_logged_at{0};

// The parts of a record at level 5, as the bits of the game's terminal message (0 engine, 1 power unit, 2 gearbox,
// 3 a wheel off, 4 suspension, 5 tyres, 6 front wing, 7-10 the rest of the body).
uint32_t rated_terminal_parts(const uint8_t *record)
{
    uint32_t mask = 0;
    for (int i = 0; i < 11; ++i) {
        int32_t level = 0;
        memcpy(&level, record + 0x10 + 4 * i, 4);
        if (level == 5)
            mask |= 1u << i;
    }
    return mask;
}

// A record as the game rates it without system+0xf90: every level 5 reads 2, the worst follows.
void rate_without_terminal(uint8_t *record)
{
    int32_t worst = 0;
    for (int i = 0; i < 11; ++i) {
        int32_t level = 0;
        memcpy(&level, record + 0x10 + 4 * i, 4);
        if (level == 5) {
            level = 2;
            memcpy(record + 0x10 + 4 * i, &level, 4);
        }
        worst = std::max(worst, level);
    }
    memcpy(record + 0x68, &worst, 4);
}

uint64_t on_damage_rating(void *system, void *record)
{
    uint64_t r = orig_damage_rating(system, record);
    auto sys = reinterpret_cast<uintptr_t>(system), rec = reinterpret_cast<uintptr_t>(record);
    uint64_t count = *reinterpret_cast<const uint64_t *>(sys + 0xc20);
    int32_t worst = 0;
    memcpy(&worst, reinterpret_cast<const void *>(rec + 0x68), 4);
    if (rec >= sys + 0x50 && rec < sys + 0x50 + count * 0x70) {
        if (int(uint64_t(rec - sys - 0x50) / 0x70) == player_vehicle.load(std::memory_order_relaxed))
            player_rating.store(worst, std::memory_order_relaxed);
        return r;
    }
    if (worst != 5 || !keep_control)
        return r;
    auto *bytes = static_cast<uint8_t *>(record);
    uint32_t parts = rated_terminal_parts(bytes);
    rate_without_terminal(bytes);
    uint64_t now = GetTickCount64();
    if (now - jump_in_logged_at.load() > 10000) {
        jump_in_logged_at = now;
        swofty::log("[td-probe] flashback: the game rates your car terminal (parts 0x%x) and would refuse the jump-in; "
                    "allowed, the addon handles the damage", parts);
    }
    return r;
}

void hook_damage_rating()
{
    detour("damage rating (flashback jump-in check)", sig_damage_rating, damage_rating_prologue,
           sizeof damage_rating_prologue, reinterpret_cast<void *>(&on_damage_rating),
           reinterpret_cast<void **>(&orig_damage_rating));
}

// The game's own flashback rule: GameFlashbackSystem (replay recordable id 32, registered at 0x1408066b6; constructor
// 0x1407f1c60 writes its vtable 0x1457cdff0 and, at +0x20, the recordable vtable 0x1457ce120). The recordable's
// canJumpIn (vtable slot 9, thunk 0x142bf5800 -> 0x14e44f740 in .data1; rcx = system+0x20) reads only its own fields:
// +0x40 a free flashback passes (the instant replay state copies its free_flashback enter parameter there, 0x142da237c);
// +0x38 flashbacks off refuses (the player's assist apply 0x142424060 sets it to setting == 1, the constructor's default
// is off); +0x48 clear (no limit) passes; else +0x3c used < +0x44 limit. +0x39 is the limit mode (low, medium, high,
// unlimited; the limit for the session type comes from the game's flashback_limits data, 0x14e4456b0). A jump-in with
// flashbacks on and no free flashback counts +0x3c and posts MessageFlashbackUsed (0x142da2a05). The replay checks its
// recordables in order and stops at the first refusal: VehicleDamageMonitor (id 24, the damage rating above) before
// GameFlashbackSystem. The stock terminal damage screen (0x142d988e0) asks the same canJumpIn and offers only the
// replay when it refuses (terminally_damaged_replay_no_restart). Watched only: the result stays the game's, a refusal
// is logged with the fields (at most every 10 s) so a log tells the settings apart from the damage.
constexpr const char *sig_flashback_system_ctor =
    "48 8D 05 ?? ?? ?? ?? 48 89 07 48 8D 05 ?? ?? ?? ?? 48 89 47 20 48 C7 47 28 00 00 00 00 66 C7 47 38 00 03 "
    "C7 47 3C 00 00 00 00 C6 47 40 00 C6 47 48 00";
constexpr uint8_t flashback_rule_body[34] = {0xB0, 0x01, 0x80, 0x79, 0x20, 0x00, 0x74, 0x01, 0xC3, 0x80, 0x79, 0x18,
                                             0x00, 0x74, 0x10, 0x80, 0x79, 0x28, 0x00, 0x74, 0xF3, 0x8B, 0x41, 0x1C,
                                             0x3B, 0x41, 0x24, 0x0F, 0x92, 0xD0, 0xC3, 0x31, 0xC0, 0xC3};
uint64_t (*orig_flashback_rule)(void *) = nullptr;
std::atomic<uint64_t> flashback_rule_logged_at{0};

struct FlashbackRule {
    bool on = false, free = false, limited = false;
    uint8_t mode = 0;
    uint32_t used = 0, limit = 0;
};
// The fields as canJumpIn sees them (recordable = system+0x20).
FlashbackRule read_flashback_rule(const uint8_t *recordable)
{
    FlashbackRule f;
    f.on = recordable[0x18] != 0;
    f.mode = recordable[0x19];
    memcpy(&f.used, recordable + 0x1c, 4);
    f.free = recordable[0x20] != 0;
    memcpy(&f.limit, recordable + 0x24, 4);
    f.limited = recordable[0x28] != 0;
    return f;
}
bool flashback_rule_allows(const FlashbackRule &f)
{
    return f.free || (f.on && (!f.limited || f.used < f.limit));
}
const char *flashback_rule_reason(const FlashbackRule &f)
{
    if (flashback_rule_allows(f))
        return "allowed";
    return f.on ? "no flashbacks left in this session" : "flashbacks are off in this game's settings";
}
const char *flashback_mode_name(uint8_t mode)
{
    static const char *const names[] = {"low", "medium", "high", "unlimited"};
    return mode < 4 ? names[mode] : "?";
}

uint64_t on_flashback_rule(void *recordable)
{
    uint64_t r = orig_flashback_rule(recordable);
    if (r & 0xff)
        return r;
    uint64_t now = GetTickCount64();
    if (now - flashback_rule_logged_at.load() > 10000) {
        flashback_rule_logged_at = now;
        FlashbackRule f = read_flashback_rule(static_cast<const uint8_t *>(recordable));
        swofty::log("[td-probe] flashback: the game's flashback rule refuses the jump-in (not the damage): %s; "
                    "flashbacks %s, limit %s%s, used %u of %u, free flashback %d",
                    flashback_rule_reason(f), f.on ? "on" : "off", flashback_mode_name(f.mode),
                    f.limited ? "" : " (not counted)", f.used, f.limit, int(f.free));
    }
    return r;
}

// The constructor gives the recordable vtable; its slot 9 must lead (through jmp thunks) to the body above.
void hook_flashback_rule()
{
    uintptr_t ctor = 0;
    int n = swofty::api->scan(".xtls", sig_flashback_system_ctor, &ctor);
    if (n != 1 || !ctor) {
        swofty::log("[td-probe] flashback rule: constructor signature has %d hits, not watched", n);
        return;
    }
    int32_t rel = 0;
    memcpy(&rel, reinterpret_cast<const void *>(ctor + 13), 4);
    uintptr_t slot = ctor + 17 + int64_t(rel) + 9 * 8, fn = 0;
    if (!guarded_read(slot, fn) || !fn) {
        swofty::log("[td-probe] flashback rule: vtable slot %p unreadable, not watched", reinterpret_cast<void *>(slot));
        return;
    }
    uintptr_t body = fn;
    for (int i = 0; i < 3 && swofty::mem::readable(body, 5) && *reinterpret_cast<const uint8_t *>(body) == 0xE9; ++i) {
        memcpy(&rel, reinterpret_cast<const void *>(body + 1), 4);
        body = body + 5 + int64_t(rel);
    }
    if (!swofty::mem::readable(body, sizeof flashback_rule_body) ||
        memcmp(reinterpret_cast<const void *>(body), flashback_rule_body, sizeof flashback_rule_body)) {
        swofty::log("[td-probe] flashback rule: slot %p leads to %p, not the expected check; not watched",
                    reinterpret_cast<void *>(slot), reinterpret_cast<void *>(body));
        return;
    }
    orig_flashback_rule = reinterpret_cast<decltype(orig_flashback_rule)>(fn);
    void *handler = reinterpret_cast<void *>(&on_flashback_rule);
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void *>(slot), 8, PAGE_READWRITE, &old))
        return;
    memcpy(reinterpret_cast<void *>(slot), &handler, 8);
    VirtualProtect(reinterpret_cast<void *>(slot), 8, old, &old);
    swofty::log("[td-probe] watching the game's flashback rule (GameFlashbackSystem canJumpIn, vtable slot %p)",
                reinterpret_cast<void *>(slot));
}

// Flashback without UDP. The instant replay state's update (0x142da2760) posts MessageFlashbackUsed (vtable
// 0x1457d4030) through its own queued-post thunk (0x142cb7130, this is its only caller, at 0x142da2a53) when the player
// resumes from a flashback. The game then rewinds the cars, including the terminal damage record and the part wear.
constexpr const char *sig_flashback_used_site =
    "4C 8B B7 68 0F 00 00 41 80 7E 38 00 74 40 41 0F B6 46 40 84 C0 75 04 41 FF 46 3C 48 8B 0D ?? ?? ?? ?? 48 C7 44 "
    "24 48 00 00 00 00 48 8D 15 ?? ?? ?? ?? 48 89 54 24 40 88 44 24 50 48 8B 49 08 48 8D 54 24 40 E8 ?? ?? ?? ??";
uint64_t (*orig_flashback_used)(void *, void *) = nullptr;
uint64_t on_flashback_used(void *bus, void *msg)
{
    uint64_t r = orig_flashback_used(bus, msg);
    flashback_used_at = GetTickCount64();
    flashback_count.fetch_add(1);
    if (uint32_t w = ai_watch_mask.load())
        swofty::log("[td-probe] AI watch: flashback with watched AI cars 0x%x", w);
    swofty::log("[td-probe] flashback: used (broken %d, terminal mask 0x%x, retire request %d, pit state %d)",
                int(player_broken.load()), terminal_mask.load(), int(retire_requested.load()), player_pit.load());
    return r;
}
void hook_flashback_used()
{
    uintptr_t at = 0;
    int hits = swofty::api->scan(".xtls", sig_flashback_used_site, &at);
    if (hits != 1 || !at) {
        swofty::log("[td-probe] flashback: signature has %d hits, using the terminal record only", hits);
        return;
    }
    uintptr_t call = at + 0x44;
    int32_t rel;
    memcpy(&rel, reinterpret_cast<const void *>(call + 1), sizeof rel);
    flashback_hooked = hook_thunk_at("MessageFlashbackUsed post", call + 5 + int64_t(rel),
                                     reinterpret_cast<void *>(&on_flashback_used),
                                     reinterpret_cast<void **>(&orig_flashback_used));
}
