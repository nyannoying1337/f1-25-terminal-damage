// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
// Pitstop scene model visibility; GarageCrewModule/car proxies are excluded.
std::atomic<uint64_t> pit_show_calls{0}, pit_show_requested_calls{0}, pit_show_owned_calls{0}, pit_show_hide_calls{0};
std::atomic<int> pit_show_last_slot{-1};
std::atomic<unsigned> pit_clearance_count{0};
std::atomic<uint64_t> pit_update_calls{0}, pit_update_approach_calls{0};
using model_visible_fn = void (*)(void *, bool);
model_visible_fn orig_model_recursive{}, orig_model_local{};
uintptr_t model_recursive_entry{}, model_local_entry{};
struct PitModelTarget {
    uintptr_t actor{}, owner{}, setter{};
    unsigned id{}, slot{};
    int player{-1};
    bool wanted{};
    uint64_t seen{};
};
SRWLOCK pit_models_lock = SRWLOCK_INIT;
std::vector<PitModelTarget> pit_models;
uint64_t pit_model_scan{};

bool pit_clearance_requested()
{
    int pit = player_pit.load(), player = player_vehicle.load();
    bool approach = player_broken && retire_in.load() > -2.0 && pit >= 2 && pit <= 9;
    bool completed_push = player >= 0 && pit_clearance_hold_vehicle.load() == player;
    return !session_ended && (approach || completed_push) &&
           keep_control && retire_in_pit_box && garage_push && pit_retire_clearance && restart_cleanup_available;
}

bool pit_clearance_owns_slot(unsigned slot)
{
    int player = player_vehicle.load();
    if (player < 0 || player >= max_vehicles || pit_slot_of[player].load() != int(slot))
        return false;
    unsigned teammates = 0;
    for (int i = 0; i < max_vehicles; ++i) {
        if (i == player || pit_slot_of[i].load() != int(slot)) continue;
        ++teammates;
        unsigned encoded = observed_pit_state[i].load();
        // Unknown, approaching/pitting or preparing to leave: preserve shared box.
        if (encoded != 1 && encoded != 2 && encoded != 10) return false;
    }
    return teammates == 1;
}

// Called only from a live pitstop callback; pointers below never survive it.
void learn_live_pit_slot(uintptr_t base)
{
    int player = player_vehicle.load();
    if (player < 0 || player >= max_vehicles || pit_slot_of[player].load() >= 0) return;
    unsigned slot = 32;
    uint8_t vehicles[2] = {0xff,0xff};
    if (!guarded_read(base + 0x60, slot) || slot >= 10 ||
        !guarded_copy(base + 0xc0, vehicles, sizeof vehicles) || vehicles[0] >= max_vehicles ||
        vehicles[1] >= max_vehicles || vehicles[0] == vehicles[1] ||
        (vehicles[0] != player && vehicles[1] != player)) return;
    pit_slot_of[vehicles[0]] = int(slot);
    pit_slot_of[vehicles[1]] = int(slot);
    swofty::log("[td-probe] garage: live race pitstop maps vehicles %u/%u to slot %u (no practice cache)",
                unsigned(vehicles[0]), unsigned(vehicles[1]), slot);
}

void observe_live_teammate(uintptr_t base, unsigned slot)
{
    int player = player_vehicle.load();
    for (int v = 0; v < max_vehicles; ++v) {
        if (v == player || pit_slot_of[v].load() != int(slot)) continue;
        uintptr_t system = 0, states = 0, record = 0;
        unsigned state = 16;
        if (guarded_read(base + 0x3c0, system) && system && guarded_read(system + 0x98, states) && states &&
            guarded_read(states + 0x68 + uintptr_t(v) * 16, record) && record &&
            guarded_read(record + 4, state) && state < 16)
            observed_pit_state[v] = state + 1;
    }
}


