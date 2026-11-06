// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Throttle store, engine audio and replay terminal gate stubs.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

int player_speed();
bool player_car_running();

// Throttle: car +0x188, written each frame by the controller update at the site below
// (0.22.4 hardware watch: 605 writes in 10 s, all from there): clamp(input, 0, 1) -> vmovss [r12+0x188], xmm0
// with r12 = car. The stub caps xmm0 at throttle_hook.cap when r12 is the player's car.
constexpr const char *sig_throttle_store =
    "C4 C1 7A 10 8C 94 00 02 00 00 C5 E8 57 D2 C5 EA 5F D1 C5 FA C2 C9 01 C4 E3 69 4A 0D ?? ?? ?? ?? 10 C5 FA 5D C1 "
    "C4 C1 7A 11 84 24 88 01 00 00 44 0F B6 10 41 C0 EA 06 41 80 E2 02 41 0F B6 94 24 B8 01 00 00";
constexpr uintptr_t throttle_store_at = 0x25;
std::atomic<bool> throttle_hooked{false};

void hook_throttle()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_throttle_store, &site);
    uint8_t *stub = n == 1 && site ? alloc_near(site) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] throttle: store has %d hits or no stub memory, not hooked", n);
        return;
    }
    uintptr_t at = site + throttle_store_at;
    emitter e{stub};
    e.bytes({0x50, 0x48, 0xB8});                       // push rax; mov rax, &throttle_hook
    e.u64(reinterpret_cast<uintptr_t>(&throttle_hook));
    e.bytes({0x4C, 0x3B, 0x20, 0x75, 0x05});           // cmp r12, [rax]; jne +5
    e.bytes({0xC5, 0xFA, 0x5D, 0x40, 0x08});           // vminss xmm0, xmm0, [rax+8]
    e.bytes({0x58});                                   // pop rax
    e.bytes({0xC4, 0xC1, 0x7A, 0x11, 0x84, 0x24, 0x88, 0x01, 0x00, 0x00}); // the game's vmovss [r12+0x188], xmm0
    e.jmp(at + 10);
    throttle_hooked = jump_to_stub(at, 10, stub, e.p);
    swofty::log("[td-probe] throttle: %s store at %p", throttle_hooked ? "hooked" : "could not hook", reinterpret_cast<void *>(at));
}

// Engine sound off: a failed ICE already has 0 rpm, but the car's engine audio keeps idling. The audio update
// (0x1409fac38) reads the vehicle's pit state (r14 = vehicle index) and, on a change to 8 (BeingPushedIntoGarage),
// plays play_engine_stop and silences the engine loop, as when a car is pushed into its garage. The stub makes
// the audio see state 8 for engine_off_vehicle; gameplay keeps the real pit state.
constexpr const char *sig_engine_audio_pit_state =
    "41 80 FE 1A 77 29 49 8B 8D 08 0E 00 00 4C 89 F2 48 C1 E2 04 48 8B 4C 11 68 8B 49 04 83 F9 10 77 0E BA FF E3 01 "
    "00 0F A3 CA";
constexpr uintptr_t engine_audio_read_at = 0x14;

void emit_engine_audio_shutdown(emitter &e)
{
    e.bytes({0x50, 0x48, 0xB8});                       // push rax; mov rax, &engine_off_vehicle
    e.u64(reinterpret_cast<uintptr_t>(&engine_off_vehicle));
    e.bytes({0x44, 0x3B, 0x30, 0x75, 0x12});           // cmp r14d, [rax]; jne past override (18 bytes)
    e.bytes({0xB9, 0x08, 0x00, 0x00, 0x00});           // mov ecx, 8
    // Engine activation (0x154a053e0) records active state in audio+0x68 bit 3.
    // If pit-lane logic reactivates it while our cached pit state is already 8,
    // the normal change-only stop path would not run again. Re-arm that path
    // only when active; its normal stop clears bit 3, avoiding per-frame events.
    e.bytes({0xF6, 0x46, 0x68, 0x08, 0x74, 0x07});     // test byte [rsi+0x68], 8; jz past or
    e.bytes({0x80, 0x8E, 0xFA, 0x0F, 0x00, 0x00, 0x20}); // or byte [rsi+0xffa], 0x20 (pit state changed)
    e.bytes({0x58});                                   // pop rax
}

void hook_engine_audio()
{
    uintptr_t site = 0;
    int n = swofty::api->scan(".xtls", sig_engine_audio_pit_state, &site);
    uint8_t *stub = n == 1 && site ? alloc_near(site) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] engine sound: audio site has %d hits or no stub memory, engine keeps idling", n);
        return;
    }
    uintptr_t at = site + engine_audio_read_at;
    emitter e{stub};
    e.bytes({0x48, 0x8B, 0x4C, 0x11, 0x68});           // the game's mov rcx, [rcx+rdx+0x68]
    e.bytes({0x8B, 0x49, 0x04});                       // the game's mov ecx, [rcx+4] (pit state)
    emit_engine_audio_shutdown(e);
    e.jmp(at + 8);
    bool ok = jump_to_stub(at, 8, stub, e.p);
    swofty::log("[td-probe] engine sound: %s audio pit state read at %p", ok ? "hooked" : "could not hook",
                reinterpret_cast<void *>(at));
}

// GameReplaySystem's entry predicate normally rejects terminal state 5 unless
// its terminal-replay permission is set. Keep all other predicate checks intact.
void emit_replay_terminal_gate(emitter &e, uintptr_t allowed, uintptr_t terminal_check)
{
    e.bytes({0x83,0xBC,0x08,0xB8,0,0,0,0x05}); // original cmp [rax+rcx+0xb8],5
    e.bytes({0x74,0x0E}); // terminal -> compare player; otherwise original pass
    e.jmp(allowed);
    e.bytes({0x50,0x48,0xB8}); // preserve rax; load the player override address
    e.u64(reinterpret_cast<uintptr_t>(&replay_terminal_vehicle));
    e.bytes({0x3A,0x18,0x58,0x75,0x0E}); // cmp bl,[rax]; pop rax; jne original terminal check
    e.jmp(allowed);
    e.jmp(terminal_check);
}

void hook_replay_terminal_gate()
{
    constexpr const char *sig =
        "83 BC 08 B8 00 00 00 05 75 0D 80 BE 60 01 00 00 00 0F 84 30 FF FF FF 48 8B 8E 50 01 00 00";
    uintptr_t at = 0;
    int hits = swofty::api->scan(".xtls", sig, &at);
    uint8_t *stub = hits == 1 && at ? alloc_near(at) : nullptr;
    if (!stub) {
        swofty::log("[td-probe] replay entry: terminal gate has %d hits or no stub memory; unchanged", hits);
        return;
    }
    emitter e{stub};
    emit_replay_terminal_gate(e, at + 23, at + 10);
    bool ok = jump_to_stub(at, 10, stub, e.p);
    swofty::log("[td-probe] replay entry: %s player-only terminal restriction at %p; other checks unchanged",
                ok ? "hooked" : "could not hook", reinterpret_cast<void *>(at));
}
