#include "palette_commands.hpp"

#include <system_error>

#include "apps.hpp"
#include "handoff.hpp"
#include "wetwaresuite/gui/palette.hpp"

namespace fs = std::filesystem;

namespace wetwaresuite::apps {

namespace {

std::uint32_t accentOf(const std::string& self) {
    const App* app = find(self);
    return app ? app->accent : App{}.accent;
}

// The colours in use now: the saved set, else this app's own.
gui::Palette current(const std::string& self, bool& custom) {
    auto saved = gui::loadPalette(paletteFile());
    custom = saved.has_value();
    return saved ? *saved : gui::defaultPalette(accentOf(self));
}

Result describe(const std::string& self, const std::string& lead) {
    bool custom = false;
    const gui::Palette p = current(self, custom);
    const gui::Theme th = gui::deriveTheme(p);
    std::string msg = lead.empty() ? std::string() : lead + "\n";
    msg += custom ? "every app uses these colours (saved in " + paletteFile().string() + "):\n"
                  : "no colours picked, so each app uses its own; this app's are:\n";
    msg += "  primary    " + gui::hex6(p.primary) + "  main colour: buttons, headings, highlights\n";
    msg += "  secondary  " + gui::hex6(p.secondary) + "  panels and boxes (" + (th.dark ? "a dark look" : "a light look") + ")\n";
    msg += "  tertiary   " + gui::hex6(p.tertiary) + "  details: links, borders, selection, focus";
    Result r = Result::success(msg, std::string(custom ? "custom" : "default"));
    r.with("primary", gui::hex6(p.primary)).with("secondary", gui::hex6(p.secondary)).with("tertiary", gui::hex6(p.tertiary));
    r.with("custom", custom).with("look", std::string(th.dark ? "dark" : "light")).with("text", gui::hex6(th.text));
    r.with("file", paletteFile().string());
    return r;
}

Result save(const std::string& self, const gui::Palette& p, const std::string& lead) {
    std::string error;
    if (!gui::savePalette(paletteFile(), p, error)) return Result::failure(error);
    return describe(self, lead + " Open windows change when you click back into them; new ones start with these.");
}

Result setColours(const std::string& self, const Params& params) {
    bool custom = false;
    gui::Palette p = current(self, custom);
    static const char* roles[3] = {"primary", "secondary", "tertiary"};
    gui::Rgba* slots[3] = {&p.primary, &p.secondary, &p.tertiary};
    int given = 0;
    for (int i = 0; i < 3; ++i) {
        const std::string text = stringParam(params, roles[i]);
        if (text.empty()) continue;
        std::string error;
        auto c = gui::parsePaletteColour(text, roles[i], error);
        if (!c) return Result::failure(error);
        *slots[i] = *c;
        ++given;
    }
    if (given == 0) return Result::failure("Give at least one colour: primary=#rrggbb, secondary=#rrggbb or tertiary=#rrggbb.");
    return save(self, p, "Colours saved.");
}

Result reset(const std::string& self) {
    std::error_code ec;
    const bool had = fs::exists(paletteFile(), ec);
    fs::remove(paletteFile(), ec);
    if (ec) return Result::failure("couldn't remove " + paletteFile().string() + ": " + ec.message());
    return describe(self, had ? "Back to each app's own colours." : "Already using each app's own colours.");
}

Result usePreset(const std::string& self, const std::string& name) {
    if (name == "default") return reset(self);
    const gui::PalettePreset* preset = gui::findPreset(name);
    if (!preset) {
        std::string names;
        for (const auto& n : gui::presetNames()) names += (names.empty() ? "" : ", ") + n;
        return Result::failure("There's no colour set called \"" + name + "\". Pick one of: " + names + ".");
    }
    return save(self, preset->palette, "Using the " + preset->title + " colours: " + preset->description);
}

std::string presetList() {
    std::string out;
    for (const auto& p : gui::palettePresets()) out += (out.empty() ? "" : "; ") + p.name + " (" + p.description + ")";
    return out;
}

}  // namespace

fs::path paletteFile() { return dataRoot() / "palette.txt"; }

void registerPaletteCommands(Session& session, const std::string& self) {
    auto& reg = session.registry();
    reg.add({"app.palette_get", "Show colours",
             "Show the three window colours every app uses: primary (buttons, headings, highlights), secondary (panels "
             "and boxes; a dark one gives a dark look) and tertiary (links, borders, selection, focus), whether they "
             "were picked (custom) or are this app's own, and the settings file.",
             {}, false, true},
            [self](const Params&) { return std::make_unique<FunctionAction>([self] { return describe(self, ""); }); });
    reg.add({"app.palette_set", "Set colours",
             "Pick the window colours for every wetwaresuite app at once. Give any of primary, secondary, tertiary as "
             "#rrggbb (or #rgb, or a plain name like blue); the others stay as they are. Text colour, hover and "
             "pressed shades are worked out so everything stays readable.",
             {{"primary", ParamType::String, "main colour: buttons, headings, highlights (#rrggbb)", false},
              {"secondary", ParamType::String, "panels and boxes, dark or light (#rrggbb)", false},
              {"tertiary", ParamType::String, "details: links, borders, selection, focus (#rrggbb)", false}},
             false},
            [self](const Params& p) { return std::make_unique<FunctionAction>([self, p] { return setColours(self, p); }); });
    reg.add({"app.palette_preset", "Use a colour set",
             "Use one of the built-in colour sets in every app: default (each app's own colours, same as "
             "app.palette_reset); " + presetList() + ".",
             {{"name", ParamType::String, "which set", true, gui::presetNames()}},
             false},
            [self](const Params& p) {
                return std::make_unique<FunctionAction>([self, p] { return usePreset(self, stringParam(p, "name")); });
            });
    reg.add({"app.palette_reset", "Reset colours",
             "Forget the picked colours so each app goes back to its own (removes the palette file).", {}, false},
            [self](const Params&) { return std::make_unique<FunctionAction>([self] { return reset(self); }); });
}

}  // namespace wetwaresuite::apps
