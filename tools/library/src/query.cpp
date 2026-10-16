// The search language. Words are ANDed together:
//   sunset                   free text in the file name, tags or notes
//   "red car"                a quoted phrase
//   *.png  img_??.jpg        a wildcard on the file name
//   type:image,video         detected type (any of a comma list); also mesh and picture groups
//   ext:png  tag:x  label:red  format:jpeg  name:*.png  path:textures  in:/abs/folder
//   has:tags|notes|rating|label|hash|phash  collection:name  hash:0123abcd
//   rating>=3  size>10mb  width>=1920  height<1080  duration>60  pages>10  triangles>1000
//   modified<7d (within 7 days)  modified>2024-01-01  indexed<1h
//   orientation:landscape|portrait|square
//   -tag:used  not:type:video  (negation)
//   sort:-size  limit:20
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <set>

#include "achclwss/core/cli.hpp"
#include "achclwss/library/library.hpp"

namespace achclwss::library {

namespace {

// Natural order: walk_2 before walk_10.
bool naturalLess(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            std::size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            std::string na = a.substr(i, i2 - i), nb = b.substr(j, j2 - j);
            na.erase(0, std::min(na.find_first_not_of('0'), na.size() - 1));
            nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size() - 1));
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
            i = i2;
            j = j2;
            continue;
        }
        char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

bool globMatch(const char* pat, const char* str) {
    // iterative * and ? matcher, case-insensitive
    const char *star = nullptr, *resume = nullptr;
    while (*str) {
        if (*pat == '?' || std::tolower(static_cast<unsigned char>(*pat)) == std::tolower(static_cast<unsigned char>(*str))) {
            ++pat;
            ++str;
        } else if (*pat == '*') {
            star = pat++;
            resume = str;
        } else if (star) {
            pat = star + 1;
            str = ++resume;
        } else {
            return false;
        }
    }
    while (*pat == '*') ++pat;
    return *pat == 0;
}

bool contains(const std::string& haystack, const std::string& lowerNeedle) {
    return lower(haystack).find(lowerNeedle) != std::string::npos;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

enum class Op { Lt, Le, Gt, Ge, Eq, Ne };

bool compare(double a, Op op, double b) {
    switch (op) {
        case Op::Lt: return a < b;
        case Op::Le: return a <= b;
        case Op::Gt: return a > b;
        case Op::Ge: return a >= b;
        case Op::Eq: return a == b;
        case Op::Ne: return a != b;
    }
    return false;
}

Op flip(Op op) {
    switch (op) {
        case Op::Lt: return Op::Gt;
        case Op::Le: return Op::Ge;
        case Op::Gt: return Op::Lt;
        case Op::Ge: return Op::Le;
        default: return op;
    }
}

double parseNumber(const std::string& text, const std::string& what) {
    try {
        std::size_t used = 0;
        double v = std::stod(text, &used);
        if (used == text.size()) return v;
    } catch (const std::exception&) {
    }
    throw ParamError("'" + text + "' is not a number in " + what);
}

double parseDuration(const std::string& text) {
    std::string t = lower(text);
    double mult = 1;
    if (!t.empty() && (t.back() == 's' || t.back() == 'm' || t.back() == 'h')) {
        mult = t.back() == 'h' ? 3600 : t.back() == 'm' ? 60 : 1;
        t.pop_back();
    }
    return parseNumber(t, "duration") * mult;
}

// Relative ages ("7d") or absolute dates ("2024-01-31", "2024-01-31T12:00").
struct TimeValue {
    bool relative = false;
    double value = 0;  // seconds of age, or seconds since 1970
};

std::int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

TimeValue parseTime(const std::string& text) {
    std::string t = lower(text);
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0;
    if (t.size() >= 10 && t[4] == '-' && t[7] == '-' && std::sscanf(t.c_str(), "%d-%d-%d", &y, &mo, &d) == 3) {
        if (t.size() > 10) std::sscanf(t.c_str() + 11, "%d:%d", &hh, &mm);
        return {false, static_cast<double>(daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + hh * 3600 + mm * 60)};
    }
    if (t.empty()) throw ParamError("empty time in query");
    double mult = 86400;  // plain numbers are days
    char unit = t.back();
    if (std::isalpha(static_cast<unsigned char>(unit))) {
        t.pop_back();
        switch (unit) {
            case 's': mult = 1; break;
            case 'h': mult = 3600; break;
            case 'd': mult = 86400; break;
            case 'w': mult = 7 * 86400; break;
            case 'm': mult = 30 * 86400; break;
            case 'y': mult = 365 * 86400; break;
            default: throw ParamError("unknown time unit '" + std::string(1, unit) + "' (use s, h, d, w, m or y)");
        }
    }
    return {true, parseNumber(t, "time") * mult};
}

bool inGroup(const std::string& type, const std::string& ext, const std::string& want) {
    if (want == "mesh") {
        static const std::set<std::string> meshes = {"stl", "obj", "ply", "off", "gltf", "glb", "3mf", "fbx", "dae", "usd", "usdz", "blend"};
        return meshes.count(ext) > 0;
    }
    if (want == "picture") return type == "image" || type == "raw" || type == "texture";
    return type == want;
}

}  // namespace

