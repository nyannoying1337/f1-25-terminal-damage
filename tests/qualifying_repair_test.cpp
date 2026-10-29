// Live repair-call regression: preserve stock work, extend only a new player ICE job in qualifying.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include <limits>

namespace {
unsigned calls;
bool enabled = true, insert_job = true;
float stock_seconds = 480;
uint32_t current_session = 5;
struct Job { uint32_t kind; float seconds; } jobs[4];
std::vector<uint8_t> record(0x58);
void discard_log(const char *, ...) {}
int watch(const char *, SwoftySettingFn, void *) { return 0; }
int setting(const char *id, uint32_t type, SwoftySettingValue *out)
{
    if (strcmp(id, "keep_control") || type != SWOFTY_SETTING_TOGGLE)
        return 0;
    out->type = type;
    out->toggle = enabled;
    return 1;
}
template<class T> void put(size_t off, T v) { memcpy(record.data() + off, &v, sizeof v); }
uint64_t original(void *ptr, uint32_t kind)
{
    assert(ptr == record.data());
    ++calls;
    uintptr_t item = 0;
    if (insert_job && repair_item(reinterpret_cast<uintptr_t>(ptr), kind, item) && !item) {
        uintptr_t last;
        memcpy(&last, record.data() + 0x28, sizeof last);
        *reinterpret_cast<Job *>(last) = {kind, stock_seconds};
        put(0x28, last + sizeof(Job));
    }
    return 0x123456789abcdef0ull;
}
void prepare(uint32_t session = 5, uint8_t vehicle = 3)
{
    memset(record.data(), 0, record.size());
    memset(jobs, 0, sizeof jobs);
    put(0x16, vehicle);
    put(0x20, reinterpret_cast<uintptr_t>(jobs));
    put(0x28, reinterpret_cast<uintptr_t>(jobs));
    player_vehicle = 3;
    session_ended = false;
    enabled = true;
    insert_job = true;
    stock_seconds = 480;
    current_session = session;
    repair_session_kind = reinterpret_cast<uintptr_t>(&current_session);
}
void invoke(uint32_t kind = 9)
{
    unsigned before = calls;
    assert(on_add_repair(record.data(), kind) == 0x123456789abcdef0ull);
    assert(calls == before + 1);
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
    orig_add_repair = original;
    for (uint32_t session = 5; session <= 14; ++session) {
        prepare(session);
        invoke();
        assert(jobs[0].kind == 9 && jobs[0].seconds == 3600);
        jobs[0].seconds = 3590;
        invoke(); // Deduplicated recalculation must not replenish the timer.
        assert(jobs[0].seconds == 3590);
    }
    for (uint32_t session : {0u, 1u, 2u, 3u, 4u, 15u, 16u, 17u, 18u, 19u}) {
        prepare(session); invoke(); assert(jobs[0].seconds == 480);
    }
    for (uint32_t kind : {2u, 8u, 10u, 15u}) {
        prepare(); invoke(kind); assert(jobs[0].seconds == 480);
    }
    prepare(5, 4); invoke(); assert(jobs[0].seconds == 480); // AI isolation.
    prepare(); player_vehicle = -1; invoke(); assert(jobs[0].seconds == 480);
    prepare(); session_ended = true; invoke(); assert(jobs[0].seconds == 480);
    prepare(); enabled = false; invoke(); assert(jobs[0].seconds == 480);
    prepare(); repair_session_kind = 0; invoke(); assert(jobs[0].seconds == 480);
    prepare(); insert_job = false; invoke(); assert(jobs[0].seconds == 0);
    for (float seconds : {0.0f, -1.0f, 3600.0f, 7200.0f, std::numeric_limits<float>::infinity()}) {
        prepare(); stock_seconds = seconds; invoke(); assert(jobs[0].seconds == seconds);
    }
    prepare(); stock_seconds = std::numeric_limits<float>::quiet_NaN(); invoke();
    assert(std::isnan(jobs[0].seconds));
    prepare(); put(0x28, reinterpret_cast<uintptr_t>(jobs) + 7); invoke();
    assert(jobs[0].seconds == 0); // Malformed vector forwarded, no mutation.
    puts("PASS: qualifying ICE time, countdown preservation, practice/race/AI isolation and forwarding");
}
