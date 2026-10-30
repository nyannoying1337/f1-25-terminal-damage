// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
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
    bool lock_known = false, pending = false, resumed = false, before_lock = false;

    struct DamagePoint { uint32_t frame, mask; };
    DamagePoint damage[128]{};
    unsigned damage_count = 0;
    bool damage_overflow = false;
    struct Rewind { uint32_t mask{}, target{}, overall{}; };
    void record_damage(uint32_t mask, uint64_t now) {
        Guard guard(mutex);
        if (!session || !stamp || now - stamp > 500) return;
        uint32_t previous = damage_count ? damage[damage_count-1].mask : 0;
        if ((previous | mask) == previous) return;
        if (damage_count == 128) { damage_overflow = true; return; }
        damage[damage_count++] = {frame, previous | mask};
        if (!lock_known) { lock_known = true; lock_frame = frame; }
    }
    bool damage_rewind(uint64_t now, Rewind &out) {
        Guard guard(mutex);
        if (!pending || !resumed || now > deadline || !damage_count || damage_overflow) return false;
        out = {0, target, event_overall};
        for (unsigned i=0; i<damage_count; ++i)
            if (damage[i].frame <= target) out.mask = damage[i].mask;
        return true;
    }
    bool commit_damage(const Rewind &proof) {
        Guard guard(mutex);
        if (!pending || !resumed || target != proof.target || event_overall != proof.overall) return false;
        while (damage_count && damage[damage_count-1].frame > target) --damage_count;
        pending = resumed = false;
        lock_known = damage_count != 0;
        return true;
    }
    void clear_lock() {
        Guard guard(mutex);
        lock_known = pending = resumed = false;
        damage_count = 0; damage_overflow = false;
    }
    void invalidate_session() {
        Guard guard(mutex);
        session=stamp=deadline=0;frame=lock_frame=target=event_frame=event_overall=0;
        lock_known=pending=resumed=before_lock=false;
        damage_count=0;damage_overflow=false;
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
            damage_count = 0; damage_overflow = false;
        }
        if (telemetry) {
            frame = f; stamp = now;
            if (pending && overall > event_overall && f >= target && f < event_frame)
                resumed = true;
        } else {
            uint32_t to;
            memcpy(&to, b + 33, 4);
            pending = resumed = false;
            if (lock_known && to < f) {
                before_lock = to < lock_frame;
                target = to; event_frame = f; event_overall = overall;
                deadline = now + 5000; pending = true; resumed = false;
            }
        }
    }
    bool consume_retained_resume(uint64_t now) {
        Guard guard(mutex);
        if (!pending || !resumed || before_lock || now > deadline) return false;
        pending = resumed = false;
        return true;
    }
    bool ready(uint64_t now) {
        Guard guard(mutex);
        if (pending && now > deadline)
            pending = resumed = false;
        return pending && resumed && before_lock;
    }
};
inline FlashbackTracker flashback_tracker;
inline FlashbackTracker damage_flashback_tracker;
