// Standalone Windows test. No game hooks, sockets or patch installation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
void log_sink(const char *, ...) {}
// A counted contact of hit size v (normal speed v, confirmed by an impulse of v / 2).
void push(int vehicle, int corner, float v)
{
    susp::Pending &p = susp::ai::pending[vehicle][corner];
    p.dv = v / 2;
    p.vn = v;
    p.contacts = 1;
    p.solid = 1;
}
}  // namespace

int main()
{
    SwoftyApi api{};
    api.log = log_sink;
    swofty::api = &api;
    susp::enabled = true;
    susp::ai::enabled = true;
    player_vehicle = 0;

    // An AI car (vehicle 5) registers from its update: car and its suspension system car+0x580.
    alignas(16) uint8_t car[0x600]{}, sys[0x100]{};
    uintptr_t sys_ptr = reinterpret_cast<uintptr_t>(sys);
    memcpy(car + 0x580, &sys_ptr, 8);
    susp::ai_game_thread(5, reinterpret_cast<uintptr_t>(car));
    assert(susp::ai::car[5].load() == reinterpret_cast<uintptr_t>(car) && susp::ai::sys[5].load() == sys_ptr);
    assert(susp::ai::index_of_sys(sys_ptr) == 5 && susp::ai::index_of_car(reinterpret_cast<uintptr_t>(car)) == 5);

    // A brush is ignored, a clip bends or weakens, the toe reaches the geometry step.
    uint64_t now = 100000;
    push(5, 2, 1.2f);
    susp::ai_tick(now, false);
    susp::ai_tick(now + 300, false);
    assert(susp::ai::model[5].c[2].health == 1.0f);
    push(5, 2, 3.0f);
    susp::ai_tick(now + 2000, false);
    susp::ai_tick(now + 2300, false);
    assert(susp::ai::model[5].c[2].health < 1.0f);
    assert(susp::ai::toe[5][2].load() == (susp::ai::model[5].c[2].collapsed ? 0.0f : susp::ai::model[5].c[2].bend));

    // Hits until the corner collapses: a collapse request for the game thread and the 15 s fallback.
    uint64_t t = now + 5000;
    for (int k = 0; k < 20 && !susp::ai::model[5].c[2].collapsed; ++k, t += 2000) {
        push(5, 2, 16.0f);
        susp::ai_tick(t, false);
        susp::ai_tick(t + 300, false);
    }
    assert(susp::ai::model[5].c[2].collapsed);
    assert(susp::ai::collapse_req[5].load() & (1u << 2));
    assert(susp::ai::retire_at[5] != 0);
    assert(susp::ai::toe[5][2].load() == 0.0f);  // a collapsed corner is the game's broken suspension

    // The game reported it: no retirement request of the addon, the timer ends.
    susp::ai_terminal(5);
    susp::ai_tick(susp::ai::retire_at[5] + 1, false);
    assert(susp::ai::retire_at[5] == 0);

    // The player's own vehicle is never an AI car.
    susp::ai_game_thread(0, reinterpret_cast<uintptr_t>(car));
    susp::ai_forget(0);
    assert(susp::ai::car[0].load() == 0);

    // A new session forgets everything.
    susp::ai_reset();
    assert(susp::ai::model[5].c[2].health == 1.0f && !susp::ai::model[5].c[2].collapsed && !susp::ai::terminal_seen);

    std::puts("ai suspension ok");
}
