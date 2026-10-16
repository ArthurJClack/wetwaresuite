// Open audio documents for the session workspace: a single clip (AudioDoc,
// ids aud1, aud2...) and a multitrack project (ProjectDoc, ids proj1...), with
// snapshot undo and the param specs commands share. Mirrors raster's ImageDoc
// and mesh's MeshDoc.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "achclwss/audio/buffer.hpp"
#include "achclwss/audio/dsp.hpp"
#include "achclwss/audio/project.hpp"
#include "achclwss/core/command.hpp"

namespace achclwss::audio {

class AudioDoc : public Document {
public:
    Buffer audio;
    NoiseProfile noise;  // learned by audio.noise_profile, used by audio.denoise

    std::string kind() const override { return "audio"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;  // waveform PNG
};

class ProjectDoc : public Document {
public:
    Project project;

    std::string kind() const override { return "audio project"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;  // track lanes PNG
};

std::shared_ptr<AudioDoc> newAudio(Buffer b);
std::shared_ptr<AudioDoc> openAudio(const std::filesystem::path& path);  // throws std::runtime_error

// Undoable edit: snapshots the document before `fn`, restores it on undo and when fn fails.
class AudioEdit : public Action {
public:
    AudioEdit(std::shared_ptr<AudioDoc> doc, std::function<Result(AudioDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<AudioDoc> doc_;
    std::function<Result(AudioDoc&)> fn_;
    Buffer before_;
    NoiseProfile beforeNoise_;
};

class ProjectEdit : public Action {
public:
    ProjectEdit(std::shared_ptr<ProjectDoc> doc, std::function<Result(ProjectDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<ProjectDoc> doc_;
    std::function<Result(ProjectDoc&)> fn_;
    Project before_;
};

// Shared param specs.
ParamSpec docParam();       // "doc": an open audio document, e.g. aud1
ParamSpec projectParam();   // "project": an open project, e.g. proj1
ParamSpec previewParam();   // "preview": attach a waveform PNG
ParamSpec startParam();     // "start": seconds, default 0
ParamSpec endParam();       // "end": seconds, default the end
ParamSpec channelsParam();  // "channels": channel indices, 0 = left; default all

// Action factories; missing documents and exceptions become failed results.
ActionFactory audioCommand(Session& session, std::function<Result(AudioDoc&, const Params&)> edit, bool undoable = true);
ActionFactory projectCommand(Session& session, std::function<Result(ProjectDoc&, const Params&)> edit, bool undoable = true);

// Range from "start"/"end"; sets error when start is past the end or end <= start.
bool rangeArg(const Buffer& b, const Params& p, Range& out, std::string& error);
// Channels from "channels"; sets error when an index is out of range.
bool channelsArg(const Buffer& b, const Params& p, Channels& out, std::string& error);

// Success result with doc, seconds, rate and channels fields (+ preview when asked).
Result audioResult(const std::string& message, const AudioDoc& doc, const Params& p);
Result& maybePreview(Result& r, const AudioDoc& doc, const Params& p);
Result& maybePreview(Result& r, const ProjectDoc& doc, const Params& p);

}  // namespace achclwss::audio
