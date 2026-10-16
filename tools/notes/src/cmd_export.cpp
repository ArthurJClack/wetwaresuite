// Rendering notes to HTML and PDF, and exporting a vault as a static site.
#include <algorithm>
#include <cctype>
#include <fstream>

#include "achclwss/office/doc.hpp"
#include "achclwss/pdf/page.hpp"
#include "achclwss/raster/codec.hpp"
#include "internal.hpp"

namespace achclwss::notes::detail {

namespace {

std::string base64(const std::string& bytes) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i < bytes.size()) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string extLower(const std::string& rel) {
    auto dot = rel.rfind('.');
    auto slash = rel.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(rel.substr(dot + 1));
}

std::string mimeOf(const std::string& ext) {
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp") return "image/bmp";
    if (ext == "avif") return "image/avif";
    return "application/octet-stream";
}

bool isImage(const std::string& ext) {
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "svg" || ext == "webp" || ext == "bmp" || ext == "avif";
}
bool isAudio(const std::string& ext) { return ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "m4a" || ext == "flac" || ext == "webm" || ext == "3gp"; }
bool isVideo(const std::string& ext) { return ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "ogv"; }

std::string tagAnchor(const std::string& tag) {
    std::string t = lower(tag);
    for (auto& c : t) {
        if (c == '/') c = '-';
    }
    return "tag-" + slugify(t);
}

std::string noteAnchor(const std::string& rel) {
    std::string r = withoutMd(rel);
    for (auto& c : r) {
        if (c == '/') c = '-';
    }
    return "note-" + slugify(r);
}

std::string relHref(const std::string& fromFolder, const std::string& toRel) {
    fs::path r = toPath(toRel).lexically_relative(toPath(fromFolder.empty() ? "." : fromFolder));
    return urlEncodePath(fromPath(r));
}

std::string htmlName(const std::string& rel) { return withoutMd(rel) + ".html"; }

bool writeOut(const std::string& path, const std::string& bytes) {
    fs::path f = toPath(path);
    std::error_code ec;
    if (f.has_parent_path()) fs::create_directories(f.parent_path(), ec);
    std::ofstream o(f, std::ios::binary);
    if (!o) return false;
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(o);
}

enum class Mode { Single, Book, Site };

// Renders notes with links and embeds suited to where the HTML goes.
struct HtmlBuilder {
    const Vault& v;
    Mode mode;
    bool embedImages = true;
    std::set<std::string>* usedFiles = nullptr;  // attachments a site needs copied
    std::set<std::string> inBook;                // notes present in a book

    std::string href(const std::string& fromRel, const std::string& target, const std::string& heading) const {
        std::string anchor;
        if (!heading.empty()) anchor = "#" + (heading[0] == '^' ? heading : slugify(heading));
        if (mode == Mode::Book) {
            if (!v.isNote(target)) return urlEncodePath(fromPath(v.abs(target)));
            if (!inBook.count(target)) return "";
            return "#" + noteAnchor(target);
        }
        if (!v.isNote(target)) {
            if (usedFiles) usedFiles->insert(target);
            return relHref(folderOf(fromRel), target);
        }
        if (target == fromRel) return anchor.empty() ? "#" : anchor;
        return relHref(folderOf(fromRel), htmlName(target)) + anchor;
    }

    std::string fileSrc(const std::string& fromRel, const std::string& target) const {
        if (mode == Mode::Site) {
            if (usedFiles) usedFiles->insert(target);
            return relHref(folderOf(fromRel), target);
        }
        if (embedImages) {
            try {
                return "data:" + mimeOf(extLower(target)) + ";base64," + base64(v.read(target));
            } catch (const std::exception&) {
            }
        }
        return urlEncodePath(fromPath(v.abs(target)));
    }

