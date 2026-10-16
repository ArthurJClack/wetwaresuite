// Turns a timeline into one ffmpeg run: a single filter_complex graph that
// composites every track over a background, chains transitions with xfade,
// draws titles and mixes audio; or a concat-demuxer stream copy when the
// timeline is only cuts of identical sources.
//
// Video layout: each track is a stack of "chains" (clips joined by
// transitions). A chain becomes one stream whose timestamps start at the
// chain's position on the timeline, overlaid on everything below it.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

#include "wetwaresuite/cut/cut.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

std::string escapeOption(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\\' || c == '\'' || c == ':') out += '\\';
        out += c;
    }
    return out;
}

std::string escapeGraph(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\\' || c == '\'' || c == '[' || c == ']' || c == ',' || c == ';') out += '\\';
        out += c;
    }
    return out;
}

std::string filter(const std::string& name, const std::vector<std::pair<std::string, std::string>>& options) {
    std::string args;
    for (const auto& [k, v] : options) {
        if (!args.empty()) args += ':';
        if (!k.empty()) args += k + "=";
        args += escapeOption(v);
    }
    return args.empty() ? name : name + "=" + escapeGraph(args);
}

namespace {

constexpr double kEps = 1e-6;

std::string t(double v) {
    if (std::abs(v) < 5e-7) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extOf(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower(e);
}

int even(double v) {
    int i = static_cast<int>(std::lround(v));
    if (i % 2) ++i;
    return std::max(2, i);
}

bool isVideoExt(const std::string& e) { return e == "mp4" || e == "mov" || e == "mkv" || e == "m4v" || e == "webm" || e == "gif"; }
bool isAudioExt(const std::string& e) {
    return e == "mp3" || e == "wav" || e == "m4a" || e == "aac" || e == "flac" || e == "ogg" || e == "opus";
}
bool isStillExt(const std::string& e) { return e == "png" || e == "jpg" || e == "jpeg"; }

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
};

Box boxOf(const Settings& s, const Clip& c) {
    if (c.width <= 0 && c.height <= 0) return {0, 0, s.width, s.height};
    double sw = c.cropW > 0 ? c.cropW : c.sourceWidth, sh = c.cropW > 0 ? c.cropH : c.sourceHeight;
    if (c.kind == "title" || sw <= 0 || sh <= 0) {
        sw = s.width;
        sh = s.height;
    }
    double w = c.width, h = c.height;
    if (w <= 0) w = h * sw / sh;
    if (h <= 0) h = w * sh / sw;
    return {static_cast<int>(std::lround(c.x)), static_cast<int>(std::lround(c.y)), even(w), even(h)};
}

// The overlap with the previous clip when this clip's transition is active, else 0.
double activeOverlap(const Clip* prev, const Clip& c) {
    if (!prev || c.transition.empty() || xfadeName(c.transition).empty()) return 0;
    const double overlap = prev->end() - c.start;
    if (overlap <= 1e-3 || overlap > std::min(prev->length(), c.length()) + kEps) return 0;
    return overlap;
}

// Media clips of a track grouped into chains joined by active transitions.
std::vector<std::vector<const Clip*>> chainsOf(const Track& track) {
    std::vector<std::vector<const Clip*>> chains;
    for (const auto& c : track.clips) {
        if (!c.media()) continue;
        const Clip* prev = chains.empty() ? nullptr : chains.back().back();
        if (prev && activeOverlap(prev, c) > 0) chains.back().push_back(&c);
        else chains.push_back({&c});
    }
    return chains;
}

struct Quality {
    const char* x264preset;
    int crf;
    int vp9crf;
    const char* vp9deadline;
    int vp9cpu;
    const char* audioRate;
    const char* mp3q;
};

Quality qualityOf(const std::string& q) {
    if (q == "draft") return {"ultrafast", 28, 42, "realtime", 8, "128k", "6"};
    if (q == "best") return {"slow", 18, 24, "good", 2, "256k", "0"};
    return {"veryfast", 22, 33, "good", 5, "192k", "2"};
}

class Builder {
public:
    Builder(const Timeline& tl, const RenderRequest& rq, bool still) : tl_(tl), rq_(rq), still_(still) {
        fps_ = t(tl.settings.fps);
        at_ = rq.at;
    }