model_visible_fn original_model_setter(uintptr_t setter)
{
    if (setter && setter == model_recursive_entry) return orig_model_recursive;
    if (setter && setter == model_local_entry) return orig_model_local;
    return nullptr;
}
void forward_model_visibility(void *self, bool visible, uintptr_t setter, model_visible_fn original)
{
    if (!pit_clearance_count.load()) { original(self, visible); return; }
    bool forwarded = visible;
    AcquireSRWLockExclusive(&pit_models_lock);
    for (auto &target : pit_models) {
        if (target.actor != reinterpret_cast<uintptr_t>(self) || target.setter != setter) continue;
        unsigned id = 0;
        // The argument is live; never dereference a saved actor/owner pointer.
        if (guarded_read(reinterpret_cast<uintptr_t>(self) + 0x248, id) && id == target.id) {
            target.wanted = visible;
            if (target.player == player_vehicle.load() && pit_clearance_requested() &&
                pit_clearance_owns_slot(target.slot)) forwarded = false;
        }
        break;
    }
    ReleaseSRWLockExclusive(&pit_models_lock);
    original(self, forwarded);
}
void on_model_recursive(void *self, bool visible)
{
    forward_model_visibility(self, visible, model_recursive_entry, orig_model_recursive);
}
void on_model_local(void *self, bool visible)
{
    forward_model_visibility(self, visible, model_local_entry, orig_model_local);
}

struct LivePitModel { uintptr_t actor{}, setter{}; unsigned id{}, flags{}; };
std::vector<LivePitModel> current_pit_models(uintptr_t base)
{
    std::vector<LivePitModel> models;
    for (unsigned scene = 0; scene <= 16; ++scene) {
        uintptr_t proxy = 0, data = 0, list = 0;
        uint64_t count = 0;
        if (!guarded_read(base + 0xe0 + scene * 8, proxy) || !proxy ||
            !guarded_read(proxy + 0xa0, data) || !data || !guarded_read(data + 0x100, count) ||
            count > 256 || !count || !guarded_read(proxy + 0x78, list) || !list) continue;
        std::vector<uintptr_t> actors(size_t(count), 0);
        if (!guarded_copy(list, actors.data(), actors.size() * sizeof(uintptr_t))) continue;
        for (auto actor : actors) {
            uintptr_t vt = 0, setter = 0;
            unsigned id = 0, flags = 0;
            if (!actor || !guarded_read(actor, vt) || !vt || !guarded_read(vt + 0x98, setter) ||
                !original_model_setter(setter) || !guarded_read(actor + 0x248, id) ||
                !guarded_read(actor + 0x40, flags)) continue;
            bool duplicate = false;
            for (const auto &known : models) if (known.actor == actor) { duplicate = true; break; }
            if (!duplicate) models.push_back({actor, setter, id, flags});
        }
    }
    return models;
}

// A retiring car is intentionally omitted from the tyre job. In a double stack,
// the teammate's job owns the crew until release, so our job-refusal hook may
// never run. Supply only the missing crew-ready signal after the shared-box guard
// says the teammate is safely clear; do not change pit state or move the car.
bool release_retirement_box_entry()
{
    if (session_ended || !keep_control || !retire_in_pit_box || !garage_push || !player_broken ||
        retire_in.load()<=-2 || player_pit.load()!=3 || !pushing || !prepare_at_box) return false;
    int player = player_vehicle.load();
    if (player < 0 || player >= max_vehicles) return false;
    int slot=pit_slot_of[player].load(); uint32_t session=0;
    if (slot<0 || !pit_clearance_owns_slot(unsigned(slot)) || !repair_session_kind ||
        !guarded_read(repair_session_kind,session) || session<15 || session>17) return false;
    uintptr_t mm = 0, pitting = 0;
    uint8_t ready = 0;
    uintptr_t sys = pit_lane_system.load();
    if (!sys || !guarded_read(sys + 0x48, mm) || !mm ||
        !guarded_read(mm + 0x938 + uintptr_t(player) * 16, pitting) || !pitting ||
        !guarded_read(pitting + 0x104, ready)) return false;
    if (!ready) {
        *reinterpret_cast<uint8_t *>(pitting + 0x104) = 1;
        if (log_telemetry) swofty::log("[td-probe] garage: shared box is clear; crew ready for retiring vehicle %d after teammate stop", player);
    }
    return true;
}

