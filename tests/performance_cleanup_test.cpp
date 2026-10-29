// Healthy AI updates must not query the obsolete limp setting. Private timing
// must finish before forwarding original game callbacks and stay optional.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
unsigned settings_calls, choice_reads, logs, updates, crew_updates;
bool check_timing = false;
void discard_log(const char *, ...) { ++logs; }
int setting(const char *, uint32_t type, SwoftySettingValue *out)
{
    ++settings_calls;
    if (type == SWOFTY_SETTING_CHOICE)
        ++choice_reads;
    out->type = type;
    out->toggle = 0;
    return 1;
}
int watch(const char *, SwoftySettingFn, void *) { return 0; }
uint64_t update(void *system, uint8_t vehicle)
{
    assert(system == nullptr && vehicle == 4);
    ++updates;
    if (check_timing) {
        assert(td_perf::counters[td_perf::vehicle_before].samples == 1);
        assert(td_perf::counters[td_perf::vehicle_after].samples == 0);
    }
    return 42;
}
uint64_t crew_original(void *state, void *logic)
{
    assert(state == nullptr && logic == nullptr);
    ++crew_updates;
    if (check_timing)
        assert(td_perf::counters[td_perf::crew].samples == 1);
    return 77;
}
}

int main()
{
    SwoftyApi api{};
    api.version = 4;
    api.log = discard_log;
    api.setting = setting;
    api.watch_setting = watch;
    swofty::api = &api;
    reset_session();
    player_vehicle = 3;
    orig_update_vehicle = update;
    crew_states[0].orig = crew_original;
    settings_calls = logs = 0;
    for (unsigned i = 0; i < 5000; ++i)
        assert(on_update_vehicle(nullptr, 4) == 42);
    assert(updates == 5000 && settings_calls == 0 && logs == 0);
    assert(td_perf::counters[td_perf::vehicle_before].samples == 0);
    assert(td_perf::counters[td_perf::vehicle_after].samples == 0);
    assert(on_crew_state<0>(nullptr, nullptr) == 77);
    assert(crew_updates == 1 && crew_state_now == -1 && logs == 0 && settings_calls == 0);
    assert(td_perf::counters[td_perf::crew].samples == 0);
    // Timing-enabled callbacks see completed addon samples before forwarding;
    // the original game call does not sit inside either timing scope.
    check_timing = true;
    td_perf::enabled = true;
    assert(on_update_vehicle(nullptr, 4) == 42);
    assert(td_perf::counters[td_perf::vehicle_after].samples == 1);
    poll_hotkey();
    assert(choice_reads == 0); // Disabled hotkeys do not read key choices/poll keys.
    assert(on_crew_state<0>(nullptr, nullptr) == 77);
    td_perf::enabled = false;
    check_timing = false;
    assert(on_update_vehicle(nullptr, 4) == 42);
    assert(td_perf::counters[td_perf::vehicle_after].samples == 1);
    puts("PASS: 5000 AI updates without settings/logs; idle crew skipped; private timing excludes originals and disables cleanly");
}
