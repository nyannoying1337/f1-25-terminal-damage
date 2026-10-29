#pragma once
#include <swofty/game/base.hpp>

namespace swofty::game {

struct GdbStringRecord {
    uint8_t _pad_0x0[0x10];
    uint64_t enc_size; // L2 experimental
    uint8_t *enc; // L2 experimental
};
static_assert(offsetof(GdbStringRecord, enc_size) == 0x10);
static_assert(offsetof(GdbStringRecord, enc) == 0x18);

using Gdb_Lookup_Key_t = void **(const char *path);
inline constexpr fn<"Gdb_Lookup_Key", Gdb_Lookup_Key_t> Gdb_Lookup_Key{}; // L2 experimental
inline constexpr data<"k_Gdb_StringXorKey", uint8_t[4]> k_Gdb_StringXorKey{}; // L2 experimental
inline constexpr site<"site_Gdb_DeptCountStore"> site_Gdb_DeptCountStore{}; // L2 experimental

}