    std::vector<std::string> inputs;
    std::vector<std::string> stmts;

    std::string label(const char* prefix) { return std::string(prefix) + std::to_string(labels_++); }

    int input(const Clip& c, double skip, double needed) {
        auto it = inputOf_.find(c.id);
        if (it != inputOf_.end()) return it->second;
        if (c.kind == "image") {
            inputs.insert(inputs.end(), {"-loop", "1", "-framerate", fps_, "-t", t(needed), "-i", c.source});
        } else {
            if (c.in + skip * c.speed > kEps) inputs.insert(inputs.end(), {"-ss", t(c.in + skip * c.speed)});
            inputs.insert(inputs.end(), {"-t", t(needed * c.speed), "-i", c.source});
        }
        inputOf_[c.id] = count_;
        return count_++;
    }

    // One clip's picture, scaled into its box, with colour, opacity and fades. Local time starts at `skip`.
    std::string clipVideo(const Clip& c, const Box& box, double skip, double needed) {
        const int i = input(c, skip, needed);
        std::vector<std::string> f;
        std::string pts = "PTS-STARTPTS";
        if (std::abs(c.speed - 1) > kEps && c.kind == "video") pts = "(PTS-STARTPTS)/" + t(c.speed);
        if (skip > kEps) pts += "+" + t(skip) + "/TB";
        f.push_back(filter("setpts", {{"", pts}}));
        f.push_back(filter("fps", {{"fps", fps_}}));
        if (c.cropW > 0 && c.cropH > 0) {
            f.push_back(filter("crop", {{"w", t(c.cropW)}, {"h", t(c.cropH)}, {"x", t(c.cropX)}, {"y", t(c.cropY)}}));
        }
        if (std::abs(c.brightness) > kEps || std::abs(c.contrast - 1) > kEps || std::abs(c.saturation - 1) > kEps ||
            std::abs(c.gamma - 1) > kEps) {
            f.push_back(filter("eq", {{"brightness", t(c.brightness)},
                                      {"contrast", t(c.contrast)},
                                      {"saturation", t(c.saturation)},
                                      {"gamma", t(c.gamma)}}));
        }
        if (c.grayscale) f.push_back(filter("hue", {{"s", "0"}}));
        if (!c.lut.empty()) f.push_back(filter("lut3d", {{"file", fs::path(c.lut).generic_string()}}));
        const std::string w = std::to_string(box.w), h = std::to_string(box.h);
        if (c.fit == "stretch") {
            f.push_back(filter("scale", {{"w", w}, {"h", h}}));
        } else if (c.fit == "fill") {
            f.push_back(filter("scale", {{"w", w}, {"h", h}, {"force_original_aspect_ratio", "increase"}}));
            f.push_back(filter("crop", {{"w", w}, {"h", h}}));
        } else {
            f.push_back(filter("scale", {{"w", w}, {"h", h}, {"force_original_aspect_ratio", "decrease"}, {"force_divisible_by", "2"}}));
        }
        f.push_back(filter("setsar", {{"", "1"}}));
        f.push_back(filter("format", {{"", "yuva420p"}}));
        if (c.fit != "stretch" && c.fit != "fill") {
            f.push_back(filter("pad", {{"w", w}, {"h", h}, {"x", "(ow-iw)/2"}, {"y", "(oh-ih)/2"}, {"color", "black@0"}}));
        }
        if (c.opacity < 1 - kEps) f.push_back(filter("lut", {{"a", "val*" + t(std::max(0.0, c.opacity))}}));
        if (c.fadeIn > kEps) f.push_back(filter("fade", {{"t", "in"}, {"st", "0"}, {"d", t(c.fadeIn)}, {"alpha", "1"}}));
        if (c.fadeOut > kEps) {
            f.push_back(filter("fade", {{"t", "out"}, {"st", t(std::max(0.0, c.length() - c.fadeOut))}, {"d", t(c.fadeOut)}, {"alpha", "1"}}));
        }
        std::string out = label("v");
        stmts.push_back("[" + std::to_string(i) + ":v]" + join(f) + "[" + out + "]");
        return out;
    }

