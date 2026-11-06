// Issue #10: AI cars with terminal damage that the game never removes (mod/native/src/ai_cleanup.hpp and ai_watch_step in
// probe/11_power_loss.inc). The clock is stepped by hand, nothing sleeps.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include "test_util.hpp"

namespace {
using tu::put;

bool cleanup_on = true;
int watch(const char *, SwoftySettingFn, void *) { return 0; }
int setting(const char *id, uint32_t type, SwoftySettingValue *out)
{
    if (strcmp(id, "ai_terminal_cleanup") || type != SWOFTY_SETTING_TOGGLE)
        return 0;
    out->type = type;
    out->toggle = cleanup_on;
    return 1;
}

int calls = 0;
uint64_t last_vehicle = 99, last_status = 99;
void *last_system = nullptr;
uint64_t fake_set_status(void *system, uint64_t vehicle, uint64_t status)
{
    ++calls;
    last_system = system;
    last_vehicle = vehicle;
    last_status = status;
    return 0;
}

constexpr uint8_t ai = 7, player = 3;
constexpr uint64_t T0 = 1'000'000;  // the tick of the terminal message (ms)

// A fake VehicleTerminalDamageSystem with one AI car: record state at +0x68, the car's physics chain for the speed.
struct World {
    std::vector<uint8_t> sys = std::vector<uint8_t>(0xd00), rec = std::vector<uint8_t>(0x70), car = std::vector<uint8_t>(0x600),
                         physics = std::vector<uint8_t>(0x400), rigid = std::vector<uint8_t>(0x100),
                         body = std::vector<uint8_t>(0x300), lane = std::vector<uint8_t>(0x400);
    World()
    {
        put(sys, 0xc20, uint64_t(22));
        put(sys, 0x50 + ai * 0x70 + 8, tu::addr(rec.data()));
        put(rec, 0x58, tu::addr(car.data()));
        put(car, 0x558, tu::addr(physics.data()));
        put(physics, 0x340, tu::addr(rigid.data()));
        put(rigid, 0x18, tu::addr(body.data()));
        set_record(aic::record_terminal);
        set_speed_ms(0.0f);
    }
    uintptr_t at() { return tu::addr(sys.data()); }
    void set_record(int32_t r) { put(sys, 0x50 + ai * 0x70 + 0x68, r); }
    void set_speed_ms(float ms)
    {
        float v[4] = {ms, 0, 0, 0};
        memcpy(body.data() + 0x220, v, sizeof v);
    }
    void step(uint64_t after_ms) { ai_watch_step(at(), ai, T0 + after_ms); }
};

// A fresh race with the AI car freshly watched.
void begin(World &w, int pit = -1)
{
    reset_session();
    session_ended = false;
    player_vehicle = player;
    player_controller_flags = 0;
    cleanup_on = true;
    calls = 0;
    last_vehicle = last_status = 99;
    w.lane[0x300] = 0;  // not practice or qualifying
    pit_lane_system = tu::addr(w.lane.data());
    orig_set_vehicle_status = fake_set_status;
    w.set_record(aic::record_terminal);
    w.set_speed_ms(0.0f);
    observed_pit_state[ai] = unsigned(pit + 1);
    ai_watch_since[ai] = T0;
    ai_watch_still_since[ai] = 0;
    ai_watch_asked = 0;
    ai_watch_mask = 1u << ai;
}
bool watching() { return (ai_watch_mask.load() & (1u << ai)) != 0; }
bool asked() { return (ai_watch_asked.load() & (1u << ai)) != 0; }
}  // namespace

