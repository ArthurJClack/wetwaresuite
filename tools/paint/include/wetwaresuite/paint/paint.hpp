// paint: layers, pressure brushes, fills, shapes and gradients, all as commands.
#pragma once

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::paint {

void registerCommands(Session& session);

}  // namespace wetwaresuite::paint
