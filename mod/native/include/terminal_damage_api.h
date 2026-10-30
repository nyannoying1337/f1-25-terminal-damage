// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERMINAL_DAMAGE_API_VERSION 1u
#define TERMINAL_DAMAGE_API_EXPORT "TerminalDamageGetApi"
enum TdPlayerStatus {
    TD_UNAVAILABLE=0, TD_RUNNING=1, TD_LIMPING=2, TD_RETIRING=3,
    TD_IN_GARAGE=4, TD_RETIRED=5
};
typedef struct TdPlayerState {
    uint32_t size;
    uint32_t state;
    int32_t vehicle_id; // Internal vehicle index, NOT the driver's racing number; -1 if unknown.
    uint32_t damage_mask;
    uint32_t player_out; // 1 only after confirmed retirement, not on a Retire request.
    uint32_t reserved;
} TdPlayerState;
typedef void (*TdStateChangedFn)(const TdPlayerState *state, void *user);
typedef struct TdApi {
    uint32_t version;
    uint32_t size;
    int (*get_state)(TdPlayerState *out, uint32_t out_size);
    uint64_t (*subscribe)(TdStateChangedFn callback, void *user);
    void (*unsubscribe)(uint64_t token);
} TdApi;
typedef const TdApi *(*TerminalDamageGetApiFn)(uint32_t requested_version);

// Windows: GetModuleHandleW(L"terminal_damage_probe.dll"), then GetProcAddress
// with TERMINAL_DAMAGE_API_EXPORT. No import library or game pointers required.
// Callbacks run on TD's on_frame thread, at most once per observed change/frame.
// subscribe queues an initial snapshot for the next frame. Callback data is valid
// only during that call. Unsubscribe before unloading the consumer; do so on the
// callback thread. A callback already running on another thread may still finish.
#ifdef __cplusplus
}
#endif
