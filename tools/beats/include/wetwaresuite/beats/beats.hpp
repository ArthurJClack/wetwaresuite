// The beats tool: headless music making. Songs (song1, song2...) hold a tempo,
// time signature and swing, tracks (a subtractive synth, a synthesized drum
// kit with sample slots, a pitched sampler), step-grid and piano-roll
// patterns, and an arrangement placing patterns by bar. Renders to WAV (mp3,
// ogg... via ffmpeg), reads and writes MIDI files, generates starting beats,
// and draws arrangement, piano-roll and waveform previews.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "wetwaresuite/beats/song.hpp"
#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::beats {

// An open song in the session workspace (song1, song2...).
class SongDoc : public Document {
public:
    Song song;
    std::string kind() const override { return "song"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;  // arrangement PNG
};

// Snapshots the song before `fn`; restores it on undo and when fn fails.
class SongEdit : public Action {
public:
    SongEdit(std::shared_ptr<SongDoc> doc, std::function<Result(SongDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override { doc_->song = before_; }

private:
    std::shared_ptr<SongDoc> doc_;
    std::function<Result(SongDoc&)> fn_;
    Song before_;
};

// Registers every beats.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::beats
