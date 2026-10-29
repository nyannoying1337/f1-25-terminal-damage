// Standalone Windows test: no game process and no game hooks are installed.
#ifdef NDEBUG
#undef NDEBUG // Zig's optimized builds define this; checks must still execute.
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include <cstdarg>
#include <string>
#include <vector>

namespace {
std::vector<std::string> lines;
void capture_log(const char *fmt, ...)
{
    char line[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    lines.emplace_back(line);
}
bool has(const char *needle)
{
    for (const auto &line : lines)
        if (line.find(needle) != std::string::npos)
            return true;
    return false;
}
struct Call {
    void *manager;
    const void *records;
    uint32_t type, selection;
    const void *start, *finish;
} seen{};
unsigned calls, completions;
bool diagnostics = true;
int read_setting(const char *id, uint32_t type, SwoftySettingValue *out)
{
    if (strcmp(id, "crew_asset_diagnostics") || type != SWOFTY_SETTING_TOGGLE)
        return 0;
    out->type = type;
    out->toggle = diagnostics;
    return 1;
}
int watch_setting(const char *, SwoftySettingFn, void *) { return 0; }
void record_call(void *manager, const void *records, uint32_t type, uint32_t selection,
                 const void *start, const void *finish)
{
    seen = {manager, records, type, selection, start, finish};
    ++calls;
}
void *expected_manager;
void *expected_result;
void record_completion(void *manager, void *result, uint32_t code)
{
    assert(manager == expected_manager && result == expected_result && code == 0xabcdef12);
    ++completions;
}
}

int main()
{
    SwoftyApi api{};
    api.log = capture_log;
    api.version = 3;
    api.setting = read_setting;
    api.watch_setting = watch_setting;
    swofty::api = &api;
    orig_change_assets = record_call;
    orig_assets_finished = record_completion;

    assert(std::string(asset_selection(0x100)) == "Garages");
    assert(std::string(asset_selection(0x101)) == "PitStops");
    assert(std::string(asset_selection(0)) == "none");
    assert(std::string(asset_selection(0x102)) == "unknown");

    std::vector<uint8_t> record(0x6780);
    const char *pit = "asset_groups/animation_package/scenes/pitstops/f1/pitstop_25.assetgroup";
    const char *garage = "asset_groups/animation_package/scenes/garagecrew/2025/garagecrew.assetgroup";
    memcpy(record.data() + 0x60b8, &pit, sizeof pit);
    memcpy(record.data() + 0x62e8, &garage, sizeof garage);
    uintptr_t base = reinterpret_cast<uintptr_t>(record.data());
    assert(asset_path(base, 0x60b8) == pit);
    assert(asset_path(base, 0x62e8) == garage);
    const char *empty = "";
    memcpy(record.data() + 0x60b8, &empty, sizeof empty);
    assert(asset_path(base, 0x60b8) == "<empty>");
    uintptr_t bad = 1;
    memcpy(record.data() + 0x60b8, &bad, sizeof bad);
    assert(asset_path(base, 0x60b8) == "<unreadable or overlong string>");
    memcpy(record.data() + 0x60b8, &pit, sizeof pit);

    struct { uintptr_t allocator, first, last, capacity; } vec{0, base, base + 0x6780, 1};
    void *manager = reinterpret_cast<void *>(0x1234);
    const void *start = reinterpret_cast<void *>(0x5678);
    const void *finish = reinterpret_cast<void *>(0x9abc);
    on_change_assets(manager, &vec, 0xaabbcc00, 0xdead0101, start, finish);
    assert(calls == 1 && seen.manager == manager && seen.records == &vec);
    assert(seen.type == 0xaabbcc00 && seen.selection == 0xdead0101);
    assert(seen.start == start && seen.finish == finish);
    assert(has("PitStops path=asset_groups/") && has("Garages path=asset_groups/"));
    assert(has("selection=0x0101 PitStops"));

    lines.clear();
    on_change_assets(manager, nullptr, 0, 0x100, start, finish);
    assert(calls == 2 && seen.records == nullptr && has("vector unreadable or unexpected"));
    vec.last = vec.first - 1;
    on_change_assets(manager, &vec, 0, 0x100, start, finish);
    assert(calls == 3);
    vec.last = vec.first + 1;
    on_change_assets(manager, &vec, 0, 0x100, start, finish);
    assert(calls == 4);
    vec.first = vec.last = 0;
    lines.clear();
    on_change_assets(manager, &vec, 0, 0, start, finish);
    assert(calls == 5 && has("records=0"));

    alignas(8) uint8_t manager_data[0x68]{};
    uint32_t state = 1;
    uint16_t selection = 0x101;
    memcpy(manager_data, &state, sizeof state);
    memcpy(manager_data + 0x60, &selection, sizeof selection);
    expected_manager = manager_data;
    expected_result = reinterpret_cast<void *>(0x1357);
    on_assets_finished(expected_manager, expected_result, 0xabcdef12);
    assert(completions == 1 && has("scene availability unverified"));
    // A diagnostic read must not mutate the manager or selection.
    assert(manager_data[0] == 1 && manager_data[0x60] == 1 && manager_data[0x61] == 1);
    diagnostics = false;
    lines.clear();
    on_change_assets(manager, nullptr, 0, 0x100, start, finish);
    on_assets_finished(expected_manager, expected_result, 0xabcdef12);
    assert(calls == 6 && completions == 2 && lines.empty());
    puts("PASS: paths, invalid/empty vectors, forwarding and diagnostics on/off");
}
