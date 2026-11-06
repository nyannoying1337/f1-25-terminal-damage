// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
//
// Emitted code stubs and the tether plane / distance experiments.
//
// Textual fragment of terminal_damage_probe.cpp: included there, in order, inside the anonymous
// namespace. It is not a standalone header; it uses what the fragments before it define.

// In ChangeResidentAssets the session selection loads either a record's Garages
// group (+0x62e8) or its PitStops group (+0x60b8); the race picks PitStops. The
// patch sends the PitStops branch (site+0x22) to a stub that adds both groups
// with the game's own three helper calls, then rejoins at site+0x73.
constexpr const char *sig_asset_selection_site =
    "80 7E 61 00 74 6D 0F B6 46 60 83 F8 01 74 13 85 C0 75 60 49 8B 94 24 E8 62 00 00 48 85 D2 75 0F EB 51 "
    "49 8B 94 24 B8 60 00 00 48 85 D2 74 44 80 3A 00 74 3F 48 8B 5C 24 40 48 89 F9 E8 ?? ?? ?? ?? "
    "48 8B 0D ?? ?? ?? ?? 48 8B 54 24 48 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 89 D9 48 89 C2 E8";

uintptr_t rel_target(uintptr_t field, uintptr_t next)
{
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void *>(field), sizeof rel);
    return next + int64_t(rel);
}

struct emitter {
    uint8_t *p;
    void bytes(std::initializer_list<uint8_t> b) { for (auto x : b) *p++ = x; }
    void u32(uint32_t v) { memcpy(p, &v, 4); p += 4; }
    void u64(uint64_t v) { memcpy(p, &v, 8); p += 8; }
    void call(uintptr_t fn) { bytes({0x48, 0xB8}); u64(fn); bytes({0xFF, 0xD0}); }
    void jmp(uintptr_t to) { bytes({0xFF, 0x25, 0, 0, 0, 0}); u64(to); }
};

