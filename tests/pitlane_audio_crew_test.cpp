// Execute the emitted audio override against a synthetic live audio object,
// and verify missing crew cannot schedule fabricated push/garage reports.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
void discard_log(const char *, ...) {}
unsigned spawns;
void spawn(void *, uint32_t) { ++spawns; }
template<class T> void put(std::vector<uint8_t> &b, size_t off, T v)
{
    assert(off + sizeof v <= b.size());
    memcpy(b.data() + off, &v, sizeof v);
}
}

int main()
{
    SwoftyApi api{};
    api.log = discard_log;
    swofty::api = &api;
    auto *code = static_cast<uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(code);
    emitter e{code};
    e.bytes({0x56, 0x41, 0x56, 0x48, 0x89, 0xCE, 0x41, 0x89, 0xD6, 0x44, 0x89, 0xC1});
    emit_engine_audio_shutdown(e);
    e.bytes({0x89, 0xC8, 0x41, 0x5E, 0x5E, 0xC3});
    DWORD old;
    assert(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old));
    FlushInstructionCache(GetCurrentProcess(), code, size_t(e.p - code));
    using run_fn = uint32_t (*)(void *, uint32_t, uint32_t);
    auto run = reinterpret_cast<run_fn>(code);
    std::vector<uint8_t> audio(0x1000);
    engine_off_vehicle = 3;
    audio[0x68] = 8;
    audio[0xffa] = 2;
    assert(run(audio.data(), 3, 2) == 8 && audio[0xffa] == 0x22);
    // Normal stop clears active bit. Do not retrigger events every frame.
    audio[0x68] = 0;
    audio[0xffa] = 2;
    assert(run(audio.data(), 3, 2) == 8 && audio[0xffa] == 2);
    audio[0x68] = 8;
    assert(run(audio.data(), 4, 2) == 2 && audio[0xffa] == 2);
    engine_off_vehicle = -1;
    assert(run(audio.data(), 3, 10) == 10 && audio[0xffa] == 2);
    assert(VirtualFree(code, 0, MEM_RELEASE));

    reset_session();
    player_vehicle = 3;
    std::vector<uint8_t> module_bytes(0x180);
    void *module = module_bytes.data();
    crew_module = &module;
    spawn_crew = spawn;
    garage_of[3] = -1;
    assert(!spawn_player_crew() && spawns == 0);
    // An existing real instance is usable without needing a learned map.
    put(module_bytes, 0x68 + 3 * 8, uintptr_t(1));
    assert(spawn_player_crew() && spawns == 0);
    put(module_bytes, 0x68 + 3 * 8, uintptr_t(0));
    garage_of[3] = 7;
    assert(!spawn_player_crew() && spawns == 0); // Missing asset-table entry.
    pushing = true;
    crew_spawn_wanted = true;
    crew_seq_t = 0;
    crew_msg_in = 1.5;
    crew_msg = crew_msg_pushing;
    crew_request_in = .5;
    crew_request = crew_push;
    pending_crew = 2;
    abandon_unavailable_push();
    assert(!pushing && !pushed && !crew_spawn_wanted && crew_seq_t == -1);
    assert(crew_msg_in == -1 && crew_request_in == -1 && !crew_msg && !crew_request && pending_crew == -1);
    assert(retire_in == 2 && !garage_return_wanted);
    puts("PASS: emitted audio re-arms stop only for reactivated failed player; missing crew cancels fake push/teleport");
}