    static std::string join(const std::vector<std::string>& f) {
        std::string s;
        for (const auto& x : f) s += (s.empty() ? "" : ",") + x;
        return s;
    }

    // Composites one video track over `cur`; returns the new current label.
    std::string videoTrack(const Track& track, std::string cur) {
        for (const auto& chain : chainsOf(track)) {
            const double cs = chain.front()->start, ce = chain.back()->end();
            std::size_t first = 0, last = chain.size() - 1;
            double skip = 0;
            if (still_) {
                if (at_ < cs - kEps || at_ >= ce - kEps) continue;
                std::size_t k = 0;
                for (std::size_t i = 0; i < chain.size(); ++i) {
                    if (chain[i]->start <= at_ + kEps) k = i;
                }
                double cut = at_;
                first = k;
                if (k > 0 && at_ < chain[k - 1]->end() - kEps) {
                    first = k - 1;
                    cut = chain[k]->start;
                }
                last = k;
                skip = std::max(0.0, cut - chain[first]->start);
            }
            const Box box = boxOf(tl_.settings, *chain.front());
            const double base = chain[first]->start + skip;
            std::string s;
            for (std::size_t i = first; i <= last; ++i) {
                const Clip& c = *chain[i];
                const double sk = i == first ? skip : 0;
                double needed = c.length() - sk;
                if (still_) needed = std::min(needed, (at_ - (c.start + sk)) + 3.0 / tl_.settings.fps + 0.1);
                needed = std::max(needed, 1.0 / tl_.settings.fps);
                std::string v = clipVideo(c, box, sk, needed);
                if (s.empty()) {
                    s = v;
                    continue;
                }
                const double d = chain[i - 1]->end() - c.start;
                std::string x = label("x");
                stmts.push_back("[" + s + "][" + v + "]" +
                                filter("xfade", {{"transition", xfadeName(c.transition)},
                                                 {"duration", t(d)},
                                                 {"offset", t(std::max(0.0, c.start - base))}}) +
                                "[" + x + "]");
                s = x;
            }
            std::string placed = label("p");
            stmts.push_back("[" + s + "]" + filter("setpts", {{"", "PTS+" + t(chain[first]->start) + "/TB"}}) + "[" + placed + "]");
            std::string next = label("o");
            stmts.push_back("[" + cur + "][" + placed + "]" +
                            filter("overlay", {{"x", std::to_string(box.x)},
                                               {"y", std::to_string(box.y)},
                                               {"eof_action", "pass"},
                                               {"enable", "gte(t," + t(cs) + ")*lt(t," + t(ce) + ")"}}) +
                            "[" + next + "]");
            cur = next;
        }
        for (const auto& c : track.clips) {
            if (c.kind != "title" || c.text.empty()) continue;
            if (still_ && (at_ < c.start - kEps || at_ >= c.end() - kEps)) continue;
            std::string next = label("t");
            stmts.push_back("[" + cur + "]" + drawtext(c) + "[" + next + "]");
            cur = next;
        }
        return cur;
    }

