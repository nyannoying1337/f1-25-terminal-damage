// Optional timing of this addon's private-hook work. Original game calls are excluded.
#pragma once
namespace td_perf {
enum Slot { vehicle_before, vehicle_after, crew, stopped_before, stopped_after, repair, count };
inline std::atomic<bool> enabled{false};
struct Counter {
    std::atomic<uint64_t> samples{0}, ticks{0}, worst{0};
};
inline Counter counters[count];
inline constexpr const char *names[count] = {
    "vehicle-before", "vehicle-after", "crew", "stopped-before", "stopped-after", "repair-work"};

struct Scope {
    Slot slot;
    uint64_t start = 0;
    explicit Scope(Slot s) : slot(s)
    {
        if (enabled.load(std::memory_order_relaxed)) {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            start = uint64_t(t.QuadPart);
        }
    }
    void finish()
    {
        if (!start)
            return;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        uint64_t elapsed = uint64_t(t.QuadPart) - start;
        start = 0;
        auto &c = counters[slot];
        c.samples.fetch_add(1, std::memory_order_relaxed);
        c.ticks.fetch_add(elapsed, std::memory_order_relaxed);
        uint64_t worst = c.worst.load(std::memory_order_relaxed);
        while (elapsed > worst && !c.worst.compare_exchange_weak(worst, elapsed, std::memory_order_relaxed)) {}
    }
    ~Scope() { finish(); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};

inline void report()
{
    static uint64_t since = 0;
    if (!enabled.load(std::memory_order_relaxed)) {
        since = 0;
        return;
    }
    uint64_t now = GetTickCount64();
    if (!since) {
        for (auto &c : counters) {
            c.samples.exchange(0); c.ticks.exchange(0); c.worst.exchange(0);
        }
        since = now;
        return;
    }
    if (now - since < 10000)
        return;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    double ms_per_tick = 1000.0 / double(frequency.QuadPart);
    double seconds = double(now - since) / 1000.0;
    since = now;
    for (int i = 0; i < count; ++i) {
        auto &c = counters[i];
        uint64_t samples = c.samples.exchange(0), ticks = c.ticks.exchange(0), worst = c.worst.exchange(0);
        swofty::log("[td-probe] perf: %s samples=%llu total=%.3f ms rate=%.3f ms/s worst=%.3f ms (addon work only)",
                    names[i], (unsigned long long)samples, double(ticks) * ms_per_tick,
                    double(ticks) * ms_per_tick / seconds, double(worst) * ms_per_tick);
    }
}
}
