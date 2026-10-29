#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
bool enabled = true;
void log_sink(const char *, ...) {}
int watch(const char *, SwoftySettingFn, void *) { return 0; }
int setting(const char *name, uint32_t type, SwoftySettingValue *out) {
    out->type = type;
    if (!strcmp(name, "keep_control")) { out->toggle = enabled; return 1; }
    return 0;
}
}
int main()
{
    SwoftyApi api{}; api.version = 4; api.log = log_sink; api.setting = setting; api.watch_setting = watch; swofty::api = &api;
    auto *code = static_cast<uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(code);
    emitter e{code};
    e.bytes({0x53,0x48,0x89,0xC8,0x31,0xC9,0x89,0xD3}); // save rbx, rax=record, rcx=0, ebx=vehicle
    emit_replay_terminal_gate(e, reinterpret_cast<uintptr_t>(code + 512), reinterpret_cast<uintptr_t>(code + 544));
    emitter pass{code + 512}; pass.bytes({0xB8,1,0,0,0,0x5B,0xC3});
    emitter legacy{code + 544}; legacy.bytes({0xB8,2,0,0,0,0x5B,0xC3});
    DWORD old;
    assert(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old));
    FlushInstructionCache(GetCurrentProcess(), code, 560);
    auto run = reinterpret_cast<int (*)(void *, uint32_t)>(code);
    uint8_t record[0xc0]{};
    auto state = [&](uint32_t value) { memcpy(record + 0xb8, &value, 4); };
    replay_terminal_vehicle = 3;
    state(5);
    assert(run(record, 3) == 1 && run(record, 4) == 2);
    replay_terminal_vehicle = -1;
    assert(run(record, 3) == 2);
    for (uint32_t value : {0u,1u,4u,6u}) {
        state(value); assert(run(record, 3) == 1 && run(record, 4) == 1);
        uint32_t after; memcpy(&after, record + 0xb8, 4); assert(after == value);
    }
    reset_session(); player_vehicle = 3; player_broken = true;
    power_loss(0); assert(replay_terminal_vehicle == 3);
    player_pit = 9; power_loss(0); assert(replay_terminal_vehicle == -1);
    player_pit = 1; enabled = false; power_loss(0); assert(replay_terminal_vehicle == -1);
    enabled = true; session_ended = true; power_loss(0); assert(replay_terminal_vehicle == -1);
    reset_car_state(); assert(replay_terminal_vehicle == -1);
    assert(VirtualFree(code, 0, MEM_RELEASE));
    puts("PASS: terminal replay branch is player-only, nonterminal/other-player legacy branches preserved; lifecycle guards clear override");
}
