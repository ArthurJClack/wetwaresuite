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
};

struct TimelineTrack {
    std::string id;    // what the edit commands call it
    std::string name;  // shown at the left
    std::string kind;  // clips only move between tracks of the same kind
    std::vector<TimelineClip> clips;
};

struct TimelineLayout {
    std::string docParam = "doc";  // the param edit commands take the document id in
    std::vector<TimelineTrack> tracks;
};

enum class TimelineEditKind { Move, TrimStart, TrimEnd, Split, Delete };

struct TimelineEdit {
    TimelineEditKind kind = TimelineEditKind::Move;
    std::string clip;   // clip id
    std::string track;  // its track id
    std::string toTrack;  // Move: the track it lands on (may be the same)
    double start = 0;   // Move/TrimStart: new start; TrimStart keeps the end
    double length = 0;  // TrimEnd: new length
    double at = 0;      // Split: timeline time
};

class Document {
public:
    virtual ~Document() = default;
    virtual std::string kind() const = 0;     // "image", "vector", ...
    virtual std::string summary() const = 0;  // one line: size, layers, path
    virtual std::unique_ptr<Attachment> preview() const { return nullptr; }
    // Timeline documents fill in their tracks and return true.
    virtual bool timeline(TimelineLayout&) const { return false; }
    // The command line that makes an edit, e.g. "cut.move_clip clip=c2 start=3.5 track=v1"
    // (the caller adds the document param); empty when the edit can't be done.
    virtual std::string timelineCommand(const TimelineEdit&) const { return {}; }

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
