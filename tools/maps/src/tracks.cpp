// GPS tracks: Garmin FIT files, splits per kilometre, smoothing, spike
// removal, trimming, splitting at pauses, and sampling points along lines.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "wetwaresuite/maps/maps.hpp"

namespace wetwaresuite::maps {

namespace {

Coord between(const Coord& a, const Coord& b, double t) {
    Coord c;
    c.x = a.x + (b.x - a.x) * t;
    c.y = a.y + (b.y - a.y) * t;
    c.z = std::isnan(a.z) || std::isnan(b.z) ? (std::isnan(a.z) ? b.z : a.z) : a.z + (b.z - a.z) * t;
    c.t = std::isnan(a.t) || std::isnan(b.t) ? kNaN : a.t + (b.t - a.t) * t;
    return c;
}

std::vector<double> cumulative(const Line& l) {
    std::vector<double> d(l.size(), 0);
    for (std::size_t i = 1; i < l.size(); ++i) d[i] = d[i - 1] + geodesicDistance(l[i - 1].x, l[i - 1].y, l[i].x, l[i].y);
    return d;
}

// ---- FIT ---------------------------------------------------------------------------------

constexpr double kFitEpoch = 631065600;  // 1989-12-31T00:00:00Z in Unix seconds

struct FitField {
    int num = 0, size = 0, type = 0;
};
struct FitDef {
    bool valid = false, bigEndian = false;
    int global = 0;
    std::vector<FitField> fields;
    int devBytes = 0;
};

std::uint64_t readUnsigned(const unsigned char* p, int size, bool big) {
    std::uint64_t v = 0;
    for (int i = 0; i < size; ++i) {
        const int k = big ? i : size - 1 - i;
        v = (v << 8) | p[k];
    }
    return v;
}

const char* sportName(int s) {
    switch (s) {
        case 0: return "generic";
        case 1: return "running";
        case 2: return "cycling";
        case 3: return "transition";
        case 4: return "fitness_equipment";
        case 5: return "swimming";
        case 10: return "training";
        case 11: return "walking";
        case 12: return "cross_country_skiing";
        case 13: return "alpine_skiing";
        case 14: return "snowboarding";
        case 15: return "rowing";
        case 16: return "mountaineering";
        case 17: return "hiking";
        case 19: return "paddling";
        case 21: return "e_biking";
        default: return "other";
    }
}

}  // namespace

std::vector<Feature> parseFit(const std::string& bytes, std::vector<std::string>* warnings) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    if (n < 12 || bytes.compare(8, 4, ".FIT") != 0) throw std::runtime_error("not a FIT file (no .FIT signature)");
    Line track;
    double hrSum = 0, hrMax = 0, cadSum = 0, lastTs = kNaN, fitDistance = kNaN;
    std::size_t hrN = 0, cadN = 0, noFix = 0;
    int sport = -1;
    std::size_t at = 0;
    while (at + 12 <= n && bytes.compare(at + 8, 4, ".FIT") == 0) {  // chained FIT files follow one another
        const std::size_t headerSize = data[at];
        const std::size_t dataSize = static_cast<std::size_t>(readUnsigned(data + at + 4, 4, false));
        std::size_t p = at + headerSize;
        const std::size_t end = std::min(n, p + dataSize);
        if (p + dataSize > n && warnings) warnings->push_back("the FIT file is cut short; read what was there");
        FitDef defs[16];
        while (p < end) {
            const unsigned char h = data[p++];
            int local = 0;
            bool definition = false, dev = false;
            double compressedTs = kNaN;
            if (h & 0x80) {  // compressed timestamp header
                local = (h >> 5) & 0x3;
                const int offset = h & 0x1F;
                if (!std::isnan(lastTs)) {
                    auto last = static_cast<std::uint32_t>(lastTs);
                    std::uint32_t ts = (last & ~0x1Fu) + static_cast<std::uint32_t>(offset);
                    if (ts < last) ts += 0x20;
                    compressedTs = ts;
                }
            } else {
                local = h & 0x0F;
                definition = (h & 0x40) != 0;
                dev = (h & 0x20) != 0;
            }
            if (definition) {
                if (p + 5 > end) break;
                FitDef d;
                d.valid = true;
                d.bigEndian = data[p + 1] == 1;
                d.global = static_cast<int>(readUnsigned(data + p + 2, 2, d.bigEndian));
                const int count = data[p + 4];
                p += 5;
                for (int i = 0; i < count && p + 3 <= end; ++i, p += 3) d.fields.push_back({data[p], data[p + 1], data[p + 2]});
                if (dev && p < end) {
                    const int devCount = data[p++];
                    for (int i = 0; i < devCount && p + 3 <= end; ++i, p += 3) d.devBytes += data[p + 1];
                }
                defs[local] = std::move(d);
                continue;
            }
            const FitDef& d = defs[local];
            if (!d.valid) throw std::runtime_error("FIT data message before its definition: the file is damaged");
            std::map<int, std::uint64_t> v;
            std::map<int, int> size;
            for (const auto& f : d.fields) {
                if (p + static_cast<std::size_t>(f.size) > end) {
                    p = end;
                    break;
                }
                if (f.size <= 8) v[f.num] = readUnsigned(data + p, f.size, d.bigEndian);
                size[f.num] = f.size;
                p += static_cast<std::size_t>(f.size);
            }
            p += static_cast<std::size_t>(d.devBytes);
            double ts = compressedTs;
            if (auto it = v.find(253); it != v.end() && it->second != 0xFFFFFFFFu) ts = static_cast<double>(it->second);
            if (!std::isnan(ts)) lastTs = ts;
            if (d.global == 18 || d.global == 12) {  // session / sport
                const int field = d.global == 18 ? 5 : 0;
                if (auto it = v.find(field); it != v.end() && it->second != 0xFF && sport < 0) sport = static_cast<int>(it->second);
                continue;
            }
            if (d.global != 20) continue;  // only records carry the track
            auto get = [&](int num, std::uint64_t invalid, int wantSize) -> std::optional<std::uint64_t> {
                auto it = v.find(num);
                if (it == v.end() || size[num] != wantSize || it->second == invalid) return std::nullopt;
                return it->second;
            };
            const auto lat = get(0, 0x7FFFFFFF, 4), lon = get(1, 0x7FFFFFFF, 4);
            if (!lat || !lon) {
                ++noFix;
                continue;
            }
            Coord c;
            const double semi = 180.0 / 2147483648.0;
            c.y = static_cast<std::int32_t>(static_cast<std::uint32_t>(*lat)) * semi;
            c.x = static_cast<std::int32_t>(static_cast<std::uint32_t>(*lon)) * semi;
            if (auto e = get(78, 0xFFFFFFFF, 4)) c.z = static_cast<double>(*e) / 5 - 500;
            else if (auto a = get(2, 0xFFFF, 2)) c.z = static_cast<double>(*a) / 5 - 500;
            if (!std::isnan(ts)) c.t = ts + kFitEpoch;
            if (auto hr = get(3, 0xFF, 1)) {
                hrSum += static_cast<double>(*hr);
                hrMax = std::max(hrMax, static_cast<double>(*hr));
                ++hrN;
            }
            if (auto cad = get(4, 0xFF, 1)) {
                cadSum += static_cast<double>(*cad);
                ++cadN;
            }
            if (auto dist = get(5, 0xFFFFFFFF, 4)) fitDistance = static_cast<double>(*dist) / 100;
            track.push_back(c);
        }
        at = at + headerSize + dataSize + 2;  // + the file CRC
    }
    if (track.empty()) throw std::runtime_error("the FIT file has no positions (an indoor activity, or not an activity file)");
    if (noFix && warnings) warnings->push_back(std::to_string(noFix) + " records without a GPS position were skipped");
    Feature f;
    f.geom = Geometry::line(std::move(track));
    if (sport >= 0) f.props["sport"] = Prop::string(sportName(sport));
    if (hrN) {
        f.props["avg_hr"] = Prop::number(std::round(hrSum / static_cast<double>(hrN)));
        f.props["max_hr"] = Prop::number(hrMax);
    }
    if (cadN) f.props["avg_cadence"] = Prop::number(std::round(cadSum / static_cast<double>(cadN)));
    if (!std::isnan(fitDistance)) f.props["device_distance_m"] = Prop::number(std::round(fitDistance));
    return {f};
}

