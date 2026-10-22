#include "wetwaresuite/forge/forge.hpp"

#include "common.hpp"

namespace wetwaresuite::forge {

void registerCommands(Session& session) {
    detail::registerMeshCommands(session);
    detail::registerImageCommands(session);
    detail::registerSpriteCommands(session);
    detail::registerAnimFileCommands(session);
    detail::registerAnimFrameCommands(session);
    detail::registerAnimDrawCommands(session);
    detail::registerAnimPaletteCommands(session);
}

}  // namespace wetwaresuite::forge
