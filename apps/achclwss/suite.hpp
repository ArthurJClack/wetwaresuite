// Registers every built tool's commands into one session.
#pragma once

#include <set>
#include <string>

#include "achclwss/core/command.hpp"

namespace achclwss::suite {

// `only` limits it to some tools by name (e.g. {"photo", "convert"}); empty = all.
void registerAll(Session& session, const std::set<std::string>& only = {});

}  // namespace achclwss::suite
