#include <algorithm>
#include <iterator>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"

namespace game {

namespace {

void Pad(int port, input::Pad& pad) {
    input::SeenPad(pad);
    if (port == 0) pad.connected = true;
    if (app::MenuOpen()) pad = input::Pad{.connected = pad.connected};
    else if (port == 0) input::Keyboard(pad);
}

void Rumble(int port, uint16_t& left, uint16_t& right) {
    if (port != 0 || !input::KeyboardActive()) return;
    left = 0;
    right = 0;
}

void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    Options& o = Opt();
    o.touch_controls = t["touch_controls"].value_or(o.touch_controls);
    o.auto_burnout = t["auto_burnout"].value_or(o.auto_burnout);
    o.mouse_sensitivity = float(t["mouse_sensitivity"].value_or(double(o.mouse_sensitivity)));
    o.touch_sensitivity = float(t["touch_sensitivity"].value_or(double(o.touch_sensitivity)));
    o.left_sensitivity = float(t["left_stick_sensitivity"].value_or(double(o.left_sensitivity)));
    o.right_sensitivity = float(t["right_stick_sensitivity"].value_or(double(o.right_sensitivity)));
    std::string language = t["language"].value_or(o.language);
    bool known = std::find(std::begin(kLanguages), std::end(kLanguages), language) != std::end(kLanguages);
    if (known) o.language = language;
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) o.keys[std::string(id.str())] = *v;
}

void Init() {
    BootLanguage();
    input::LoadBindings();
    audio::SetOutputMode(Opt().audio_output);
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Options& o = Opt();
    t.insert("touch_controls", o.touch_controls);
    t.insert("auto_burnout", o.auto_burnout);
    t.insert("mouse_sensitivity", double(o.mouse_sensitivity));
    t.insert("touch_sensitivity", double(o.touch_sensitivity));
    t.insert("left_stick_sensitivity", double(o.left_sensitivity));
    t.insert("right_stick_sensitivity", double(o.right_sensitivity));
    t.insert("language", o.language);
    toml::table keys;
    for (const auto& [id, value] : o.keys) keys.insert(id, value);
    t.insert("keys", keys);
}

}  // namespace

Options& Opt() {
    static Options o;
    return o;
}

const std::string& BootLanguage() {
    static const std::string language = Opt().language;
    return language;
}

bool HasLanguages() { return false; }
bool LanguageAvailable(const std::string& code) { return code == "int"; }
bool HudShown() { return true; }

}  // namespace game

const app::GameInfo app::kGame = {
    .title = L"Midnight Club 3: DUB Edition Remix",
    .short_title = L"MC3D Remix",
    .name = L"MC3D Remix",
    .log = "mc3dr",
    .settings = "mc3dr.toml",
    .splash = nullptr,
    .missing = L"Put the game in the mc3dr folder on the phone's storage (/storage/emulated/0/mc3dr or /sdcard/mc3dr): its disc image (.iso), or default.xbe with assets_x.dat beside it.",
    .images = {{"default.xbe", "MC3D Remix"}},
    .options = {{"fps_counter", &game::Opt().fps_counter, nullptr},
                {"display_logs", nullptr, &game::Opt().display_logs},
                {"tutorial_seen", nullptr, &game::Opt().tutorial_seen},
                {"bloom", &game::Opt().bloom, nullptr},
                {"motion_blur", nullptr, &game::Opt().motion_blur},
                {"reflections", nullptr, &game::Opt().reflections},
                {"anti_aliasing", nullptr, &game::Opt().anti_aliasing},
                {"volumetric", nullptr, &game::Opt().volumetric},
                {"screen_filter", nullptr, &game::Opt().screen_filter},
                {"left_stick_deadzone", nullptr, &game::Opt().left_deadzone},
                {"right_stick_deadzone", nullptr, &game::Opt().right_deadzone},
                {"dynamic_resolution", &game::Opt().dynamic_resolution, nullptr},
                {"dynamic_target_fps", nullptr, &game::Opt().dynamic_target_fps},
                {"dynamic_min_percent", nullptr, &game::Opt().dynamic_min_percent},
                {"dynamic_max_percent", nullptr, &game::Opt().dynamic_max_percent},
                {"dynamic_hold", nullptr, &game::Opt().dynamic_hold},
                {"audio_output", nullptr, &game::Opt().audio_output}},
    .load = game::Load,
    .save = game::Save,
    .init = game::Init,
    .loaded = game::Loaded,
    .ui = app::DrawUi,
    .pad = game::Pad,
    .rumble = game::Rumble,
};
