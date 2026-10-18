// app.view: any 3D document from any angle (headless and MCP side of the orbit view).
#pragma once

#include <string>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::apps {

void registerViewCommands(Session& session);
// yaw/pitch of a fixed view name (iso, front, back, left, right, top, bottom).
bool viewPreset(const std::string& name, double& yaw, double& pitch);

}  // namespace wetwaresuite::apps
