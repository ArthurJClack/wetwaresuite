// Registers every built tool's commands into one session.
#pragma once

#include <set>
#include <string>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::suite {

// Names of every tool this binary carries, e.g. "photo".
const std::set<std::string>& toolNames();

// `only` limits it to some tools by name (e.g. {"photo", "convert"}); empty = all.
// Tools that need another one bring it along: paint opens and saves through photo,
// convert renders SVG through vector.
void registerAll(Session& session, const std::set<std::string>& only = {});

}  // namespace wetwaresuite::suite
