// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
// Presets (0.51.117): the "preset" option sets the main gameplay options at once. The addon moves each option
// through Swofty's own settings command (swofty::hub::set_option, the path ADDON SETTINGS takes), so the menu shows
// the new values and they stay saved. Picked in the game it applies at once; picked in the launcher it applies at the
// next game start (the preset last applied is kept in the addon's storage). Changing one of these options away from
// the preset's value afterwards turns the preset to Custom. Hotkeys, the failure season, wheel physics, sounds and
// every debug option are never touched, and since 0.75 neither is suspension damage (experimental and opt-in; it
// adds up with other damage mods such as Heavier Damage). Since 0.75 the option defaults are the Realistic preset and
// Realistic is the preset's default. Pure tables and checks here; the probe does the reads and writes.
#pragma once
#include <string_view>

namespace presets {

inline constexpr int n_presets = 3;
inline constexpr const char *names[n_presets] = {"casual", "realistic", "hardcore"};
inline constexpr const char *default_preset = "realistic";  // the manifest's default (0.75)
inline constexpr const char *titles[n_presets] = {"Casual", "Realistic", "Hardcore"};

// Values as the settings command takes them: a choice value, or "true"/"false" for a toggle.
struct Entry {
    const char *option;
    const char *value[n_presets];  // casual, realistic, hardcore
    bool restart;                  // takes effect after a game restart
};
inline constexpr Entry table[] = {
    {"keep_control", {"true", "true", "true"}, false},
    {"keep_game_failure", {"true", "true", "true"}, false},
    {"gp_failures", {"off", "realistic", "realistic"}, false},
    {"damaged_power", {"0.5", "0.3", "0.2"}, false},
    {"red_flag_retire", {"false", "true", "true"}, false},
    {"game_failure_timeout", {"retire", "dsq", "dsq"}, false},
    {"damage_panel", {"always", "auto", "off"}, false},
    {"engineer_radio", {"true", "true", "true"}, false},
    {"pause_menu_retire", {"false", "true", "true"}, false},
};
inline constexpr int n_entries = int(sizeof table / sizeof table[0]);

// The preset's index, or -1 for "custom" or anything unknown.
inline int index_of(std::string_view name)
{
    for (int i = 0; i < n_presets; ++i)
        if (name == names[i])
            return i;
    return -1;
}

// The entry for an option, or nullptr when no preset sets it.
inline const Entry *entry_of(std::string_view option)
{
    for (const Entry &e : table)
        if (option == e.option)
            return &e;
    return nullptr;
}

// How many of the preset's options differ from the current values. get(option) returns the current value as the
// settings command takes it ("true"/"false" for a toggle).
template <class Get> int differing(int preset, Get get)
{
    if (preset < 0 || preset >= n_presets)
        return 0;
    int n = 0;
    for (const Entry &e : table)
        n += std::string_view(get(e.option)) != e.value[preset];
    return n;
}

// What to do at the game start. stored: the preset applied last (has_stored false: nothing kept yet, e.g. the first
// start with this version), current: the option's value now, differ: differing(current) for the current values.
// A preset picked in the launcher (nothing kept yet, or another one kept) is applied; the same preset as last time
// with an option changed since turns to Custom, since that change was made by hand. The first start with nothing kept
// and the default preset (an update from a version without presets, or a fresh install) never applies it: options
// set by hand before stay, and the preset shows Custom. legacy: the kept value was written before 0.75, when Custom
// was the default; a kept Custom with the default preset now is that new default, not a pick in the launcher.
enum class Start { nothing, apply, to_custom, remember };
inline Start at_start(bool has_stored, std::string_view stored, std::string_view current, int differ,
                      bool legacy = false)
{
    int now = index_of(current);
    if (now < 0)
        return has_stored && stored == current ? Start::nothing : Start::remember;
    if ((!has_stored || (legacy && stored == "custom")) && current == default_preset)
        return differ > 0 ? Start::to_custom : Start::remember;
    if (!has_stored || stored != current)
        return differ > 0 ? Start::apply : Start::remember;  // picked in the launcher since the last start
    return differ > 0 ? Start::to_custom : Start::nothing;   // an option changed in the launcher since
}

}  // namespace presets
