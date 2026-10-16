// Standard MIDI Files, written and read by hand: format 1 out, format 0/1 in.
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

#include "achclwss/beats/song.hpp"

namespace achclwss::beats {

namespace {

constexpr int kPpq = 480;

void put16(std::string& s, unsigned v) {
    s += static_cast<char>((v >> 8) & 0xFF);
    s += static_cast<char>(v & 0xFF);
}
void put32(std::string& s, std::uint32_t v) {
    for (int sh = 24; sh >= 0; sh -= 8) s += static_cast<char>((v >> sh) & 0xFF);
}
void putVar(std::string& s, std::uint32_t v) {
    char buf[5];
    int n = 0;
    buf[n++] = static_cast<char>(v & 0x7F);
    while ((v >>= 7) != 0) buf[n++] = static_cast<char>((v & 0x7F) | 0x80);
    while (n) s += buf[--n];
}

struct Raw {
    std::uint32_t tick;
    int order;  // note-offs before note-ons at the same tick
    std::string bytes;
};

std::string chunk(const char* id, const std::string& body) {
    std::string s = id;
    put32(s, static_cast<std::uint32_t>(body.size()));
    return s + body;
}

std::string trackChunk(std::vector<Raw> ev, const std::string& name) {
    std::stable_sort(ev.begin(), ev.end(), [](const Raw& a, const Raw& b) { return a.tick != b.tick ? a.tick < b.tick : a.order < b.order; });
    std::string body;
    if (!name.empty()) {
        putVar(body, 0);
        body += "\xFF\x03";
        putVar(body, static_cast<std::uint32_t>(name.size()));
        body += name;
    }
    std::uint32_t last = 0;
    for (const auto& e : ev) {
        putVar(body, e.tick - last);
        last = e.tick;
        body += e.bytes;
    }
    putVar(body, 0);
    body += std::string("\xFF\x2F\x00", 3);
    return chunk("MTrk", body);
}

std::uint32_t ticks(double beats) { return static_cast<std::uint32_t>(std::max(0L, std::lround(beats * kPpq))); }

}  // namespace

std::string exportMidi(const Song& s) {
    std::string out;
    std::string hdr;
    put16(hdr, 1);
    put16(hdr, static_cast<unsigned>(s.tracks.size() + 1));
    put16(hdr, kPpq);
    out += chunk("MThd", hdr);

    // tempo track: tempo, time signature, key name as a text marker
    std::vector<Raw> meta;
    {
        std::string b = "\xFF\x51\x03";
        const auto us = static_cast<std::uint32_t>(std::lround(60000000.0 / s.bpm));
        b += static_cast<char>((us >> 16) & 0xFF);
        b += static_cast<char>((us >> 8) & 0xFF);
        b += static_cast<char>(us & 0xFF);
        meta.push_back({0, 0, b});
        int pow2 = 0;
        for (int u = s.beatUnit; u > 1; u >>= 1) ++pow2;
        std::string ts = "\xFF\x58\x04";
        ts += static_cast<char>(s.beatsPerBar);
        ts += static_cast<char>(pow2);
        ts += static_cast<char>(24);
        ts += static_cast<char>(8);
        meta.push_back({0, 0, ts});
    }
    out += trackChunk(meta, s.name);

    int nextChannel = 0;
    for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
        const Track& t = s.tracks[static_cast<std::size_t>(i)];
        int ch;
        if (t.kind == TrackKind::Drum) {
            ch = 9;
        } else {
            if (nextChannel == 9) ++nextChannel;
            ch = nextChannel % 16;
            if (ch == 9) ch = 10;
            ++nextChannel;
        }
        std::vector<Raw> ev;
        {
            // channel volume: 100 = 0 dB; muted tracks stay in the file at volume 0
            std::string cc;
            cc += static_cast<char>(0xB0 | ch);
            cc += static_cast<char>(7);
            cc += static_cast<char>(t.mute ? 0 : std::clamp(static_cast<int>(std::lround(100 * std::pow(10.0, t.volumeDb / 20))), 1, 127));
            ev.push_back({0, 0, cc});
        }
        if (t.kind != TrackKind::Drum) {
            std::string pc;
            pc += static_cast<char>(0xC0 | ch);
            // a rough General MIDI program from the preset
            int prog = 81;  // lead (sawtooth)
            if (t.presetName == "bass" || t.presetName == "sub") prog = 38;
            else if (t.presetName == "pad") prog = 89;
            else if (t.presetName == "pluck") prog = 45;
            else if (t.presetName == "keys") prog = 4;
            else if (t.presetName == "brass") prog = 61;
            pc += static_cast<char>(prog);
            ev.push_back({0, 0, pc});
        }
        {
            std::string pan;
            pan += static_cast<char>(0xB0 | ch);
            pan += static_cast<char>(10);
            pan += static_cast<char>(std::clamp(static_cast<int>(std::lround(64 + t.pan * 63)), 0, 127));
            ev.push_back({0, 0, pan});
        }
        for (const Event& e : trackEvents(s, i)) {
            int pitch = e.pitch;
            if (t.kind == TrackKind::Drum) {
                const int gm = drumMidi(e.voice);
                pitch = gm >= 0 ? gm : (e.pitch >= 0 ? e.pitch : 37);
            } else if (t.kind == TrackKind::Synth) {
                pitch += 12 * t.synth.octave;
            }
            pitch = std::clamp(pitch, 0, 127);
            const std::uint32_t on = ticks(e.beat);
            std::uint32_t off = ticks(e.beat + (t.kind == TrackKind::Drum ? std::min(e.length, 0.25) : e.length));
            if (off <= on) off = on + 1;
            std::string a, b;
            a += static_cast<char>(0x90 | ch);
            a += static_cast<char>(pitch);
            a += static_cast<char>(std::clamp(e.velocity, 1, 127));
            b += static_cast<char>(0x80 | ch);
            b += static_cast<char>(pitch);
            b += static_cast<char>(0);
            ev.push_back({on, 1, a});
            ev.push_back({off, 0, b});
        }
        out += trackChunk(ev, t.name);
    }
    return out;
}