void apply_pit_clearance(void *self)
{
    bool requested = pit_clearance_requested();
    if (requested) ++pit_show_requested_calls;
    if (!requested && !pit_clearance_count.load()) return;
    uintptr_t base = reinterpret_cast<uintptr_t>(self);
    unsigned slot = 32;
    if (!guarded_read(base + 0x60, slot) || slot >= 10) return;
    pit_show_last_slot = int(slot);
    int player = player_vehicle.load();
    bool mine = player >= 0 && player < max_vehicles && pit_slot_of[player].load() == int(slot);
    // A new live instance occupying this team slot invalidates old ownership.
    // Forget old addresses without calling them, even across session teardown.
    AcquireSRWLockExclusive(&pit_models_lock);
    for (auto it = pit_models.begin(); it != pit_models.end();) {
        if (it->slot == slot && it->owner != base) it = pit_models.erase(it);
        else ++it;
    }
    pit_clearance_count = unsigned(pit_models.size());
    ReleaseSRWLockExclusive(&pit_models_lock);
    bool pending = false;
    AcquireSRWLockShared(&pit_models_lock);
    for (const auto &t : pit_models) if (t.owner == base) { pending = true; break; }
    ReleaseSRWLockShared(&pit_models_lock);
    if (!mine && !pending) return;
    if (requested && mine) observe_live_teammate(base, slot);
    bool owns = mine && pit_clearance_owns_slot(slot);
    if (requested && owns) ++pit_show_owned_calls;
    uint32_t session = 0;
    bool hide = requested && owns && repair_session_kind && guarded_read(repair_session_kind, session) &&
                session >= 15 && session <= 17;

    if (!hide && !pending) return;
    auto live = current_pit_models(base);
    struct Action { uintptr_t actor, setter; bool visible; };
    std::vector<Action> actions;
    unsigned registered = 0, restored = 0;
    AcquireSRWLockExclusive(&pit_models_lock);
    uint64_t scan = ++pit_model_scan;
    for (const auto &model : live) {
        auto found = pit_models.end();
        for (auto it = pit_models.begin(); it != pit_models.end(); ++it)
            if (it->actor == model.actor && it->owner == base && it->id == model.id && it->setter == model.setter) { found = it; break; }
        if (hide) {
            if (found == pit_models.end()) {
                pit_models.push_back({model.actor, base, model.setter, model.id, slot, player, bool(model.flags & 0x20), scan});
                ++registered;
            } else found->seen = scan;
            if (model.flags & 0x20) actions.push_back({model.actor, model.setter, false});
        } else if (found != pit_models.end()) {
            // Only a model proven to remain in this live owner can be restored.
            actions.push_back({model.actor, model.setter, found->wanted});
            pit_models.erase(found);
            ++restored;
        }
    }
    // Destroyed/replaced models are forgotten without touching their addresses.
    for (auto it = pit_models.begin(); it != pit_models.end();) {
        if (it->owner == base && (!hide || it->seen != scan)) it = pit_models.erase(it);
        else ++it;
    }
    pit_clearance_count = unsigned(pit_models.size());
    // Arm after registering models, including the first callback during a push.
    if (hide && player_pit.load() == 8 && !live.empty()) pit_clearance_hold_vehicle = player;
    ReleaseSRWLockExclusive(&pit_models_lock);
    for (const auto &a : actions) original_model_setter(a.setter)(reinterpret_cast<void *>(a.actor), a.visible);
    if (hide) ++pit_show_hide_calls;
    if (registered || restored)
        swofty::log("[td-probe] pit clearance models: slot=%u live=%u registered=%u restored=%u changed=%u; garage crew/car proxies excluded",
                    slot, unsigned(live.size()), registered, restored, unsigned(actions.size()));
}

uint64_t (*orig_pit_update)(void *);
uint64_t on_pit_update(void *task)
{
    ++pit_update_calls;
    uintptr_t base = 0;
    int player = player_vehicle.load();
    if (!session_ended && player >= 0 && player < max_vehicles && pit_slot_of[player].load() < 0 &&
        guarded_read(reinterpret_cast<uintptr_t>(task) + 8, base) && base) learn_live_pit_slot(base);
    uint64_t result = orig_pit_update(task);
    // Movement readiness is independent of the cosmetic equipment option.
    if (player_broken && player_pit.load()==3 &&
        guarded_read(reinterpret_cast<uintptr_t>(task)+8,base) && base) {
        unsigned slot=32;
        if (guarded_read(base+0x60,slot) && player>=0 && player<max_vehicles && pit_slot_of[player].load()==int(slot)) {
            observe_live_teammate(base,slot);
            release_retirement_box_entry();
        }
    }
    if (pit_clearance_requested()) ++pit_update_approach_calls;
    if ((player_broken || pit_clearance_hold_vehicle.load() >= 0 || pit_clearance_count.load()) && guarded_read(reinterpret_cast<uintptr_t>(task) + 8, base) && base)
        apply_pit_clearance(reinterpret_cast<void *>(base));
    return result;
}

