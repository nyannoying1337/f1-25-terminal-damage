# Player-state API, version 1

For spectator integrations requested in [issue #1](https://github.com/nyannoying1337/f1-25-terminal-damage/issues/1).

Add `nick.terminal-damage-probe` as a required manifest dependency. Include
`mod/native/include/terminal_damage_api.h`; resolve the exported C function
`TerminalDamageGetApi` from `terminal_damage_probe.dll`. No Swofty SDK change,
import library, game-memory reads or per-frame state polling are required.

```cpp
#include <windows.h>
#include <terminal_damage_api.h>

static const TdApi *td;
static uint64_t subscription;

static void changed(const TdPlayerState *s, void *) {
    if (s->player_out) {
        // Queue your spectator action. Preserve an existing selected driver.
    }
    // A running/limping/retiring/garage state has player_out == 0.
    // Undo your own automatic spectator mode on recovery, as appropriate.
}

bool connect_terminal_damage() {
    HMODULE dll = GetModuleHandleW(L"terminal_damage_probe.dll");
    auto get = dll ? reinterpret_cast<TerminalDamageGetApiFn>(
        GetProcAddress(dll, TERMINAL_DAMAGE_API_EXPORT)) : nullptr;
    td = get ? get(TERMINAL_DAMAGE_API_VERSION) : nullptr;
    if (!td || td->size < sizeof(TdApi)) return false;
    subscription = td->subscribe(changed, nullptr);
    return subscription != 0;
}

void disconnect_terminal_damage() {
    if (td && subscription) td->unsubscribe(subscription);
    subscription = 0;
    td = nullptr;
}
```

Call connect once after the provider has loaded; older releases have no export,
so handle a missing export gracefully. If addon initialization order makes it
unavailable initially, retry connection from your frame callback until connected,
then stop retrying. Do not repeatedly subscribe. Disconnect on the frame/callback
thread before unloading your addon or releasing callback user data.

## States

| Value | Meaning | player_out |
| --- | --- | --- |
| `TD_UNAVAILABLE` | No known active player/session | 0 |
| `TD_RUNNING` | No active TD damage or retirement intent | 0 |
| `TD_LIMPING` | Active damage while retaining control; includes zero-drive failures | 0 |
| `TD_RETIRING` | Retirement requested or garage-push work pending/in progress | 0 |
| `TD_IN_GARAGE` | Player pit state is InGarage, without confirmed retirement | 0 |
| `TD_RETIRED` | Player controller received the RETIRED flag | 1 |

`vehicle_id` is the game's internal vehicle index (normally 0..31), **not** the
driver's racing number or necessarily a UDP packet-array slot. It is -1 when
unknown. Confirmed retirement retains the index through DNF/session cleanup until
an actual reset. `damage_mask` is TD's current terminal-damage mask; cleanup can
clear it even while `TD_RETIRED` remains true. Do not infer player_out from mask.

## Delivery and recovery

- `get_state(out, sizeof(*out))` copies the latest coherent frame snapshot. It
  returns 0 for null/undersized output, otherwise 1. It is thread-safe and does
  not dereference game memory.
- `subscribe` returns a nonzero token (0 on failure, maximum 32 subscribers).
  It queues an initial callback for the next TD frame, even if no state changed.
- Subsequent callbacks run only when state, vehicle or damage mask changes.
  Multiple intermediate changes within a frame may be coalesced. Publication
  happens at the end of TD's frame callback, including early-return paths.
- Callback pointers refer to temporary snapshots. Copy the struct if needed
  later. Callbacks must return quickly and must not throw C++ exceptions across
  the API. Prefer queuing camera changes for your own frame handler.
- Callbacks run outside service locks; reading state or unsubscribing from a
  callback is supported. Concurrent unsubscribe prevents future dispatch, but a
  callback already in progress on another thread may still finish.
- Rewinding before the damage/request or restarting restores `TD_RUNNING` once
  TD's existing recovery path has completed. Rewinding after retained damage
  continues to report damage; it must not falsely report running. A component
  replacement likewise follows TD's existing recovery behavior.
- Merely requesting Retire is **not** player_out: the driver can still reach
  their stopping place/garage. Practice garage return is not race retirement.

The API is observational: it never activates spectator mode, changes camera
selection, forces retirement or repairs components. Older ABI versions are not
silently substituted; unsupported requested versions return null.
