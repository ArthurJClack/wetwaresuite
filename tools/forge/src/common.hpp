// Shared bits of forge's command files.
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "achclwss/core/command.hpp"

namespace achclwss::forge::detail {

namespace fs = std::filesystem;

inline std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

inline std::string joinList(const std::vector<std::string>& items, const std::string& sep = ", ") {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : sep) + i;
    return out;
}

// Creates the folder a file will be written into.
inline void makeParent(const fs::path& file) {
    if (auto parent = file.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
}

void registerMeshCommands(Session& session);
void registerImageCommands(Session& session);
void registerSpriteCommands(Session& session);

}  // namespace achclwss::forge::detail
