#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include <algorithm>

namespace {
void discard_log(const char *, ...) {}
template <class T> void put(std::vector<uint8_t> &data, size_t at, T value)
{ assert(at + sizeof value <= data.size()); memcpy(data.data() + at, &value, sizeof value); }
}
int main()
{
    SwoftyApi api{}; api.log = discard_log; swofty::api = &api;
    std::vector<uint8_t> sys(0xd00), rec(0x70), car(0x600), replacement(0x600), engine(0x2600), gearbox(0x700);
    uintptr_t base = reinterpret_cast<uintptr_t>(sys.data());
    put(sys, 0xc20, uint64_t(4));
    put(sys, 0x50 + 3 * 0x70 + 8, reinterpret_cast<uintptr_t>(rec.data()));
    put(rec, 0x58, reinterpret_cast<uintptr_t>(car.data()));
    put(car, 0x530, reinterpret_cast<uintptr_t>(engine.data()));
    put(car, 0x538, reinterpret_cast<uintptr_t>(gearbox.data()));
    assert(car_of(base, 3) == reinterpret_cast<uintptr_t>(car.data()));
    put(rec, 0x58, reinterpret_cast<uintptr_t>(replacement.data()));
    assert(car_of(base, 3) == reinterpret_cast<uintptr_t>(replacement.data()));
    assert(!car_of(base, -1) && !car_of(base, 4) && !car_of(1, 3));
    put(rec, 0x58, reinterpret_cast<uintptr_t>(car.data()));
    put(sys, 0x50 + 3 * 0x70 + 8, uintptr_t(1));
    assert(!car_of(base, 3));
    uint64_t unchanged = 42;
    assert(!guarded_read(1, unchanged) && unchanged == 42);
    assert(!guarded_copy(~uintptr_t(0) - 2, &unchanged, sizeof unchanged));
    // Each failed component retains its established power/audio behavior.
    for (int part = 0; part < 7; ++part) {
        reset_session(); player_vehicle = 3; player_broken = true;
        std::fill(engine.begin(), engine.end(), 0); std::fill(gearbox.begin(), gearbox.end(), 0);
        if (part < 6) put(engine, part_offsets[part], 1.0f); else put(gearbox, 0x674, 1.0f);
        power_loss(reinterpret_cast<uintptr_t>(car.data()));
        assert(std::fabs(throttle_hook.cap - (part == 0 || part == 6 ? 0.0f : 0.3f)) < 0.0001f);
        assert(engine_off_vehicle == (part == 0 ? 3 : -1));
    }
    // A replacement must still clear blown/seized, while a 999-per-mille
    // unswapped engine stays dead. Other flag bits must survive the clear.
    reset_session(); player_vehicle = 3; car_dead = true;
    std::fill(engine.begin(), engine.end(), 0); std::fill(gearbox.begin(), gearbox.end(), 0);
    put(engine, part_offsets[0], 0.999f); put(engine, 0x2530, uint8_t(0xa3));
    power_loss(reinterpret_cast<uintptr_t>(car.data()));
    assert(car_dead && garage_part_failed && engine[0x2530] == 0xa3);
    put(engine, part_offsets[0], 0.001f);
    power_loss(reinterpret_cast<uintptr_t>(car.data()));
    assert(!car_dead && !garage_part_failed && engine[0x2530] == 0xa0);
    reset_session(); player_vehicle = 3; car_dead = true; garage_part_failed = true; terminal_mask = 0x18;
    power_loss(reinterpret_cast<uintptr_t>(car.data()));
    assert(car_dead); // crash damage never unlocks via an engine swap
    puts("PASS: guarded live vehicle reads, all component caps, ICE audio and repair unlock/flags");
}