    // Links are resolved from `rel` (the note the text is in) and written relative to `page`
    // (the note whose page it ends up on: different for embedded notes).
    RenderHooks hooks(const std::string& rel, int depth, std::string page = {}) const {
        if (page.empty()) page = rel;
        RenderHooks h;
        h.depth = depth;
        h.link = [this, rel, page](const Link& l) -> std::optional<std::string> {
            std::string to = v.resolve(l, rel);
            if (to.empty()) return std::nullopt;
            std::string r = href(page, to, l.heading);
            if (r.empty()) return std::nullopt;
            return r;
        };
        h.image = [this, rel, page](const std::string& src) {
            std::string to = v.resolve(src, rel, true);
            if (to.empty()) to = v.resolve(src, rel, false);
            return to.empty() ? urlEncodePath(src) : fileSrc(page, to);
        };
        h.embed = [this, rel, page](const Link& l, int d) -> std::optional<std::string> {
            std::string to = v.resolve(l, rel);
            if (to.empty()) return std::nullopt;
            const std::string ext = extLower(to);
            if (!v.isNote(to)) {
                std::string src = fileSrc(page, to);
                std::string alt = htmlEscape(l.target);
                if (isImage(ext)) {
                    std::string size;
                    std::string w = trim(l.alias);
                    if (!w.empty() && w.find_first_not_of("0123456789x") == std::string::npos) {
                        auto x = w.find('x');
                        size = " width=\"" + w.substr(0, x) + "\"" + (x != std::string::npos ? " height=\"" + w.substr(x + 1) + "\"" : "");
                    }
                    return "<img src=\"" + htmlEscape(src) + "\" alt=\"" + alt + "\"" + size + ">";
                }
                if (mode != Mode::Site) src = urlEncodePath(fromPath(v.abs(to)));  // don't inline big media
                if (isAudio(ext)) return "<audio controls src=\"" + htmlEscape(src) + "\"></audio>";
                if (isVideo(ext)) return "<video controls src=\"" + htmlEscape(src) + "\"></video>";
                if (ext == "pdf") return "<iframe class=\"embed-pdf\" src=\"" + htmlEscape(src) + "\" width=\"100%\" height=\"600\"></iframe>";
                return "<a href=\"" + htmlEscape(src) + "\">" + alt + "</a>";
            }
            if (d >= 3) return "<a class=\"internal-link\" href=\"" + htmlEscape(href(page, to, l.heading)) + "\">" + htmlEscape(l.target) + "</a>";
            std::string text = bodyOf(v.read(to));
            if (!l.heading.empty() && l.heading[0] != '^') {
                auto range = sectionRange(text, l.heading);
                if (range) text = text.substr(range->first, range->second - range->first);
            } else if (!l.heading.empty()) {
                // ^block-id: the paragraph or list item ending with it
                auto lines = splitLines(text);
                for (const auto& line : lines) {
                    if (endsWith(trim(line), " " + l.heading) || trim(line) == l.heading) {
                        text = line;
                        break;
                    }
                }
            }
            const Note* n = v.note(to);
            std::string title = n ? n->title : l.target;
            std::string link = href(page, to, l.heading);
            std::string head = link.empty() ? htmlEscape(title) : "<a class=\"internal-link\" href=\"" + htmlEscape(link) + "\">" + htmlEscape(title) + "</a>";
            return "<div class=\"embed\"><div class=\"embed-title\">" + head + "</div>\n" + markdownToHtml(text, hooks(to, d + 1, page)) + "</div>";
        };
        h.tag = [this, page](const std::string& tag) -> std::string {
            if (mode == Mode::Site) return relHref(folderOf(page), "tags.html") + "#" + tagAnchor(tag);
            return "";
        };
        return h;
    }

