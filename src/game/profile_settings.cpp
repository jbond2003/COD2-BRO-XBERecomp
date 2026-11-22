// Settings of their own for each of the game's profiles.
//
// The port keeps its settings (the graphics, the mouse, the key bindings, the gameplay switches) in
// one file beside the exe, which every profile shared. Now the file belongs to the profile that is signed in (its
// name is the file's "profile" key), and each other profile's settings wait in profile_settings\<name>.toml:
//  - signing in to another profile puts the file away under its owner's name and brings that profile's own back;
//  - a profile with none (a new one) starts from the defaults, every setting and every key;
//  - a profile deleted in the Profile Manager (the game's own delete, 0x334960) loses its file, so a new profile
//    of the same name is new;
//  - a settings file from before this (no owner) goes to the first profile signed in, as it is.
// The game applies a profile through 0x332E20 (edi the profile, its name first; src/game/pad_layout.cpp), in the
// Profile Manager and wherever else it reads one: the settings change there, before the game takes the controller
// configuration and the look sensitivities from them.

#include <filesystem>
#include <string>
#include <system_error>

#include "app/app.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/texture_override.h"
#include "kernel/kernel.h"

namespace game {

namespace {

constexpr char kNobody[] = "*";  // the file's owner was deleted: the next profile signed in does not get it

std::filesystem::path Kept(const std::string& profile) {
    std::string file;
    for (char c : profile) {
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
                     c == '.' || c == '@' || c == '-';
        file += plain ? c : '_';
    }
    return app::ExeDir() / "profile_settings" / (file + ".toml");
}

std::filesystem::path Current() { return app::ExeDir() / app::kGame.settings; }

// every setting as a new profile has it: the settings panel's Defaults, the keys too
void Defaults() {
    app::Settings fresh;
    fresh.fps_cap = 60;
    fresh.vsync = false;
    fresh.join_ip = app::Get().join_ip;
    app::Get() = fresh;
    Opt() = Options{};
}

}  // namespace

// the game's thread: a profile is being applied
void ProfileSettings(const std::string& profile) {
    Options& o = Opt();
    if (profile.empty() || profile == o.settings_profile) return;
    if (o.settings_profile.empty()) {
        o.settings_profile = profile;
        app::Save();
        LOGR("profile settings: the settings file is now profile %s's", profile.c_str());
        return;
    }
    std::error_code ec;
    std::string owner = o.settings_profile;
    if (owner != kNobody) {
        app::Save();
        std::filesystem::create_directories(Kept(owner).parent_path(), ec);
        std::filesystem::copy_file(Current(), Kept(owner), std::filesystem::copy_options::overwrite_existing, ec);
    }
    const app::Settings before = app::Get();
    bool modern = o.modern_layout;
    Defaults();
    bool kept = std::filesystem::exists(Kept(profile), ec);
    if (kept) {
        std::filesystem::copy_file(Kept(profile), Current(), std::filesystem::copy_options::overwrite_existing, ec);
        app::Load();
    }
    app::Settings& s = app::Get();
    if ((s.fps_cap && s.fps_cap < 5) || s.fps_cap > 999) s.fps_cap = 60;
    // the window is the computer's, not the profile's: the game opens before anyone has signed in, and one profile's
    // display mode and resolution must not be what the next one starts with. They stay as they are
    s.display_mode = before.display_mode;
    s.width = before.width;
    s.height = before.height;
    o.settings_profile = profile;
    input::LoadBindings();
    app::Save();
    if (o.modern_layout != modern) gpu::texture_override::Changed();  // the Controller screen's picture
    LOGR("profile settings: profile %s signed in after %s, %s", profile.c_str(), owner.c_str(),
         kept ? "its own settings are back" : "a new profile: every setting at its default");
}

}  // namespace game

// The Profile Manager's delete, confirmed: the slot's name (esi) is about to be emptied (0x3349E0). Its settings go
// with it.
void ProfileDeleted(X86Context& ctx, uint8_t*) {
    constexpr uint32_t kLowest = 0x10000;
    if (ctx.esi < kLowest) return;  // no record for the slot
    const char* text = static_cast<const char*>(kernel::HostPtr(ctx.esi));
    std::string name(text, strnlen(text, 32));
    if (name.empty()) return;
    std::error_code ec;
    bool had = std::filesystem::remove(game::Kept(name), ec);
    game::Options& o = game::Opt();
    if (o.settings_profile == name) {
        o.settings_profile = game::kNobody;
        app::Save();
    }
    LOGR("profile settings: profile %s deleted%s", name.c_str(), had ? ", its kept settings with it" : "");
}