// A page within 2 GB of near_to, so a `jmp rel32` there and back is possible.
uint8_t *alloc_near(uintptr_t near_to)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t step = si.dwAllocationGranularity;
    for (uintptr_t at = (near_to & ~(step - 1)) - step; at > near_to - 0x7ff00000; at -= step)
        if (auto *p = static_cast<uint8_t *>(
                VirtualAlloc(reinterpret_cast<void *>(at), step, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
            return p;
    return nullptr;
}

// Seals the stub [stub, end) and overwrites `len` bytes at `at` with `jmp stub` plus nops.
bool jump_to_stub(uintptr_t at, size_t len, uint8_t *stub, uint8_t *end)
{
    DWORD old;
    VirtualProtect(stub, end - stub, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, end - stub);
    int64_t rel = int64_t(reinterpret_cast<uintptr_t>(stub)) - int64_t(at + 5);
    if (rel < INT32_MIN || rel > INT32_MAX || len < 5 || len > 32)
        return false;
    uint8_t patch[32];
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    int32_t rel32 = int32_t(rel);
    memcpy(patch + 1, &rel32, 4);
    auto *code = reinterpret_cast<uint8_t *>(at);
    if (!VirtualProtect(code, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(code, patch, len);
    VirtualProtect(code, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, len);
    return true;
}

// The tether stub below reads these objects as raw memory: session_ended as one byte (cmp byte [rdx], 0),
// player_vehicle as four (mov eax, [rdx]) and the car pointer at +0 of throttle_hook (checked at its declaration).
static_assert(sizeof(session_ended) == 1 && std::atomic<bool>::is_always_lock_free,
              "the tether stub reads session_ended as one byte");
static_assert(sizeof(player_vehicle) == 4 && std::atomic<int>::is_always_lock_free,
              "the tether stub reads player_vehicle as four bytes");

// The stub of both tether experiments: the plane one (reference 144664348) and the distance one, which also passes
// original_skip and load_owner. It isolates ONLY the chassis-plane projection/velocity rewrite. Joint creation, radial
// limit, mesh and attached suspension stay stock. No callback, allocation, logging or object mutation in the update
// hot path.
void emit_tether_plane_stub(emitter &e, uintptr_t stock, uintptr_t skip, uintptr_t vt,
                            uintptr_t original_skip=0, bool load_owner=false)
{
    e.bytes({0x0f,0x86}); auto *already_skip=e.p; e.u32(0); // original jbe
    e.bytes({0x9c,0x50,0x52}); // preserve flags, rax, rdx
    if (load_owner) e.bytes({0x53,0x48,0x8b,0x5e,0x10}); // preserve rbx, live owner
    std::vector<uint8_t *> failures;
    auto fail=[&](uint8_t condition) { e.bytes({0x0f,condition}); failures.push_back(e.p); e.u32(0); };
    e.bytes({0x48,0xba}); e.u64(vt);
    e.bytes({0x48,0x39,0x16}); fail(0x85); // wheel vtable
    e.bytes({0x48,0x39,0x5e,0x10}); fail(0x85); // live owner == rbx
    e.bytes({0x48,0xba}); e.u64(reinterpret_cast<uintptr_t>(&throttle_hook));
    e.bytes({0x48,0x3b,0x1a}); fail(0x85); // current player car
    e.bytes({0x48,0xba}); e.u64(reinterpret_cast<uintptr_t>(&session_ended));
    e.bytes({0x80,0x3a,0x00}); fail(0x85);
    e.bytes({0x48,0xba}); e.u64(reinterpret_cast<uintptr_t>(&player_vehicle));
    e.bytes({0x8b,0x02,0x83,0xf8,0x20}); fail(0x83); // unsigned ID <32, rejects -1
    e.bytes({0x0f,0xb6,0x93,0xe8,0x00,0x00,0x00,0x39,0xd0}); fail(0x85);
    if (load_owner) e.bytes({0x5b});
    e.bytes({0x5a,0x58,0x9d}); e.jmp(skip);
    auto *stock_label=e.p;
    if (load_owner) e.bytes({0x5b});
    e.bytes({0x5a,0x58,0x9d}); e.jmp(stock);
    auto *skip_label=e.p; e.jmp(original_skip ? original_skip : skip);
    auto relative=[](uint8_t *field,uint8_t *target) {
        int32_t displacement=int32_t(target-(field+4)); memcpy(field,&displacement,4);
    };
    relative(already_skip,skip_label);
    for (auto *field:failures) relative(field,stock_label);
}

void hook_tether_distance_experiment()
{
    if (!tether_distance_experiment) return;
    if (wheel_tether_test) {
        swofty::log("[td-probe] tether distance experiment: disable Remove player wheel tethers first; experiment not installed");
        return;
    }
    constexpr const char *sig=
        "C5 F8 2E DC 0F 86 9F 01 00 00 C5 DA 5E CB C5 B0 5C D0 C5 F0 C6 C9 00 C5 E8 59 C9 C5 78 58 C9";
    uintptr_t site=0;
    if (swofty::api->scan(".xtls",sig,&site)!=1 || !site) {
        swofty::log("[td-probe] tether distance experiment: signature unavailable; stock retained");return;
    }
    uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t vt=base+0x5bf9420,update=0;
    const uint8_t branch[]={0x0f,0x86,0x9f,0x01,0x00,0x00};
    uintptr_t at=site+4;
    if (!guarded_read(vt+0x68,update) || update!=base+0x4664080 ||
        memcmp(reinterpret_cast<void *>(at),branch,sizeof branch)) return;
    auto *stub=alloc_near(at);if (!stub) return;
    emitter e{stub};
    // Original under-limit branch retains anchor/length updates at ca5.
    // Over-limit experiment rejoins at the SAME continuation used by the
    // stock hard clamp (ca0 -> 5387), not with its overwritten SIMD temporaries.
    emit_tether_plane_stub(e,at+6,base+0x4665387,vt,at+6+0x19f,true);
    bool installed=jump_to_stub(at,6,stub,e.p);
    swofty::log("[td-probe] tether distance experiment: installed=%d player only; joint and break cutoff retained; restart required",int(installed));
}

void hook_tether_plane_experiment()
{
    if (!tether_plane_experiment) return;
    if (wheel_tether_test) {
        swofty::log("[td-probe] tether plane experiment: disable Remove player wheel tethers first; experiment not installed");
        return;
    }
    constexpr const char *sig=
        "C5 F2 59 96 F0 00 00 00 C4 41 10 57 ED C4 C1 78 2E D5 0F 86 03 01 00 00 C4 C3 31 0C D4 08";
    uintptr_t site=0;
    if (swofty::api->scan(".xtls",sig,&site)!=1 || !site) {
        swofty::log("[td-probe] tether plane experiment: signature unavailable; stock retained"); return;
    }
    uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t vt=base+0x5bf9420, update=0;
    // The signature spans 144664336..353; exact branch and vtable validation.
    uintptr_t at=site+0x12;
    const uint8_t branch[]={0x0f,0x86,0x03,0x01,0x00,0x00};
    if (!guarded_read(vt+0x68,update) || update!=base+0x4664080 ||
        memcmp(reinterpret_cast<void *>(at),branch,sizeof branch)) return;
    auto *stub=alloc_near(at);
    if (!stub) return;
    emitter e{stub}; emit_tether_plane_stub(e,at+6,at+6+0x103,vt);
    bool installed=jump_to_stub(at,6,stub,e.p);
    swofty::log("[td-probe] tether plane experiment: installed=%d player only; physical tether retained; restart required",int(installed));
}