void hook_pit_clearance()
{
    const char *recursive_sig = "41 57 41 56 41 54 56 57 53 48 83 EC 28 48 89 CE 8B 41 40 0F B6 FA C1 E7 05 83 E0 DF";
    const char *local_sig = "56 57 53 48 83 EC 20 48 89 CE 8B 41 40 0F B6 CA C1 E1 05 83 E0 DF";
    uintptr_t recursive = 0, local = 0;
    if (swofty::api->scan(".xtls", recursive_sig, &recursive) != 1 || !recursive ||
        swofty::api->scan(".xtls", local_sig, &local) != 1 || !local) {
        swofty::log("[td-probe] pit clearance: model visibility signatures unavailable; unchanged"); return;
    }
    constexpr uint8_t recursive_prologue[] = {0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xec,0x28,0x48,0x89,0xce};
    constexpr uint8_t local_prologue[] = {0x56,0x57,0x53,0x48,0x83,0xec,0x20,0x48,0x89,0xce,0x8b,0x41,0x40,0x0f,0xb6,0xca};
    if (detour("Model visibility recursive", recursive_sig, recursive_prologue, sizeof recursive_prologue,
               reinterpret_cast<void *>(&on_model_recursive), reinterpret_cast<void **>(&orig_model_recursive))) model_recursive_entry = recursive;
    if (detour("Model visibility local", local_sig, local_prologue, sizeof local_prologue,
               reinterpret_cast<void *>(&on_model_local), reinterpret_cast<void **>(&orig_model_local))) model_local_entry = local;
    constexpr uint8_t update_prologue[] = {0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x81,0xec,0x70,0x02,0,0};
    detour("Pitstop live update", "41 57 41 56 41 54 56 57 55 53 48 81 EC 70 02 00 00 C5 78 29 BC 24 60 02 00 00",
           update_prologue, sizeof update_prologue, reinterpret_cast<void *>(&on_pit_update), reinterpret_cast<void **>(&orig_pit_update));
}

// Value-only diagnostics on frame: no game-object reads or cached pointers.
void report_pit_clearance()
{
    static uint64_t next = 0;
    if (!log_telemetry || !player_broken || session_ended || !pit_retire_clearance) { next = 0; return; }
    uint64_t now = GetTickCount64();
    if (now < next) return;
    next = now + 5000;
    int player = player_vehicle.load();
    int slot = player >= 0 && player < max_vehicles ? pit_slot_of[player].load() : -1;
    int teammate = -1, encoded = 0;
    if (slot >= 0) for (int i = 0; i < max_vehicles; ++i)
        if (i != player && pit_slot_of[i].load() == slot) { teammate = i; encoded = int(observed_pit_state[i].load()); break; }
    swofty::log("[td-probe] pit clearance diagnostic: player=%d pit=%d slot=%d teammate=%d teammate_state=%d enabled=%d keep=%d box=%d push=%d retire=%.1f callbacks=%llu requested=%llu owned=%llu eligible=%llu last_slot=%d updates=%llu approach_updates=%llu",
                player, player_pit.load(), slot, teammate, encoded - 1, bool(pit_retire_clearance), bool(keep_control),
                bool(retire_in_pit_box), bool(garage_push), retire_in.load(),
                (unsigned long long)pit_show_calls.load(), (unsigned long long)pit_show_requested_calls.load(),
                (unsigned long long)pit_show_owned_calls.load(), (unsigned long long)pit_show_hide_calls.load(), pit_show_last_slot.load(),
                (unsigned long long)pit_update_calls.load(), (unsigned long long)pit_update_approach_calls.load());
}
