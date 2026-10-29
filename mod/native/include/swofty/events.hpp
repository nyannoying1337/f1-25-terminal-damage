// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <swofty/mixin.hpp>

// Game lifecycle events and a per-frame tick. The core fires lifecycle events from the
// flow-state machine and session hooks, and the frame tick once per rendered frame. Every
// callback runs on the game's flow thread, so handlers must return quickly and may be
// re-entered across frames. Subscribe at any time; a subscription lasts until unsubscribe.

namespace swofty::events {

enum class Result { ok, late, unavailable, invalid };

using Event = SwoftyEvent;

namespace detail {

inline const SwoftyEvents *table()
{
    return api && api->version >= 5 ? api->events : nullptr;
}

inline Result result(int r)
{
    return r == SWOFTY_ASSET_OK ? Result::ok
           : r == SWOFTY_ASSET_LATE ? Result::late
           : r == SWOFTY_ASSET_UNAVAILABLE ? Result::unavailable
                                           : Result::invalid;
}

}

// The events library takes registrations: this core implements it, the feature is on, and the
// core's setup has not found the frame tick missing on this build. Registration is accepted at any
// time from SwoftyAddonInit on: a subscribe or on_frame made before the core's events setup (in
// SwoftyAddonInit or a SWOFTY_ON_LOAD of the first load) returns ok, is queued, and fires once the
// setup completes. If the setup then finds no frame tick, the core log names each queued on_frame.
inline bool available()
{
    auto t = detail::table();
    return t && t->available();
}

// Subscribe to a named lifecycle event (SWOFTY_EVENT_*). The token identifies the
// subscription for unsubscribe; pass nullptr to ignore it.
inline Result subscribe(std::string_view event, SwoftyEventFn fn, void *user = nullptr, uint32_t *token = nullptr)
{
    auto t = detail::table();
    if (!t)
        return Result::unavailable;
    return detail::result(t->subscribe(std::string(event).c_str(), fn, user, token));
}

// Run a callback once per rendered frame with the seconds elapsed since the previous frame.
// unavailable once the core's setup found no frame tick on this build.
inline Result on_frame(SwoftyFrameFn fn, void *user = nullptr, uint32_t *token = nullptr)
{
    auto t = detail::table();
    if (!t)
        return Result::unavailable;
    return detail::result(t->on_frame(fn, user, token));
}

// Drop a subscription or frame callback by its token.
inline Result unsubscribe(uint32_t token)
{
    auto t = detail::table();
    if (!t)
        return Result::unavailable;
    return detail::result(t->unsubscribe(token));
}

inline const char *name(Result r)
{
    return r == Result::ok ? "ok" : r == Result::late ? "late" : r == Result::unavailable ? "unavailable" : "invalid";
}

}
