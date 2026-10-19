// audio.open/new/save/close/info/preview/measure, silence detection and
// splitting, and folder batches.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/audio/project.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "common.hpp"

namespace wetwaresuite::audio::tool {

namespace {

std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : ", ") + i;
    return out;
}

std::string readableList() {
    return joinList(builtinReadExtensions()) + " built in; " + joinList(ffmpegReadExtensions()) + " (and video files) with ffmpeg";
}

Result& infoFields(Result& r, const std::string& id, const AudioDoc& doc) {
    const Buffer& b = doc.audio;
    r.with("doc", id)
        .with("seconds", b.seconds())
        .with("rate", static_cast<std::int64_t>(b.rate))
        .with("channels", static_cast<std::int64_t>(b.channelCount()))
        .with("frames", static_cast<std::int64_t>(b.frames()));
    if (!doc.path.empty()) r.with("path", doc.path);
    return r;
}

// Opens `doc` from the session or `input` from disk; sets error otherwise.
bool sourceArg(Session& s, const Params& p, Buffer& out, std::string& label, std::string& error) {
    if (p.count("doc")) {
        auto doc = s.workspace().getAs<AudioDoc>(stringParam(p, "doc"));
        if (!doc) {
            error = "no open audio document called '" + stringParam(p, "doc") + "'";
            return false;
        }
        out = doc->audio;
        label = doc->path.empty() ? stringParam(p, "doc") : fs::path(doc->path).stem().string();
        return true;
    }
    if (p.count("input")) {
        try {
            out = load(stringParam(p, "input"));
            label = fs::path(stringParam(p, "input")).stem().string();
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }
    error = "give doc=aud1 (an open document) or input=file.wav";
    return false;
}

ParamSpec optionalDoc() { return {"doc", ParamType::String, "id of an open audio document (or use input=)", false}; }
ParamSpec inputParam() { return {"input", ParamType::String, "audio file to read instead of an open document", false}; }
ParamSpec bitsParam() {
    return {"bits", ParamType::String, "sample format for WAV/FLAC/AIFF: 8, 16, 24, 32 or 32f (float)", false,
            {"8", "16", "24", "32", "32f"}, std::string("16")};
}
ParamSpec bitrateParam() { return {"bitrate", ParamType::Int, "kbit/s for mp3/ogg/opus/m4a (0 = a good default)", false, {}, std::int64_t{0}}; }

std::string regionsText(const std::vector<Region>& regions, const std::string& noun) {
    std::string s;
    for (std::size_t i = 0; i < regions.size() && i < 50; ++i) {
        s += "\n  " + noun + " " + std::to_string(i + 1) + ": " + formatTime(regions[i].start) + " to " + formatTime(regions[i].end) + " (" +
             formatTime(regions[i].end - regions[i].start) + ")";
    }
    if (regions.size() > 50) s += "\n  ... and " + std::to_string(regions.size() - 50) + " more";
    return s;
}

NumberList flatten(const std::vector<Region>& regions) {
    NumberList out;
    for (const auto& r : regions) out.push_back(r.start), out.push_back(r.end);
    return out;
}

// Draws a waveform or spectrogram of `b` as the preview/render params ask.
Result picture(const Buffer& b, const std::string& label, const Params& p, raster::Image& img) {
    Range r;
    std::string error;
    if (!rangeArg(b, p, r, error)) return failed(error);
    if (b.empty()) return failed("the audio is empty");
    const int w = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 1200), 64, 4096));
    const bool spec = boolParam(p, "spectrogram", false);
    const double top = numberParam(p, "max_frequency", 0.0) > 0 ? std::min(numberParam(p, "max_frequency", 0.0), b.rate / 2.0) : b.rate / 2.0;
    if (spec) {
        SpectrogramStyle st;
        st.width = w;
        st.height = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height", 400), 32, 4096));
        st.logFrequency = boolParam(p, "log_frequency", true);
        st.maxFrequency = top;
        st.fftSize = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "fft", 2048), 256, 16384));
        st.floorDb = numberParam(p, "floor", -100.0);
        st.channel = static_cast<int>(intParam(p, "channel", -1));
        if (st.channel >= b.channelCount()) return failed("no channel " + std::to_string(st.channel));
        img = spectrogram(b, r, st);
    } else {
        WaveformStyle st;
        st.width = w;
        st.height = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height", 300), 32, 4096));
        st.decibels = boolParam(p, "decibels", false);
        img = waveform(b, r, st);
    }
    const double t0 = static_cast<double>(r.begin) / b.rate, t1 = static_cast<double>(r.end) / b.rate;
    std::string msg = std::string(spec ? "spectrogram" : "waveform") + " of " + label + " from " + formatTime(t0) + " to " + formatTime(t1);
    if (spec) msg += boolParam(p, "log_frequency", true) ? ", 20 Hz to " + num(top, 6) + " Hz (log scale)" : ", 0 to " + num(top, 6) + " Hz";
    return Result::success(msg);
}

}  // namespace