int main()
{
    SwoftyApi api{};
    api.version = 4;
    api.log = tu::quiet_log;
    api.setting = setting;
    api.watch_setting = watch;
    swofty::api = &api;

    // ---- the decision alone ----
    {
        aic::Input in;
        in.racing = true;
        in.record = aic::record_terminal;
        in.since_terminal = 119.9;
        in.standing = 100;
        assert(aic::decide(in) == aic::Action::keep_watching);  // the game still has its two minutes
        in.since_terminal = 120.0;
        in.standing = 29.9;
        assert(aic::decide(in) == aic::Action::keep_watching);  // not stood long enough
        in.standing = 30.0;
        assert(aic::decide(in) == aic::Action::send_to_garage);
        in.pit = 1;
        assert(aic::decide(in) == aic::Action::send_to_garage);  // OnTrack is on track
        // The pit lane: much sooner.
        in.pit = 14;
        in.since_terminal = 29.9;
        in.standing = 20;
        assert(aic::decide(in) == aic::Action::keep_watching);
        in.since_terminal = 30.0;
        in.standing = 11.9;
        assert(aic::decide(in) == aic::Action::keep_watching);
        in.standing = 12.0;
        for (int pit : {2, 7, 13, 14, 15})
            assert((in.pit = pit, aic::decide(in) == aic::Action::send_to_garage));
        // The crew's states and the garage are never touched.
        in.since_terminal = 300;
        in.standing = 300;
        for (int pit : {3, 4, 5, 6, 8})
            assert((in.pit = pit, aic::decide(in) == aic::Action::keep_watching));
        for (int pit : {0, 9})
            assert((in.pit = pit, aic::decide(in) == aic::Action::in_garage));
        // A whole car (a flashback) ends the watch; a record that cannot be read does not.
        in.pit = -1;
        in.record = 1;
        assert(aic::decide(in) == aic::Action::healed);
        in.record = -1;
        assert(aic::decide(in) == aic::Action::send_to_garage);
        in.record = 1;
        in.since_terminal = 2.0;
        in.standing = 0;
        assert(aic::decide(in) == aic::Action::keep_watching);  // the message just came: give the record a moment
        // Option off, not racing, asked already: watch only; and give up after 400 s.
        in = aic::Input{};
        in.record = aic::record_terminal;
        in.since_terminal = 200;
        in.standing = 200;
        in.racing = false;
        assert(aic::decide(in) == aic::Action::keep_watching);
        in.racing = true;
        in.enabled = false;
        assert(aic::decide(in) == aic::Action::keep_watching);
        in.enabled = true;
        in.already_asked = true;
        assert(aic::decide(in) == aic::Action::keep_watching);
        in.since_terminal = 400.1;
        assert(aic::decide(in) == aic::Action::give_up);
    }

    World w;
    // ---- an abandoned wreck on the track: the game gets two minutes, then the addon asks once ----
    begin(w);
    w.step(10'000);  // the car stands from here
    assert(calls == 0 && watching());
    w.step(60'000);
    w.step(119'000);
    assert(calls == 0 && !asked());
    w.step(120'000);
    assert(calls == 1 && last_vehicle == ai && last_status == 9 && last_system == w.lane.data());
    assert(asked() && watching());
    w.step(121'000);
    w.step(130'000);
    assert(calls == 1);  // once per car
    w.step(135'000);     // 15 s after the request and still not in the garage: left to the game
    assert(calls == 1 && !watching() && !asked());

    // ---- the request works: the pit state hook reports InGarage, the watch ends ----
    begin(w);
    w.step(10'000);
    w.step(120'000);
    assert(calls == 1);
    observed_pit_state[ai] = 9 + 1;
    w.step(121'000);
    assert(calls == 1 && !watching() && !asked());

    // ---- a moving car is left alone; the standing time starts when it stops ----
    begin(w);
    w.set_speed_ms(10.0f);  // 36 km/h
    w.step(10'000);
    w.step(125'000);
    w.step(130'000);
    assert(calls == 0 && ai_watch_still_since[ai].load() == 0);
    w.set_speed_ms(0.0f);
    w.step(140'000);  // stands from here
    w.step(169'000);
    assert(calls == 0);
    w.step(170'000);  // 30 s later, and past 120 s since the message
    assert(calls == 1);

    // ---- a flashback made the car whole again: no removal ----
    begin(w);
    w.step(10'000);
    w.set_record(1);
    w.step(20'000);
    assert(calls == 0 && !watching());
    begin(w);
    w.step(10'000);
    w.set_record(1);
    w.step(130'000);
    assert(calls == 0 && !watching());

    // ---- not while the session flow owns the cars ----
    begin(w);
    w.step(10'000);
    player_controller_flags = 1u << 11;  // safety car (also a red flag)
    w.step(130'000);
    assert(calls == 0 && watching());
    player_controller_flags = 0;
    w.step(140'000);  // the stand time kept running
    assert(calls == 1);
    for (int bit : {3, 10, 13, 16}) {
        begin(w);
        w.step(10'000);
        player_controller_flags = 1u << bit;
        w.step(150'000);
        assert(calls == 0);
    }
    begin(w);
    w.step(10'000);
    session_ended = true;
    w.step(150'000);
    assert(calls == 0);
    begin(w);
    w.step(10'000);
    red_flag_screen_open = true;
    w.step(150'000);
    assert(calls == 0);
    red_flag_screen_open = false;
    begin(w);
    w.step(10'000);
    w.lane[0x300] = 1;  // practice or qualifying
    w.step(150'000);
    assert(calls == 0);

    // ---- the option off ----
    begin(w);
    w.step(10'000);
    cleanup_on = false;
    w.step(150'000);
    assert(calls == 0 && watching());
    w.step(401'000);  // watched long enough
    assert(calls == 0 && !watching());

    // ---- in the pit lane the wreck goes after 30 s; in the crew's states never ----
    begin(w, 14);  // LeavingPitLane
    w.step(10'000);
    w.step(29'000);
    assert(calls == 0);
    w.step(30'000);
    assert(calls == 1 && last_status == 9);
    for (int pit : {3, 4, 5, 6, 8}) {
        begin(w, pit);
        w.step(10'000);
        w.step(300'000);
        assert(calls == 0);
    }
    begin(w, 9);  // already in its garage
    w.step(10'000);
    assert(calls == 0 && !watching());

    // ---- a speed that cannot be read never counts as standing ----
    begin(w);
    put(w.car, 0x558, uintptr_t(0));
    w.step(10'000);
    w.step(300'000);
    assert(calls == 0 && ai_watch_still_since[ai].load() == 0);
    put(w.car, 0x558, tu::addr(w.physics.data()));

    // ---- never the player's car, never without the pit lane system ----
    begin(w);
    assert(!send_ai_car_to_garage(player) && calls == 0);
    assert(!send_ai_car_to_garage(32) && calls == 0);
    pit_lane_system = 0;
    assert(!send_ai_car_to_garage(ai) && calls == 0);
    w.step(10'000);
    w.step(125'000);  // the request cannot be made: logged, asked once, nothing called
    assert(calls == 0 && asked());
    w.step(126'000);
    assert(calls == 0);

    // ---- a new session forgets the watch ----
    begin(w);
    ai_watch_asked = 1u << ai;
    reset_session();
    assert(ai_watch_mask.load() == 0 && ai_watch_asked.load() == 0);

    puts("PASS: AI wrecks the game does not remove are sent to their garage once, only when racing, standing and still terminal");
}
