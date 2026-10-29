#pragma once

// Copy current game memory without VirtualQuery's potentially expensive heap
// region enumeration. No readability result or game pointer is cached.
inline bool guarded_copy(uintptr_t at, void *out, size_t bytes)
{
    if (!at || at + bytes < at)
        return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), out, bytes, &copied) &&
           copied == bytes;
}

template <class T> bool guarded_read(uintptr_t at, T &out)
{
    T value{};
    if (!guarded_copy(at, &value, sizeof value))
        return false;
    out = value;
    return true;
}