// ---- splits ------------------------------------------------------------------------------

std::vector<Split> trackSplits(const std::vector<const Line*>& segments, double every, double hysteresis) {
    if (!(every > 0)) throw std::runtime_error("the split length must be more than 0 metres");
    std::vector<Split> out;
    Split cur;
    double D = 0, ref = kNaN, startT = kNaN;
    bool started = false;
    auto climb = [&](double z) {
        if (std::isnan(z)) return;
        if (std::isnan(ref)) {
            ref = z;
        } else if (z - ref >= hysteresis) {
            cur.gain += z - ref;
            ref = z;
        } else if (ref - z >= hysteresis) {
            cur.loss += ref - z;
            ref = z;
        }
    };
    auto close = [&](double to, const Coord& at) {
        cur.to = to;
        cur.distance = to - cur.from;
        cur.endEle = at.z;
        cur.duration = std::isnan(startT) || std::isnan(at.t) ? kNaN : at.t - startT;
        out.push_back(cur);
        cur = Split{};
        cur.from = to;
        cur.startEle = at.z;
        startT = at.t;
    };
    Coord prev;
    for (const Line* seg : segments) {
        for (std::size_t i = 0; i < seg->size(); ++i) {
            const Coord& c = (*seg)[i];
            if (!started) {
                started = true;
                cur.startEle = c.z;
                startT = c.t;
                climb(c.z);
                prev = c;
                continue;
            }
            if (i == 0) {  // a new segment: no distance across the gap
                prev = c;
                climb(c.z);
                continue;
            }
            double d = geodesicDistance(prev.x, prev.y, c.x, c.y);
            while (d > 0 && D + d >= cur.from + every) {
                const double need = cur.from + every - D;
                const Coord b = between(prev, c, need / d);
                climb(b.z);
                D += need;
                d -= need;
                close(D, b);
                prev = b;
            }
            D += d;
            climb(c.z);
            prev = c;
        }
    }
    if (started && D > cur.from + 1e-6) close(D, prev);
    return out;
}

