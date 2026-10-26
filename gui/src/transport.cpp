// The play bar over sound documents (clips, audio projects, songs, video
// timelines): Play/Pause, Stop, a seek bar, loop and volume, plus Space to play
// and pause and a playhead on the waveform. Everything goes through the
// <tool>.play / pause / seek ... commands, so the window does exactly what a
// script or Claude would.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

std::string clock(double seconds) {
    seconds = std::max(0.0, seconds);
    const int m = static_cast<int>(seconds / 60);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%04.1f", m, seconds - m * 60);
    return buf;
}

double numberField(const Result& r, const char* key) {
    if (const Value* v = r.field(key)) {
        if (auto d = std::get_if<double>(v)) return *d;
        if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    }
    return 0;
}

bool boolField(const Result& r, const char* key) {
    if (const Value* v = r.field(key)) {
        if (auto b = std::get_if<bool>(v)) return *b;
    }
    return false;
}

std::string stringField(const Result& r, const char* key) {
    if (const Value* v = r.field(key)) {
        if (auto s = std::get_if<std::string>(v)) return *s;
    }
    return {};
}

}  // namespace

bool Shell::playTarget(std::string& tool, Params& target) const {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return false;
    const std::string kind = doc->kind();
    if (kind == "audio") tool = "audio", target = {{"doc", currentDoc_}};
    else if (kind == "audio project") tool = "audio", target = {{"project", currentDoc_}};
    else if (kind == "song") tool = "beats", target = {{"song", currentDoc_}};
    else if (kind == "timeline") tool = "cut", target = {{"doc", currentDoc_}};
    else return false;
    return find(tool + ".play") != nullptr;
}

void Shell::pollTransport() {
    Transport& t = transport_;
    if (t.tool.empty() || !find(t.tool + ".playhead")) return;
    const Result r = session_.run(t.tool + ".playhead", {});
    if (!r.ok) return;
    t.playing = boolField(r, "playing");
    t.paused = boolField(r, "paused");
    t.position = numberField(r, "position");
    t.length = numberField(r, "length");
    t.start = numberField(r, "start");
    t.end = numberField(r, "end");
    t.source = stringField(r, "source");
    t.silent = boolField(r, "silent");
}

void Shell::startPlaying(double from) {
    std::string tool;
    Params params;
    if (!playTarget(tool, params)) return;
    transport_.tool = tool;
    params["start"] = std::max(0.0, from);
    params["loop"] = transport_.loop;
    params["volume"] = static_cast<double>(transport_.volumeDb);
    Result r = session_.run(tool + ".play", params);
    if (!r.ok) {
        resultFor_ = "Play";
        r.message = friendlyError(r.message);
        result_ = std::move(r);
        scrollToResult_ = true;
        return;
    }
    transport_.doc = currentDoc_;
    pollTransport();
}

void Shell::togglePlay() {
    Transport& t = transport_;
    const bool ours = t.doc == currentDoc_ && t.source == currentDoc_;
    // a timeline has its own playhead: play starts there, and pausing leaves it where the picture stopped
    TimelineLayout layout;
    auto doc = session_.workspace().get(currentDoc_);
    const bool ownPlayhead = doc && doc->timeline(layout) && layout.cursor >= 0;
    if (ours && t.playing) {
        session_.run(t.tool + ".pause", {});
        pollTransport();
        if (ownPlayhead) runTimelineEdit(layout, {TimelineEditKind::Cursor, {}, {}, {}, 0, 0, t.position});
    } else if (ours && t.paused) {
        session_.run(t.tool + ".resume", {});
    } else {
        // start from where the playhead was left, unless it ran to the end
        const bool atEnd = !ours || t.length <= 0 || t.position >= t.end - 0.01;
        double from = atEnd ? t.cue : t.position;
        if (atEnd && ownPlayhead) {
            double end = 0;
            for (const auto& tr : layout.tracks)
                for (const auto& c : tr.clips) end = std::max(end, c.start + c.length);
            from = layout.cursor < end - 0.05 ? layout.cursor : 0.0;  // from the top once it reached the end
        }
        startPlaying(from);
    }
    pollTransport();
}

void Shell::seekTo(double seconds) {
    Transport& t = transport_;
    t.cue = std::max(0.0, seconds);
    if (t.doc == currentDoc_ && (t.playing || t.paused)) {
        session_.run(t.tool + ".seek", {{"seconds", t.cue}});
        pollTransport();
    }
}

void Shell::transportBar() {
    std::string tool;
    Params target;
    if (!playTarget(tool, target)) {
        playheadFraction_ = -1;
        return;
    }
    Transport& t = transport_;
    if (t.tool.empty()) t.tool = tool;
    pollTransport();
    const bool ours = t.doc == currentDoc_ && t.source == currentDoc_;
    const bool playing = ours && t.playing;

    // Space plays and pauses, unless someone is typing
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay();

    if (accentButton(playing ? "Pause" : (ours && t.paused) ? "Resume" : "Play", {ImGui::GetFontSize() * 5, 0})) togglePlay();
    ImGui::SetItemTooltip("Play or pause (Space)");
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        session_.run(tool + ".stop", {});
        t.cue = 0;
        pollTransport();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &t.loop) && ours) session_.run(tool + ".loop", {{"on", t.loop}});
    ImGui::SetItemTooltip("Play the sound over and over");
    ImGui::SameLine();

    // a timeline keeps its own playhead: the bar shows and moves that one while stopped
    auto doc = session_.workspace().get(currentDoc_);
    TimelineLayout layout;
    const bool ownPlayhead = doc && doc->timeline(layout) && layout.cursor >= 0;
    const bool running = ours && (t.playing || t.paused);
    double length = ours && t.length > 0 ? t.length : 0;
    if (ownPlayhead && length <= 0) {
        for (const auto& tr : layout.tracks)
            for (const auto& c : tr.clips) length = std::max(length, c.start + c.length);
    }
    const double shown = running ? t.position : ownPlayhead ? layout.cursor : t.cue;
    ImGui::TextUnformatted((clock(shown) + (length > 0 ? " / " + clock(length) : std::string())).c_str());
    ImGui::SameLine();
    const float volumeWidth = ImGui::GetFontSize() * 7;
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6, ImGui::GetContentRegionAvail().x - volumeWidth - ImGui::GetStyle().ItemSpacing.x));
    float pos = static_cast<float>(shown);
    const float maxPos = static_cast<float>(length > 0 ? length : std::max(1.0, shown));
    if (ImGui::SliderFloat("##playhead", &pos, 0.0f, maxPos, "", ImGuiSliderFlags_NoInput)) {
        if (ownPlayhead && !running) runTimelineEdit(layout, {TimelineEditKind::Cursor, {}, {}, {}, 0, 0, pos});
        else seekTo(pos);
    }
    ImGui::SetItemTooltip("Drag to move the playhead");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(volumeWidth);
    if (ImGui::SliderFloat("##volume", &t.volumeDb, -40.0f, 6.0f, "vol %.0f dB")) session_.run(tool + ".volume", {{"db", static_cast<double>(t.volumeDb)}});
    ImGui::SetItemTooltip("Playback volume (doesn't change the file)");
    if (ours && t.silent) ImGui::TextDisabled("No sound device found, so it plays silently.");

    // the playhead line only lines up with a clip's waveform
    playheadFraction_ = doc && doc->kind() == "audio" && length > 0 ? static_cast<float>(shown / length) : -1;
}

}  // namespace wetwaresuite::gui
