// Regression: crew diagnostics must never invoke predicates with an unknown ABI.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
unsigned predicate_calls, original_calls;
void *expected_state, *expected_logic;
void discard_log(const char *, ...) {}
bool predicate_with_context(uintptr_t, uintptr_t)
{
    ++predicate_calls;
    return true;
}
uint64_t original(void *state, void *logic)
{
    assert(state == expected_state && logic == expected_logic);
    ++original_calls;
    return 0x12345678;
}
template <class T> void put(std::vector<uint8_t> &bytes, size_t at, T value)
{
    assert(at + sizeof value <= bytes.size());
    memcpy(bytes.data() + at, &value, sizeof value);
}
}

int main()
{
    SwoftyApi api{};
    api.log = discard_log;
    swofty::api = &api;
    crew_diagnostics_active = true;
    reset_session();
    player_vehicle = 3;
    std::vector<uint8_t> logic(0xb0), car(0x128), rules(0x90), object(8), vtable(0x40), queue(0x18);
    put(logic, 0x70, reinterpret_cast<uintptr_t>(car.data()));
    put(car, 0x120, uint8_t(3));
    put(logic, 0x78, reinterpret_cast<uintptr_t>(rules.data()));
    put(rules, 0x88, reinterpret_cast<uintptr_t>(object.data()));
    put(object, 0, reinterpret_cast<uintptr_t>(vtable.data()));
    put(vtable, 0x30, reinterpret_cast<uintptr_t>(&predicate_with_context));
    put(vtable, 0x38, reinterpret_cast<uintptr_t>(&predicate_with_context));
    put(logic, 0xa0, reinterpret_cast<uintptr_t>(queue.data()));
    put(logic, 0xa8, reinterpret_cast<uintptr_t>(queue.data() + queue.size()));
    put(queue, 0x14, uint32_t(5)); // Instant garage return, as in the crash log.
    crew_states[0].name = "StateCarIsReadyForTrack";
    crew_states[0].orig = original;
    int state = 0;
    expected_state = &state;
    expected_logic = logic.data();
    assert(on_crew_state<0>(&state, logic.data()) == 0x12345678);
    assert(original_calls == 1 && predicate_calls == 0);
    // Repeated state updates and a missing check object still forward unchanged.
    assert(on_crew_state<0>(&state, logic.data()) == 0x12345678);
    put(rules, 0x88, uintptr_t(0));
    assert(on_crew_state<0>(&state, logic.data()) == 0x12345678);
    assert(original_calls == 3 && predicate_calls == 0);

    // A crew left over after race retirement must not send the restarted car
    // from its grid/OnTrack state into the pit lane (Nick's results-screen restart).
    std::vector<uint8_t> pit_system(0x100), pit_entry(8);
    put(pit_system, 0x68 + 3 * 16, reinterpret_cast<uintptr_t>(pit_entry.data()));
    put(pit_entry, 4, uint32_t(1));
    pit_lane_system = reinterpret_cast<uintptr_t>(pit_system.data());
    crew_diagnostics_active = false;
    crew_states[crew_ready_for_track].name = "StateCarIsReadyForTrack";
    reset_car_state();
    log_crew_state(crew_ready_for_track, logic.data());
    assert(pending_crew == -1 && sendoff_stage == 0);
    // Real race pushing still prepares its newly spawned crew exactly once.
    player_broken = true;
    pushing = true;
    crew_spawn_wanted = true;
    log_crew_state(crew_ready_for_track, logic.data());
    assert(pending_crew == 2 && sendoff_stage == 2);
    pending_crew = -1;
    log_crew_state(crew_ready_for_track, logic.data());
    assert(pending_crew == -1);
    // Session termination and an ordinary damaged car do not prepare a crew.
    sendoff_stage = 0;
    session_ended = true;
    log_crew_state(crew_ready_for_track, logic.data());
    assert(pending_crew == -1 && sendoff_stage == 0);
    session_ended = false;
    crew_spawn_wanted = false;
    log_crew_state(crew_ready_for_track, logic.data());
    assert(pending_crew == -1 && sendoff_stage == 0);
    puts("PASS: crew predicates untouched; restart leaves grid alone; active push prepares once");
}
