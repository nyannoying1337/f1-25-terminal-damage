// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Presets.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, at file scope.
// It is not a standalone header; it uses what the fragments before it define.

// ---- Presets (presets.hpp) ----
namespace {
constexpr const char *addon_id = "nick.terminal-damage-probe";
constexpr const char *preset_file = "preset_applied.txt";  // the preset applied last, in the addon's storage
std::atomic<bool> preset_changed{false}, preset_option_changed{false};  // set by the settings watchers

// An option a preset sets, as the settings command takes it ("true"/"false" for a toggle); empty if unknown.
std::string preset_current(std::string_view o)
{
    auto tf = [](bool b) { return std::string(b ? "true" : "false"); };
    auto ch = [](std::string_view v) { return std::string(v); };
    if (o == "keep_control") return tf(keep_control);
    if (o == "keep_game_failure") return tf(keep_game_failure);
    if (o == "gp_failures") return ch(gp_failures);
    if (o == "damaged_power") return ch(damaged_power);
    if (o == "red_flag_retire") return tf(red_flag_retire);
    if (o == "game_failure_timeout") return ch(game_failure_timeout);
    if (o == "damage_panel") return ch(damage_panel);
    if (o == "engineer_radio") return tf(engineer_radio);
    if (o == "pause_menu_retire") return tf(pause_menu_retire);
    return {};
}

struct preset_state {
    bool started = false;
    uint64_t start_at = 0;
    int applying = -1;  // the preset being applied, -1 none
    int entry = 0, set = 0, failed = 0, tries = 0;
    bool restart = false, in_game = false;  // in_game: picked in ADDON SETTINGS, not in the launcher
    uint64_t next_try = 0, quiet_until = 0;  // no Custom check until the applied values have settled
    std::string stored;
    bool has_stored = false;
    bool legacy = false;  // the kept file is from before 0.75 (no ";v2" mark): Custom was the default then
    std::string popup;  // the result for the menus (a toast is drawn only in a session), empty: none
    uint64_t first = 0, popup_next = 0;
};
preset_state preset_rt;

void preset_store(std::string_view v)
{
    if (preset_rt.has_stored && preset_rt.stored == v && !preset_rt.legacy)
        return;
    auto r = swofty::storage::write(preset_file, std::string(v) + ";v2");
    preset_rt.stored = std::string(v);
    preset_rt.has_stored = true;
    preset_rt.legacy = false;
    if (r != swofty::storage::Result::ok)
        swofty::log("[td-probe] preset: could not save which preset was applied (storage %d); a preset picked in the "
                    "launcher then applies at every start", int(r));
}

void preset_begin(int p, const char *why)
{
    preset_rt.applying = p;
    preset_rt.entry = preset_rt.set = preset_rt.failed = preset_rt.tries = 0;
    preset_rt.restart = false;
    preset_rt.in_game = !strcmp(why, "picked in the game");
    preset_rt.next_try = 0;
    swofty::log("[td-probe] preset: applying %s (%s)", presets::titles[p], why);
}

// The result for the player. In testing the option list did not change with the preset. The open ADDON SETTINGS screen
// keeps the rows it showed (the core publishes them when the screen opens), and the launcher runs no addon code: a
// preset picked there applies at the next game start. Both are said here and in the preset's description
// (0.51.135).
void preset_result_text(char *out, size_t n, int p, const preset_state &st)
{
    char failed[80] = "";
    if (st.failed)
        snprintf(failed, sizeof failed, " %d could not be set: update Swofty, or set them by hand.", st.failed);
    if (st.set == 0 && st.failed == 0)
        snprintf(out, n, "%s: every option already matches.", presets::titles[p]);
    else
        snprintf(out, n, "%s%s: %d option(s) changed.%s%s%s", presets::titles[p],
                 st.in_game ? "" : " (picked in the launcher)", st.set,
                 st.in_game && st.set ? " Reopen ADDON SETTINGS to see them." : "",
                 st.restart ? " Some need a game restart." : "", failed);
}

// One pass over the table: each option the preset sets differently goes through the settings command. A "late"
// answer is tried again after half a second (up to 20 times), anything else is logged and skipped.
void preset_apply_step(uint64_t now)
{
    preset_state &st = preset_rt;
    if (now < st.next_try)
        return;
    int p = st.applying;
    while (st.entry < presets::n_entries) {
        const presets::Entry &e = presets::table[st.entry];
        if (preset_current(e.option) == e.value[p]) {
            ++st.entry;
            st.tries = 0;
            continue;
        }
        auto r = swofty::hub::set_option(addon_id, e.option, e.value[p]);
        if (r == swofty::hub::Result::late && ++st.tries <= 20) {
            st.next_try = now + 500;
            return;
        }
        if (r == swofty::hub::Result::ok) {
            ++st.set;
            st.restart |= e.restart;
        } else {
            ++st.failed;
            swofty::log("[td-probe] preset: %s could not be set to %s (%s)", e.option, e.value[p], swofty::hub::name(r));
        }
        ++st.entry;
        st.tries = 0;
    }
    swofty::log("[td-probe] preset: %s applied, %d option(s) changed%s%s", presets::titles[p], st.set,
                st.failed ? ", some could not be set (update Swofty, or set them by hand)" : "",
                st.restart ? "; some take effect after a game restart" : "");
    // In the menus a toast is not drawn, so there it is a popup (preset_tick).
    char text[200];
    preset_result_text(text, sizeof text, p, st);
    if (!session_ended && player_vehicle.load() >= 0) {
        if (swofty::hud::available())
            swofty::hud::toast("PRESET", text, 6);
    } else {
        st.popup = text;
        st.popup_next = 0;
    }
    preset_store(presets::names[p]);
    st.applying = -1;
    st.quiet_until = now + 2000;
    preset_option_changed = false;
}

// Frame thread, every frame (menus too). At the start: apply a preset picked in the launcher, or turn the preset to
// Custom if one of its options was changed there. In the game: apply a picked preset; a changed option turns it to
// Custom.
void preset_tick()
{
    preset_state &st = preset_rt;
    uint64_t now = GetTickCount64();
    if (!st.first)
        st.first = now;
    // The result popup: at once for a pick in ADDON SETTINGS, on the main menu for a pick in the launcher.
    if (!st.popup.empty() && now >= st.popup_next && !time_trial &&
        (session_ended || player_vehicle.load() < 0)) {
        st.popup_next = now + 1000;
        if (st.in_game || welcome_menu_ready(now - st.first)) {
            auto r = swofty::popup::show("TERMINAL DAMAGE PRESET", st.popup);
            swofty::log("[td-probe] preset: popup %s", swofty::popup::name(r));
            if (r != swofty::popup::Result::late)
                st.popup.clear();
        }
    }
    if (!st.started) {
        if (!st.start_at)
            st.start_at = now + 3000;  // let the core settle
        if (now < st.start_at)
            return;
        st.started = true;
        std::string kept;
        st.has_stored = swofty::storage::read(preset_file, kept) == swofty::storage::Result::ok;
        while (!kept.empty() && (kept.back() == '\n' || kept.back() == '\r' || kept.back() == ' '))
            kept.pop_back();
        // Since 0.75 the name is followed by ";v2"; a file without it was written while Custom was the default.
        size_t mark = kept.find(';');
        st.legacy = st.has_stored && mark == std::string::npos;
        if (mark != std::string::npos)
            kept.resize(mark);
        st.stored = kept;
        std::string cur = std::string(std::string_view(preset));
        int p = presets::index_of(cur);
        int differ = presets::differing(p, [](const char *o) { return preset_current(o); });
        switch (presets::at_start(st.has_stored, st.stored, cur, differ, st.legacy)) {
        case presets::Start::apply: preset_begin(p, "picked in the launcher"); break;
        case presets::Start::to_custom:
            swofty::log("[td-probe] preset: %d option(s) differ from %s, the preset shows Custom", differ,
                        presets::titles[p]);
            swofty::hub::set_option(addon_id, "preset", "custom");
            preset_store("custom");
            break;
        case presets::Start::remember: preset_store(cur); break;
        case presets::Start::nothing: break;
        }
        if (st.legacy && st.applying < 0)
            preset_store(st.stored);  // the kept file in the 0.75 form
        preset_changed = false;
        preset_option_changed = false;
        st.quiet_until = now + 2000;
        return;
    }
    if (preset_changed.exchange(false)) {
        std::string cur = std::string(std::string_view(preset));
        if (int p = presets::index_of(cur); p >= 0)
            preset_begin(p, "picked in the game");
        else
            preset_store(cur);
    }
    if (st.applying >= 0) {
        preset_apply_step(now);
        return;
    }
    if (now < st.quiet_until || !preset_option_changed.exchange(false))
        return;
    int p = presets::index_of(std::string_view(preset));
    if (int differ = presets::differing(p, [](const char *o) { return preset_current(o); }); p >= 0 && differ > 0) {
        swofty::log("[td-probe] preset: %d option(s) changed from %s, the preset shows Custom", differ,
                    presets::titles[p]);
        if (swofty::hub::set_option(addon_id, "preset", "custom") == swofty::hub::Result::ok)
            preset_store("custom");
    }
}

}  // namespace

SWOFTY_ON_CHANGE(preset, std::string_view)
{
    preset_changed = true;
}