void registerFileCommands(Session& session, const std::shared_ptr<State>&) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"audio.open",
             "Open audio",
             "Open an audio file as a document (aud1, aud2, ...): " + readableList() +
                 ". Opening a video takes its first audio track. Returns the document id and its length, rate and channels.",
             {{"path", ParamType::String, "audio (or video) file to open", true}, previewParam()},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string path = stringParam(p, "path");
                    try {
                        auto doc = openAudio(path);
                        const std::string id = s->workspace().add(doc, "aud");
                        Result r = Result::success("opened " + path + " as " + id + ": " + describe(doc->audio));
                        infoFields(r, id, *doc);
                        return maybePreview(r, *doc, p);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    reg.add({"audio.new",
             "New audio",
             "Make a new audio document: silence, a test tone (sine, square, saw, triangle), a sweep from frequency to "
             "to_frequency, or white/pink/brown noise.",
             {{"kind", ParamType::String, "what to generate", false, {"silence", "tone", "sweep", "noise"}, std::string("silence")},
              {"seconds", ParamType::Number, "length", false, {}, 1.0},
              {"rate", ParamType::Int, "sample rate in Hz", false, {}, std::int64_t{44100}},
              {"channels", ParamType::Int, "1 = mono, 2 = stereo", false, {}, std::int64_t{2}},
              {"frequency", ParamType::Number, "tone frequency in Hz (sweep start)", false, {}, 440.0},
              {"to_frequency", ParamType::Number, "sweep end frequency in Hz", false, {}, 20000.0},
              {"waveform", ParamType::String, "tone shape", false, {"sine", "square", "saw", "triangle"}, std::string("sine")},
              {"color", ParamType::String, "noise colour", false, {"white", "pink", "brown"}, std::string("white")},
              {"level", ParamType::Number, "peak level in dBFS", false, {}, -6.0},
              previewParam()},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const double seconds = numberParam(p, "seconds", 1.0);
                    const auto rate = intParam(p, "rate", 44100), ch = intParam(p, "channels", 2);
                    if (!(seconds >= 0 && seconds <= 6 * 3600)) return failed("seconds must be 0..21600 (six hours)");
                    if (rate < 1000 || rate > 768000) return failed("rate must be 1000..768000 Hz");
                    if (ch < 1 || ch > 32) return failed("channels must be 1..32");
                    const double amp = dbToGain(std::min(0.0, numberParam(p, "level", -6.0)));
                    const std::string kind = stringParam(p, "kind", "silence");
                    const int r = static_cast<int>(rate), c = static_cast<int>(ch);
                    const double f = numberParam(p, "frequency", 440.0);
                    if (kind != "silence" && kind != "noise" && (f <= 0 || f >= r / 2.0)) return failed("frequency must be between 0 and half the sample rate");
                    Buffer b;
                    if (kind == "tone") {
                        const std::string w = stringParam(p, "waveform", "sine");
                        b = tone(c, seconds, r, f, amp,
                                 w == "square" ? Waveform::Square : w == "saw" ? Waveform::Saw : w == "triangle" ? Waveform::Triangle : Waveform::Sine);
                    } else if (kind == "sweep") {
                        const double f1 = std::clamp(numberParam(p, "to_frequency", 20000.0), 1.0, r / 2.0 - 1);
                        b = silence(c, seconds, r);
                        const double k = std::log(f1 / f), T = std::max(seconds, 1e-6);
                        for (std::size_t i = 0; i < b.frames(); ++i) {
                            const double t = static_cast<double>(i) / r;
                            const double phase = std::fabs(k) < 1e-9 ? 2 * 3.141592653589793 * f * t
                                                                     : 2 * 3.141592653589793 * f * T / k * (std::exp(t / T * k) - 1);
                            for (auto& chan : b.channels) chan[i] = static_cast<float>(amp * std::sin(phase));
                        }
                    } else if (kind == "noise") {
                        const std::string col = stringParam(p, "color", "white");
                        b = noise(c, seconds, r, amp, col == "pink" ? NoiseColor::Pink : col == "brown" ? NoiseColor::Brown : NoiseColor::White);
                    } else {
                        b = silence(c, seconds, r);
                    }
                    auto doc = newAudio(std::move(b));
                    const std::string id = s->workspace().add(doc, "aud");
                    Result res = Result::success("made " + id + ": " + kind + ", " + describe(doc->audio));
                    infoFields(res, id, *doc);
                    return maybePreview(res, *doc, p);
                });
            });

    reg.add({"audio.save",
             "Save audio",
             "Write the audio to a file; the extension picks the format. wav is built in (bits=16, 24, 32 or 32f); flac, "
             "mp3, ogg, opus, m4a, aac and aiff go through ffmpeg. start/end save just that part.",
             {docParam(), {"path", ParamType::String, "file to write, e.g. take.wav or episode.mp3", true}, bitsParam(), bitrateParam(),
              {"dither", ParamType::Bool, "add TPDF dither when saving 8/16-bit", false, {}, true}, startParam(), endParam(),
              {"rate", ParamType::Int, "write at this sample rate (the document keeps its own)", false}},
             false},
            audioCommand(
                session,
                [](AudioDoc& doc, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    if (!canWrite(path)) {
                        const std::string e = lowerExt(path);
                        if (std::find(ffmpegWriteExtensions().begin(), ffmpegWriteExtensions().end(), e) != ffmpegWriteExtensions().end()) {
                            return failed("saving ." + e + " needs ffmpeg on PATH; save as .wav instead, or install ffmpeg");
                        }
                        return failed("can't save ." + e + "; use wav, " + joinList(ffmpegWriteExtensions()));
                    }
                    SaveOptions o;
                    std::string error;
                    Range r;
                    if (!saveOptionsArg(p, o, error) || !rangeArg(doc.audio, p, r, error)) return failed(error);
                    const bool part = r.begin > 0 || r.end < doc.audio.frames();
                    const auto rate = intParam(p, "rate", 0);
                    if (p.count("rate") && (rate < 1000 || rate > 768000)) return failed("rate must be 1000..768000 Hz");
                    if (rate > 0 && rate != doc.audio.rate) save(resample(part ? slice(doc.audio, r) : doc.audio, static_cast<int>(rate)), path, o);
                    else save(part ? slice(doc.audio, r) : doc.audio, path, o);
                    if (!part) doc.path = path.string();
                    std::error_code ec;
                    return Result::success("saved " + path.string() + " (" + formatTime(static_cast<double>(r.size()) / doc.audio.rate) + ")")
                        .with("path", path.string())
                        .with("bytes", static_cast<std::int64_t>(fs::file_size(path, ec)));
                },
                false));

    reg.add({"audio.close",
             "Close audio",
             "Close an open audio document or project (unsaved changes are lost).",
             {{"doc", ParamType::String, "id of an open audio document or project, e.g. aud1 or proj1", true}},
             false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, id = stringParam(p, "doc")] {
                    if (!s->workspace().getAs<AudioDoc>(id) && !s->workspace().getAs<ProjectDoc>(id)) {
                        return failed("no open audio document or project called '" + id + "'");
                    }
                    s->workspace().take(id);
                    return Result::success("closed " + id);
                });
            });

    reg.add({"audio.info",
             "Audio info",
             "Length, sample rate, channels and file of an open audio document (audio.measure gives levels and loudness).",
             {docParam()},
             false, true},
            audioCommand(
                session,
                [](AudioDoc& doc, const Params& p) {
                    Result r = Result::success(stringParam(p, "doc") + ": " + doc.summary() + (doc.noise.empty() ? "" : ", noise profile learned"));
                    return infoFields(r, stringParam(p, "doc"), doc).with("noise_profile", !doc.noise.empty());
                },
                false));

    const std::vector<ParamSpec> pictureParams = {
        startParam(), endParam(),
        {"spectrogram", ParamType::Bool, "draw a spectrogram instead of a waveform", false, {}, false},
        {"width", ParamType::Int, "image width in px", false, {}, std::int64_t{1200}},
        {"height", ParamType::Int, "image height in px (default 300 for waveforms, 400 for spectrograms)", false},
        {"log_frequency", ParamType::Bool, "spectrogram: log frequency axis (false = linear Hz)", false, {}, true},
        {"max_frequency", ParamType::Number, "spectrogram: top frequency in Hz (default half the sample rate)", false},
        {"fft", ParamType::Int, "spectrogram: FFT size, bigger = finer frequency, blurrier time", false, {}, std::int64_t{2048}},
        {"floor", ParamType::Number, "spectrogram: dB shown as black", false, {}, -100.0},
        {"channel", ParamType::Int, "spectrogram: one channel (default: the mix)", false},
        {"decibels", ParamType::Bool, "waveform: vertical dB scale, shows quiet detail", false, {}, false}};
    const std::string pictureText =
        "a waveform (one lane per channel, clipped samples marked red, a time scale along the bottom) or, with "
        "spectrogram=true, a spectrogram (time left to right, frequency bottom to top on a log scale with lines at "
        "100 Hz, 1 kHz and 10 kHz, brighter = louder). start/end zoom in.";

    reg.add({"audio.preview", "Preview audio", "Draw an open document (doc=) or a file (input=) as a PNG to look at: " + pictureText,
             concat({optionalDoc(), inputParam()}, pictureParams), false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    Buffer b;
                    std::string label, error;
                    if (!sourceArg(*s, p, b, label, error)) return failed(error);
                    raster::Image img;
                    Result r = picture(b, label, p, img);
                    if (r.ok) r.attach({"image/png", raster::encodePng(img), boolParam(p, "spectrogram", false) ? "spectrogram.png" : "waveform.png"});
                    return r;
                });
            });

    reg.add({"audio.render",
             "Render picture",
             "Write a picture to a PNG file: of a document (doc=) or file (input=), " + pictureText +
                 " Or of a project (project=): its tracks as lanes, each drawn as it will sound.",
             concat({optionalDoc(), inputParam(), {"project", ParamType::String, "draw this project's tracks instead", false},
                     {"output", ParamType::String, "PNG file to write", true}},
                    pictureParams),
             false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const fs::path out = stringParam(p, "output");
                    raster::Image img;
                    Result r;
                    if (p.count("project")) {
                        auto proj = s->workspace().getAs<ProjectDoc>(stringParam(p, "project"));
                        if (!proj) return failed("no open project called '" + stringParam(p, "project") + "'");
                        const int w = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 1200), 64, 4096));
                        img = renderProject(proj->project, w);
                        r = Result::success("tracks of " + stringParam(p, "project"));
                    } else {
                        Buffer b;
                        std::string label, error;
                        if (!sourceArg(*s, p, b, label, error)) return failed(error);
                        r = picture(b, label, p, img);
                        if (!r.ok) return r;
                    }
                    try {
                        raster::save(img, out);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                    r.message = "wrote " + out.string() + ": " + r.message;
                    return r.with("output", out.string()).with("width", static_cast<std::int64_t>(img.width())).with("height", static_cast<std::int64_t>(img.height()));
                });
            });

    reg.add({"audio.measure",
             "Measure audio",
             "Levels of a document or file: duration, sample peak and true peak (dBFS/dBTP), RMS, integrated loudness "
             "(LUFS, ITU-R BS.1770 / EBU R128), loudest momentary and short-term loudness, loudness range, DC offset, "
             "and how many samples clip. Podcasts aim for -16 LUFS (mono -19), streaming music -14, broadcast -23.",
             {optionalDoc(), inputParam(), startParam(), endParam(),
              {"true_peak", ParamType::Bool, "measure the 4x oversampled true peak (slower on long files)", false, {}, true}},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    Buffer b;
                    std::string label, error;
                    if (!sourceArg(*s, p, b, label, error)) return failed(error);
                    Range r;
                    if (!rangeArg(b, p, r, error)) return failed(error);
                    Result res = Result::success("");
                    res.message = label + ": " + levelsReport(res, measure(b, r, boolParam(p, "true_peak", true)));
                    return res;
                });
            });

    reg.add({"audio.detect_silence",
             "Find silence",
             "List the silent stretches (quieter than threshold for at least min_silence seconds) of a document or file. "
             "Returns them as start,end pairs in seconds.",
             {optionalDoc(), inputParam(), {"threshold", ParamType::Number, "dBFS (RMS over 10 ms) counted as silence", false, {}, -40.0},
              {"min_silence", ParamType::Number, "shortest silence to report, seconds", false, {}, 0.5}},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    Buffer b;
                    std::string label, error;
                    if (!sourceArg(*s, p, b, label, error)) return failed(error);
                    const auto quiet = detectSilence(b, numberParam(p, "threshold", -40.0), numberParam(p, "min_silence", 0.5));
                    double total = 0;
                    for (const auto& q : quiet) total += q.end - q.start;
                    return Result::success(std::to_string(quiet.size()) + " silent stretches in " + label + ", " + formatTime(total) + " in all" +
                                           regionsText(quiet, "silence"))
                        .with("count", static_cast<std::int64_t>(quiet.size()))
                        .with("silence_seconds", total)
                        .with("regions", flatten(quiet));
                });
            });

    reg.add({"audio.split_on_silence",
             "Split on silence",
             "Cut a recording into separate files at its silences (one file per song, sample or sentence), numbered "
             "<prefix>_001.wav, <prefix>_002.wav... in output_dir.",
             {optionalDoc(), inputParam(), {"output_dir", ParamType::String, "folder for the pieces", true},
              {"prefix", ParamType::String, "file name start (default: the source's name)", false},
              {"format", ParamType::String, "file type of the pieces", false, {"wav", "flac", "mp3", "ogg", "opus", "m4a"}, std::string("wav")},
              {"threshold", ParamType::Number, "dBFS counted as silence", false, {}, -40.0},
              {"min_silence", ParamType::Number, "a gap must be this long to split, seconds", false, {}, 0.5},
              {"min_length", ParamType::Number, "drop pieces shorter than this, seconds", false, {}, 0.25},
              {"pad", ParamType::Number, "silence kept around each piece, seconds", false, {}, 0.1},
              {"fade", ParamType::Number, "fade each piece in and out over this many seconds", false, {}, 0.005},
              bitsParam(), bitrateParam()},
             false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    Buffer b;
                    std::string label, error;
                    if (!sourceArg(*s, p, b, label, error)) return failed(error);
                    SaveOptions o;
                    if (!saveOptionsArg(p, o, error)) return failed(error);
                    const std::string ext = stringParam(p, "format", "wav");
                    if (!canWrite("x." + ext)) return failed("saving ." + ext + " needs ffmpeg on PATH; use format=wav");
                    const auto parts = splitOnSilence(b, numberParam(p, "threshold", -40.0), numberParam(p, "min_silence", 0.5),
                                                      numberParam(p, "min_length", 0.25), std::max(0.0, numberParam(p, "pad", 0.1)));
                    if (parts.empty()) return failed("found nothing but silence (try a lower threshold, e.g. -55)");
                    const fs::path dir = stringParam(p, "output_dir");
                    const std::string prefix = stringParam(p, "prefix", label.empty() ? "part" : label);
                    std::vector<std::string> files;
                    std::string list;
                    const double fade = std::max(0.0, numberParam(p, "fade", 0.005));
                    for (std::size_t i = 0; i < parts.size(); ++i) {
                        Buffer piece = slice(b, rangeOf(b, parts[i].start, parts[i].end));
                        fadeIn(piece, fade, all(piece));
                        fadeOut(piece, fade, all(piece));
                        char name[32];
                        std::snprintf(name, sizeof name, "_%03zu.", i + 1);
                        const fs::path out = dir / (prefix + name + ext);
                        save(piece, out, o);
                        files.push_back(out.string());
                        list += "\n  " + out.string() + " (" + formatTime(parts[i].start) + " to " + formatTime(parts[i].end) + ")";
                    }
                    std::string joined;
                    for (const auto& f : files) joined += (joined.empty() ? "" : ", ") + f;
                    return Result::success("wrote " + std::to_string(files.size()) + " pieces:" + list)
                        .with("count", static_cast<std::int64_t>(files.size()))
                        .with("files", joined)
                        .with("regions", flatten(parts));
                });
            });

    reg.add({"audio.batch",
             "Process folder",
             "Process every audio file in a folder into output_dir (originals are never touched): convert format, "
             "mix to mono/stereo, resample, high-pass filter, reduce noise, change tempo or pitch, normalize peak or loudness, trim "
             "leading/trailing silence, fade. Steps run in that order, so the silence threshold applies to the "
             "normalized level.",
             {{"input_dir", ParamType::String, "folder of audio files", true},
              {"output_dir", ParamType::String, "where results go (a different folder)", true},
              {"to", ParamType::String, "output format; default keeps each file's own", false,
               {"wav", "flac", "mp3", "ogg", "opus", "m4a", "aac", "aiff"}},
              {"normalize", ParamType::String, "level matching", false, {"none", "peak", "loudness"}, std::string("none")},
              {"target", ParamType::Number, "peak dBFS (default -1) or loudness LUFS (default -16)", false},
              {"ceiling", ParamType::Number, "loudness mode: true-peak ceiling in dBFS", false, {}, -1.0},
              {"trim_silence", ParamType::Bool, "cut leading and trailing silence", false, {}, false},
              {"threshold", ParamType::Number, "dBFS treated as silence for trimming", false, {}, -50.0},
              {"fade_in", ParamType::Number, "seconds", false, {}, 0.0},
              {"fade_out", ParamType::Number, "seconds", false, {}, 0.0},
              {"rate", ParamType::Int, "resample to this rate in Hz", false},
              {"channels", ParamType::Int, "mix to 1 (mono) or 2 (stereo)", false},
              {"highpass", ParamType::Number, "remove rumble below this many Hz (e.g. 80)", false},
              {"denoise", ParamType::Number, "reduce steady background noise by this many dB (noise estimated from each file's quietest parts)", false},
              {"tempo", ParamType::Number, "speed up (1.25) or slow down (0.8) keeping the pitch", false},
              {"pitch", ParamType::Number, "shift by this many semitones keeping the length", false},
              {"recursive", ParamType::Bool, "include subfolders (kept in output_dir)", false, {}, false},
              {"overwrite", ParamType::Bool, "replace existing outputs", false, {}, false},
              bitsParam(), bitrateParam()},
             false},
            [](const Params& p) {
                return std::make_unique<FunctionAction>([p] {
                    const fs::path in = stringParam(p, "input_dir"), outDir = stringParam(p, "output_dir");
                    std::error_code ec;
                    if (!fs::is_directory(in, ec)) return failed("not a folder: " + in.string());
                    if (fs::exists(outDir, ec) && fs::equivalent(in, outDir, ec)) return failed("output_dir must be a different folder from input_dir");
                    SaveOptions o;
                    std::string error;
                    if (!saveOptionsArg(p, o, error)) return failed(error);
                    const std::string mode = stringParam(p, "normalize", "none");
                    const double target = numberParam(p, "target", mode == "loudness" ? -16.0 : -1.0);
                    const std::string to = stringParam(p, "to");
                    if (!to.empty() && !canWrite("x." + to)) return failed("saving ." + to + " needs ffmpeg on PATH");
                    const auto rate = intParam(p, "rate", 0), chans = intParam(p, "channels", 0);
                    if (p.count("rate") && (rate < 1000 || rate > 768000)) return failed("rate must be 1000..768000 Hz");
                    if (p.count("channels") && (chans < 1 || chans > 2)) return failed("channels must be 1 or 2");
                    if (p.count("highpass") && !(numberParam(p, "highpass") > 0)) return failed("highpass is a frequency in Hz");
                    if (p.count("tempo") && !(numberParam(p, "tempo") >= 0.1 && numberParam(p, "tempo") <= 10)) return failed("tempo must be 0.1..10");
                    if (p.count("pitch") && std::fabs(numberParam(p, "pitch")) > 24) return failed("pitch must be within -24..24 semitones");
                    auto files = audioFiles(in, boolParam(p, "recursive", false));
                    const fs::path outAbs = fs::weakly_canonical(outDir, ec);
                    files.erase(std::remove_if(files.begin(), files.end(),
                                               [&](const fs::path& f) {
                                                   auto rel = fs::weakly_canonical(f, ec).lexically_relative(outAbs);
                                                   return !rel.empty() && *rel.begin() != "..";
                                               }),
                                files.end());
                    if (files.empty()) return failed("no audio files in " + in.string());
                    int done = 0, skipped = 0;
                    std::string lines, failures;
                    for (const auto& f : files) {
                        fs::path target_path = outDir / fs::relative(f, in, ec);
                        std::string ext = to.empty() ? lowerExt(f) : to;
                        if (!canWrite("x." + ext)) ext = "wav";  // e.g. a video or wma in: write wav
                        target_path.replace_extension("." + ext);
                        if (fs::exists(target_path, ec) && !boolParam(p, "overwrite", false)) {
                            ++skipped;
                            continue;
                        }
                        try {
                            Buffer b = load(f);
                            if (chans > 0) setChannels(b, static_cast<int>(chans));
                            if (rate > 0 && rate != b.rate) b = resample(b, static_cast<int>(rate));
                            if (p.count("highpass")) equalize(b, FilterType::HighPass, numberParam(p, "highpass"), 0, 0.7071, all(b), {}, 2);
                            if (p.count("denoise") && numberParam(p, "denoise") > 0) {
                                reduceNoise(b, estimateNoise(b), std::min(60.0, numberParam(p, "denoise")), 1.5, all(b));
                            }
                            if (p.count("tempo")) b = stretch(b, 1.0 / numberParam(p, "tempo"));
                            if (p.count("pitch")) b = pitchShift(b, numberParam(p, "pitch"));
                            double applied = 0;
                            if (mode == "peak") applied = normalizePeak(b, target, all(b));
                            else if (mode == "loudness") applied = normalizeLoudness(b, target, numberParam(p, "ceiling", -1.0));
                            if (boolParam(p, "trim_silence", false)) {
                                Range keep = nonSilent(b, numberParam(p, "threshold", -50.0));
                                if (keep.size() == 0) throw std::runtime_error("it's all silence");
                                trim(b, keep);
                            }
                            if (numberParam(p, "fade_in", 0) > 0) fadeIn(b, numberParam(p, "fade_in", 0), all(b));
                            if (numberParam(p, "fade_out", 0) > 0) fadeOut(b, numberParam(p, "fade_out", 0), all(b));
                            save(b, target_path, o);
                            ++done;
                            lines += "\n  " + target_path.string() + " (" + formatTime(b.seconds());
                            if (mode != "none") lines += ", " + (applied >= 0 ? std::string("+") : std::string()) + num(applied, 3) + " dB";
                            lines += ")";
                        } catch (const std::exception& e) {
                            failures += "\n  " + f.filename().string() + ": " + e.what();
                        }
                    }
                    const auto total = static_cast<std::int64_t>(files.size());
                    std::string msg = "processed " + std::to_string(done) + " of " + std::to_string(total) + " files into " + outDir.string() + lines;
                    if (skipped) msg += "\n" + std::to_string(skipped) + " skipped because the output exists (pass overwrite=true)";
                    if (!failures.empty()) msg += "\nfailed:" + failures;
                    Result r = done > 0 || skipped > 0 ? Result::success(msg) : Result::failure(msg);
                    return r.with("processed", static_cast<std::int64_t>(done))
                        .with("skipped", static_cast<std::int64_t>(skipped))
                        .with("failed", total - done - skipped);
                });
            });
}

}  // namespace wetwaresuite::audio::tool