namespace {

struct Reader {
    const std::string& d;
    std::size_t p = 0;
    std::size_t end;
    Reader(const std::string& data, std::size_t from, std::size_t to) : d(data), p(from), end(to) {}
    bool more() const { return p < end; }
    int byte() {
        if (p >= end) throw std::runtime_error("the MIDI file ends in the middle of a track");
        return static_cast<unsigned char>(d[p++]);
    }
    std::uint32_t var() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const int b = byte();
            v = (v << 7) | static_cast<std::uint32_t>(b & 0x7F);
            if (!(b & 0x80)) return v;
        }
        throw std::runtime_error("bad variable-length number in the MIDI file");
    }
};

std::uint32_t be32(const std::string& d, std::size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("the MIDI file is cut short");
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(d[at])) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(d[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d[at + 2])) << 8) | static_cast<unsigned char>(d[at + 3]);
}
unsigned be16(const std::string& d, std::size_t at) {
    if (at + 2 > d.size()) throw std::runtime_error("the MIDI file is cut short");
    return (static_cast<unsigned>(static_cast<unsigned char>(d[at])) << 8) | static_cast<unsigned char>(d[at + 1]);
}

struct Part {
    std::string name;
    int channel = 0;
    int program = -1;
    int pan = 64;
    int volume = 100;
    std::vector<Note> notes;  // in beats
};

}  // namespace

