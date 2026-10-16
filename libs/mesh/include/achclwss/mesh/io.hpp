// Mesh files. STL (ascii/binary, auto-detected), OBJ (v/vt/vn/f, polygons
// triangulated, materials ignored), PLY (ascii and binary read, binary
// little-endian write), OFF, glTF 2.0 (.glb write; .glb/.gltf read with
// embedded, base64 or .bin buffers) and 3MF (zip of XML).
//
// glTF is Y-up, so it's rotated to and from this library's Z-up on the way
// through; the other formats are taken as they are. Everything else (names,
// materials, colours) is dropped: these are geometry exchanges.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "achclwss/mesh/mesh.hpp"

namespace achclwss::mesh {

// Lowercase extensions without the dot.
const std::vector<std::string>& readableExtensions();  // stl obj ply off glb gltf 3mf
const std::vector<std::string>& writableExtensions();  // stl obj ply off glb gltf 3mf

bool canRead(const std::filesystem::path& path);
bool canWrite(const std::filesystem::path& path);

struct SaveOptions {
    bool ascii = false;  // STL and PLY: write text instead of binary
};

// Throw std::runtime_error with a readable reason.
Mesh load(const std::filesystem::path& path);
void save(const std::filesystem::path& path, const Mesh& mesh, const SaveOptions& options = {});

// In-memory forms. `ext` is a format name such as "stl". For .gltf files that
// reference external buffers, `baseDir` is where to look for them.
Mesh decode(const std::string& bytes, const std::string& ext, const std::filesystem::path& baseDir = {});
std::string encode(const Mesh& mesh, const std::string& ext, const SaveOptions& options = {});

Mesh readStl(const std::string& bytes);
std::string writeStl(const Mesh& mesh, bool ascii = false);
Mesh readObj(const std::string& text);
std::string writeObj(const Mesh& mesh);
Mesh readPly(const std::string& bytes);
std::string writePly(const Mesh& mesh, bool ascii = false);
Mesh readOff(const std::string& text);
std::string writeOff(const Mesh& mesh);
Mesh readGltf(const std::string& bytes, const std::filesystem::path& baseDir = {});  // .glb or .gltf
std::string writeGlb(const Mesh& mesh);
std::string writeGltf(const Mesh& mesh);  // JSON with the buffer embedded as base64
Mesh read3mf(const std::string& bytes);
std::string write3mf(const Mesh& mesh);

}  // namespace achclwss::mesh
