// Search, backlinks, outgoing and unresolved links, orphans and the graph.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

#include "achclwss/raster/codec.hpp"
#include "internal.hpp"

namespace achclwss::notes::detail {

using nlohmann::json;

namespace {

std::string lineAt(const std::string& text, int line) {
    auto lines = splitLines(text);
    if (line < 1 || line > static_cast<int>(lines.size())) return {};
    return trim(lines[static_cast<std::size_t>(line - 1)]);
}

std::string clip(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    std::size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

void registerSearch(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.search", "Search notes",
             "Full-text search over every note, ranked (BM25, title and alias matches count extra) with snippets that "
             "bold the matches. Words are ANDed; \"exact phrase\"; -word or -\"phrase\" excludes; prefix* matches word "
             "starts; tag:project (or #project, nested included), path:Projects/ (folder or path text) and file:name "
             "filter; any of these can be negated with -. A query of only filters lists matching notes, newest first.",
             {vaultParam(), {"query", ParamType::String, "the search, e.g. 'budget \"next quarter\" -draft tag:work'", true},
              limitParam(20), boolParamSpec("snippets", "include a snippet of each match", true),
              jsonParam("path, title, score, line and snippet of each hit")},
             false, true},
            reader(st, [](Vault& v, const Params& p) {
                auto limit = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "limit", 20)));
                SearchResults r = v.search(stringParam(p, "query"), limit, boolParam(p, "snippets", true));
                std::string msg = std::to_string(r.total) + (r.total == 1 ? " match" : " matches");
                if (r.hits.size() < r.total) msg += ", showing " + std::to_string(r.hits.size());
                json items = json::array();
                std::string paths;
                for (const auto& h : r.hits) {
                    char score[32];
                    std::snprintf(score, sizeof score, "%.2f", h.score);
                    msg += "\n" + h.note->rel + (h.note->title != h.note->name ? "  \"" + h.note->title + "\"" : "") + (h.score > 0 ? std::string("  (") + score + ")" : "");
                    if (!h.snippet.empty()) msg += "\n    " + (h.line ? std::to_string(h.line) + ": " : "") + h.snippet;
                    items.push_back({{"path", h.note->rel}, {"title", h.note->title}, {"score", h.score}, {"line", h.line}, {"snippet", h.snippet}});
                    paths += (paths.empty() ? "" : "\n") + h.note->rel;
                }
                Result res = Result::success(msg);
                res.with("count", static_cast<std::int64_t>(r.hits.size())).with("total", static_cast<std::int64_t>(r.total)).with("notes", paths);
                if (boolParam(p, "json")) res.with("items", items.dump(-1, ' ', false, json::error_handler_t::replace));
                return res;
            }));
}

