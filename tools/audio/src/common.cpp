#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "wetwaresuite/audio/commands.hpp"

namespace wetwaresuite::audio {

void registerCommands(Session& session) {
    auto state = std::make_shared<tool::State>();
    tool::registerFileCommands(session, state);
    tool::registerEditCommands(session, state);
    tool::registerProjectCommands(session);
    tool::registerPlayCommands(session);
    tool::registerEffectCommands(session);
    tool::registerRackCommands(session);
    tool::registerMixCommands(session);
}

}  // namespace wetwaresuite::audio

namespace wetwaresuite::audio::tool {

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool saveOptionsArg(const Params& p, SaveOptions& out, std::string& error) {
    out = {};
    const std::string bits = stringParam(p, "bits", "16");
    if (!parseSampleFormat(bits, out.format)) {
        error = "bits must be 8, 16, 24, 32 or 32f (32-bit float)";
        return false;
    }
    out.dither = boolParam(p, "dither", true);
    const auto kbps = intParam(p, "bitrate", 0);
    if (kbps < 0 || kbps > 512) {
        error = "bitrate is in kbit/s, 8..512 (or 0 for the encoder's default)";
        return false;
    }
    out.bitrateKbps = static_cast<int>(kbps);
    return true;
}

std::string levelsReport(Result& r, const Levels& lv) {
    r.with("seconds", lv.seconds)
        .with("peak_db", lv.peakDb)
        .with("true_peak_db", lv.truePeakDb)
        .with("rms_db", lv.rmsDb)
        .with("lufs", lv.lufs)
        .with("momentary_max_lufs", lv.momentaryMaxLufs)
        .with("short_term_max_lufs", lv.shortTermMaxLufs)
        .with("loudness_range", lv.loudnessRange)
        .with("dc_offset", lv.dcOffset)
        .with("clipped", static_cast<std::int64_t>(lv.clipped))
        .with("channel_peak_db", NumberList(lv.channelPeakDb.begin(), lv.channelPeakDb.end()))
        .with("channel_rms_db", NumberList(lv.channelRmsDb.begin(), lv.channelRmsDb.end()));
    std::string s = "duration " + formatTime(lv.seconds) + ", peak " + dbText(lv.peakDb) + " (true peak " + dbText(lv.truePeakDb) +
                    "), RMS " + dbText(lv.rmsDb) + ", loudness " + (lv.lufs <= -69.9 ? std::string("silent") : num(lv.lufs, 3) + " LUFS");
    if (lv.loudnessRange > 0) s += ", range " + num(lv.loudnessRange, 3) + " LU";
    if (lv.clipped) s += ", " + std::to_string(lv.clipped) + " clipped samples";
    if (std::fabs(lv.dcOffset) > 0.005) s += ", DC offset " + num(lv.dcOffset, 2) + " (run audio.remove_dc)";
    return s;
}

ActionFactory regionCommand(Session& session, RegionEdit fn, bool undoable) {
    return audioCommand(
        session,
        [fn](AudioDoc& doc, const Params& p) {
            Range r;
            Channels ch;
            std::string error;
            if (!rangeArg(doc.audio, p, r, error) || !channelsArg(doc.audio, p, ch, error)) return failed(error);
            if (doc.audio.empty()) return failed("the audio is empty");
            return fn(doc, p, r, ch);
        },
        undoable);
}

void replaceRange(Buffer& b, Range r, const Buffer& with) {
    r.end = std::min(r.end, b.frames());
    for (std::size_t c = 0; c < b.channels.size(); ++c) {
        auto& d = b.channels[c];
        d.erase(d.begin() + static_cast<std::ptrdiff_t>(r.begin), d.begin() + static_cast<std::ptrdiff_t>(r.end));
        const auto& w = with.channels[std::min(c, with.channels.size() - 1)];
        d.insert(d.begin() + static_cast<std::ptrdiff_t>(r.begin), w.begin(), w.end());
    }
}

std::vector<fs::path> audioFiles(const fs::path& dir, bool recursive) {
    std::vector<fs::path> files;
    std::error_code ec;
    auto consider = [&](const fs::directory_entry& e) {
        if (e.is_regular_file() && canRead(e.path())) files.push_back(e.path());
    };
    if (recursive) {
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) consider(e);
    } else {
        for (const auto& e : fs::directory_iterator(dir, ec)) consider(e);
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace wetwaresuite::audio::tool
