// File type detection by content (magic bytes), falling back to the
// extension. Tells you what a file is and which suite tool opens it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::toolkit {

struct FileType {
    std::string format;    // e.g. "PNG image"
    std::string category;  // image, raw, vector, cad, 3d, texture, audio, video, document,
                           // spreadsheet, slides, archive, font, data, code, executable, binary
    std::string tool;      // the suite tool that opens it
    std::string mime;
};

// Detects from the first bytes of a file plus its name.
FileType detect(std::string_view head, const std::filesystem::path& name);

// Reads the start of the file at `path` and detects it. Throws std::runtime_error if unreadable.
FileType detectFile(const std::filesystem::path& path);

// Classic hex + ASCII dump of `length` bytes starting at `offset`.
std::string hexDump(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t length);

// Registers toolkit.inspect and toolkit.hex in the registry.
void registerCommands(CommandRegistry& registry);

}  // namespace wetwaresuite::toolkit