    std::string drawtext(const Clip& c) const {
        std::vector<std::pair<std::string, std::string>> o;
        std::error_code ec;
        if (!c.font.empty() && fs::is_regular_file(c.font, ec)) o.push_back({"fontfile", fs::path(c.font).generic_string()});
        else if (!c.font.empty()) o.push_back({"font", c.font});
        else if (!rq_.fontFile.empty()) o.push_back({"fontfile", fs::path(rq_.fontFile).generic_string()});
        o.push_back({"text", c.text});
        o.push_back({"expansion", "none"});
        o.push_back({"fontsize", t(c.fontSize)});
        o.push_back({"fontcolor", c.color});
        std::string x = "(w-text_w)/2", y = "(h-text_h)/2";
        const std::string& p = c.position;
        if (p == "top") y = "h*0.08";
        else if (p == "bottom") y = "h*0.92-text_h";
        else if (p == "lower-third") {
            x = "w*0.06";
            y = "h*0.72";
        } else if (p == "top-left") {
            x = "w*0.05";
            y = "h*0.06";
        } else if (p == "top-right") {
            x = "w*0.95-text_w";
            y = "h*0.06";
        } else if (p == "bottom-left") {
            x = "w*0.05";
            y = "h*0.94-text_h";
        } else if (p == "bottom-right") {
            x = "w*0.95-text_w";
            y = "h*0.94-text_h";
        } else if (p == "custom") {
            x = t(c.x);
            y = t(c.y);
        }
        o.push_back({"x", x});
        o.push_back({"y", y});
        if (c.box) {
            o.push_back({"box", "1"});
            o.push_back({"boxcolor", c.boxColor});
            o.push_back({"boxborderw", std::to_string(std::max(4, static_cast<int>(c.fontSize / 4)))});
        }
        const std::string s = t(c.start), e = t(c.end());
        o.push_back({"enable", "gte(t," + s + ")*lt(t," + e + ")"});
        if (c.fadeIn > kEps || c.fadeOut > kEps || c.opacity < 1 - kEps) {
            std::string a = "1";
            if (c.fadeOut > kEps) a = "if(gt(t," + t(c.end() - c.fadeOut) + "),(" + e + "-t)/" + t(c.fadeOut) + "," + a + ")";
            if (c.fadeIn > kEps) a = "if(lt(t," + t(c.start + c.fadeIn) + "),(t-" + s + ")/" + t(c.fadeIn) + "," + a + ")";
            if (c.opacity < 1 - kEps) a = t(c.opacity) + "*" + a;
            o.push_back({"alpha", "clip(" + a + ",0,1)"});
        }
        return filter("drawtext", o);
    }

    // Every sound, delayed to its place and mixed. Returns the label, or empty if there is no sound.
    std::string audio() {
        std::vector<std::string> parts;
        for (const auto& track : tl_.tracks) {
            if (track.muted) continue;
            for (std::size_t n = 0; n < track.clips.size(); ++n) {
                const Clip& c = track.clips[n];
                if (!c.sound() || c.mute || c.volume <= kEps) continue;
                // transitions crossfade the sound too
                double tin = 0, tout = 0;
                if (c.media()) {
                    tin = activeOverlap(tl_.previousMedia(track, c), c);
                    for (std::size_t m = n + 1; m < track.clips.size(); ++m) {
                        if (!track.clips[m].media()) continue;
                        tout = activeOverlap(&c, track.clips[m]);
                        break;
                    }
                }
                const int i = input(c, 0, c.length());
                std::vector<std::string> f;
                f.push_back(filter("asetpts", {{"", "PTS-STARTPTS"}}));
                double sp = c.speed;
                while (sp > 2 + kEps) {
                    f.push_back(filter("atempo", {{"", "2"}}));
                    sp /= 2;
                }
                while (sp < 0.5 - kEps) {
                    f.push_back(filter("atempo", {{"", "0.5"}}));
                    sp /= 0.5;
                }
                if (std::abs(sp - 1) > kEps) f.push_back(filter("atempo", {{"", t(sp)}}));
                if (std::abs(c.volume - 1) > kEps) f.push_back(filter("volume", {{"", t(c.volume)}}));
                const double fi = std::max(c.fadeIn, tin), fo = std::max(c.fadeOut, tout);
                if (fi > kEps) f.push_back(filter("afade", {{"t", "in"}, {"st", "0"}, {"d", t(fi)}}));
                if (fo > kEps) f.push_back(filter("afade", {{"t", "out"}, {"st", t(std::max(0.0, c.length() - fo))}, {"d", t(fo)}}));
                f.push_back(filter("aformat", {{"sample_fmts", "fltp"}, {"sample_rates", "48000"}, {"channel_layouts", "stereo"}}));
                const auto ms = static_cast<long long>(std::llround(c.start * 1000));
                if (ms > 0) f.push_back(filter("adelay", {{"delays", std::to_string(ms)}, {"all", "1"}}));
                std::string out = label("a");
                stmts.push_back("[" + std::to_string(i) + ":a]" + join(f) + "[" + out + "]");
                parts.push_back(out);
            }
        }
        if (parts.empty()) return {};
        std::string in;
        for (const auto& p : parts) in += "[" + p + "]";
        std::string mixed = label("m");
        if (parts.size() == 1) stmts.push_back(in + "anull[" + mixed + "]");
        else {
            stmts.push_back(in + filter("amix", {{"inputs", std::to_string(parts.size())},
                                                 {"duration", "longest"},
                                                 {"dropout_transition", "0"},
                                                 {"normalize", "0"}}) +
                            "[" + mixed + "]");
        }
        return mixed;
    }