void registerLinks(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.backlinks", "Backlinks",
             "Every note that links to (or embeds) a note, with the line each link is on. mentions=true also finds "
             "unlinked mentions: notes that name it (its name or an alias) without linking.",
             {vaultParam(), noteParam("the note (or attachment) to find links to"), boolParamSpec("mentions", "also list unlinked mentions"),
              jsonParam("from, line and context of each link")},
             false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::string rel = needNote(v, p, "note", false);
                auto links = v.backlinks(rel);
                std::set<std::string> fromNotes;
                std::string msg;
                json items = json::array();
                std::map<std::string, std::string> cache;
                for (const auto& [from, link] : links) {
                    fromNotes.insert(from->rel);
                    if (!cache.count(from->rel)) cache[from->rel] = v.read(from->rel);
                    std::string ctx = clip(lineAt(cache[from->rel], link->line), 160);
                    msg += "\n" + from->rel + ":" + std::to_string(link->line) + (link->embed ? " (embed)" : "") + "  " + ctx;
                    items.push_back({{"from", from->rel}, {"line", link->line}, {"embed", link->embed}, {"context", ctx}});
                }
                msg = plural(links.size(), "link") + " to " + rel + " from " + plural(fromNotes.size(), "note") + msg;
                std::size_t mentions = 0;
                if (boolParam(p, "mentions") && v.isNote(rel)) {
                    const Note* target = v.note(rel);
                    std::vector<std::string> names = {target->name};
                    for (const auto& a : target->aliases) names.push_back(a);
                    std::string mtext;
                    for (const auto& [orel, n] : v.notes) {
                        if (orel == rel || fromNotes.count(orel)) continue;
                        for (const auto& name : names) {
                            auto toks = tokenize(name);
                            bool all = !toks.empty();
                            for (const auto& t : toks) all = all && n.terms.count(t);
                            if (!all) continue;
                            std::string text = v.read(orel);
                            std::string body = lower(bodyOf(text));
                            auto pos = body.find(lower(name));
                            if (pos == std::string::npos) continue;
                            std::size_t bodyStart = text.size() - bodyOf(text).size();
                            int line = static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(bodyStart + pos), '\n')) + 1;
                            mtext += "\n" + orel + ":" + std::to_string(line) + "  " + clip(lineAt(text, line), 160);
                            ++mentions;
                            break;
                        }
                    }
                    msg += "\nunlinked mentions: " + std::to_string(mentions) + mtext;
                }
                Result r = Result::success(msg);
                r.with("count", static_cast<std::int64_t>(links.size())).with("notes", static_cast<std::int64_t>(fromNotes.size())).with("mentions", static_cast<std::int64_t>(mentions));
                if (boolParam(p, "json")) r.with("items", items.dump(-1, ' ', false, json::error_handler_t::replace));
                return r;
            }));

    reg.add({"notes.links", "Outgoing links",
             "The links and embeds in a note, each with the note or file it resolves to (or 'unresolved').",
             {vaultParam(), noteParam(), jsonParam("each link's target, resolved path, line, embed and alias")}, false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::string rel = needNote(v, p);
                const Note* n = v.note(rel);
                std::string msg;
                json items = json::array();
                std::size_t unresolved = 0;
                for (const auto& l : n->links) {
                    std::string to = v.resolve(l, rel);
                    if (to.empty()) ++unresolved;
                    std::string shown = l.wiki ? std::string(l.embed ? "!" : "") + "[[" + l.target + (l.heading.empty() ? "" : "#" + l.heading) + (l.alias.empty() ? "" : "|" + l.alias) + "]]"
                                               : std::string(l.embed ? "!" : "") + "[" + l.alias + "](" + l.target + ")";
                    msg += "\n" + std::to_string(l.line) + ": " + shown + " -> " + (to.empty() ? "unresolved" : to);
                    items.push_back({{"target", l.target}, {"heading", l.heading}, {"alias", l.alias}, {"resolved", to}, {"line", l.line}, {"embed", l.embed}, {"wiki", l.wiki}});
                }
                msg = plural(n->links.size(), "link") + " in " + rel + (unresolved ? " (" + std::to_string(unresolved) + " unresolved)" : "") + msg;
                Result r = Result::success(msg);
                r.with("count", static_cast<std::int64_t>(n->links.size())).with("unresolved", static_cast<std::int64_t>(unresolved));
                if (boolParam(p, "json")) r.with("items", items.dump(-1, ' ', false, json::error_handler_t::replace));
                return r;
            }));

    reg.add({"notes.links.unresolved", "Unresolved links",
             "Links that point to notes or files that don't exist yet, grouped by target, most-wanted first, with the "
             "notes that mention each (create one with notes.create note=<target>).",
             {vaultParam(), limitParam(100), jsonParam("each target with its source notes")}, false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::map<std::string, std::pair<std::string, std::vector<std::string>>> missing;  // lower target -> (as written, sources)
                for (const auto& [rel, n] : v.notes) {
                    for (const auto& l : n.links) {
                        if (l.target.empty() || !v.resolve(l, rel).empty()) continue;
                        auto& m = missing[lower(l.target)];
                        if (m.first.empty()) m.first = l.target;
                        if (std::find(m.second.begin(), m.second.end(), rel) == m.second.end()) m.second.push_back(rel);
                    }
                }
                std::vector<std::pair<std::string, std::vector<std::string>>> list;
                for (auto& [k, m] : missing) list.emplace_back(m.first, m.second);
                std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
                const std::size_t total = list.size();
                auto limit = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "limit", 100)));
                if (limit && list.size() > limit) list.resize(limit);
                std::string msg = plural(total, "unresolved target");
                json items = json::array();
                for (const auto& [t, from] : list) {
                    msg += "\n" + t + "  <- ";
                    for (std::size_t i = 0; i < from.size() && i < 5; ++i) msg += (i ? ", " : "") + from[i];
                    if (from.size() > 5) msg += ", +" + std::to_string(from.size() - 5);
                    items.push_back({{"target", t}, {"from", from}});
                }
                Result r = Result::success(msg);
                r.with("count", static_cast<std::int64_t>(total));
                if (boolParam(p, "json")) r.with("items", items.dump(-1, ' ', false, json::error_handler_t::replace));
                return r;
            }));

    reg.add({"notes.orphans", "Orphan notes",
             "Notes nothing links to and that link to nothing (incoming_only=true: just nothing links to them). Notes "
             "in the templates folder are left out unless templates=true.",
             {vaultParam(), boolParamSpec("incoming_only", "count notes with outgoing links too, as long as nothing links to them"),
              boolParamSpec("templates", "include notes in the templates folder")},
             false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::vector<std::string> out;
                const std::string tf = v.settings.templatesFolder;
                for (const auto& [rel, n] : v.notes) {
                    if (!boolParam(p, "templates") && !tf.empty() && startsWith(rel, tf + "/")) continue;
                    if (!v.backlinks(rel).empty()) continue;
                    if (!boolParam(p, "incoming_only")) {
                        bool links = false;
                        for (const auto& l : n.links) {
                            std::string to = v.resolve(l, rel);
                            links = links || (!to.empty() && to != rel);
                        }
                        if (links) continue;
                    }
                    out.push_back(rel);
                }
                std::string msg = plural(out.size(), "orphan"), all;
                for (const auto& r : out) {
                    msg += "\n" + r;
                    all += (all.empty() ? "" : "\n") + r;
                }
                return Result::success(msg).with("count", static_cast<std::int64_t>(out.size())).with("notes", all);
            }));
}

