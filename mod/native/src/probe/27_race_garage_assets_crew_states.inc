// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Garage assets in a race, crew state hooks.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

void patch_garage_assets_in_race()
{
    if (!garage_assets_in_race)
        return;
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_asset_selection_site, &site);
    if (n != 1 || !site) {
        swofty::log("[td-probe] assets: selection site has %d hits, race keeps PitStops only", n);
        return;
    }
    uintptr_t make_name = rel_target(site + 0x3d, site + 0x41);
    uintptr_t registry = rel_target(site + 0x44, site + 0x48);
    uintptr_t find_group = rel_target(site + 0x4e, site + 0x52);
    uintptr_t fail = rel_target(site + 0x57, site + 0x5b);
    uintptr_t add_group = rel_target(site + 0x62, site + 0x66);
    uintptr_t patch_at = site + 0x22, resume = site + 0x73;

    uint8_t *stub = alloc_near(patch_at);
    if (!stub) {
        swofty::log("[td-probe] assets: no memory within 2 GB for the stub, race keeps PitStops only");
        return;
    }
    emitter e{stub};
    for (uint32_t field : {0x60b8u, 0x62e8u}) { // PitStops first, as the game does, then Garages
        e.bytes({0x49, 0x8B, 0x94, 0x24}); // mov rdx, [r12+field]
        e.u32(field);
        e.bytes({0x48, 0x85, 0xD2, 0x74, 0}); // test rdx, rdx; jz next
        uint8_t *jz1 = e.p - 1;
        e.bytes({0x80, 0x3A, 0x00, 0x74, 0}); // cmp byte [rdx], 0; jz next
        uint8_t *jz2 = e.p - 1;
        e.bytes({0x48, 0x8B, 0x5C, 0x24, 0x40, 0x48, 0x89, 0xF9}); // mov rbx, [rsp+0x40]; mov rcx, rdi
        e.call(make_name);
        e.bytes({0x48, 0xB8}); // mov rax, registry; mov rcx, [rax]; mov rdx, [rsp+0x48]
        e.u64(registry);
        e.bytes({0x48, 0x8B, 0x08, 0x48, 0x8B, 0x54, 0x24, 0x48});
        e.call(find_group);
        e.bytes({0x48, 0x85, 0xC0, 0x75, 14}); // test rax, rax; jnz over the fail jump
        e.jmp(fail);
        e.bytes({0x48, 0x89, 0xD9, 0x48, 0x89, 0xC2}); // mov rcx, rbx; mov rdx, rax
        e.call(add_group);
        *jz1 = uint8_t(e.p - (jz1 + 1));
        *jz2 = uint8_t(e.p - (jz2 + 1));
    }
    e.jmp(resume);
    if (!jump_to_stub(patch_at, 13, stub, e.p))
        return;
    auto *code = reinterpret_cast<uint8_t *>(patch_at);
    swofty::log("[td-probe] assets: race loads Garages next to PitStops (patched %p, stub %p, %d bytes)", code, stub,
                int(e.p - stub));
}

// GarageCrewInstance setup reads `[scene+0xe0]` before its own null check of the
// spawned scene (instance+0xc8), the 0.11.0 race crash. The stub checks first:
// null takes the game's existing no-scene path (site+0xe6) with esi = 0.
constexpr const char *sig_crew_scene_read =
    "48 8B BD C8 00 00 00 0F B6 B7 E0 00 00 00 48 85 FF 0F 84 CF 00 00 00 4C 8B B5 D0 00 00 00";

void guard_crew_scene_read()
{
    if (!garage_crew_in_race || !garage_assets_in_race)
        return;
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_crew_scene_read, &site);
    uint8_t *stub = n == 1 && site ? alloc_near(site) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] garage: crew scene read has %d hits or no stub memory, not guarded", n);
        return;
    }
    emitter e{stub};
    e.bytes({0x31, 0xF6, 0x48, 0x85, 0xFF, 0x75, 14}); // xor esi, esi; test rdi, rdi; jnz over
    e.jmp(site + 0x17 + 0xcf);                           // the game's null path
    e.bytes({0x0F, 0xB6, 0xB7, 0xE0, 0x00, 0x00, 0x00}); // movzx esi, byte [rdi+0xe0]
    e.jmp(site + 0x17);
    if (jump_to_stub(site + 7, 16, stub, e.p))
        swofty::log("[td-probe] garage: crew scene read null-guarded at %p", reinterpret_cast<void *>(site + 7));
}

void hook_crew_states()
{
    static const char first_name[] = "StateCarIsReadyForTrack";
    char sig[sizeof first_name * 3 + 1] = {};
    for (size_t i = 0; i < sizeof first_name; i++)
        snprintf(sig + i * 3, 4, "%02X ", unsigned(uint8_t(first_name[i])));
    sig[sizeof first_name * 3 - 1] = 0;
    uintptr_t name = 0;
    int n = swofty::api->scan(".xtext", sig, &name);
    if (n != 1 || !name) {
        swofty::log("[td-probe] crew states: name has %d hits, not hooked", n);
        return;
    }
    uintptr_t base = name - crew_state_name;
    for (int i = 0; i < crew_state_count; i++) {
        auto *nm = reinterpret_cast<const char *>(base + i * crew_state_size + crew_state_name);
        uintptr_t fn = 0;
        if (strncmp(nm, "State", 5) || !guarded_read(base + i * crew_state_size + crew_state_slot, fn) || !fn) {
            swofty::log("[td-probe] crew states: entry %d does not look like a state, not hooked", i);
            return;
        }
    }
    void *handlers[crew_state_count];
    crew_handlers(std::make_integer_sequence<int, crew_state_count>{}, handlers);
    for (int i = 0; i < crew_state_count; i++) {
        uintptr_t slot = base + i * crew_state_size + crew_state_slot;
        crew_states[i].name = reinterpret_cast<const char *>(base + i * crew_state_size + crew_state_name);
        memcpy(&crew_states[i].orig, reinterpret_cast<void *>(slot), 8);
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void *>(slot), 8, PAGE_READWRITE, &old))
            continue;
        memcpy(reinterpret_cast<void *>(slot), &handlers[i], 8);
        VirtualProtect(reinterpret_cast<void *>(slot), 8, old, &old);
        swofty::log("[td-probe] hooked crew %s (slot %p)", crew_states[i].name, reinterpret_cast<void *>(slot));
    }
}
