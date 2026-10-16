// Preflight: the checks a print shop would run before accepting a file.
#include <set>
#include <system_error>

#include "internal.hpp"

namespace achclwss::layout {

std::vector<Issue> preflight(const LayoutDoc& doc, double minDpi) {
    std::vector<Issue> out;
    auto add = [&](const char* sev, const char* kind, const std::string& where, const std::string& msg) {
        out.push_back({sev, kind, where, msg});
    };
    auto where = [](int page, const Frame& f) {
        return (page >= 0 ? "page " + std::to_string(page + 1) : std::string("master")) + ", " + f.id + (f.name.empty() ? "" : " (" + f.name + ")");
    };

    // overset text
    const auto layouts = layoutAll(doc);
    for (const auto& [id, sl] : layouts) {
        if (sl.overset == 0) continue;
        auto chain = storyChain(doc, id);
        int page = -1;
        const Frame* last = chain.empty() ? nullptr : chain.back();
        if (last) doc.findFrame(last->id, &page);
        add("error", "overset", last ? where(page, *last) : id,
            std::to_string(sl.overset) + " of " + std::to_string(sl.total) + " characters of story " + id +
                " don't fit; enlarge the frame, add a linked frame (layout.frame_link) or use layout.place_text autoflow");
    }

    std::set<std::string> rgb;
    bool standardFonts = false;
    auto noteColour = [&](const std::string& spec) {
        auto p = resolvePaint(doc, spec);
        if (p && !p->none && !p->cmyk) rgb.insert(spec);
    };
    for (const auto& st : doc.stories) {
        for (const auto& para : st.second.paragraphs) {
            for (const auto& r : para.runs) {
                if (r.props.color) noteColour(*r.props.color);
            }
        }
    }
    for (const auto& s : doc.styles) {
        if (s.chars.color) noteColour(*s.chars.color);
        if (s.chars.font && !doc.findFont(*s.chars.font)) standardFonts = true;
    }

    auto checkFrame = [&](int page, const Frame& f, bool master) {
        noteColour(f.fill);
        noteColour(f.stroke);
        const std::string at = where(page, f);
        if (f.type == FrameType::Image || f.type == FrameType::Vector) {
            if (f.path.empty()) {
                add("warning", "empty", at, "picture frame has nothing placed in it");
            } else {
                const PictureInfo info = pictureInfo(f.path);
                if (!info.ok) {
                    add("error", "missing_file", at, info.error.empty() ? "can't read " + f.path : info.error);
                } else if (!info.vector) {
                    const double dpi = effectiveDpi(f);
                    if (dpi > 0 && dpi < minDpi) {
                        add(dpi < minDpi / 2 ? "error" : "warning", "low_res", at,
                            f.path + " prints at " + num(dpi, 0) + " dpi (" + std::to_string(info.pxWidth) + " x " +
                                std::to_string(info.pxHeight) + " px); " + num(minDpi, 0) + " dpi needs it at most " +
                                num(info.pxWidth / minDpi * 25.4, 1) + " mm wide");
                    }
                }
            }
        }
        if (f.type == FrameType::Text) {
            auto it = doc.stories.find(f.story);
            if (!master && (it == doc.stories.end() || it->second.length() <= it->second.paragraphs.size())) {
                if (f.next.empty()) {
                    bool linkedTo = false;
                    for (const Frame* g : storyChain(doc, f.story)) linkedTo = linkedTo || g->next == f.id;
                    if (!linkedTo) add("warning", "empty", at, "text frame is empty");
                }
            }
        }
        if (master || page < 0) return;
        const Page& pg = doc.pages[static_cast<std::size_t>(page)];
        const Box b = frameBounds(f);
        const double bl = doc.bleed, eps = 0.01;
        if (b.x + b.w < -bl || b.y + b.h < -bl || b.x > pg.width + bl || b.y > pg.height + bl) {
            add("warning", "off_page", at, "is entirely off the page and won't print");
            return;
        }
        if (b.x < -bl - eps || b.y < -bl - eps || b.x + b.w > pg.width + bl + eps || b.y + b.h > pg.height + bl + eps) {
            add("info", "off_page", at, "extends past the bleed; the part beyond is cut off");
        }
        const bool painted = f.type == FrameType::Image || f.type == FrameType::Vector || !f.fill.empty();
        if (painted && f.type != FrameType::Text) {
            std::string edges;
            auto edge = [&](bool touches, bool reaches, const char* name) {
                if (touches && !reaches) edges += (edges.empty() ? "" : ", ") + std::string(name);
            };
            edge(b.x <= eps, b.x <= -bl + eps, "left");
            edge(b.y <= eps, b.y <= -bl + eps, "top");
            edge(b.x + b.w >= pg.width - eps, b.x + b.w >= pg.width + bl - eps, "right");
            edge(b.y + b.h >= pg.height - eps, b.y + b.h >= pg.height + bl - eps, "bottom");
            if (!edges.empty()) {
                add("warning", "bleed", at,
                    bl <= 0 ? "touches the " + edges + " page edge but the document has no bleed (layout.doc_set bleed=3)"
                            : "touches the " + edges + " page edge but doesn't extend into the " + num(bl, 1) +
                                  " mm bleed; a slightly off cut leaves a white sliver");
            }
        }
        if (f.type == FrameType::Text) {
            const Box m = marginBox(doc, page);
            if (b.x < m.x - 0.5 || b.y < m.y - 0.5 || b.x + b.w > m.x + m.w + 0.5 || b.y + b.h > m.y + m.h + 0.5) {
                const bool pastTrim = b.x < 0 || b.y < 0 || b.x + b.w > pg.width || b.y + b.h > pg.height;
                add(pastTrim ? "warning" : "info", "margin", at,
                    pastTrim ? "text frame crosses the page edge; text there will be cut off"
                             : "text frame is outside the margins (fine on purpose, but keep text 3+ mm from the trim)");
            }
        }
    };
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        for (const auto& f : doc.pages[i].frames) checkFrame(static_cast<int>(i), f, false);
    }
    for (const auto& m : doc.masters) {
        for (const auto& f : m.frames) checkFrame(-1, f, true);
    }

    for (const auto& fam : doc.fonts) {
        for (const auto& p : {fam.regular, fam.bold, fam.italic, fam.boldItalic}) {
            std::error_code ec;
            if (!p.empty() && !std::filesystem::exists(p, ec)) {
                add("error", "missing_file", "font " + fam.name, "font file " + p + " is missing; text falls back to Helvetica");
            }
        }
    }
    if (!rgb.empty()) {
        std::string list;
        for (const auto& c : rgb) list += (list.empty() ? "" : ", ") + c;
        add("info", "rgb", "document",
            std::to_string(rgb.size()) + " colour(s) are RGB (" + list + "); the printer converts them. For exact print colour define CMYK swatches with layout.color_define");
    }
    if (standardFonts) {
        add("info", "fonts", "document",
            "standard PDF fonts aren't embedded by default; export with embed_fonts=true so the print shop gets the exact glyphs");
    }
    return out;
}

}  // namespace achclwss::layout
