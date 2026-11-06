// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Detour_at / detour: absolute-jump trampolines.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// Detour a function whose first len (>= 14) bytes are whole, position-independent instructions.
bool detour_at(const char *name, uintptr_t at, const uint8_t *prologue, size_t len, void *handler, void **orig)
{
    if (!at || len < 14 || len > 32 || !swofty::mem::readable(at, len) ||
        memcmp(reinterpret_cast<void *>(at), prologue, len)) {
        swofty::log("[td-probe] %s: prologue does not match, not hooked", name);
        return false;
    }
    auto *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp)
        return false;
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t back = at + len;
    memcpy(tramp, prologue, len);
    memcpy(tramp + len, jmp, 6);
    memcpy(tramp + len + 6, &back, 8);
    DWORD old;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 14);
    *orig = tramp;
    uint8_t patch[32];
    memset(patch, 0xCC, sizeof patch);
    memcpy(patch, jmp, 6);
    memcpy(patch + 6, &handler, 8);
    auto *code = reinterpret_cast<uint8_t *>(at);
    if (!VirtualProtect(code, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(code, patch, len);
    VirtualProtect(code, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, len);
    swofty::log("[td-probe] hooked %s at %p", name, code);
    return true;
}

bool detour(const char *name, const char *sig, const uint8_t *prologue, size_t len, void *handler, void **orig)
{
    uintptr_t at = 0;
    int n = swofty::api->scan(".xtls", sig, &at);
    if (n != 1) {
        swofty::log("[td-probe] %s: signature has %d hits, not hooked", name, n);
        return false;
    }
    return detour_at(name, at, prologue, len, handler, orig);
}