std::uint64_t parseSize(const std::string& text) {
    std::string t = lower(text);
    if (!t.empty() && t.back() == 'b') t.pop_back();  // "mb" -> "m", "b" alone -> ""
    double mult = 1;
    if (!t.empty()) {
        switch (t.back()) {
            case 'k': mult = 1024.0; break;
            case 'm': mult = 1024.0 * 1024; break;
            case 'g': mult = 1024.0 * 1024 * 1024; break;
            case 't': mult = 1024.0 * 1024 * 1024 * 1024; break;
            default: mult = 0;
        }
        if (mult != 0) t.pop_back();
        else mult = 1;
    }
    double v = parseNumber(t, "size");
    if (v < 0) throw ParamError("sizes can't be negative");
    return static_cast<std::uint64_t>(std::llround(v * mult));
}

std::string formatSize(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        ++u;
    }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    return buf;
}

struct Query {
    using Pred = std::function<bool(const Item&)>;
    std::vector<Pred> preds;
    std::string sort;
    std::int64_t limit = -1;
};

namespace {

Query parse(const Catalog& catalog, const std::string& text, std::int64_t now, int depth);

Query::Pred term(const Catalog& catalog, const std::string& token, std::int64_t now, int depth) {
    // comparisons: field op value
    static const std::vector<std::pair<std::string, Op>> ops = {
        {">=", Op::Ge}, {"<=", Op::Le}, {"!=", Op::Ne}, {">", Op::Gt}, {"<", Op::Lt}, {"=", Op::Eq}};
    std::size_t opPos = std::string::npos;
    std::string opText;
    Op op = Op::Eq;
    for (std::size_t i = 0; i < token.size() && opPos == std::string::npos; ++i) {
        if (token[i] == ':' ) break;
        for (const auto& [t, o] : ops) {
            if (token.compare(i, t.size(), t) == 0) {
                opPos = i;
                opText = t;
                op = o;
                break;
            }
        }
    }
    std::string field, value;
    if (opPos != std::string::npos && opPos > 0) {
        field = lower(token.substr(0, opPos));
        value = token.substr(opPos + opText.size());
    } else if (auto colon = token.find(':'); colon != std::string::npos && colon > 0) {
        field = lower(token.substr(0, colon));
        value = token.substr(colon + 1);
        // rating:>=3 works too
        for (const auto& [t, o] : ops) {
            if (value.compare(0, t.size(), t) == 0 && field != "name" && field != "path" && field != "in") {
                op = o;
                value = value.substr(t.size());
                opText = t;
                break;
            }
        }
    }

    using Num = std::function<double(const Item&)>;
    static const std::map<std::string, Num> numeric = {
        {"rating", [](const Item& i) { return static_cast<double>(i.rating); }},
        {"width", [](const Item& i) { return static_cast<double>(i.width); }},
        {"height", [](const Item& i) { return static_cast<double>(i.height); }},
        {"pages", [](const Item& i) { return static_cast<double>(i.pages); }},
        {"triangles", [](const Item& i) { return static_cast<double>(i.triangles); }},
        {"megapixels", [](const Item& i) { return static_cast<double>(i.width) * i.height / 1e6; }},
        {"mp", [](const Item& i) { return static_cast<double>(i.width) * i.height / 1e6; }},
    };

    if (!field.empty() && (!opText.empty() || numeric.count(field) || field == "size" || field == "duration" ||
                           field == "modified" || field == "indexed")) {
        if (field == "size") {
            double v = static_cast<double>(parseSize(value));
            return [op, v](const Item& i) { return compare(static_cast<double>(i.size), op, v); };
        }
        if (field == "duration") {
            double v = parseDuration(value);
            return [op, v](const Item& i) { return compare(i.duration, op, v); };
        }
        if (field == "modified" || field == "indexed") {
            TimeValue tv = parseTime(value);
            bool mod = field == "modified";
            // age < 7d means time > now - 7d
            double threshold = tv.relative ? static_cast<double>(now) - tv.value : tv.value;
            Op o = tv.relative ? flip(op) : op;
            return [mod, o, threshold](const Item& i) { return compare(static_cast<double>(mod ? i.mtime : i.indexed), o, threshold); };
        }
        auto it = numeric.find(field);
        if (it == numeric.end()) {
            throw ParamError("can't compare '" + field + "' (numbers: rating, size, width, height, duration, pages, "
                             "triangles, megapixels, modified, indexed)");
        }
        double v = parseNumber(value, field);
        auto get = it->second;
        return [get, op, v](const Item& i) { return compare(get(i), op, v); };
    }

    if (!field.empty()) {
        const std::vector<std::string> any = splitList(lower(value));
        auto anyOf = [any](const std::function<bool(const std::string&)>& f) {
            for (const auto& a : any) {
                if (f(a)) return true;
            }
            return false;
        };
        if (field == "type" || field == "kind") {
            return [any](const Item& i) {
                for (const auto& a : any) {
                    if (inGroup(i.type, i.ext, a)) return true;
                }
                return false;
            };
        }
        if (field == "ext") {
            return [any](const Item& i) { return std::find(any.begin(), any.end(), i.ext) != any.end(); };
        }
        if (field == "tag") {
            return [any](const Item& i) {
                for (const auto& a : any) {
                    if (a.find_first_of("*?") != std::string::npos) {
                        for (const auto& t : i.tags) {
                            if (globMatch(a.c_str(), t.c_str())) return true;
                        }
                    } else if (i.hasTag(a)) {
                        return true;
                    }
                }
                return false;
            };
        }
        if (field == "label" || field == "color" || field == "colour") {
            return [any](const Item& i) { return std::find(any.begin(), any.end(), i.label) != any.end(); };
        }
        if (field == "format") {
            return [anyOf](const Item& i) {
                const std::string f = lower(i.format);
                return anyOf([&](const std::string& a) { return f.find(a) != std::string::npos; });
            };
        }
        if (field == "name") {
            const std::string pat = value;
            return [pat](const Item& i) {
                return pat.find_first_of("*?") != std::string::npos ? globMatch(pat.c_str(), i.name().c_str())
                                                                     : contains(i.name(), lower(pat));
            };
        }
        if (field == "path") {
            const std::string pat = lower(value);
            return [pat](const Item& i) {
                return pat.find_first_of("*?") != std::string::npos ? globMatch(pat.c_str(), i.path.c_str()) : contains(i.path, pat);
            };
        }
        if (field == "in" || field == "folder" || field == "dir") {
            std::string base = normPath(value);
            return [base](const Item& i) {
                return i.path.size() > base.size() && i.path.compare(0, base.size(), base) == 0 && i.path[base.size()] == '/';
            };
        }
        if (field == "hash") {
            const std::string h = lower(value);
            return [h](const Item& i) { return i.hash.rfind(h, 0) == 0; };
        }
        if (field == "notes" || field == "note") {
            const std::string n = lower(value);
            return [n](const Item& i) { return contains(i.notes, n); };
        }
        if (field == "has") {
            return [any](const Item& i) {
                for (const auto& a : any) {
                    bool ok = a == "tags" || a == "tag" ? !i.tags.empty()
                            : a == "notes" || a == "note" ? !i.notes.empty()
                            : a == "rating" ? i.rating > 0
                            : a == "label" ? !i.label.empty()
                            : a == "hash" ? !i.hash.empty()
                            : a == "phash" || a == "dhash" ? !i.dhash.empty()
                            : a == "size" || a == "dimensions" ? i.width > 0
                            : a == "duration" ? i.duration > 0
                            : false;
                    if (ok) return true;
                }
                return false;
            };
        }
        if (field == "orientation") {
            return [any](const Item& i) {
                if (i.width == 0 || i.height == 0) return false;
                const std::string o = i.width > i.height ? "landscape" : i.width < i.height ? "portrait" : "square";
                return std::find(any.begin(), any.end(), o) != any.end();
            };
        }
        if (field == "collection" || field == "c") {
            if (depth > 8) throw ParamError("collections nest too deeply (a collection includes itself?)");
            std::vector<Query::Pred> alts;
            for (const auto& name : splitList(value)) {
                auto it = catalog.collections.find(name);
                if (it == catalog.collections.end()) throw ParamError("no collection called '" + name + "'");
                const Collection& col = it->second;
                if (col.smart()) {
                    auto sub = std::make_shared<Query>(parse(catalog, col.query, now, depth + 1));
                    alts.push_back([sub](const Item& i) {
                        for (const auto& p : sub->preds) {
                            if (!p(i)) return false;
                        }
                        return true;
                    });
                } else {
                    auto set = std::make_shared<std::set<std::string>>(col.paths.begin(), col.paths.end());
                    alts.push_back([set](const Item& i) { return set->count(i.path) > 0; });
                }
            }
            return [alts](const Item& i) {
                for (const auto& a : alts) {
                    if (a(i)) return true;
                }
                return false;
            };
        }
        throw ParamError("unknown search field '" + field + "' (fields: type, ext, tag, label, format, name, path, in, "
                         "has, notes, hash, orientation, collection, sort, limit, and comparisons like rating>=3)");
    }

    // free text: wildcard on the name, or substring in name/tags/notes
    if (token.find_first_of("*?") != std::string::npos) {
        return [token](const Item& i) { return globMatch(token.c_str(), i.name().c_str()); };
    }
    const std::string needle = lower(token);
    return [needle](const Item& i) {
        if (contains(i.name(), needle) || contains(i.notes, needle)) return true;
        for (const auto& t : i.tags) {
            if (contains(t, needle)) return true;
        }
        return false;
    };
}

Query parse(const Catalog& catalog, const std::string& text, std::int64_t now, int depth) {
    Query q;
    for (std::string token : cli::split(text)) {
        bool negate = false;
        if (token.size() > 1 && token[0] == '-' && !std::isdigit(static_cast<unsigned char>(token[1]))) {
            negate = true;
            token.erase(0, 1);
        } else if (lower(token).rfind("not:", 0) == 0 && token.size() > 4) {
            negate = true;
            token.erase(0, 4);
        }
        std::string lt = lower(token);
        if (lt.rfind("sort:", 0) == 0) {
            q.sort = lt.substr(5);
            continue;
        }
        if (lt.rfind("limit:", 0) == 0) {
            q.limit = static_cast<std::int64_t>(parseNumber(lt.substr(6), "limit"));
            continue;
        }
        if (lt == "and") continue;
        auto pred = term(catalog, token, now, depth);
        if (negate) q.preds.push_back([pred](const Item& i) { return !pred(i); });
        else q.preds.push_back(std::move(pred));
    }
    return q;
}

std::int64_t nowSeconds(std::int64_t now) {
    if (now) return now;
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void sortItems(std::vector<const Item*>& items, const std::string& sortSpec) {
    std::string key = sortSpec.empty() ? "path" : lower(sortSpec);
    bool desc = false;
    if (!key.empty() && (key[0] == '-' || key[0] == '+')) {
        desc = key[0] == '-';
        key.erase(0, 1);
    }
    if (key.size() > 5 && key.compare(key.size() - 5, 5, ":desc") == 0) {
        desc = true;
        key.resize(key.size() - 5);
    }
    using Less = std::function<bool(const Item*, const Item*)>;
    static const std::map<std::string, Less> keys = {
        {"path", [](const Item* a, const Item* b) { return naturalLess(a->path, b->path); }},
        {"name", [](const Item* a, const Item* b) { return naturalLess(a->name(), b->name()); }},
        {"size", [](const Item* a, const Item* b) { return a->size < b->size; }},
        {"modified", [](const Item* a, const Item* b) { return a->mtime < b->mtime; }},
        {"date", [](const Item* a, const Item* b) { return a->mtime < b->mtime; }},
        {"indexed", [](const Item* a, const Item* b) { return a->indexed < b->indexed; }},
        {"rating", [](const Item* a, const Item* b) { return a->rating < b->rating; }},
        {"width", [](const Item* a, const Item* b) { return a->width < b->width; }},
        {"height", [](const Item* a, const Item* b) { return a->height < b->height; }},
        {"pixels", [](const Item* a, const Item* b) { return static_cast<double>(a->width) * a->height < static_cast<double>(b->width) * b->height; }},
        {"duration", [](const Item* a, const Item* b) { return a->duration < b->duration; }},
        {"pages", [](const Item* a, const Item* b) { return a->pages < b->pages; }},
        {"type", [](const Item* a, const Item* b) { return a->type < b->type; }},
        {"ext", [](const Item* a, const Item* b) { return a->ext < b->ext; }},
        {"label", [](const Item* a, const Item* b) { return a->label < b->label; }},
    };
    auto it = keys.find(key);
    if (it == keys.end()) {
        throw ParamError("can't sort by '" + key + "' (path, name, size, modified, indexed, rating, width, height, pixels, "
                         "duration, pages, type, ext, label; prefix - for descending)");
    }
    auto less = it->second;
    std::stable_sort(items.begin(), items.end(), [&](const Item* a, const Item* b) {
        if (desc ? less(b, a) : less(a, b)) return true;
        if (desc ? less(a, b) : less(b, a)) return false;
        return a->path < b->path;
    });
}

}  // namespace

SearchResult search(const Catalog& catalog, const std::string& query, const SearchOptions& options, std::int64_t now) {
    Query q = parse(catalog, query, nowSeconds(now), 0);
    SearchResult r;
    std::vector<const Item*> hits;
    for (const auto& item : catalog.items) {
        bool ok = true;
        for (const auto& p : q.preds) {
            if (!p(item)) {
                ok = false;
                break;
            }
        }
        if (ok) hits.push_back(&item);
    }
    sortItems(hits, options.sort.empty() ? q.sort : options.sort);
    r.total = hits.size();
    std::int64_t limit = options.limit >= 0 ? options.limit : q.limit;
    std::size_t start = static_cast<std::size_t>(std::max<std::int64_t>(0, options.offset));
    if (start > hits.size()) start = hits.size();
    std::size_t end = limit >= 0 ? std::min(hits.size(), start + static_cast<std::size_t>(limit)) : hits.size();
    r.items.assign(hits.begin() + static_cast<std::ptrdiff_t>(start), hits.begin() + static_cast<std::ptrdiff_t>(end));
    return r;
}

bool matches(const Catalog& catalog, const Item& item, const std::string& query, std::int64_t now) {
    Query q = parse(catalog, query, nowSeconds(now), 0);
    for (const auto& p : q.preds) {
        if (!p(item)) return false;
    }
    return true;
}

}  // namespace achclwss::library