// ---- cleaning ----------------------------------------------------------------------------

Line smoothLine(const Line& l, int window, bool positions, bool elevation) {
    if (l.size() < 3 || window < 2) return l;
    const std::ptrdiff_t h = window / 2, n = static_cast<std::ptrdiff_t>(l.size());
    Line out = l;
    for (std::ptrdiff_t i = 1; i + 1 < n; ++i) {  // the ends stay where they are
        const std::ptrdiff_t a = std::max<std::ptrdiff_t>(0, i - h), b = std::min(n - 1, i + h);
        double sx = 0, sy = 0, sz = 0;
        int k = 0, kz = 0;
        for (std::ptrdiff_t j = a; j <= b; ++j) {
            sx += l[static_cast<std::size_t>(j)].x;
            sy += l[static_cast<std::size_t>(j)].y;
            ++k;
            if (!std::isnan(l[static_cast<std::size_t>(j)].z)) {
                sz += l[static_cast<std::size_t>(j)].z;
                ++kz;
            }
        }
        Coord& c = out[static_cast<std::size_t>(i)];
        if (positions) {
            c.x = sx / k;
            c.y = sy / k;
        }
        if (elevation && kz && !std::isnan(c.z)) c.z = sz / kz;
    }
    return out;
}

std::size_t removeSpikes(Line& l, double maxSpeed) {
    if (l.size() < 3 || !(maxSpeed > 0)) return 0;
    Line keep{l.front()};
    for (std::size_t i = 1; i < l.size(); ++i) {
        const Coord& a = keep.back();
        const Coord& c = l[i];
        const double dt = c.t - a.t;
        if (std::isfinite(dt) && dt > 0 && geodesicDistance(a.x, a.y, c.x, c.y) / dt > maxSpeed) continue;
        keep.push_back(c);
    }
    const std::size_t removed = l.size() - keep.size();
    l = std::move(keep);
    return removed;
}

