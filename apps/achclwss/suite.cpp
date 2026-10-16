#include "suite.hpp"

#include "achclwss/audio/commands.hpp"
#include "achclwss/blueprint/blueprint.hpp"
#include "achclwss/cad/cad.hpp"
#include "achclwss/convert/convert.hpp"
#include "achclwss/forge/forge.hpp"
#include "achclwss/paint/paint.hpp"
#include "achclwss/photo/photo.hpp"
#include "achclwss/slicer/slicer.hpp"
#include "achclwss/toolkit/detect.hpp"
#include "achclwss/vector/vector.hpp"

namespace achclwss::suite {

const std::set<std::string>& toolNames() {
    static const std::set<std::string> names = {
        "toolkit",
        "photo",
        "paint",
        "vector",
        "blueprint",
        "convert",
        "forge",
        "slicer",
        "cad",
        "audio",
    };
    return names;
}

void registerAll(Session& session, const std::set<std::string>& only) {
    std::set<std::string> tools = only;
    if (tools.count("paint")) tools.insert("photo");
    if (tools.count("convert")) tools.insert("vector");
    if (tools.count("forge")) tools.insert("photo");
    auto want = [&](const char* tool) { return only.empty() || tools.count(tool) > 0; };
    if (want("toolkit")) toolkit::registerCommands(session.registry());
    if (want("photo")) photo::registerCommands(session);
    if (want("paint")) paint::registerCommands(session);
    if (want("vector")) vector::registerCommands(session);
    if (want("blueprint")) blueprint::registerCommands(session);
    if (want("convert")) convert::registerCommands(session);
    if (want("forge")) forge::registerCommands(session);
    if (want("slicer")) slicer::registerCommands(session);
    if (want("cad")) cad::registerCommands(session);
    if (want("audio")) audio::registerCommands(session);
}

}  // namespace achclwss::suite