// ---- graph ---------------------------------------------------------------------------------

std::vector<ParamSpec> graphParams() {
    return {vaultParam(),
            {"focus", ParamType::String, "a local graph: only notes within depth links of this note", false},
            {"depth", ParamType::Int, "how many links out from focus", false, {}, std::int64_t{2}},
            {"folder", ParamType::String, "only notes whose path contains this", false},
            boolParamSpec("tags", "add a node per tag, linked to its notes"),
            boolParamSpec("unresolved", "add nodes for links that go nowhere"),
            boolParamSpec("attachments", "add nodes for linked images and files"),
            boolParamSpec("orphans", "keep notes without links", true),
            {"width", ParamType::Int, "picture width in pixels", false, {}, std::int64_t{1200}},
            {"height", ParamType::Int, "picture height in pixels", false, {}, std::int64_t{900}},
            {"labels", ParamType::String, "note names on the picture", false, {"auto", "all", "none"}, std::string("auto")},
            {"seed", ParamType::Int, "layout seed (same seed, same picture)", false, {}, std::int64_t{1}}};
}

Graph makeGraph(Vault& v, const Params& p, std::string& focus) {
    GraphOptions o;
    if (has(p, "focus") && !stringParam(p, "focus").empty()) {
        Params q;
        q["note"] = stringParam(p, "focus");
        o.focus = needNote(v, q);
    }
    focus = o.focus;
    o.depth = static_cast<int>(std::max<std::int64_t>(1, intParam(p, "depth", 2)));
    o.path = stringParam(p, "folder");
    o.tags = boolParam(p, "tags");
    o.unresolved = boolParam(p, "unresolved");
    o.attachments = boolParam(p, "attachments");
    o.orphans = boolParam(p, "orphans", true);
    Graph g = buildGraph(v, o);
    layoutGraph(g, 0, static_cast<unsigned>(intParam(p, "seed", 1)));
    return g;
}

int labelMode(const Params& p) {
    std::string l = stringParam(p, "labels", "auto");
    return l == "all" ? 1 : l == "none" ? 0 : -1;
}

std::string graphSvg(const Graph& g, int width, int height, int labels, const std::string& focus) {
    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
    for (const auto& n : g.nodes) {
        minX = std::min(minX, n.x);
        maxX = std::max(maxX, n.x);
        minY = std::min(minY, n.y);
        maxY = std::max(maxY, n.y);
    }
    const bool show = labels > 0 || (labels < 0 && g.nodes.size() <= 150);
    const double margin = show ? 60 : 24;
    double s = g.nodes.size() <= 1 ? 1 : std::min({(width - 2 * margin) / std::max(1e-9, maxX - minX), (height - 2 * margin) / std::max(1e-9, maxY - minY), 1.6});
    double ox = (width - (maxX - minX) * s) / 2, oy = (height - (maxY - minY) * s) / 2;
    auto X = [&](const GraphNode& n) { return ox + (n.x - minX) * s; };
    auto Y = [&](const GraphNode& n) { return oy + (n.y - minY) * s; };
    char buf[256];
    std::string out = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(width) + "\" height=\"" + std::to_string(height) +
                      "\" viewBox=\"0 0 " + std::to_string(width) + " " + std::to_string(height) + "\" font-family=\"Helvetica, Arial, sans-serif\" font-size=\"12\">\n"
                      "<rect width=\"100%\" height=\"100%\" fill=\"#fcfcfa\"/>\n<g stroke=\"#b4b6c4\" stroke-width=\"1.2\">\n";
    for (const auto& [a, b] : g.edges) {
        const auto& A = g.nodes[static_cast<std::size_t>(a)];
        const auto& B = g.nodes[static_cast<std::size_t>(b)];
        std::snprintf(buf, sizeof buf, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\"/>\n", X(A), Y(A), X(B), Y(B));
        out += buf;
    }
    out += "</g>\n<g>\n";
    for (const auto& n : g.nodes) {
        double r = 3.5 + std::sqrt(static_cast<double>(n.degree)) * 1.8;
        const char* c = n.kind == "unresolved" ? "#aaaab0" : n.kind == "tag" ? "#2f9e5b" : n.kind == "attachment" ? "#c88c3c" : "#4f5bd5";
        if (n.id == focus) c = "#e56e28";
        std::snprintf(buf, sizeof buf, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"%.1f\" fill=\"%s\"><title>", X(n), Y(n), r, c);
        out += buf + htmlEscape(n.id) + "</title></circle>\n";
        if (show) {
            std::snprintf(buf, sizeof buf, "<text x=\"%.1f\" y=\"%.1f\" text-anchor=\"middle\" fill=\"#32323a\">", X(n), Y(n) + r + 13);
            out += buf + htmlEscape(n.label) + "</text>\n";
        }
    }
    return out + "</g>\n</svg>\n";
}

