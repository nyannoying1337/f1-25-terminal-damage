// Standalone Windows test. No game hooks, sockets or patch installation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
#include <cstdarg>
#include <string>
#include "test_util.hpp"

// The game's own flashback rule (GameFlashbackSystem canJumpIn) is only watched: the result stays the game's, a
// refusal is logged with its reason (flashbacks off, none left) at most every 10 s, a pass logs nothing.
namespace {
std::string last_log;
int log_count = 0;
void capture_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    last_log = buf;
    ++log_count;
}
int no_setting(const char *, uint32_t, SwoftySettingValue *) { return 0; }
int no_watch(const char *, SwoftySettingFn, void *) { return 0; }
// The game's canJumpIn as its body (0x14e44f740) has it: a free flashback passes, off refuses, no limit passes, else
// used < limit. Only al is the result; the upper bits hold whatever the game left there.
uint64_t game_rule(void *recordable)
{
    auto *r = static_cast<const uint8_t *>(recordable);
    uint32_t used = 0, limit = 0;
    memcpy(&used, r + 0x1c, 4);
    memcpy(&limit, r + 0x24, 4);
    bool ok = r[0x20] || (r[0x18] && (!r[0x28] || used < limit));
    return 0xabcdef00ull | (ok ? 1 : 0);
}
struct Recordable {
    uint8_t b[0x30] = {};
    Recordable(bool on, uint8_t mode, uint32_t used, bool free, uint32_t limit, bool limited)
    {
        b[0x18] = on;
        b[0x19] = mode;
        memcpy(b + 0x1c, &used, 4);
        b[0x20] = free;
        memcpy(b + 0x24, &limit, 4);
        b[0x28] = limited;
    }
};
}

int main()
{
    SwoftyApi api{};
    api.version = 4;
    api.log = capture_log;
    api.setting = no_setting;
    api.watch_setting = no_watch;
    swofty::api = &api;
    orig_flashback_rule = &game_rule;

    // Flashbacks off: refused with the game's value, logged as the settings, not again within 10 s.
    {
        Recordable r(false, 3, 0, false, 0, false);
        flashback_rule_logged_at = 0;
        log_count = 0;
        assert(on_flashback_rule(r.b) == 0xabcdef00ull);
        assert(log_count == 1);
        assert(last_log.find("not the damage") != std::string::npos);
        assert(last_log.find("flashbacks are off in this game's settings") != std::string::npos);
        assert(last_log.find("flashbacks off, limit unlimited (not counted), used 0 of 0, free flashback 0") !=
               std::string::npos);
        on_flashback_rule(r.b);
        assert(log_count == 1);
    }
    // Limited and used up: refused, logged as none left.
    {
        Recordable r(true, 0, 2, false, 2, true);
        flashback_rule_logged_at = 0;
        log_count = 0;
        assert((on_flashback_rule(r.b) & 0xff) == 0);
        assert(log_count == 1);
        assert(last_log.find("no flashbacks left in this session") != std::string::npos);
        assert(last_log.find("flashbacks on, limit low, used 2 of 2") != std::string::npos);
    }
    // A limit of 0 refuses too (a session type the game gives none).
    {
        Recordable r(true, 1, 0, false, 0, true);
        FlashbackRule f = read_flashback_rule(r.b);
        assert(!flashback_rule_allows(f));
        assert(!strcmp(flashback_mode_name(f.mode), "medium"));
        assert(!strcmp(flashback_rule_reason(f), "no flashbacks left in this session"));
    }
    // Passes are returned unchanged and log nothing: on without a limit, on with flashbacks left, a free flashback
    // while off.
    {
        Recordable a(true, 3, 9, false, 0, false), b(true, 2, 1, false, 3, true), c(false, 0, 0, true, 0, true);
        flashback_rule_logged_at = 0;
        log_count = 0;
        assert(on_flashback_rule(a.b) == 0xabcdef01ull);
        assert(on_flashback_rule(b.b) == 0xabcdef01ull);
        assert(on_flashback_rule(c.b) == 0xabcdef01ull);
        assert(log_count == 0);
        assert(!strcmp(flashback_rule_reason(read_flashback_rule(c.b)), "allowed"));
    }
    // The decoder agrees with the game's body on every combination.
    for (int on = 0; on < 2; ++on)
        for (int free = 0; free < 2; ++free)
            for (int limited = 0; limited < 2; ++limited)
                for (uint32_t used = 0; used < 3; ++used)
                    for (uint32_t limit = 0; limit < 3; ++limit) {
                        Recordable r(on, 0, used, free, limit, limited);
                        assert(flashback_rule_allows(read_flashback_rule(r.b)) == bool(game_rule(r.b) & 1));
                    }
    assert(!strcmp(flashback_mode_name(3), "unlimited"));
    assert(!strcmp(flashback_mode_name(7), "?"));
    // The body the hook checks before it patches the slot: the game's bytes (F1 25, 0x14e44f740).
    static const uint8_t game_bytes[] = {0xb0, 0x01, 0x80, 0x79, 0x20, 0x00, 0x74, 0x01, 0xc3, 0x80, 0x79, 0x18,
                                         0x00, 0x74, 0x10, 0x80, 0x79, 0x28, 0x00, 0x74, 0xf3, 0x8b, 0x41, 0x1c,
                                         0x3b, 0x41, 0x24, 0x0f, 0x92, 0xd0, 0xc3, 0x31, 0xc0, 0xc3};
    static_assert(sizeof game_bytes == sizeof flashback_rule_body);
    assert(!memcmp(game_bytes, flashback_rule_body, sizeof game_bytes));
    puts("flashback_rule_test ok");
    return 0;
}
