// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#pragma once

// Re-read every pointer in the current callback. Only an array index is retained
// as a hint; replacement/reordering is checked against the live vehicle ID.
template <class Read>
uintptr_t find_stopped_player(uintptr_t base, int vehicle, uint8_t &flags, size_t &hint, Read read)
{
    uintptr_t cars = 0;
    struct Bounds { uintptr_t first, last; } bounds{};
    if (!read(base + 0x40, &cars, sizeof cars) || !cars ||
        !read(cars + 0x88, &bounds, sizeof bounds) || bounds.last < bounds.first ||
        bounds.last - bounds.first >= 0x800 || (bounds.last - bounds.first) % sizeof(uintptr_t))
        return 0;
    size_t count = (bounds.last - bounds.first) / sizeof(uintptr_t);
    uintptr_t entries[256];
    if (!count || !read(bounds.first, entries, count * sizeof(uintptr_t)))
        return 0;
    auto matches = [&](size_t i) {
        uint8_t state[8]; // +0x31 flags through +0x38 vehicle ID
        if (!entries[i] || !read(entries[i] + 0x31, state, sizeof state) || state[7] != vehicle)
            return false;
        flags = state[0];
        hint = i;
        return true;
    };
    size_t previous = hint;
    if (previous < count && matches(previous))
        return entries[previous];
    for (size_t i = 0; i < count; ++i)
        if (i != previous && matches(i))
            return entries[i];
    return 0;
}
