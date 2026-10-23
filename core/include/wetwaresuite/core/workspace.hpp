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

// Documents laid out in time (a video timeline, an audio project) describe
// their tracks and clips so the window can draw a timeline you drag clips on.
// Every change still goes through a command the document names.
struct TimelineClip {
    std::string id;     // what the edit commands call it
    std::string label;  // shown on the clip
    double start = 0;   // seconds on the timeline
    double length = 0;
    bool trimStart = true;  // its start can be dragged (false for clips with no source to reveal)
    double headroom = -1;   // seconds that can still be revealed before the start (-1 = no limit)
    double tailroom = -1;   // ... and after the end
    // editors with more to show (video timelines) fill these in
    std::vector<double> keyframes;  // timeline seconds of the clip's keyframes, drawn as diamonds
    double transitionIn = 0;        // seconds at its start that blend with the clip before
    std::string link;               // clips with the same non-empty link are selected together
};

// A switch at the head of a track (lock, mute, solo, hide). Clicking it runs the document's
// TimelineEditKind::Toggle command with `name`.
struct TimelineToggle {
    std::string name;     // e.g. "locked"
    std::string label;    // one or two letters on the button
    std::string tooltip;
    bool on = false;
};

struct TimelineMarker {
    std::string id;
    double t = 0;
    std::string name;
    std::string color;  // a name or #rrggbb
};

struct TimelineTrack {
    std::string id;    // what the edit commands call it
    std::string name;  // shown at the left
    std::string kind;  // clips only move between tracks of the same kind
    std::vector<TimelineClip> clips;
    std::vector<TimelineToggle> toggles;
};

struct TimelineLayout {
    std::string docParam = "doc";  // the param edit commands take the document id in
    std::vector<TimelineTrack> tracks;
    // editing tools beyond move/trim/split/delete: razor, ripple delete, markers, in/out, the
    // document's own playhead. Documents that set `tools` answer those edit kinds.
    bool tools = false;
    double cursor = -1;  // the document's playhead (-1: the window keeps its own)
    double inPoint = -1, outPoint = -1;
    std::vector<TimelineMarker> markers;
};

enum class TimelineEditKind { Move, TrimStart, TrimEnd, Split, Delete, Cursor, Razor, RippleDelete, Toggle, Marker, InPoint, OutPoint, Transition };

struct TimelineEdit {
    TimelineEditKind kind = TimelineEditKind::Move;
    std::string clip;   // clip id
    std::string track;  // its track id
    std::string toTrack;  // Move: the track it lands on (may be the same)
    double start = 0;   // Move/TrimStart: new start; TrimStart keeps the end
    double length = 0;  // TrimEnd: new length
    double at = 0;      // Split, Cursor, Razor, Marker, InPoint, OutPoint: timeline time
    std::string name;   // Toggle: which switch; Transition: its type
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
    bool italic = false, underline = false, strike = false;
    char align = 0;                          // 0 by type, 'l', 'c' or 'r'
    std::uint32_t color = 0, fill = 0;       // 0xRRGGBB plus 0x1000000 when set; 0 = default
    float bar = -1;                          // data bar length 0..1, or -1 for none
    std::uint32_t barColor = 0;
    int spanRows = 1, spanCols = 1;          // top-left of a merged area: its size
    bool covered = false;                    // inside a merged area, but not its top-left
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
    // 3D documents draw themselves from any angle into rgba (width * height * 4 bytes)
    // and return true; everything else returns false. A 0 x 0 call only asks
    // whether the document can (true for 3D documents), without drawing.
    virtual bool renderView(const OrbitView&, int, int, std::vector<std::uint8_t>&) const { return false; }
    // Timeline documents fill in their tracks and return true.
    virtual bool timeline(TimelineLayout&) const { return false; }
    // The command line that makes an edit, e.g. "cut.move_clip clip=c2 start=3.5 track=v1"
    // (the caller adds the document param); empty when the edit can't be done.
    virtual std::string timelineCommand(const TimelineEdit&) const { return {}; }
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
