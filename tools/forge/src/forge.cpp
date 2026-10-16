#include "wetwaresuite/forge/forge.hpp"

#include "common.hpp"

namespace wetwaresuite::forge {

void registerCommands(Session& session) {
    detail::registerMeshCommands(session);
    detail::registerImageCommands(session);
    detail::registerSpriteCommands(session);
}

}  // namespace wetwaresuite::forge
