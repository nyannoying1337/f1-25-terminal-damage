// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// The retire command in the radio menu.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// vcCommandSelected registration resolves to this callback on the reference EXE
// (0x142f79e40). Its first 17 bytes are whole, RIP-independent instructions.
// Forward normal commands unchanged; the custom command queues retirement on frame.
constexpr const char *sig_radio_selected =
    "56 57 55 53 48 83 EC 28 89 D7 48 89 CE 48 8B 59 40 48 8B 03 48 89 D9 FF 90 90 00 00 00";
constexpr uint8_t radio_selected_prologue[] = {
    0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xec, 0x28, 0x89, 0xd7, 0x48, 0x89, 0xce, 0x48, 0x8b, 0x59, 0x40};
using radio_selected_fn = uint64_t (*)(void *, uint32_t);
radio_selected_fn orig_radio_selected;
// The existing Weather Report id matches the game's DJB2 keyword-name hash at
// runtime; the custom id was confirmed with the visible entry in 0.32.12.
constexpr uint32_t radio_retire_probe_id = 1984770275u; // Terminal Damage Retire Probe

// Inspect the launcher's deployed XML, not the package/store copy. This is a
// bounded read-only check; the original file is managed by the launcher's file.put.
int radio_probe_xml_check(FILE *file)
{
    if (!file)
        return -1;
    std::vector<char> data(256 * 1024);
    size_t bytes = fread(data.data(), 1, data.size(), file);
    if (ferror(file))
        return -1;
    return std::string_view(data.data(), bytes).find("name=\"Terminal Damage Retire Probe\"") !=
                   std::string_view::npos ? 1 : 0;
}

void log_radio_probe_xml()
{
    wchar_t executable[MAX_PATH];
    DWORD size = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    int present = -1;
    if (size && size < MAX_PATH) {
        if (auto slash = std::wcsrchr(executable, L'\\')) {
            std::wstring path(executable, slash + 1);
            path += L"audio\\2025\\speech_recognition.xml";
            FILE *file = _wfopen(path.c_str(), L"rb");
            present = radio_probe_xml_check(file);
            if (file)
                fclose(file);
        }
    }
    swofty::log("[td-probe] radio probe: launcher XML contains test command=%s",
                present < 0 ? "unreadable" : present ? "yes" : "no");
}

void register_radio_menu_probe()
{
    // The launcher file operation remains present even with the native action
    // disabled; keep its label readable in that case too.
    log_radio_probe_xml();
    auto assets = swofty::assets::detail::table();
    if (!assets || !assets->lang_set) {
        swofty::log("[td-probe] radio probe: language API unavailable; custom label not registered");
        return;
    }
    auto label = swofty::assets::lang_set("*", "lng_td_retire_probe", "Retire");
    swofty::log("[td-probe] radio retire: launcher-managed command file, label %s; command id=%u",
                swofty::assets::name(label), radio_retire_probe_id);
    swofty::log("[td-probe] radio retire: engineer reply controlled by Retirement radio voice (restart required)");
}

uint64_t on_radio_selected(void *module, uint32_t command)
{
    uint64_t result = orig_radio_selected(module, command);
    if (command == radio_retire_probe_id && radio_command_probe && !session_ended) {
        swofty::log("[td-probe] radio retire: selected id=%u; queued request (original callback forwarded)", command);
        pending_radio_command = (1ull << 32) | command;
    }
    return result;
}

bool take_radio_command(uint32_t &command)
{
    uint64_t pending = pending_radio_command.exchange(0);
    if (!pending)
        return false;
    command = uint32_t(pending);
    return true;
}

void request_retire(const char *source);

void process_radio_command()
{
    uint32_t command;
    if (!take_radio_command(command) || command != radio_retire_probe_id || !radio_command_probe || session_ended)
        return;
    if (!keep_control || player_vehicle.load() < 0 || player_pit.load() != 1 || retire_in.load() != -1.0) {
        swofty::log("[td-probe] radio retire: ignored (keep control %d, vehicle %d, pit %d, retire timer %.1f)",
                    bool(keep_control), player_vehicle.load(), player_pit.load(), retire_in.load());
        return;
    }
    if (player_broken) {
        // Match the pause menu: another menu selection neither cancels nor shortcuts retirement.
        swofty::log("[td-probe] radio retire again: still pull over or box");
        if (swofty::hud::available())
            swofty::hud::toast("RETIREMENT", "Pull over and stop, or box", 4);
        return;
    }
    request_retire("radio menu retire");
}

void hook_radio_probe()
{
    if (!radio_command_probe)
        return;
    detour("radio menu command selection", sig_radio_selected, radio_selected_prologue,
           sizeof radio_selected_prologue, reinterpret_cast<void *>(&on_radio_selected),
           reinterpret_cast<void **>(&orig_radio_selected));
}