Song importMidi(const std::string& d, const std::string& name) {
    if (d.size() < 14 || d.compare(0, 4, "MThd") != 0) throw std::runtime_error("not a MIDI file (no MThd header)");
    const std::uint32_t hlen = be32(d, 4);
    const unsigned format = be16(d, 8);
    const unsigned ntracks = be16(d, 10);
    const unsigned division = be16(d, 12);
    if (format > 1) throw std::runtime_error("MIDI format 2 (independent sequences) isn't supported; use format 0 or 1");
    if (division & 0x8000) throw std::runtime_error("SMPTE-timed MIDI files aren't supported; use a file with ticks per beat");
    const double ppq = division ? division : 480;
    Song s;
    s.name = name;
    bool haveTempo = false, haveSig = false;
    // key: (track chunk index, channel)
    std::map<std::pair<unsigned, int>, Part> parts;
    std::size_t pos = 8 + hlen;
    for (unsigned tr = 0; tr < ntracks && pos + 8 <= d.size(); ++tr) {
        const std::uint32_t len = be32(d, pos + 4);
        const bool isTrack = d.compare(pos, 4, "MTrk") == 0;
        const std::size_t body = pos + 8;
        const std::size_t end = std::min<std::size_t>(d.size(), body + len);
        pos = body + len;
        if (!isTrack) {
            --tr;
            continue;
        }
        Reader r(d, body, end);
        std::uint64_t tick = 0;
        int status = 0;
        std::string trackName;
        std::map<std::pair<int, int>, std::vector<std::pair<std::uint64_t, int>>> open;  // (ch, note) -> starts
        std::map<int, int> programs, pans, volumes;
        while (r.more()) {
            tick += r.var();
            int b = r.byte();
            if (b == 0xFF) {
                const int type = r.byte();
                const std::uint32_t l = r.var();
                std::string data;
                for (std::uint32_t i = 0; i < l; ++i) data += static_cast<char>(r.byte());
                if (type == 0x51 && l == 3 && !haveTempo) {
                    const double us = (static_cast<unsigned char>(data[0]) << 16) | (static_cast<unsigned char>(data[1]) << 8) | static_cast<unsigned char>(data[2]);
                    if (us > 0) s.bpm = std::clamp(std::round(60000000.0 / us * 100) / 100, 20.0, 400.0);
                    haveTempo = true;
                } else if (type == 0x58 && l >= 2 && !haveSig) {
                    s.beatsPerBar = std::clamp(static_cast<int>(static_cast<unsigned char>(data[0])), 1, 32);
                    s.beatUnit = std::clamp(1 << std::min(5, static_cast<int>(static_cast<unsigned char>(data[1]))), 1, 32);
                    haveSig = true;
                } else if (type == 0x03 && trackName.empty()) {
                    trackName = data;
                } else if (type == 0x2F) {
                    break;
                }
                continue;
            }
            if (b == 0xF0 || b == 0xF7) {
                const std::uint32_t l = r.var();
                for (std::uint32_t i = 0; i < l; ++i) r.byte();
                continue;
            }
            int d1;
            if (b & 0x80) {
                status = b;
                d1 = r.byte();
            } else {
                if (!status) throw std::runtime_error("running status without a status byte in the MIDI file");
                d1 = b;
            }
            const int type = status & 0xF0, ch = status & 0x0F;
            const bool two = !(type == 0xC0 || type == 0xD0);
            const int d2 = two ? r.byte() : 0;
            if (type == 0x90 && d2 > 0) {
                open[{ch, d1}].push_back({tick, d2});
                parts[{tr, ch}].channel = ch;
            } else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
                auto it = open.find({ch, d1});
                if (it != open.end() && !it->second.empty()) {
                    const auto [start, vel] = it->second.front();
                    it->second.erase(it->second.begin());
                    Note n;
                    n.pitch = d1;
                    n.start = static_cast<double>(start) / ppq;
                    n.length = std::max(1.0 / 64, static_cast<double>(tick - start) / ppq);
                    n.velocity = std::clamp(vel, 1, 127);
                    parts[{tr, ch}].notes.push_back(n);
                }
            } else if (type == 0xC0) {
                programs[ch] = d1;
            } else if (type == 0xB0 && d1 == 10) {
                pans[ch] = d2;
            } else if (type == 0xB0 && d1 == 7) {
                volumes[ch] = d2;
            }
        }
        for (auto& [key, part] : parts) {
            if (key.first != tr) continue;
            part.name = trackName;
            if (programs.count(part.channel)) part.program = programs[part.channel];
            if (pans.count(part.channel)) part.pan = pans[part.channel];
            if (volumes.count(part.channel)) part.volume = volumes[part.channel];
        }
    }
    if (parts.empty()) throw std::runtime_error("the MIDI file has no notes");
    double lastBeat = 0;
    for (const auto& [k, part] : parts) {
        for (const auto& n : part.notes) lastBeat = std::max(lastBeat, n.start);
    }
    const int bars = std::max(1, static_cast<int>(std::floor(lastBeat / s.beatsInBar() + 1e-9) + 1));
    for (auto& [k, part] : parts) {
        if (part.notes.empty()) continue;
        Track t;
        const bool drums = part.channel == 9;
        t.kind = drums ? TrackKind::Drum : TrackKind::Synth;
        t.name = !part.name.empty() ? part.name : drums ? "Drums" : "Channel " + std::to_string(part.channel + 1);
        for (const auto& other : s.tracks) {
            if (other.name == t.name) t.name += " " + std::to_string(part.channel + 1);
        }
        if (!drums) {
            const int pg = part.program;
            std::string pr = "lead";
            if (pg >= 32 && pg < 40) pr = "bass";
            else if (pg >= 0 && pg < 8) pr = "keys";
            else if (pg >= 88 && pg < 96) pr = "pad";
            else if (pg >= 48 && pg < 56) pr = "pad";
            else if (pg >= 56 && pg < 64) pr = "brass";
            else if (pg >= 24 && pg < 32) pr = "pluck";
            else if (pg < 0) {
                int lowest = 127;
                for (const auto& n : part.notes) lowest = std::min(lowest, n.pitch);
                pr = lowest < 48 ? "bass" : "keys";
            }
            preset(pr, t.synth);
            t.presetName = pr;
        }
        t.pan = std::clamp((part.pan - 64) / 63.0, -1.0, 1.0);
        if (part.volume == 0) t.mute = true;
        else t.volumeDb = std::round(20 * std::log10(part.volume / 100.0) * 10) / 10;
        Pattern p;
        p.name = t.name;
        for (const auto& other : s.patterns) {
            if (other.name == p.name) p.name += " " + std::to_string(s.patterns.size() + 1);
        }
        p.bars = bars;
        p.notes = part.notes;
        std::sort(p.notes.begin(), p.notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
        s.tracks.push_back(t);
        s.patterns.push_back(p);
        s.arrangement.push_back({static_cast<int>(s.tracks.size()) - 1, p.name, 0, 1});
    }
    return s;
}

}  // namespace achclwss::beats