    std::string properties(const Note& n) const {
        if (n.frontmatter.empty()) return "";
        std::string out = "<table class=\"properties\">\n";
        for (const auto& [k, fv] : n.frontmatter) {
            std::string value;
            if (lower(k) == "tags" || lower(k) == "tag") {
                for (const auto& t : frontmatterTags({{k, fv}})) {
                    auto hr = hooks(n.rel, 0).tag(t);
                    value += (hr.empty() ? "<span class=\"tag\">#" + htmlEscape(t) + "</span>" : "<a class=\"tag\" href=\"" + htmlEscape(hr) + "\">#" + htmlEscape(t) + "</a>") + " ";
                }
            } else {
                // values may hold [[links]]
                std::string html = markdownToHtml(fv.text(), hooks(n.rel, 0));
                if (startsWith(html, "<p>") && endsWith(html, "</p>\n")) html = html.substr(3, html.size() - 8);
                value = html;
            }
            out += "<tr><th>" + htmlEscape(k) + "</th><td>" + value + "</td></tr>\n";
        }
        return out + "</table>\n";
    }

    std::string article(const std::string& rel, bool props, bool backlinks) const {
        const Note* n = v.note(rel);
        std::string body = bodyOf(v.read(rel));
        std::string out;
        std::string first = trim(body.substr(0, body.find('\n')));
        bool hasH1 = startsWith(trim(body), "# ");
        if (!hasH1) out += "<h1 class=\"note-title\">" + htmlEscape(n->title) + "</h1>\n";
        if (props) out += properties(*n);
        out += markdownToHtml(body, hooks(rel, 0));
        if (backlinks) {
            auto links = v.backlinks(rel);
            std::set<std::string> from;
            for (const auto& [f, l] : links) {
                if (mode != Mode::Book || inBook.count(f->rel)) from.insert(f->rel);
            }
            if (!from.empty()) {
                out += "<section class=\"backlinks\"><h2>Linked from</h2><ul>\n";
                for (const auto& f : from) {
                    std::string hr = href(rel, f, "");
                    out += "<li><a class=\"internal-link\" href=\"" + htmlEscape(hr) + "\">" + htmlEscape(v.note(f)->title) + "</a></li>\n";
                }
                out += "</ul></section>\n";
            }
        }
        (void)first;
        return out;
    }
};

// ---- PDF ---------------------------------------------------------------------------------

// Markdown the office layout engine understands: wikilinks become text, embeds are inlined.
std::string plainMarkdown(const Vault& v, const std::string& rel, int depth) {
    std::string text = bodyOf(v.read(rel));
    // %% comments %%
    for (auto a = text.find("%%"); a != std::string::npos; a = text.find("%%", a)) {
        auto b = text.find("%%", a + 2);
        text.erase(a, b == std::string::npos ? std::string::npos : b + 2 - a);
    }
    std::string out;
    bool fence = false;
    for (const auto& rawLine : splitLines(text)) {
        std::string line = rawLine;
        std::string t = trim(line);
        if (startsWith(t, "```") || startsWith(t, "~~~")) {
            fence = !fence;
            out += line + "\n";
            continue;
        }
        if (fence) {
            out += line + "\n";
            continue;
        }
        // standalone embeds and images become their own blocks
        if (startsWith(t, "![[") && endsWith(t, "]]") && t.find("]]") == t.size() - 2) {
            std::string inner = t.substr(3, t.size() - 5);
            std::string target = inner.substr(0, inner.find_first_of("|#"));
            std::string to = v.resolve(target, rel);
            if (!to.empty() && !v.isNote(to) && isImage(extLower(to))) {
                out += "\n![" + target + "](" + urlEncodePath(to) + ")\n\n";
                continue;
            }
            if (!to.empty() && v.isNote(to) && depth < 3) {
                std::string sub = plainMarkdown(v, to, depth + 1);
                auto hash = inner.find('#');
                if (hash != std::string::npos) {
                    std::string h = inner.substr(hash + 1, inner.find('|') == std::string::npos ? std::string::npos : inner.find('|') - hash - 1);
                    auto range = sectionRange(sub, h);
                    if (range) sub = sub.substr(range->first, range->second - range->first);
                }
                out += "\n" + sub + "\n";
                continue;
            }
        }
        if (startsWith(t, "![") && !startsWith(t, "![[") && endsWith(t, ")")) {
            auto mid = t.find("](");
            if (mid != std::string::npos) {
                std::string src = t.substr(mid + 2, t.size() - mid - 3);
                std::string to = v.resolve(urlDecode(src), rel, true);
                if (!to.empty()) {
                    out += "\n![" + t.substr(2, mid - 2) + "](" + urlEncodePath(to) + ")\n\n";
                    continue;
                }
            }
        }
        // callouts: > [!note] Title -> > **Title**
        auto q = line.find("[!");
        if (startsWith(t, ">") && q != std::string::npos) {
            auto close = line.find(']', q);
            if (close != std::string::npos) {
                std::string type = line.substr(q + 2, close - q - 2);
                std::string title = trim(line.substr(close + 1));
                if (!title.empty() && (title[0] == '+' || title[0] == '-')) title = trim(title.substr(1));
                if (title.empty()) title = type;
                line = line.substr(0, q) + "**" + title + "**";
            }
        }
        // inline wikilinks and embeds -> text; ==highlight== -> text
        std::string done;
        for (std::size_t i = 0; i < line.size();) {
            bool bang = line[i] == '!' && line.compare(i + 1, 2, "[[") == 0;
            if (bang || line.compare(i, 2, "[[") == 0) {
                std::size_t open = i + (bang ? 3 : 2);
                auto close = line.find("]]", open);
                if (close != std::string::npos) {
                    std::string inner = line.substr(open, close - open);
                    auto bar = inner.find('|');
                    std::string shown = bar != std::string::npos && !bang ? inner.substr(bar + 1) : inner.substr(0, bar);
                    if (bang && isImage(extLower(shown))) shown = "[image: " + shown + "]";
                    if (bar == std::string::npos) {
                        auto hash = shown.find('#');
                        if (hash == 0) shown = shown.substr(1);
                        else if (hash != std::string::npos) shown = shown.substr(0, hash) + " > " + shown.substr(hash + 1);
                    }
                    if (!shown.empty() && shown.back() == '\\') shown.pop_back();
                    done += shown;
                    i = close + 2;
                    continue;
                }
            }
            if (line.compare(i, 2, "==") == 0) {
                i += 2;
                continue;
            }
            done += line[i++];
        }
        // the PDF fonts have no emoji or ballot boxes
        Task task;
        if (parseTaskLine(done, task)) {
            auto open = done.find('[');
            done = done.substr(0, open) + "\xC2\xA0[" + (task.done ? "x" : "  ") + "]" + done.substr(open + 3);
        }
        for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{{"\xF0\x9F\x93\x85", "due"}, {"\xE2\x9C\x85", "done"}}) {
            for (auto pos = done.find(from); pos != std::string::npos; pos = done.find(from, pos)) done.replace(pos, from.size(), to);
        }
        out += done + "\n";
    }
    return out;
}

