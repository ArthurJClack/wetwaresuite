#include "suite.hpp"

#include "achclwss/convert/convert.hpp"
#include "achclwss/paint/paint.hpp"
#include "achclwss/photo/photo.hpp"
#include "achclwss/toolkit/detect.hpp"

namespace achclwss::suite {

void registerAll(Session& session, const std::set<std::string>& only) {
    auto want = [&](const char* tool) { return only.empty() || only.count(tool) > 0; };
    if (want("toolkit")) toolkit::registerCommands(session.registry());
    if (want("photo")) photo::registerCommands(session);
    if (want("paint")) paint::registerCommands(session);
    if (want("convert")) convert::registerCommands(session);
}

}  // namespace achclwss::suite
