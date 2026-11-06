// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Welcome popup and setting watchers.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, at file scope.
// It is not a standalone header; it uses what the fragments before it define.

// ---- Welcome popup (0.51.120): once per install, on the main menu, pointing to the preset and the option texts ----
namespace {
// In the addon's storage once the popup was queued. 0.51.121: a new name, as 0.51.120's welcome_shown.txt was
// written when the popup came with the legal screen (log 20261104-161542: 17:16:14, the main menu at 17:16:37).
constexpr const char *welcome_file = "welcome_seen.txt";
// F1 25's front-end home state: it enters with the main menu (logs 0.51.119 and 0.51.120: 7-10 s before the menu
// gets its ADDON SETTINGS item) and exits into a session.
constexpr uint32_t home_flow = 0x957880c6u;
struct welcome_state {
    uint64_t first = 0, next = 0;  // the first frame; the next look (twice a second)
    bool done = false;             // queued, shown before, or no storage to remember it in
};
welcome_state welcome_rt;

// The main menu is up: the home state entered 10 s ago or more. A game build without that state, or a core without
// the flow states: 3 minutes after the first frame.
bool welcome_menu_ready(uint64_t since_first_ms)
{
    swofty::career::Status st = swofty::career::Status::none;
    uint64_t age = 0;
    if (swofty::career::flow_status(home_flow, &st, &age) == swofty::career::Result::ok)
        return (st == swofty::career::Status::entering || st == swofty::career::Status::updating) && age >= 10000;
    return since_first_ms >= 180000;
}

std::string welcome_text()
{
    int p = presets::index_of(std::string_view(preset));
    char text[512];
    snprintf(text, sizeof text,
             "Terminal Damage is on. Pick how hard it is in ADDON SETTINGS > Terminal Damage > Preset:\n"
             "Casual: no random failures, forgiving rules.\n"
             "Realistic: real failure rates, FIA rules.\n"
             "Hardcore: Realistic, no damage panel, less power when broken.\n"
             "Your preset now: %s. Every option has a description; suspension damage is experimental and off. "
             "This message shows once.",
             p >= 0 ? presets::titles[p] : "Custom");
    return text;
}

// Frame thread, every frame until done. The core shows the popup at once on top of the current screen (it found the
// game's message system without waiting for a game message), so wait for the main menu.
void welcome_tick(uint64_t now)
{
    welcome_state &w = welcome_rt;
    if (w.done)
        return;
    if (!w.first) {
        w.first = now;
        return;
    }
    if (now < w.next || (!session_ended && player_vehicle.load() >= 0))
        return;  // in a session: wait for the menus
    w.next = now + 500;
    if (!welcome_menu_ready(now - w.first))
        return;
    if (!swofty::storage::available()) {
        w.done = true;
        swofty::log("[td-probe] welcome: no addon storage on this core, no welcome popup (it could not be shown once)");
        return;
    }
    std::string seen;
    auto r = swofty::storage::read(welcome_file, seen);
    if (r == swofty::storage::Result::late) {
        w.next = now + 5000;
        return;
    }
    w.done = true;
    if (r == swofty::storage::Result::ok)
        return;  // shown before
    auto shown = swofty::popup::show("TERMINAL DAMAGE", welcome_text());
    swofty::log("[td-probe] welcome: popup %s", swofty::popup::name(shown));
    if (shown == swofty::popup::Result::ok &&
        swofty::storage::write(welcome_file, std::string_view("shown 0.51.121\n")) != swofty::storage::Result::ok)
        swofty::log("[td-probe] welcome: could not save that the popup was shown; it comes again at the next start");
}
}  // namespace
SWOFTY_ON_CHANGE(keep_game_failure, bool) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(gp_failures, std::string_view) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(damaged_power, std::string_view) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(red_flag_retire, bool) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(game_failure_timeout, std::string_view) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(damage_panel, std::string_view) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(engineer_radio, bool) { preset_option_changed = true; }
SWOFTY_ON_CHANGE(pause_menu_retire, bool) { preset_option_changed = true; }

SWOFTY_ON_CHANGE(keep_control, bool on)
{
    preset_option_changed = true;
    swofty::log("[td-probe] keep_control %s", on ? "on" : "off");
}

SWOFTY_ON_CHANGE(performance_diagnostics, bool on)
{
    td_perf::enabled = on;
    swofty::log("[td-probe] private-hook performance counters %s", on ? "on" : "off");
}

SWOFTY_ON_CHANGE(crew_asset_diagnostics, bool on)
{
    crew_diagnostics_active = on;
}
