#include "achclwss/forge/forge.hpp"

#include "common.hpp"

namespace achclwss::forge {

void registerCommands(Session& session) {
    detail::registerMeshCommands(session);
    detail::registerImageCommands(session);
    detail::registerSpriteCommands(session);
}

}  // namespace achclwss::forge