void registerRender(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.render", "Render notes",
             "Render a note (or the whole vault, or one folder of it) to HTML or PDF. HTML is one self-contained file "
             "(images inlined) styled for reading, with the note's properties, working [[links]] (to other notes' "
             ".html files next to it; within the file for a whole-vault render), embedded notes and images, callouts, "
             "tables, task lists and code. PDF goes through the office layout engine (A4, page numbers). Without out=, "
             "HTML comes back as text in `html`. preview=true attaches page 1 of a PDF as a PNG.",
             {vaultParam(), noteParam("the note to render (default: every note, one after another)", false),
              {"folder", ParamType::String, "with no note=: only notes in this folder", false},
              {"format", ParamType::String, "html or pdf (default: from out=, else html)", false, {"html", "pdf"}},
              {"out", ParamType::String, "the file to write", false},
              {"title", ParamType::String, "document title (default: the note's, or the vault folder's name)", false},
              boolParamSpec("properties", "show frontmatter as a properties table (HTML)", true),
              boolParamSpec("backlinks", "list the notes linking to each note (HTML)"),
              boolParamSpec("embed_images", "inline images as data: URIs so the HTML stands alone", true),
              boolParamSpec("preview", "attach the first PDF page as a PNG")},
             false, false},
            reader(st, [](Vault& v, const Params& p) {
                std::vector<std::string> rels;
                std::string title;
                const bool single = has(p, "note") && !stringParam(p, "note").empty();
                if (single) {
                    rels.push_back(needNote(v, p));
                    title = v.note(rels[0])->title;
                } else {
                    std::string folder = trim(stringParam(p, "folder"));
                    while (!folder.empty() && folder.back() == '/') folder.pop_back();
                    for (const auto& [rel, n] : v.notes) {
                        if (!folder.empty() && !startsWith(lower(rel), lower(folder) + "/")) continue;
                        if (!v.settings.templatesFolder.empty() && startsWith(rel, v.settings.templatesFolder + "/")) continue;
                        rels.push_back(rel);
                    }
                    if (rels.empty()) return Result::failure("no notes to render");
                    title = fromPath(v.root.filename());
                }
                if (has(p, "title") && !stringParam(p, "title").empty()) title = stringParam(p, "title");
                const std::string out = stringParam(p, "out");
                std::string format = stringParam(p, "format");
                if (format.empty()) format = endsWith(lower(out), ".pdf") ? "pdf" : "html";
                if (format == "pdf") {
                    if (out.empty()) return Result::failure("give out=<file.pdf> for a PDF");
                    std::string md;
                    for (std::size_t i = 0; i < rels.size(); ++i) {
                        if (i) md += "\n<!-- pagebreak -->\n\n";
                        std::string body = plainMarkdown(v, rels[i], 0);
                        if (!startsWith(trim(body), "# ")) md += "# " + v.note(rels[i])->title + "\n\n";
                        md += body + "\n";
                    }
                    office::TextDocument doc = office::readMarkdown(md, fromPath(v.root));
                    doc.title = title;
                    auto pages = office::layoutDocument(doc);
                    std::string bytes = pdf::writePdf(pages, {title, "", ""});
                    if (!writeOut(out, bytes)) return Result::failure("can't write " + out);
                    Result r = Result::success("wrote " + out + " (" + plural(pages.size(), "page") + ", " + plural(rels.size(), "note") + ")", out);
                    r.with("path", out).with("pages", static_cast<std::int64_t>(pages.size())).with("notes", static_cast<std::int64_t>(rels.size()));
                    if (boolParam(p, "preview") && !pages.empty()) r.attach({"image/png", raster::previewPng(pdf::renderPage(pages[0], 1.5), 900), "page1.png"});
                    return r;
                }
                HtmlBuilder b{v, single ? Mode::Single : Mode::Book};
                b.embedImages = boolParam(p, "embed_images", true);
                std::string body;
                if (single) {
                    body = "<article>\n" + b.article(rels[0], boolParam(p, "properties", true), boolParam(p, "backlinks")) + "</article>\n";
                } else {
                    b.inBook = std::set<std::string>(rels.begin(), rels.end());
                    body = "<nav class=\"toc\"><h1>" + htmlEscape(title) + "</h1><ul class=\"note-list\">\n";
                    for (const auto& r : rels) body += "<li><a href=\"#" + noteAnchor(r) + "\">" + htmlEscape(v.note(r)->title) + "</a> <span class=\"folder\">" + htmlEscape(folderOf(r)) + "</span></li>\n";
                    body += "</ul></nav>\n";
                    for (const auto& r : rels) {
                        body += "<article id=\"" + noteAnchor(r) + "\">\n" + b.article(r, boolParam(p, "properties", true), boolParam(p, "backlinks")) + "</article>\n<hr>\n";
                    }
                }
                std::string html = htmlPage(title, body);
                Result r = Result::success("");
                if (!out.empty()) {
                    if (!writeOut(out, html)) return Result::failure("can't write " + out);
                    r = Result::success("wrote " + out + " (" + plural(rels.size(), "note") + ", " + std::to_string(html.size()) + " bytes)", out);
                    r.with("path", out);
                } else {
                    r = Result::success("rendered " + plural(rels.size(), "note") + " to HTML (" + std::to_string(html.size()) + " bytes, in html)");
                    r.with("html", html);
                }
                return r.with("notes", static_cast<std::int64_t>(rels.size()));
            }));
}

