// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Asset change diagnostics.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Read-only probes of the normal asset selection and its completion callback.
// The six arguments of ChangeResidentAssets are forwarded unchanged. Its first
// 16 bytes and the callback's first 15 bytes are whole, RIP-independent instructions.
constexpr const char *sig_change_resident_assets =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 78 44 89 CF 48 89 D3 48 89 CE 80 3D ?? ?? ?? ?? 01 "
    "4C 8D 0D ?? ?? ?? ?? 45 0F B6 F0 0F 85 90 00 00 00 B8 C1 00 00 00";
constexpr uint8_t change_assets_prologue[] = {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xec, 0x78};
// Start/finish callbacks have matching entry signatures. Resolve the finish
// callback from its unique registration site instead (first lea rdx).
constexpr const char *sig_assets_finished_site =
    "48 8D 15 ?? ?? ?? ?? 48 89 54 24 48 48 89 74 24 50 48 8D 15 ?? ?? ?? ?? "
    "48 89 54 24 68 48 89 74 24 70 31 D2 48 85 C0";
constexpr uint8_t assets_finished_prologue[] = {
    0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xec, 0x28, 0x44, 0x89, 0xc6, 0x48, 0x89, 0xd7};
using change_assets_fn = void (*)(void *, const void *, uint32_t, uint32_t, const void *, const void *);
using assets_finished_fn = void (*)(void *, void *, uint32_t);
change_assets_fn orig_change_assets;
assets_finished_fn orig_assets_finished;
std::atomic<uint32_t> asset_request_serial{0};

const char *asset_selection(uint32_t selection)
{
    if (!(selection & 0xff00))
        return "none";
    return (selection & 0xff) == 0 ? "Garages" : (selection & 0xff) == 1 ? "PitStops" : "unknown";
}

std::string asset_path(uintptr_t record, uintptr_t offset)
{
    uintptr_t ptr = 0;
    std::string value;
    if (!guarded_read(record + offset, ptr))
        return "<unreadable pointer>";
    if (!ptr)
        return "<null>";
    char first = 0;
    if (guarded_read(ptr, first) && !first)
        return "<empty>";
    if (!swofty::mem::c_string(ptr, value, 512))
        return "<unreadable or overlong string>";
    // Keep one path per log line even if an unexpected string contains controls.
    for (char &c : value)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    return value.empty() ? "<empty>" : value;
}

void on_change_assets(void *manager, const void *records, uint32_t type, uint32_t selection,
                      const void *on_start, const void *on_finish)
{
    if (crew_asset_diagnostics) {
        uint32_t request = asset_request_serial.fetch_add(1) + 1;
        uint8_t kind = uint8_t(type);
        swofty::log("[td-probe] assets: request=%u manager=%p type=%u %s selection=0x%04x %s caller=%p",
                    request, manager, unsigned(kind), kind == 0 ? "Session" : kind == 1 ? "Frontend" : "unknown",
                    unsigned(selection & 0xffff), asset_selection(selection), __builtin_return_address(0));
        constexpr uintptr_t stride = 0x6780;
        uintptr_t first = 0, last = 0;
        uintptr_t vec = reinterpret_cast<uintptr_t>(records);
        if (!vec || !guarded_read(vec + 8, first) || !guarded_read(vec + 0x10, last) ||
            last < first || (last - first) % stride || (last - first) / stride > 64 || (!first && last)) {
            swofty::log("[td-probe] assets: request=%u record vector unreadable or unexpected; original call unchanged",
                        request);
        } else {
            unsigned count = unsigned((last - first) / stride);
            swofty::log("[td-probe] assets: request=%u records=%u (record indices are not vehicle IDs)", request, count);
            for (unsigned i = 0; i < count; i++) {
                uintptr_t record = first + uintptr_t(i) * stride;
                auto pit = asset_path(record, 0x60b8);
                auto garage = asset_path(record, 0x62e8);
                swofty::log("[td-probe] assets: request=%u record=%u PitStops path=%s", request, i, pit.c_str());
                swofty::log("[td-probe] assets: request=%u record=%u Garages path=%s", request, i, garage.c_str());
            }
        }
    }
    orig_change_assets(manager, records, type, selection, on_start, on_finish);
}

void on_assets_finished(void *manager, void *result, uint32_t code)
{
    if (crew_asset_diagnostics) {
        uint32_t state = 0xffffffff;
        uint16_t selection = 0;
        auto at = reinterpret_cast<uintptr_t>(manager);
        guarded_read(at, state);
        guarded_read(at + 0x60, selection);
        // The callback payload/code are logged raw: their semantics are not yet
        // established. Callback invocation alone does not prove a usable scene.
        swofty::log("[td-probe] assets: load-finished callback manager=%p state=%u selection=0x%04x %s "
                    "payload=%p code=%u (raw; scene availability unverified)",
                    manager, state, unsigned(selection), asset_selection(selection), result, code);
    }
    orig_assets_finished(manager, result, code);
}

void hook_asset_diagnostics()
{
    if (!crew_asset_diagnostics)
        return;
    bool change = detour("VehicleAssetManager::changeResidentAssets", sig_change_resident_assets,
                         change_assets_prologue, sizeof change_assets_prologue,
                         reinterpret_cast<void *>(&on_change_assets), reinterpret_cast<void **>(&orig_change_assets));
    uintptr_t site = 0;
    int hits = swofty::api->scan(".xtls", sig_assets_finished_site, &site);
    bool finish = false;
    if (hits == 1 && site) {
        int32_t rel = 0;
        if (guarded_read(site + 3, rel))
            finish = detour_at("VehicleAssetManager::onLoadFinished", site + 7 + int64_t(rel),
                               assets_finished_prologue, sizeof assets_finished_prologue,
                               reinterpret_cast<void *>(&on_assets_finished),
                               reinterpret_cast<void **>(&orig_assets_finished));
    } else {
        swofty::log("[td-probe] assets: finish callback registration has %d hits, completion not hooked", hits);
    }
    swofty::log("[td-probe] assets: diagnostic hooks selection=%d completion=%d; asset requests remain unchanged",
                change, finish);
}
