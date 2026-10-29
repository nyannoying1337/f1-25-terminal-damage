#pragma once

// UDP supplies timeline evidence only. Gameplay state is reset in updateVehicle.
struct FlashbackTracker {
    SRWLOCK mutex = SRWLOCK_INIT;
    struct Guard {
        SRWLOCK *p;
        explicit Guard(SRWLOCK &m) : p(&m) { AcquireSRWLockExclusive(p); }
        ~Guard() { ReleaseSRWLockExclusive(p); }
    };
    uint64_t session = 0, stamp = 0, deadline = 0;
    uint32_t frame = 0, lock_frame = 0, target = 0, event_frame = 0, event_overall = 0;
    bool lock_known = false, pending = false, resumed = false;

    void clear_lock() {
        Guard guard(mutex);
        lock_known = pending = resumed = false;
    }
    void mark(uint64_t now) {
        Guard guard(mutex);
        if (!lock_known && session && stamp && now - stamp <= 500) {
            lock_frame = frame;
            lock_known = true;
        }
    }
    void packet(const uint8_t *b, size_t n, uint64_t now) {
        if (n < 29 || b[0] != 0xea || b[1] != 7 || b[27] >= 24)
            return; // format 2026
        bool telemetry = b[6] == 6 && n == 29 + 24 * 59 + 3;
        bool flashback = b[6] == 3 && n == 45 && !memcmp(b + 29, "FLBK", 4);
        if (!telemetry && !flashback)
            return;
        uint64_t uid;
        uint32_t f, overall;
        memcpy(&uid, b + 7, 8); memcpy(&f, b + 19, 4); memcpy(&overall, b + 23, 4);
        if (!uid)
            return;
        Guard guard(mutex);
        if (session != uid) {
            session = uid; lock_known = pending = resumed = false;
        }
        if (telemetry) {
            frame = f; stamp = now;
            if (pending && overall > event_overall && f >= target && f < event_frame)
                resumed = true;
        } else {
            uint32_t to;
            memcpy(&to, b + 33, 4);
            pending = resumed = false;
            if (lock_known && to < lock_frame && to < f) {
                target = to; event_frame = f; event_overall = overall;
                deadline = now + 5000; pending = true; resumed = false;
            }
        }
    }
    bool ready(uint64_t now) {
        Guard guard(mutex);
        if (pending && now > deadline)
            pending = resumed = false;
        return pending && resumed;
    }
};
inline FlashbackTracker flashback_tracker;