std::string graphSummary(const Graph& g) {
    return plural(g.nodes.size(), "node") + ", " + plural(g.edges.size(), "link");
}

void registerGraph(CommandRegistry& reg, const StatePtr& st) {
    auto params = graphParams();
    params.push_back({"format", ParamType::String, "what to return", false, {"json", "dot", "png", "svg"}, std::string("json")});
    params.push_back(boolParamSpec("preview", "attach the graph as a PNG"));
    reg.add({"notes.graph", "Link graph",
             "The vault's link graph: notes as nodes, links as edges, laid out with a force-directed layout. Returns it as "
             "JSON (nodes with positions and degree, edges) or Graphviz DOT text, or as a picture (format=png, or "
             "preview=true with any format). focus=<note> depth=2 makes a local graph like Obsidian's.",
             params, false, true},
            reader(st, [](Vault& v, const Params& p) {
                std::string focus;
                Graph g = makeGraph(v, p, focus);
                const std::string format = stringParam(p, "format", "json");
                const int w = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 1200), 64, 8000));
                const int h = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height", 900), 64, 8000));
                Result r = Result::success(graphSummary(g));
                r.with("nodes", static_cast<std::int64_t>(g.nodes.size())).with("edges", static_cast<std::int64_t>(g.edges.size()));
                if (format == "json") r.with("graph", graphJson(g));
                if (format == "dot") r.with("graph", graphDot(g));
                if (format == "svg") r.with("graph", graphSvg(g, w, h, labelMode(p), focus));
                if (format == "png" || boolParam(p, "preview")) r.attach({"image/png", raster::encodePng(drawGraph(g, w, h, labelMode(p), focus)), "graph.png"});
                return r;
            }));

    auto eparams = graphParams();
    eparams.insert(eparams.begin() + 1, {"out", ParamType::String, "the file to write: .json, .dot (or .gv), .png or .svg", true});
    eparams.push_back(boolParamSpec("preview", "attach the graph as a PNG"));
    reg.add({"notes.graph.export", "Export graph",
             "Write the link graph to a file: JSON, Graphviz DOT, PNG or SVG, picked by the file's extension.",
             eparams, false, false},
            reader(st, [](Vault& v, const Params& p) {
                std::string focus;
                Graph g = makeGraph(v, p, focus);
                fs::path out = toPath(stringParam(p, "out"));
                std::string ext = lower(fromPath(out.extension()));
                const int w = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 1200), 64, 8000));
                const int h = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height", 900), 64, 8000));
                std::string bytes;
                raster::Image img;
                if (ext == ".json") bytes = graphJson(g);
                else if (ext == ".dot" || ext == ".gv") bytes = graphDot(g);
                else if (ext == ".svg") bytes = graphSvg(g, w, h, labelMode(p), focus);
                else if (ext == ".png") {
                    img = drawGraph(g, w, h, labelMode(p), focus);
                    bytes = raster::encodePng(img);
                } else {
                    return Result::failure("out= should end in .json, .dot, .gv, .png or .svg");
                }
                std::error_code ec;
                if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
                std::ofstream f(out, std::ios::binary);
                if (!f) return Result::failure("can't write " + fromPath(out));
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                f.close();
                Result r = Result::success("wrote " + fromPath(out) + " (" + graphSummary(g) + ")", fromPath(out));
                r.with("path", fromPath(out)).with("nodes", static_cast<std::int64_t>(g.nodes.size())).with("edges", static_cast<std::int64_t>(g.edges.size()));
                if (boolParam(p, "preview")) {
                    if (img.empty()) img = drawGraph(g, w, h, labelMode(p), focus);
                    r.attach({"image/png", raster::previewPng(img, 1024), "graph.png"});
                }
                return r;
            }));
}

}  // namespace

void registerLinkCommands(CommandRegistry& reg, const StatePtr& st) {
    registerSearch(reg, st);
    registerLinks(reg, st);
    registerGraph(reg, st);
}

}  // namespace achclwss::notes::detail
