// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
// Optional timing of this addon's private-hook work. Original game calls are excluded.
#pragma once
namespace td_perf {
enum Slot { vehicle_before, vehicle_after, crew, stopped_before, stopped_after, repair,
            wheel_before, wheel_original, wheel_player_after, wheel_ai_after,
            frame_total, frame_suspension, frame_failures, frame_power, frame_state,
            frame_menu, frame_input, frame_test, frame_cars, count };
// The frame thread's work (on_frame) is always timed, two QueryPerformanceCounter calls a part, and logged once a
// minute in a session (report_frame): Swofty's perf.log showed 43-69 us a frame for the whole callback (0.51.115).
inline constexpr int first_frame_slot = frame_total;
inline std::atomic<bool> enabled{false};
// Times the wheel slots (wheel_before onwards) without the general diagnostics. Nothing in the addon sets it; only
// tests/wheel_tether_test.cpp does. Do not include original-game time in addon work totals.
inline std::atomic<bool> wheel_capture{false};
struct Counter {
    std::atomic<uint64_t> samples{0}, ticks{0}, worst{0};
};
inline Counter counters[count];
inline constexpr const char *names[count] = {
    "vehicle-before", "vehicle-after", "crew", "stopped-before", "stopped-after", "repair-work",
    "wheel-before", "wheel-original", "wheel-player-after", "wheel-ai-after",
    "frame-total", "frame-suspension", "frame-failures", "frame-power", "frame-state",
    "frame-menu", "frame-input", "frame-test", "frame-cars"};

struct Scope {
    Slot slot;
    uint64_t start = 0;
    explicit Scope(Slot s) : slot(s)
    {
        if (s >= first_frame_slot || enabled.load(std::memory_order_relaxed) ||
            (s>=wheel_before && wheel_capture.load(std::memory_order_relaxed))) {
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
    if (!enabled.load(std::memory_order_relaxed) && !wheel_capture.load(std::memory_order_relaxed)) {
        since = 0;
        return;
    }
    uint64_t now = GetTickCount64();
    if (!since) {
        for (int i = 0; i < first_frame_slot; ++i) {
            auto &c = counters[i];
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
    for (int i = 0; i < first_frame_slot; ++i) {
        if (!enabled.load(std::memory_order_relaxed) && i<wheel_before) continue;
        auto &c = counters[i];
        uint64_t samples = c.samples.exchange(0), ticks = c.ticks.exchange(0), worst = c.worst.exchange(0);
        swofty::log("[td-probe] perf: %s samples=%llu total=%.3f ms rate=%.3f ms/s worst=%.3f ms (%s)",
                    names[i], (unsigned long long)samples, double(ticks) * ms_per_tick,
                    double(ticks) * ms_per_tick / seconds, double(worst) * ms_per_tick,
                    i==wheel_original ? "original game call, not addon work" : "addon work only");
    }
}

// Once a minute while a session runs: the frame work per frame, in parts.
inline void report_frame(bool in_session)
{
    static uint64_t since = 0;
    uint64_t now = GetTickCount64();
    if (!in_session) {
        since = 0;
        return;
    }
    if (!since || now - since >= 60000) {
        uint64_t n = counters[frame_total].samples.exchange(0);
        double ticks[count - first_frame_slot], worst = double(counters[frame_total].worst.exchange(0));
        for (int i = first_frame_slot; i < count; ++i) {
            ticks[i - first_frame_slot] = double(counters[i].ticks.exchange(0));
            if (i != frame_total) {
                counters[i].samples.exchange(0);
                counters[i].worst.exchange(0);
            }
        }
        double seconds = since ? double(now - since) / 1000.0 : 0;
        since = now;
        if (!n || seconds <= 0)
            return;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        double us = 1e6 / double(frequency.QuadPart), per = us / double(n);
        swofty::log("[td-probe] perf: frame work %.1f us a frame (%.2f ms/s, worst %.0f us, %llu frames); suspension "
                    "%.1f, failures %.1f, power %.1f, player state %.1f, menus %.1f, keys %.1f, test run %.1f, cars "
                    "%.1f us a frame",
                    ticks[0] * per, ticks[0] * us / 1000.0 / seconds, worst * us, (unsigned long long)n, ticks[1] * per,
                    ticks[2] * per, ticks[3] * per, ticks[4] * per, ticks[5] * per, ticks[6] * per, ticks[7] * per,
                    ticks[8] * per);
    }
}
}
