// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#pragma once
#include <cstdint>
// SpawnGarageCrew filters this current 0x98-stride roster by record+88->28,
// then uses vehicle ID record+82. Pitstop render-slot IDs are not assumed equal.
struct GarageAssignment { int slot = -1, teammate = -1; };
template<class Copy> GarageAssignment current_garage_assignment(uintptr_t module, int vehicle, Copy copy)
{
    GarageAssignment invalid;
    if (!module || vehicle < 0 || vehicle >= 32) return invalid;
    uintptr_t roster = 0, begin = 0, end = 0;
    if (!copy(module + 0x50, &roster, 8) || !roster ||
        !copy(roster + 0x1d8, &begin, 8) || !copy(roster + 0x1e0, &end, 8) ||
        !begin || end < begin || (end - begin) % 0x98 || (end - begin) / 0x98 > 32) return invalid;
    struct Member { uint8_t vehicle; uint32_t slot; } members[32]{};
    unsigned n = unsigned((end - begin) / 0x98), matches = 0;
    int slot = -1;
    for (unsigned i = 0; i < n; ++i) {
        uintptr_t record = begin + i * 0x98, info = 0;
        if (!copy(record + 0x82, &members[i].vehicle, 1) || members[i].vehicle >= 32 ||
            !copy(record + 0x88, &info, 8) || !info || !copy(info + 0x28, &members[i].slot, 4) || members[i].slot >= 32)
            return invalid;
        if (members[i].vehicle == vehicle) { ++matches; slot = int(members[i].slot); }
    }
    if (matches != 1) return invalid;
    int mate = -1;
    unsigned pair = 0;
    for (unsigned i = 0; i < n; ++i) if (int(members[i].slot) == slot) {
        ++pair;
        if (members[i].vehicle != vehicle) mate = members[i].vehicle;
    }
    return pair == 2 && mate >= 0 ? GarageAssignment{slot, mate} : invalid;
}
