// app.palette_*: the three colours (primary, secondary, tertiary) every app's
// window uses. Saved in <data root>/palette.txt so all apps share one set;
// with no file each app keeps its own colours.
#pragma once

#include <filesystem>
#include <string>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::apps {

std::filesystem::path paletteFile();

// `self` is this binary's app id ("suite" for the all-in-one); it decides the default colours.
void registerPaletteCommands(Session& session, const std::string& self);

}  // namespace wetwaresuite::apps
