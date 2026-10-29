#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
unsigned calls, gates, logs;
uint8_t game_entry = 1;
void discard_log(const char *, ...) { ++logs; }
uint64_t original(void *self) {
    ++calls;
    if (self) static_cast<uint8_t *>(self)[1] = game_entry;
    return 0x12345678;
}
bool gate(void *) { ++gates; return false; }
}
int main()
{
    SwoftyApi api{}; api.log = discard_log; swofty::api = &api;
    uint8_t options[0xf0]{}, replay[8]{}, vt[0x138]{};
    uintptr_t value = reinterpret_cast<uintptr_t>(replay); memcpy(options + 0xe8, &value, 8);
    value = reinterpret_cast<uintptr_t>(vt); memcpy(replay, &value, 8);
    value = reinterpret_cast<uintptr_t>(&gate); memcpy(vt + 0x130, &value, 8);
    orig_pause_options = original;
    session_ended = false;
    assert(on_pause_options(options) == 0x12345678 && options[1] == 1 && calls == 1);
    player_broken = true; game_entry = 0;
    assert(on_pause_options(options) == 0x12345678 && options[1] == 0 && calls == 2);
    assert(!gates && logs == 2); // no guessed ABI call and no availability override
    assert(on_pause_options(options) == 0x12345678 && calls == 3 && logs == 2);
    session_ended = true;
    assert(on_pause_options(nullptr) == 0x12345678 && calls == 4 && !gates);
    puts("PASS: replay entry probe forwards original, preserves blocked entry, never invokes gate and handles invalid reads");
}
