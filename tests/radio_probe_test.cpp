// Radio diagnostics must preserve every original invocation and queue ids only.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
unsigned calls;
unsigned overlay_calls, label_calls;
bool enabled = true;
bool keep_enabled = true;
void *expected_module;
uint32_t expected_command;
void discard_log(const char *, ...) {}
int read_setting(const char *id, uint32_t type, SwoftySettingValue *out)
{
    if ((strcmp(id, "radio_command_probe") && strcmp(id, "keep_control")) || type != SWOFTY_SETTING_TOGGLE)
        return 0;
    out->type = type;
    out->toggle = !strcmp(id, "keep_control") ? keep_enabled : enabled;
    return 1;
}
int watch_setting(const char *, SwoftySettingFn, void *) { return 0; }
int overlay(const char *target, const char *source)
{
    assert(!strcmp(target, "audio/2025/speech_recognition.xml"));
    assert(!strcmp(source, "assets/radio/speech_recognition.xml"));
    ++overlay_calls;
    return SWOFTY_ASSET_OK;
}
int label(const char *language, const char *key, const char *text)
{
    assert(!strcmp(language, "*") && !strcmp(key, "lng_td_retire_probe") && !strcmp(text, "Retire"));
    ++label_calls;
    return SWOFTY_ASSET_OK;
}
uint64_t original(void *module, uint32_t command)
{
    assert(module == expected_module && command == expected_command);
    // Notice publication must happen after the original callback.
    assert(pending_radio_command == 0);
    ++calls;
    return 0x123456789abcdef0ull;
}
}

int main()
{
    SwoftyApi api{};
    api.version = 4;
    api.log = discard_log;
    api.setting = read_setting;
    api.watch_setting = watch_setting;
    swofty::api = &api;
    // Older/unavailable asset tables fail without touching the game.
    register_radio_menu_probe();
    assert(!overlay_calls && !label_calls);
    SwoftyAssets assets{};
    assets.overlay_file = overlay;
    assets.lang_set = label;
    api.assets = &assets;
    register_radio_menu_probe();
    assert(overlay_calls == 0 && label_calls == 1);
    orig_radio_selected = original;
    reset_session();
    session_ended = false;
    // No module dereference: a deliberately invalid value is forwarded only.
    expected_module = reinterpret_cast<void *>(0x1234);
    for (uint32_t id : {0u, 42u, 0xffffffffu, radio_retire_probe_id}) {
        expected_command = id;
        assert(on_radio_selected(expected_module, id) == 0x123456789abcdef0ull);
        uint32_t seen = 0;
        bool custom = id == radio_retire_probe_id;
        assert(take_radio_command(seen) == custom);
        if (custom)
            assert(seen == id);
        assert(!take_radio_command(seen));
        assert(!player_broken && !retire_requested && !car_dead && throttle_hook.cap == 1.0f);
    }
    enabled = false;
    register_radio_menu_probe();
    assert(overlay_calls == 0 && label_calls == 2);
    expected_command = radio_retire_probe_id;
    on_radio_selected(expected_module, expected_command);
    assert(!pending_radio_command);
    enabled = true;
    session_ended = true;
    on_radio_selected(expected_module, expected_command);
    assert(!pending_radio_command);
    session_ended = false;
    on_radio_selected(expected_module, expected_command);
    assert(pending_radio_command);
    reset_car_state();
    assert(!pending_radio_command && calls == 7);
    auto select_retire = [] {
        pending_radio_command = (1ull << 32) | radio_retire_probe_id;
        process_radio_command();
    };
    player_vehicle = 3;
    select_retire();
    assert(player_broken && retire_requested && !car_dead);
    assert(throttle_hook.cap == 1.0f && terminal_mask == 0 && retire_in == -1.0);
    still_for = 4.5;
    select_retire();
    assert(player_broken && retire_requested && still_for == 4.5 && retire_in == -1.0);
    // A normal command must not overwrite a queued retirement request.
    pending_radio_command = (1ull << 32) | radio_retire_probe_id;
    expected_command = 42;
    // Original-forwarding stub checks an empty queue; use a forwarding-only stub here.
    orig_radio_selected = [](void *, uint32_t id) -> uint64_t { assert(id == 42); return 17; };
    assert(on_radio_selected(expected_module, 42) == 17);
    assert(pending_radio_command == ((1ull << 32) | radio_retire_probe_id));
    reset_car_state();
    player_broken = true;
    terminal_mask = 3;
    throttle_hook.cap = 0.0f;
    select_retire();
    assert(player_broken && !retire_requested && terminal_mask == 3 && throttle_hook.cap == 0.0f);
    // Reject requests outside the same on-track conditions as pause-menu retirement.
    for (int gate = 0; gate < 7; ++gate) {
        reset_car_state();
        player_vehicle = gate == 0 ? -1 : 3;
        player_pit = gate == 1 ? 9 : 1;
        retire_in = gate == 2 ? 0.0 : gate == 3 ? -2.0 : -1.0;
        keep_enabled = gate != 4;
        enabled = gate != 5;
        session_ended = gate == 6;
        select_retire();
        assert(!player_broken && !retire_requested);
    }
    enabled = keep_enabled = true;
    session_ended = false;
    assert(radio_probe_xml_check(nullptr) == -1);
    FILE *xml = tmpfile();
    assert(xml);
    fputs("<SpeechRecognition/>", xml);
    rewind(xml);
    assert(radio_probe_xml_check(xml) == 0);
    fclose(xml);
    xml = tmpfile();
    assert(xml);
    fputs("<Keyword name=\"Terminal Damage Retire Probe\"/>", xml);
    rewind(xml);
    assert(radio_probe_xml_check(xml) == 1);
    fclose(xml);
    puts("PASS: radio forwarding, queued retirement, full power, repeated selection, failure and session guards");
}