    // The composited picture. Returns the label.
    std::string video(double duration) {
        const auto& s = tl_.settings;
        std::string base = label("base");
        std::string src = filter("color", {{"c", s.background},
                                           {"s", std::to_string(s.width) + "x" + std::to_string(s.height)},
                                           {"r", fps_},
                                           {"d", t(still_ ? 2.0 / s.fps : duration)}});
        if (still_ && at_ > kEps) src += "," + filter("setpts", {{"", "PTS+" + t(at_) + "/TB"}});
        stmts.push_back(src + "," + filter("format", {{"", "yuv420p"}}) + "[" + base + "]");
        std::string cur = base;
        for (const auto& track : tl_.tracks) {
            if (track.kind != "video" || track.hidden) continue;
            cur = videoTrack(track, cur);
        }
        return cur;
    }

    std::string graph() const {
        std::string g;
        for (const auto& s : stmts) g += (g.empty() ? "" : ";") + s;
        return g;
    }

private:
    const Timeline& tl_;
    const RenderRequest& rq_;
    bool still_;
    std::string fps_;
    double at_ = 0;
    int labels_ = 0;
    int count_ = 0;
    std::map<std::string, int> inputOf_;
};

std::vector<std::string> head() { return {"ffmpeg", "-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-y"}; }

}  // namespace

std::string streamCopyBlocker(const Timeline& tl, const std::string& outExtRaw) {
    const std::string outExt = lower(outExtRaw);
    const Track* only = nullptr;
    for (const auto& t : tl.tracks) {
        if (t.clips.empty()) continue;
        if (only) return "it has more than one track with clips";
        only = &t;
    }
    if (!only) return "the timeline is empty";
    if (only->kind != "video") return "the only track is audio";
    if (only->hidden || only->muted) return "the track is hidden or muted";
    const Clip& f = only->clips.front();
    const std::string srcExt = extOf(f.source);
    auto family = [](const std::string& e) { return (e == "mp4" || e == "mov" || e == "m4v") ? std::string("mp4") : e; };
    if (family(srcExt) != family(outExt)) return "the output format (." + outExt + ") differs from the source (." + srcExt + ")";
    double expected = 0;
    for (const auto& c : only->clips) {
        if (c.kind != "video") return "clip " + c.id + " is a " + c.kind + ", not a video";
        if (c.videoCodec != f.videoCodec || c.audioCodec != f.audioCodec || c.sourceWidth != f.sourceWidth ||
            c.sourceHeight != f.sourceHeight || std::abs(c.sourceFps - f.sourceFps) > 0.01 || c.hasAudio != f.hasAudio) {
            return "clip " + c.id + " comes from a source with different codecs, size or frame rate";
        }
        if (c.sourceWidth != tl.settings.width || c.sourceHeight != tl.settings.height) {
            return "the sources are " + std::to_string(c.sourceWidth) + "x" + std::to_string(c.sourceHeight) + ", not the timeline's " +
                   std::to_string(tl.settings.width) + "x" + std::to_string(tl.settings.height);
        }
        if (c.sourceFps > 0 && std::abs(c.sourceFps - tl.settings.fps) > 0.01) return "the sources' frame rate differs from the timeline's";
        if (std::abs(c.speed - 1) > kEps || std::abs(c.volume - 1) > kEps || c.mute != f.mute) return "clip " + c.id + " changes speed or sound";
        if (c.fadeIn > kEps || c.fadeOut > kEps || c.opacity < 1 - kEps) return "clip " + c.id + " has fades or opacity";
        if (c.width > 0 || c.height > 0 || c.cropW > 0 || c.x != 0 || c.y != 0) return "clip " + c.id + " is resized, moved or cropped";
        if (std::abs(c.brightness) > kEps || std::abs(c.contrast - 1) > kEps || std::abs(c.saturation - 1) > kEps ||
            std::abs(c.gamma - 1) > kEps || c.grayscale || !c.lut.empty()) {
            return "clip " + c.id + " has colour filters";
        }
        if (!c.transition.empty() && c.transition != "none") return "clip " + c.id + " has a transition";
        if (std::abs(c.start - expected) > 1.0 / std::max(1.0, tl.settings.fps)) return "there is a gap or overlap before clip " + c.id;
        expected = c.end();
    }
    return {};
}