void registerSite(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"notes.export.site", "Export site",
             "Export the vault (or a folder of it) as a static website: one .html page per note at the same path, "
             "index.html (the home note, if any, and every note by folder), tags.html, style.css (light and dark), "
             "the attachments the notes use, working links and backlinks on every page. Open index.html or put the "
             "folder on any web host. Templates and hidden folders are left out.",
             {vaultParam(), {"out", ParamType::String, "the folder to write the site into", true},
              {"folder", ParamType::String, "only notes in this folder", false},
              {"home", ParamType::String, "the note shown on index.html (default: index, Home, README or Welcome when present)", false},
              {"title", ParamType::String, "the site's title (default: the vault folder's name)", false},
              boolParamSpec("attachments", "copy every attachment, not just the linked ones"),
              boolParamSpec("graph", "add graph.png and a picture of it on the home page"),
              boolParamSpec("properties", "show frontmatter on each page", true)},
             false, false},
            reader(st, [](Vault& v, const Params& p) {
                const fs::path out = toPath(stringParam(p, "out"));
                std::error_code ec;
                fs::path outAbs = fs::weakly_canonical(fs::absolute(out), ec);
                if (outAbs == v.root || startsWith(fromPath(v.root), fromPath(outAbs) + "/")) return Result::failure("out= can't be the vault or a folder holding it");
                fs::create_directories(out, ec);
                if (ec) return Result::failure("can't make " + fromPath(out) + ": " + ec.message());
                std::string folder = trim(stringParam(p, "folder"));
                while (!folder.empty() && folder.back() == '/') folder.pop_back();
                std::vector<std::string> rels;
                for (const auto& [rel, n] : v.notes) {
                    if (!folder.empty() && !startsWith(lower(rel), lower(folder) + "/")) continue;
                    if (!v.settings.templatesFolder.empty() && startsWith(rel, v.settings.templatesFolder + "/")) continue;
                    rels.push_back(rel);
                }
                const std::string title = has(p, "title") && !stringParam(p, "title").empty() ? stringParam(p, "title") : fromPath(v.root.filename());
                std::string home;
                if (has(p, "home") && !stringParam(p, "home").empty()) {
                    Params q;
                    q["note"] = stringParam(p, "home");
                    home = needNote(v, q);
                } else {
                    for (const char* h : {"index", "Index", "Home", "home", "README", "Readme", "Welcome"}) {
                        std::string md = std::string(h) + ".md";
                        if (v.isNote(md)) {
                            home = md;
                            break;
                        }
                    }
                }
                std::set<std::string> used;
                HtmlBuilder b{v, Mode::Site};
                b.usedFiles = &used;
                auto write = [&](const std::string& rel, const std::string& bytes) {
                    fs::path f = out / toPath(rel);
                    fs::create_directories(f.parent_path(), ec);
                    std::ofstream o(f, std::ios::binary);
                    if (!o) throw std::runtime_error("can't write " + fromPath(f));
                    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                };
                auto nav = [&](const std::string& rel) {
                    const std::string f = folderOf(rel);
                    return "<a href=\"" + relHref(f, "index.html") + "\"><strong>" + htmlEscape(title) + "</strong></a><a href=\"" +
                           relHref(f, "index.html") + "#all\">All notes</a><a href=\"" + relHref(f, "tags.html") + "\">Tags</a>";
                };
                const bool props = boolParam(p, "properties", true);
                for (const auto& rel : rels) {
                    std::string page = htmlPage(v.note(rel)->title + " - " + title, "<article>\n" + b.article(rel, props, true) + "</article>\n",
                                                relHref(folderOf(rel), "style.css"), nav(rel));
                    write(htmlName(rel), page);
                }
                // index
                std::string index;
                if (!home.empty()) index += "<article>\n" + b.article(home, props, false) + "</article>\n";
                else index += "<h1>" + htmlEscape(title) + "</h1>\n";
                if (boolParam(p, "graph")) {
                    Graph g = buildGraph(v, {"", 2, false, false, false, true, folder});
                    layoutGraph(g);
                    write("graph.png", raster::encodePng(drawGraph(g, 1200, 900, -1)));
                    index += "<p><img src=\"graph.png\" alt=\"link graph\"></p>\n";
                }
                index += "<h2 id=\"all\">All notes</h2>\n";
                std::map<std::string, std::vector<std::string>> byFolder;
                for (const auto& r : rels) byFolder[folderOf(r)].push_back(r);
                for (const auto& [f, list] : byFolder) {
                    if (!f.empty()) index += "<h3 class=\"folder\">" + htmlEscape(f) + "/</h3>\n";
                    index += "<ul class=\"note-list\">\n";
                    for (const auto& r : list) index += "<li><a class=\"internal-link\" href=\"" + urlEncodePath(htmlName(r)) + "\">" + htmlEscape(v.note(r)->title) + "</a></li>\n";
                    index += "</ul>\n";
                }
                write("index.html", htmlPage(title, index, "style.css", nav("index.html")));
                // tags
                std::map<std::string, std::pair<std::string, std::vector<std::string>>> tags;
                for (const auto& r : rels) {
                    for (const auto& t : v.note(r)->tags) {
                        auto& e = tags[lower(t)];
                        if (e.first.empty()) e.first = t;
                        e.second.push_back(r);
                    }
                }
                std::string tagPage = "<h1>Tags</h1>\n";
                for (const auto& [k, e] : tags) {
                    tagPage += "<h2 id=\"" + tagAnchor(e.first) + "\">#" + htmlEscape(e.first) + " <span class=\"folder\">" + std::to_string(e.second.size()) + "</span></h2>\n<ul>\n";
                    for (const auto& r : e.second) tagPage += "<li><a class=\"internal-link\" href=\"" + urlEncodePath(htmlName(r)) + "\">" + htmlEscape(v.note(r)->title) + "</a></li>\n";
                    tagPage += "</ul>\n";
                }
                write("tags.html", htmlPage("Tags - " + title, tagPage, "style.css", nav("tags.html")));
                write("style.css", pageCss());
                if (boolParam(p, "attachments")) {
                    for (const auto& [rel, f] : v.files) used.insert(rel);
                }
                std::size_t copied = 0;
                for (const auto& rel : used) {
                    if (!v.files.count(rel)) continue;
                    fs::path dst = out / toPath(rel);
                    fs::create_directories(dst.parent_path(), ec);
                    fs::copy_file(v.abs(rel), dst, fs::copy_options::overwrite_existing, ec);
                    if (!ec) ++copied;
                }
                Result r = Result::success("exported " + plural(rels.size(), "page") + ", " + plural(tags.size(), "tag") + " and " +
                                               plural(copied, "attachment") + " to " + fromPath(out) + " (open index.html)",
                                           fromPath(out / "index.html"));
                return r.with("path", fromPath(out)).with("pages", static_cast<std::int64_t>(rels.size())).with("attachments", static_cast<std::int64_t>(copied)).with("tags", static_cast<std::int64_t>(tags.size()));
            }));
}

}  // namespace

void registerExportCommands(CommandRegistry& reg, const StatePtr& st) {
    registerRender(reg, st);
    registerSite(reg, st);
}

}  // namespace achclwss::notes::detail