Line trimLine(const Line& l, double from, double to) {
    if (l.size() < 2) return l;
    const auto d = cumulative(l);
    from = std::clamp(from, 0.0, d.back());
    to = std::clamp(to, from, d.back());
    Line out;
    for (std::size_t i = 0; i + 1 < l.size(); ++i) {
        const double a = d[i], b = d[i + 1];
        if (b < from || a > to) continue;
        if (out.empty()) out.push_back(a >= from ? l[i] : between(l[i], l[i + 1], b > a ? (from - a) / (b - a) : 0));
        if (b <= to) {
            out.push_back(l[i + 1]);
        } else {
            out.push_back(between(l[i], l[i + 1], b > a ? (to - a) / (b - a) : 0));
            break;
        }
    }
    // drop zero-length repeats at the cut points
    Line clean;
    for (const auto& c : out) {
        if (clean.empty() || clean.back().x != c.x || clean.back().y != c.y) clean.push_back(c);
    }
    return clean;
}

double distanceAtTime(const Line& l, double t) {
    if (l.empty() || std::isnan(t)) return kNaN;
    const auto d = cumulative(l);
    double firstT = kNaN, lastT = kNaN;
    for (const auto& c : l) {
        if (std::isnan(c.t)) continue;
        if (std::isnan(firstT)) firstT = c.t;
        lastT = c.t;
    }
    if (std::isnan(firstT)) return kNaN;
    if (t <= firstT) return 0;
    if (t >= lastT) return d.back();
    for (std::size_t i = 0; i + 1 < l.size(); ++i) {
        const double a = l[i].t, b = l[i + 1].t;
        if (std::isnan(a) || std::isnan(b) || t < a || t > b) continue;
        return b > a ? d[i] + (d[i + 1] - d[i]) * (t - a) / (b - a) : d[i];
    }
    return kNaN;
}

std::vector<Line> splitByGaps(const Line& l, double gapSeconds, double gapMetres) {
    std::vector<Line> out;
    Line cur;
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (i) {
            const double dt = l[i].t - l[i - 1].t;
            const bool timeGap = gapSeconds > 0 && std::isfinite(dt) && dt > gapSeconds;
            const bool distGap = gapMetres > 0 && geodesicDistance(l[i - 1].x, l[i - 1].y, l[i].x, l[i].y) > gapMetres;
            if (timeGap || distGap) {
                if (cur.size() >= 2) out.push_back(std::move(cur));
                cur.clear();
            }
        }
        cur.push_back(l[i]);
    }
    if (cur.size() >= 2) out.push_back(std::move(cur));
    return out;
}

std::vector<Coord> samplePoints(const Line& l, double spacing) {
    std::vector<Coord> out;
    if (l.empty()) return out;
    out.push_back(l.front());
    if (!(spacing > 0)) return out;
    double next = spacing, D = 0;
    for (std::size_t i = 1; i < l.size(); ++i) {
        const double d = geodesicDistance(l[i - 1].x, l[i - 1].y, l[i].x, l[i].y);
        while (d > 0 && D + d >= next) {
            out.push_back(between(l[i - 1], l[i], (next - D) / d));
            next += spacing;
            if (out.size() > 5000000) return out;
        }
        D += d;
    }
    if (l.size() > 1) out.push_back(l.back());
    return out;
}

}  // namespace wetwaresuite::maps
