// Standalone Windows test. No game hooks, sockets or patch installation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

// The game's damage rating: only the flashback check's own record (on its stack) is rated without the terminal level
// while keep_control handles the damage; the stored records, which send the terminal message, stay the game's.
namespace {
void discard_log(const char *, ...) {}
int watch(const char *, SwoftySettingFn, void *) { return 0; }
bool keep = true;
int setting(const char *id, uint32_t type, SwoftySettingValue *out)
{
    if (strcmp(id, "keep_control") || type != SWOFTY_SETTING_TOGGLE)
        return 0;
    out->type = type;
    out->toggle = keep;
    return 1;
}
int calls = 0;
// The game's rating, simplified: a wheel off and the front wing at 5, the suspension at 3.
uint64_t fake_rating(void *, void *record)
{
    auto *r = static_cast<uint8_t *>(record);
    for (int i = 0; i < 11; ++i) {
        int32_t level = i == 3 || i == 6 ? 5 : i == 4 ? 3 : 0;
        memcpy(r + 0x10 + 4 * i, &level, 4);
    }
    int32_t worst = 5;
    memcpy(r + 0x68, &worst, 4);
    ++calls;
    return 5;
}
int32_t at(const uint8_t *r, size_t off)
{
    int32_t v;
    memcpy(&v, r + off, 4);
    return v;
}
}

int main()
{
    SwoftyApi api{}; api.version = 4; api.log = discard_log; api.setting = setting; api.watch_setting = watch;
    swofty::api = &api;
    orig_damage_rating = &fake_rating;
    std::vector<uint8_t> system(0x1000);
    uint64_t count = 22;
    memcpy(system.data() + 0xc20, &count, 8);
    system[0xf90] = 1;
    player_vehicle = 7;

    // The parts mask: levels at 5 only.
    {
        std::vector<uint8_t> r(0x70);
        fake_rating(nullptr, r.data());
        assert(rated_terminal_parts(r.data()) == 0x48);
        rate_without_terminal(r.data());
        assert(at(r.data(), 0x10 + 4 * 3) == 2 && at(r.data(), 0x10 + 4 * 6) == 2 && at(r.data(), 0x10 + 4 * 4) == 3);
        assert(at(r.data(), 0x68) == 3 && rated_terminal_parts(r.data()) == 0);
    }

    // The stored record of the player's car (updateVehicle): the game's rating stands, it feeds the limp watch.
    uint8_t *stored = system.data() + 0x50 + 0x70 * 7;
    calls = 0;
    on_damage_rating(system.data(), stored);
    assert(calls == 1 && at(stored, 0x68) == 5 && player_rating.load() == 5 && system[0xf90] == 1);
    // Another car's stored record: no change to the watch value.
    player_rating = -1;
    on_damage_rating(system.data(), system.data() + 0x50 + 0x70 * 3);
    assert(at(system.data() + 0x50 + 0x70 * 3, 0x68) == 5 && player_rating.load() == -1);

    // The flashback check's record on the stack: rated without the terminal level, so the jump-in passes.
    std::vector<uint8_t> check(0x70);
    calls = 0;
    on_damage_rating(system.data(), check.data());
    assert(calls == 1 && at(check.data(), 0x68) == 3 && rated_terminal_parts(check.data()) == 0);
    assert(system[0xf90] == 1);

    // keep_control off: the game's refusal stands.
    keep = false;
    on_damage_rating(system.data(), check.data());
    assert(at(check.data(), 0x68) == 5);
    keep = true;

    puts("damage_rating_test passed");
    return 0;
}
