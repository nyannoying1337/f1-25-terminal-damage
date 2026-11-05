// The manifest (mod/swofty.json) and the presets (presets.hpp, 0.51.117): every text within the launcher's 255 bytes,
// every default a valid value, every preset value one the option has, the version the DLL logs the manifest's, and
// the preset's start rules. Run from the repository root (the test loop does).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <swofty/json.hpp>
#include <cassert>
#include <fstream>
#include <set>
#include <sstream>

namespace {
void log_sink(const char *, ...) {}
std::string slurp(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

int main()
{
    SwoftyApi api{};
    api.log = log_sink;
    swofty::api = &api;
    namespace json = swofty::json;

    std::string text = slurp("mod/swofty.json");
    assert(!text.empty());
    json::Value m = json::parse(text);
    assert(m.is_object());
    const json::Array *opts = m["options"].if_array();
    assert(opts && !opts->empty());

    // Every option: id, type, label and description, texts within 255 bytes, a valid default, no id twice.
    std::set<std::string> ids;
    for (const json::Value &o : *opts) {
        std::string id(o["id"].as_string());
        assert(!id.empty() && ids.insert(id).second);
        std::string_view type = o["type"].as_string();
        assert(type == "choice" || type == "toggle" || type == "slider");
        for (const char *k : {"label", "description"}) {
            std::string_view t = o[k].as_string();
            if (t.empty() || t.size() > 255)
                fprintf(stderr, "option %s: %s is %zu bytes\n", id.c_str(), k, t.size());
            assert(!t.empty() && t.size() <= 255);
        }
        if (type == "toggle") {
            assert(o["default"].is_bool());
        } else if (type == "choice") {
            const json::Array *vals = o["values"].if_array();
            assert(vals && !vals->empty());
            bool found = false;
            for (const json::Value &v : *vals) {
                std::string_view value = v.is_object() ? v["value"].as_string() : v.as_string();
                if (v.is_object())
                    assert(!v["label"].as_string().empty() && v["label"].as_string().size() <= 255);
                found |= value == o["default"].as_string();
            }
            assert(found);
        }
    }

    // The preset option: first, Realistic by default (0.75), one value per preset.
    const json::Value &first = (*opts)[0];
    assert(first["id"].as_string() == "preset" && first["default"].as_string() == "realistic" &&
           first["default"].as_string() == presets::default_preset);
    // 0.75: suspension damage is experimental and off by default, and no preset touches it; the debug test keys are
    // off by default; every Debug option comes after the others.
    {
        bool debug_seen = false;
        for (const json::Value &o : *opts) {
            std::string id(o["id"].as_string());
            bool debug = o["label"].as_string().substr(0, 6) == "Debug:";
            if (debug_seen && !debug)
                fprintf(stderr, "option %s comes after a Debug option\n", id.c_str());
            assert(!debug_seen || debug);
            debug_seen |= debug;
            if (id == "suspension_damage" || id == "debug_failure_hotkey")
                assert(o["default"].is_bool() && !o["default"].as_bool());
        }
        assert(debug_seen && !presets::entry_of("suspension_damage"));
    }
    {
        std::set<std::string> vals;
        for (const json::Value &v : *first["values"].if_array())
            vals.insert(std::string(v["value"].as_string()));
        assert(vals.count("custom") && vals.size() == size_t(presets::n_presets) + 1);
        for (const char *n : presets::names)
            assert(vals.count(n));
    }

    // Every option a preset sets exists, and every preset value is one the option takes.
    for (const presets::Entry &e : presets::table) {
        const json::Value *opt = nullptr;
        for (const json::Value &o : *opts)
            if (o["id"].as_string() == e.option)
                opt = &o;
        if (!opt)
            fprintf(stderr, "preset option %s is not in the manifest\n", e.option);
        assert(opt);
        for (const char *v : e.value) {
            std::string_view val = v;
            if ((*opt)["type"].as_string() == "toggle") {
                assert(val == "true" || val == "false");
                continue;
            }
            bool ok = false;
            for (const json::Value &x : *(*opt)["values"].if_array())
                ok |= (x.is_object() ? x["value"].as_string() : x.as_string()) == val;
            if (!ok)
                fprintf(stderr, "preset value %s=%s is not a value of the option\n", e.option, v);
            assert(ok);
        }
        // The probe reads it: no preset option without a current value.
        assert(!preset_current(e.option).empty());
    }
    // The defaults the code falls back to match the manifest's for the preset options.
    for (const presets::Entry &e : presets::table)
        for (const json::Value &o : *opts)
            if (o["id"].as_string() == e.option) {
                std::string def = o["type"].as_string() == "toggle" ? (o["default"].as_bool() ? "true" : "false")
                                                                    : std::string(o["default"].as_string());
                if (def != preset_current(e.option))
                    fprintf(stderr, "default of %s: manifest %s, code %s\n", e.option, def.c_str(),
                            preset_current(e.option).c_str());
                assert(def == preset_current(e.option));
            }

    // The version the DLL logs is the manifest's.
    {
        std::string src = slurp("mod/native/src/terminal_damage_probe.cpp");
        std::string want = "loaded version=" + std::string(m["version"].as_string()) + ",";
        if (src.find(want) == std::string::npos)
            fprintf(stderr, "the probe does not log %s\n", want.c_str());
        assert(src.find(want) != std::string::npos);
    }

    // Presets: names, entries, and how many options differ.
    assert(presets::index_of("casual") == 0 && presets::index_of("realistic") == 1 && presets::index_of("hardcore") == 2);
    assert(presets::index_of("custom") == -1 && presets::index_of("") == -1);
    assert(presets::entry_of("gp_failures") && !presets::entry_of("retire_key") && !presets::entry_of("failure_era"));
    {
        // Every option at Realistic: nothing differs; one changed: one differs.
        auto realistic = [](const char *o) { return std::string(presets::entry_of(o)->value[1]); };
        assert(presets::differing(1, realistic) == 0);
        auto one_off = [](const char *o) {
            return std::string(std::string_view(o) == "damage_panel" ? "always" : presets::entry_of(o)->value[1]);
        };
        assert(presets::differing(1, one_off) == 1 && presets::differing(-1, one_off) == 0);
        // The code defaults are the Realistic preset (0.75).
        assert(presets::differing(1, [](const char *o) { return preset_current(o); }) == 0);
    }

    // At the game start.
    using presets::Start;
    assert(presets::at_start(false, "", "custom", 0) == Start::remember);       // first start, nothing picked
    // The default preset at the first start with nothing kept (an update, a fresh install): never applied, options set
    // by hand stay and the preset shows Custom (0.75).
    assert(presets::at_start(false, "", "realistic", 3) == Start::to_custom);
    assert(presets::at_start(false, "", "hardcore", 3) == Start::apply);        // picked in the launcher before
    assert(presets::at_start(false, "", "realistic", 0) == Start::remember);    // already matches
    assert(presets::at_start(true, "custom", "hardcore", 4) == Start::apply);   // picked in the launcher since
    assert(presets::at_start(true, "hardcore", "hardcore", 0) == Start::nothing);
    assert(presets::at_start(true, "hardcore", "hardcore", 1) == Start::to_custom);  // an option changed since
    assert(presets::at_start(true, "realistic", "custom", 2) == Start::remember);
    assert(presets::at_start(true, "custom", "custom", 2) == Start::nothing);
    // Kept before 0.75 (no ";v2"): Custom was the default then, so Realistic now is the new default, not a pick.
    assert(presets::at_start(true, "custom", "realistic", 2, true) == Start::to_custom);
    assert(presets::at_start(true, "custom", "realistic", 0, true) == Start::remember);
    assert(presets::at_start(true, "custom", "hardcore", 2, true) == Start::apply);
    assert(presets::at_start(true, "custom", "realistic", 2, false) == Start::apply);  // 0.75 kept it: a real pick
    assert(presets::at_start(true, "realistic", "realistic", 0, true) == Start::nothing);

    // The frame-thread logic without a Swofty core: no storage, no settings command. It must not apply anything or
    // loop; it only remembers.
    preset_tick();
    assert(!preset_rt.started);  // waits 3 s first
    preset_rt.start_at = 1;
    preset_tick();
    assert(preset_rt.started && preset_rt.applying == -1 && preset_rt.stored == "realistic");
    // A preset picked in the game is applied option by option; without a core each set fails and the pass ends.
    preset_begin(2, "test");
    preset_apply_step(GetTickCount64());
    assert(preset_rt.applying == -1 && preset_rt.set == 0 && preset_rt.failed == 2 && preset_rt.stored == "hardcore");

    // The welcome popup (0.51.120, 0.51.121): once, on the main menu (the home flow state for 10 s), never in a
    // session; the storage remembers it. Without the home state: 3 minutes after the first frame.
    {
        static std::string file, title, body;
        static bool has_file = false;
        static int shows = 0;
        static SwoftyStorage st{};
        st.available = [] { return 1; };
        st.size = [](const char *, uint64_t *n) {
            if (!has_file)
                return int(SWOFTY_ASSET_INVALID);
            *n = file.size();
            return int(SWOFTY_ASSET_OK);
        };
        st.read = [](const char *, void *buf, size_t cap, size_t *len) {
            if (!has_file || cap < file.size())
                return int(SWOFTY_ASSET_INVALID);
            memcpy(buf, file.data(), file.size());
            *len = file.size();
            return int(SWOFTY_ASSET_OK);
        };
        st.write = [](const char *path, const void *d, size_t n) {
            assert(!strcmp(path, "welcome_seen.txt"));
            file.assign(static_cast<const char *>(d), n);
            has_file = true;
            return int(SWOFTY_ASSET_OK);
        };
        static SwoftyPopup pp{};
        pp.available = [] { return 1; };
        pp.show = [](const char *t, const char *b) {
            ++shows;
            title = t;
            body = b;
            return int(SWOFTY_ASSET_OK);
        };
        static int home_result = SWOFTY_ASSET_INVALID;  // the core has not seen the home state yet
        static uint32_t home_status = SWOFTY_FLOW_NONE;
        static uint64_t home_age = 0;
        static SwoftyCareer cr{};
        cr.available = [] { return 1; };
        cr.flow_status = [](uint32_t hash, uint32_t *status, uint64_t *age) {
            if (hash != 0x957880c6u)
                return int(SWOFTY_ASSET_INVALID);
            *status = home_status;
            *age = home_age;
            return home_result;
        };
        uint32_t version = api.version;
        api.version = 7;
        api.storage = &st;
        api.popup = &pp;
        api.career = &cr;
        welcome_rt = {};
        session_ended = false;
        player_vehicle = -1;
        welcome_tick(1000);  // the first frame
        assert(welcome_rt.first == 1000 && !welcome_rt.done && shows == 0);
        welcome_tick(30000);  // the legal and splash screens: no home state yet
        assert(shows == 0 && !welcome_rt.done);
        home_result = SWOFTY_ASSET_OK;  // the main menu since 9 s
        home_status = SWOFTY_FLOW_ENTERING;
        home_age = 9000;
        welcome_tick(31000);
        assert(shows == 0);
        home_age = 10000;
        welcome_tick(31200);  // twice a second only
        assert(shows == 0);
        player_vehicle = 3;  // in a session: it waits for the menus
        welcome_tick(32000);
        assert(shows == 0 && !welcome_rt.done);
        player_vehicle = -1;
        home_status = SWOFTY_FLOW_EXITING;  // left the main menu: waits for the next visit
        welcome_tick(33000);
        assert(shows == 0);
        home_status = SWOFTY_FLOW_UPDATING;
        welcome_tick(34000);
        assert(shows == 1 && welcome_rt.done && has_file && title == "TERMINAL DAMAGE");
        assert(body.find("ADDON SETTINGS > Terminal Damage > Preset") != std::string::npos &&
               body.find("Your preset now: Realistic.") != std::string::npos && body.size() < 400);
        welcome_tick(40000);
        assert(shows == 1);
        welcome_rt = {};  // the next game start: the storage remembers it
        welcome_tick(1000);
        welcome_tick(40000);
        assert(shows == 1 && welcome_rt.done);
        // A game build without the home state: 3 minutes after the first frame.
        has_file = false;
        home_result = SWOFTY_ASSET_INVALID;
        welcome_rt = {};
        welcome_tick(1000);
        welcome_tick(170000);
        assert(shows == 1 && !welcome_rt.done);
        welcome_tick(181000);
        assert(shows == 2 && welcome_rt.done && has_file);
        // No storage (an older core): no popup, as it could not be shown only once.
        has_file = false;
        st.available = [] { return 0; };
        home_result = SWOFTY_ASSET_OK;
        welcome_rt = {};
        welcome_tick(1000);
        welcome_tick(40000);
        assert(shows == 2 && welcome_rt.done && !has_file);
        api.version = version;
        api.storage = nullptr;
        api.popup = nullptr;
        api.career = nullptr;
    }

    // The preset's result (0.51.135): the open ADDON SETTINGS keep their rows and the launcher runs
    // no addon code, so the message says where the changes show. A popup in the menus (a toast is not drawn there):
    // at once for a pick in ADDON SETTINGS, on the main menu for a pick in the launcher; a toast in a session.
    {
        char text[200];
        preset_state r;
        r.in_game = true;
        r.set = 7;
        r.restart = true;
        preset_result_text(text, sizeof text, 1, r);
        assert(!strcmp(text, "Realistic: 7 option(s) changed. Reopen ADDON SETTINGS to see them. Some need a game "
                             "restart."));
        r.in_game = false;
        r.restart = false;
        preset_result_text(text, sizeof text, 2, r);
        assert(!strcmp(text, "Hardcore (picked in the launcher): 7 option(s) changed."));
        r.set = 0;
        preset_result_text(text, sizeof text, 0, r);
        assert(!strcmp(text, "Casual: every option already matches."));
        r.in_game = true;
        r.failed = 2;
        preset_result_text(text, sizeof text, 0, r);
        assert(!strcmp(text, "Casual: 0 option(s) changed. 2 could not be set: update Swofty, or set them by hand."));

        static std::string title, body;
        static int shows = 0;
        static SwoftyPopup pp{};
        pp.available = [] { return 1; };
        pp.show = [](const char *t, const char *b) {
            ++shows;
            title = t;
            body = b;
            return int(SWOFTY_ASSET_OK);
        };
        static SwoftyCareer cr{};
        cr.available = [] { return 1; };
        cr.flow_status = [](uint32_t, uint32_t *, uint64_t *) { return int(SWOFTY_ASSET_INVALID); };  // no home state
        uint32_t version = api.version;
        api.version = 7;
        api.popup = &pp;
        api.career = &cr;
        session_ended = false;
        player_vehicle = -1;
        preset_begin(1, "picked in the game");  // no core here: the option fails, the pass ends
        preset_apply_step(GetTickCount64());
        assert(preset_rt.in_game && preset_rt.applying == -1 && !preset_rt.popup.empty());
        preset_tick();
        assert(shows == 1 && title == "TERMINAL DAMAGE PRESET" && body.find("Realistic: ") == 0 &&
               preset_rt.popup.empty());
        // Picked in the launcher: on the main menu (this build has no home state: 3 minutes after the first frame).
        // 0.75: the defaults are Realistic, so Hardcore here (it differs in two options).
        preset_begin(2, "picked in the launcher");
        preset_apply_step(GetTickCount64());
        assert(!preset_rt.in_game && !preset_rt.popup.empty());
        preset_rt.first = GetTickCount64();
        preset_tick();
        assert(shows == 1 && !preset_rt.popup.empty());
        preset_rt.first = GetTickCount64() - 181000;
        preset_rt.popup_next = 0;
        preset_tick();
        assert(shows == 2 && body.find("Hardcore (picked in the launcher): ") == 0 && preset_rt.popup.empty());
        // In a session: a toast (no core here: nothing), no popup.
        player_vehicle = 3;
        preset_begin(1, "picked in the game");
        preset_apply_step(GetTickCount64());
        assert(preset_rt.popup.empty());
        preset_tick();
        assert(shows == 2);
        player_vehicle = -1;
        api.version = version;
        api.popup = nullptr;
        api.career = nullptr;
    }

    // Fixed-chance failures by laps: between 5 % and 95 % of the race, never past its last lap.
    {
        fm::RacePoint a = fm::race_point(0.0, 20), b = fm::race_point(1.0, 20), c = fm::race_point(0.5, 3);
        assert(a.lap == 1 && std::fabs(a.in_lap - 0.0) < 1e-9);  // 5 % of 20 laps: the start of lap 2
        assert(b.lap == 19 && std::fabs(b.in_lap - 0.0) < 1e-9);  // 95 %: the start of lap 20
        assert(c.lap == 1 && std::fabs(c.in_lap - 0.5) < 1e-9);  // half of 3 laps: the middle of lap 2
        assert(fm::race_point(0.5, 0).lap == -1);
        fm::RacePoint d = fm::race_point(1.0, 1);
        assert(d.lap == 0 && d.in_lap <= 1.0);
        for (int laps : {1, 3, 5, 10, 29, 57, 78})
            for (double u = 0; u <= 1.0; u += 0.01) {
                fm::RacePoint p = fm::race_point(u, laps);
                double at = (p.lap + p.in_lap) / laps;
                assert(p.lap >= 0 && p.lap < laps && at >= 0.05 - 1e-9 && at <= 0.95 + 1e-9);
            }
    }
    puts("manifest and presets ok");
    return 0;
}
