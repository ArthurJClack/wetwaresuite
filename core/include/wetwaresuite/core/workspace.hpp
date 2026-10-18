// Open documents, addressed by short ids ("img1", "vec2") so commands from
// the CLI, batch scripts and MCP can work on the same document step by step.
#pragma once

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

// Spreadsheet-like documents let the window show an editable cell grid. Cells
// are read one at a time (only the visible ones are asked for), and typing into
// a cell becomes the command the document names.
struct GridCell {
    std::string text;   // as shown (formatted value)
    std::string input;  // what you'd type to recreate it ("=SUM(A1:A3)", "12.5")
    bool bold = false;
    bool number = false;  // right-aligned
    bool error = false;
};

struct CellGrid {
    std::string docParam = "doc";
    std::vector<std::string> sheets;
    int active = 0;
    int usedRows = 0, usedCols = 0;  // the used range, from A1
};

class Document {
public:
    virtual ~Document() = default;
    virtual std::string kind() const = 0;     // "image", "vector", ...
    virtual std::string summary() const = 0;  // one line: size, layers, path
    virtual std::unique_ptr<Attachment> preview() const { return nullptr; }
    // Grid documents fill in their sheets and return true.
    virtual bool grid(CellGrid&) const { return false; }
    // One cell (0-based); false when it's empty.
    virtual bool gridCell(int /*sheet*/, int /*row*/, int /*col*/, GridCell&) const { return false; }
    virtual double gridColumnChars(int /*sheet*/, int /*col*/) const { return 10; }
    // The command and its "key=value" args that type `input` into a cell (empty input clears it;
    // text with tabs or line breaks is a pasted block starting there); the caller adds the
    // document param. Empty when it can't be done.
    virtual std::vector<std::string> gridSetCommand(int /*sheet*/, int /*row*/, int /*col*/, const std::string& /*input*/) const {
        return {};
    }

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