RenderPlan planRender(const Timeline& tl, const RenderRequest& rq) {
    const std::string ext = extOf(rq.output);
    if (ext.empty()) throw std::runtime_error("the output needs an extension (.mp4, .webm, .mov, .mkv, .gif, .mp3, .wav, .m4a, .png)");
    const bool still = isStillExt(ext);
    if (!isVideoExt(ext) && !isAudioExt(ext) && !still) {
        throw std::runtime_error("can't render to ." + ext + "; use mp4, webm, mov, mkv, gif, mp3, wav, m4a, flac, ogg, opus, png or jpg");
    }
    if (rq.mode != "auto" && rq.mode != "copy" && rq.mode != "encode") throw std::runtime_error("mode is auto, copy or encode");
    const double total = tl.duration();
    if (total <= kEps) throw std::runtime_error("the timeline is empty; add clips first (cut.add_clip, cut.add_image, cut.add_title)");

    RenderPlan plan;
    plan.duration = total;
    const std::string output = rq.output;

    // stream copy: cuts of identical sources, no re-encoding
    if (!still && ext != "gif" && !isAudioExt(ext) && rq.mode != "encode" && rq.subtitles.empty()) {
        const std::string why = streamCopyBlocker(tl, ext);
        if (rq.mode == "copy" && !why.empty()) throw std::runtime_error("can't stream-copy: " + why + " (use mode=encode or auto)");
        if (why.empty()) {
            if (rq.listPath.empty()) throw std::runtime_error("stream copy needs a list file path");
            plan.streamCopy = true;
            const Track* track = nullptr;
            for (const auto& tr : tl.tracks) {
                if (!tr.clips.empty()) track = &tr;
            }
            for (const auto& c : track->clips) {
                std::string p = fs::path(c.source).generic_string(), quoted;
                for (char ch : p) {
                    if (ch == '\'') quoted += "'\\''";
                    else quoted += ch;
                }
                plan.concatList += "file '" + quoted + "'\ninpoint " + t(c.in) + "\noutpoint " + t(c.out) + "\n";
            }
            plan.args = head();
            plan.args.insert(plan.args.end(), {"-f", "concat", "-safe", "0", "-i", rq.listPath, "-map", "0:v:0"});
            if (track->clips.front().hasAudio && !track->clips.front().mute) plan.args.insert(plan.args.end(), {"-map", "0:a:0?"});
            plan.args.insert(plan.args.end(), {"-c", "copy"});
            if (ext == "mp4" || ext == "mov" || ext == "m4v") plan.args.insert(plan.args.end(), {"-movflags", "+faststart"});
            plan.args.push_back(output);
            return plan;
        }
    }
    if (rq.mode == "copy") throw std::runtime_error("can't stream-copy to ." + ext + " (use mode=encode or auto)");

    RenderRequest r = rq;
    if (still) r.at = std::clamp(rq.at, 0.0, std::max(0.0, total - 1.0 / tl.settings.fps));
    Builder b(tl, r, still);
    const Quality q = qualityOf(rq.quality);
    std::vector<std::string> maps;
    std::vector<std::string> codec;

    if (isAudioExt(ext)) {
        std::string a = b.audio();
        if (a.empty()) throw std::runtime_error("the timeline has no sound to export (every clip is silent or muted)");
        maps = {"-map", "[" + a + "]"};
        if (ext == "mp3") codec = {"-c:a", "libmp3lame", "-q:a", q.mp3q};
        else if (ext == "wav") codec = {"-c:a", "pcm_s16le"};
        else if (ext == "flac") codec = {"-c:a", "flac"};
        else if (ext == "ogg" || ext == "opus") codec = {"-c:a", "libopus", "-b:a", q.audioRate};
        else codec = {"-c:a", "aac", "-b:a", q.audioRate};
        codec.insert(codec.end(), {"-t", t(total)});
    } else {
        std::string v = b.video(total);
        std::vector<std::string> tail;
        if (!rq.subtitles.empty()) tail.push_back(filter("subtitles", {{"filename", fs::path(rq.subtitles).generic_string()}}));
        if (still) {
            if (rq.stillWidth > 0 && rq.stillWidth != tl.settings.width) {
                const int h = even(static_cast<double>(rq.stillWidth) * tl.settings.height / tl.settings.width);
                tail.push_back(filter("scale", {{"w", std::to_string(even(rq.stillWidth))}, {"h", std::to_string(h)}}));
            }
            tail.push_back(filter("format", {{"", "rgb24"}}));
        } else if (ext != "gif") {
            tail.push_back(filter("format", {{"", "yuv420p"}}));
        }
        std::string vout = b.label("vout");
        if (ext == "gif") {
            tail.push_back(filter("fps", {{"fps", std::to_string(std::max(1, rq.gifFps))}}));
            tail.push_back(filter("scale", {{"w", std::to_string(std::max(16, rq.gifWidth))}, {"h", "-1"}, {"flags", "lanczos"}}));
            tail.push_back("split[g1][g2]");
            b.stmts.push_back("[" + v + "]" + Builder::join(tail));
            b.stmts.push_back("[g1]" + filter("palettegen", {{"stats_mode", "diff"}}) + "[pal]");
            b.stmts.push_back("[g2][pal]" + filter("paletteuse", {{"dither", "bayer"}, {"bayer_scale", "4"}}) + "[" + vout + "]");
        } else {
            b.stmts.push_back("[" + v + "]" + Builder::join(tail) + "[" + vout + "]");
        }
        maps = {"-map", "[" + vout + "]"};
        if (still) {
            codec = {"-frames:v", "1", "-update", "1"};
            if (ext != "png") codec.insert(codec.end(), {"-q:v", "2"});
        } else {
            if (ext != "gif") {
                std::string a = b.audio();
                if (!a.empty()) maps.insert(maps.end(), {"-map", "[" + a + "]"});
                const bool sound = !a.empty();
                if (ext == "webm") {
                    codec = {"-c:v", "libvpx-vp9", "-crf", std::to_string(q.vp9crf), "-b:v", "0", "-deadline", q.vp9deadline,
                             "-cpu-used", std::to_string(q.vp9cpu), "-row-mt", "1"};
                    if (sound) codec.insert(codec.end(), {"-c:a", "libopus", "-b:a", q.audioRate});
                } else {
                    codec = {"-c:v", "libx264", "-preset", q.x264preset, "-crf", std::to_string(q.crf), "-pix_fmt", "yuv420p"};
                    if (sound) codec.insert(codec.end(), {"-c:a", "aac", "-b:a", q.audioRate});
                    if (ext == "mp4" || ext == "mov" || ext == "m4v") codec.insert(codec.end(), {"-movflags", "+faststart"});
                }
            } else {
                codec = {"-loop", "0"};
            }
            codec.insert(codec.end(), {"-r", t(tl.settings.fps), "-t", t(total)});
        }
    }
    plan.graph = b.graph();
    plan.args = head();
    plan.args.insert(plan.args.end(), b.inputs.begin(), b.inputs.end());
    plan.args.insert(plan.args.end(), {"-filter_complex", plan.graph});
    plan.args.insert(plan.args.end(), maps.begin(), maps.end());
    plan.args.insert(plan.args.end(), codec.begin(), codec.end());
    plan.args.push_back(output);
    return plan;
}

}  // namespace wetwaresuite::cut
