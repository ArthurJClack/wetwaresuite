// Open documents, addressed by short ids ("img1", "vec2") so commands from
// the CLI, batch scripts and MCP can work on the same document step by step.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wetwaresuite {

// A file produced in memory: a preview image, a rendered export.
struct Attachment {
    std::string mime;   // e.g. "image/png"
    std::string bytes;  // raw bytes
    std::string name;   // suggested file name, may be empty
};

// A camera for documents you can turn around in 3D (meshes, scans, scenes):
// yaw around the vertical axis and pitch up/down in degrees (0, 0 = front;
// 45, 35.26 = iso), zoom (1 = fits), pan in model radii.
struct OrbitView {
    double yaw = 45;
    double pitch = 35.26;
    double zoom = 1;
    double panX = 0;
    double panY = 0;
    bool antialias = true;  // off while dragging, for speed
};

class Document {
public:
    virtual ~Document() = default;
    virtual std::string kind() const = 0;     // "image", "vector", ...
    virtual std::string summary() const = 0;  // one line: size, layers, path
    virtual std::unique_ptr<Attachment> preview() const { return nullptr; }
    // 3D documents draw themselves from any angle into rgba (width * height * 4 bytes)
    // and return true; everything else returns false. A 0 x 0 call only asks
    // whether the document can (true for 3D documents), without drawing.
    virtual bool renderView(const OrbitView&, int, int, std::vector<std::uint8_t>&) const { return false; }

    std::string path;  // where it was opened from or last saved to, may be empty
};

class Workspace {
public:
    // Stores the document and returns its new id, e.g. prefix "img" -> "img1".
    std::string add(std::shared_ptr<Document> doc, const std::string& prefix);
    // Puts a document back under a known id (used by undo of close).
    void put(const std::string& id, std::shared_ptr<Document> doc);
    std::shared_ptr<Document> get(const std::string& id) const;
    std::shared_ptr<Document> take(const std::string& id);  // removes and returns
    std::vector<std::string> ids() const;

    template <typename T>
    std::shared_ptr<T> getAs(const std::string& id) const {
        return std::dynamic_pointer_cast<T>(get(id));
    }

private:
    std::map<std::string, std::shared_ptr<Document>> docs_;
    std::map<std::string, int> counters_;
};

}  // namespace wetwaresuite
