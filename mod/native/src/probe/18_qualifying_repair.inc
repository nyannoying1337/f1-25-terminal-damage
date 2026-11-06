// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Qualifying and practice engine repair hook.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// The engine manager schedules replacements through this AddRepair thunk
// (reference 0x142bf0200). Entries are {u32 kind, float seconds}; ICE is kind 9,
// established by the engine-manager caller and RepairTimesViewModel jump table.
constexpr const char *sig_replacement_repair_site =
    "48 89 F1 89 FA E8 ?? ?? ?? ?? C5 F8 28 74 24 60 C5 F8 28 7C 24 70 C5 78 28 84 24 80 00 00 00";
constexpr const char *sig_repair_session_kind =
    "8B 05 ?? ?? ?? ?? 83 C0 F1 83 F8 02 77 26 80 7D 48 00 0F 85 ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 8B 4D 40 3B 48 10";
using add_repair_fn = uint64_t (*)(void *, uint32_t);
add_repair_fn orig_add_repair;
uintptr_t repair_session_kind;

// Only used inside the game's live AddRepair call. Never retain a repair entry.
bool repair_item(uintptr_t record, uint32_t kind, uintptr_t &item)
{
    item = 0;
    uintptr_t first = 0, last = 0;
    if (!record || !guarded_read(record + 0x20, first) || !guarded_read(record + 0x28, last) ||
        last < first || (last - first) % 8 || (last - first) / 8 > 32 || (!first && last))
        return false;
    for (uintptr_t at = first; at < last; at += 8) {
        uint32_t current = 0;
        if (!guarded_read(at, current))
            return false;
        if (current == kind) {
            item = at;
            return true;
        }
    }
    return true;
}

uint64_t on_add_repair(void *record_ptr, uint32_t kind)
{
    td_perf::Scope before(td_perf::repair);
    auto record = reinterpret_cast<uintptr_t>(record_ptr);
    uint8_t vehicle = 0xff;
    uint32_t session = 0;
    uintptr_t previous = 0;
    // Game session kinds 1..4 are practice, 5..14 qualifying/shootout,
    // 15..17 races. Require an identifiable player and the live game kind.
    bool extend = kind == 9 && keep_control && !session_ended &&
                  guarded_read(record + 0x16, vehicle) && int(vehicle) == player_vehicle.load() &&
                  repair_session_kind && guarded_read(repair_session_kind, session) &&
                  session >= 5 && session <= 14 && repair_item(record, kind, previous) && !previous;
    before.finish();
    uint64_t result = orig_add_repair(record_ptr, kind);
    td_perf::Scope after(td_perf::repair);
    // AddRepair deduplicates requests. Extend only a newly inserted job, so
    // normal recalculation cannot keep resetting a running repair's countdown.
    uintptr_t item = 0;
    float seconds = 0;
    if (extend && repair_item(record, kind, item) && item && guarded_read(item + 4, seconds) &&
        std::isfinite(seconds) && seconds > 0 && seconds < 3600.0f) {
        *reinterpret_cast<volatile float *>(item + 4) = 3600.0f;
        swofty::log("[td-probe] qualifying repair: vehicle %u ICE replacement %.1f -> 3600 s; skip repairs consumes the remaining session",
                    unsigned(vehicle), double(seconds));
    }
    return result;
}

void hook_qualifying_engine_repair()
{
    uintptr_t site = 0, session_site = 0;
    int a = swofty::api->scan(".xtls", sig_replacement_repair_site, &site);
    int b = swofty::api->scan(".xtls", sig_repair_session_kind, &session_site);
    if (a != 1 || b != 1 || !site || !session_site) {
        swofty::log("[td-probe] qualifying repair: signatures %d/%d hits, stock repair times retained", a, b);
        return;
    }
    int32_t target_rel = 0, session_rel = 0;
    memcpy(&target_rel, reinterpret_cast<void *>(site + 6), 4);
    memcpy(&session_rel, reinterpret_cast<void *>(session_site + 2), 4);
    repair_session_kind = session_site + 6 + int64_t(session_rel);
    hook_thunk_at("qualifying ICE replacement repair time", site + 10 + int64_t(target_rel),
                  reinterpret_cast<void *>(&on_add_repair), reinterpret_cast<void **>(&orig_add_repair));
}
