// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#pragma once
#include <terminal_damage_api.h>
#include <array>
#include <atomic>
#include <mutex>

namespace td_state {
class Service {
    struct Listener {
        uint64_t token=0, seen=UINT64_MAX;
        TdStateChangedFn fn=nullptr;
        void *user=nullptr;
    };
    std::atomic<uint64_t> current{0};
    std::atomic<unsigned> subscribers{0};
    std::mutex mutex;
    std::array<Listener,32> listeners{};
    uint64_t next_token=1;
    bool dispatching=false; // dispatch is called only on the frame thread
    static TdPlayerState decode(uint64_t value) {
        auto state=uint32_t(value&255);
        return {sizeof(TdPlayerState),state,int32_t((value>>8)&255)-1,
                uint32_t(value>>16),uint32_t(state==TD_RETIRED),0};
    }
public:
    int get(TdPlayerState *out,uint32_t size) const {
        if (!out || size<sizeof(TdPlayerState)) return 0;
        *out=decode(current.load());
        return 1;
    }
    uint64_t subscribe(TdStateChangedFn fn,void *user) {
        if (!fn) return 0;
        std::lock_guard lock(mutex);
        for (auto &entry:listeners) if (!entry.token) {
            if (!next_token) return 0;
            entry={next_token++,UINT64_MAX,fn,user};
            subscribers.fetch_add(1);
            return entry.token;
        }
        return 0;
    }
    void unsubscribe(uint64_t token) {
        std::lock_guard lock(mutex);
        for (auto &entry:listeners) if (token && entry.token==token) {entry={};subscribers.fetch_sub(1);}
    }
    void publish(uint32_t state,int vehicle,uint32_t mask) {
        current.store(uint64_t(state) | uint64_t(vehicle+1)<<8 | uint64_t(mask)<<16);
    }
    void dispatch() {
        if (!subscribers.load() || dispatching) return;
        dispatching=true;
        std::array<uint64_t,32> tokens{};
        {
            std::lock_guard lock(mutex);
            for (size_t i=0;i<listeners.size();++i) tokens[i]=listeners[i].token;
        }
        const uint64_t value=current.load();
        const auto snapshot=decode(value);
        for (uint64_t token:tokens) {
            if (!token) continue;
            TdStateChangedFn fn=nullptr;void *user=nullptr;
            {
                std::lock_guard lock(mutex);
                for (auto &entry:listeners) if (entry.token==token && entry.seen!=value) {
                    entry.seen=value;fn=entry.fn;user=entry.user;break;
                }
            }
            if (fn) fn(&snapshot,user); // no locks across consumer code
        }
        dispatching=false;
    }
};
inline Service service;
inline int get(TdPlayerState *out,uint32_t size) {return service.get(out,size);}
inline uint64_t subscribe(TdStateChangedFn fn,void *user) {return service.subscribe(fn,user);}
inline void unsubscribe(uint64_t token) {service.unsubscribe(token);}
inline const TdApi api{TERMINAL_DAMAGE_API_VERSION,sizeof(TdApi),get,subscribe,unsubscribe};
}
