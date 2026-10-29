#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
uint8_t system_data[0x48]{}, collection[0x98]{}, cars[24][0x40]{};
uintptr_t entries[24];
unsigned reads, forwarded;
void original(void *) { ++forwarded; assert(cars[23][0x31] == 0x28); }
void log_sink(const char *, ...) {}
int setting(const char *, uint32_t type, SwoftySettingValue *out)
{ out->type = type; out->toggle = 1; return 1; }
bool read_bytes(uintptr_t at, void *out, size_t bytes)
{
    ++reads;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), out, bytes, &copied) && copied == bytes;
}
void bounds(uintptr_t first, uintptr_t last)
{ memcpy(collection + 0x88, &first, 8); memcpy(collection + 0x90, &last, 8); }
}
int main()
{
    uintptr_t collection_address = reinterpret_cast<uintptr_t>(collection);
    memcpy(system_data + 0x40, &collection_address, 8);
    for (unsigned i = 0; i < 24; ++i) {
        entries[i] = reinterpret_cast<uintptr_t>(cars[i]); cars[i][0x38] = uint8_t(i);
    }
    cars[23][0x31] = 0x20;
    uintptr_t first = reinterpret_cast<uintptr_t>(entries);
    bounds(first, first + sizeof entries);
    size_t hint = 0;
    uint8_t flags = 0;
    uintptr_t base = reinterpret_cast<uintptr_t>(system_data);
    assert(find_stopped_player(base, 23, flags, hint, read_bytes) == entries[23]);
    assert(flags == 0x20 && hint == 23 && reads == 27);
    reads = 0;
    assert(find_stopped_player(base, 23, flags, hint, read_bytes) == entries[23] && reads == 4);
    std::swap(entries[0], entries[23]);
    assert(find_stopped_player(base, 23, flags, hint, read_bytes) == entries[0] && hint == 0);
    assert(find_stopped_player(base, 99, flags, hint, read_bytes) == 0);
    entries[0] = 1; // unreadable replacement must fail without dereferencing it
    assert(find_stopped_player(base, 23, flags, hint, read_bytes) == 0);
    entries[0] = reinterpret_cast<uintptr_t>(cars[23]);
    bounds(first + 8, first); assert(!find_stopped_player(base, 23, flags, hint, read_bytes));
    bounds(first, first + 7); assert(!find_stopped_player(base, 23, flags, hint, read_bytes));
    bounds(first, first + 0x800); assert(!find_stopped_player(base, 23, flags, hint, read_bytes));
    bounds(first, first); assert(!find_stopped_player(base, 23, flags, hint, read_bytes));
    bounds(first, first + sizeof entries);
    SwoftyApi api{}; api.version = 4; api.log = log_sink; api.setting = setting; swofty::api = &api;
    player_vehicle = 23; player_broken = true; orig_stopped_cars_update = original;
    on_stopped_cars_update(system_data);
    assert(forwarded == 1 && cars[23][0x31] == 0x20);
    cars[23][0x31] = 0x28;
    on_stopped_cars_update(system_data);
    assert(forwarded == 2 && cars[23][0x31] == 0x28);
    puts("stopped player lookup tests passed");
}
