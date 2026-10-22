// office.deck_* commands for presenting: transitions and timings, the master (footer,
// slide numbers), duplicating slides, speaker notes and the browser slide show.
#include <algorithm>
#include <cmath>

#include "deck_cmd.hpp"
#include "text.hpp"

namespace wetwaresuite::office::deckcmd {

namespace {

std::string transitionText(const Slide& s) {
    std::string t = s.transition.empty() ? std::string("no transition") : s.transition;
    if (s.transition == "push" || s.transition == "wipe" || s.transition == "cover") t += " " + s.direction;
    if (!s.transition.empty()) t += " " + textNumber(std::round(s.transitionSeconds * 100) / 100) + " s";
    t += s.advanceAfter > 0 ? ", advances after " + textNumber(s.advanceAfter) + " s" : ", advances on click";
    return t;
}

std::string masterText(const Deck& d) {
    std::string t = "master: " + std::string(d.slideNumbers ? "slide numbers on" : "slide numbers off");
    t += d.footer.empty() ? ", no footer" : ", footer \"" + d.footer + "\"";
    t += ", " + std::to_string(d.master.size()) + " object(s)";
    t += d.masterOnTitle ? ", shown on title and section slides too" : ", not on title and section slides";
    return t;
}

}  // namespace

void registerDeckShowCommands(Session& session) {
    auto& reg = session.registry();
    std::vector<std::string> kinds = {"none"};
    for (const auto& n : transitionNames()) kinds.push_back(n);

    reg.add({"office.deck_transition", "Slide Transition",
             "Set how a slide comes in during a show (fade, push, wipe, cover, zoom or none) and its timing: advance_after= moves on "
             "by itself after that many seconds (0 = on click). all=true sets every slide. PowerPoint keeps them.",
             {doc(), integer("slide", "slide number"), flag("all", "every slide", false), str("type", "transition", false, {}, kinds),
              str("direction", "push, wipe, cover: the way the new slide moves", false, {}, {"left", "right", "up", "down"}),
              number("duration", "seconds the transition takes (default 0.75)"), number("advance_after", "seconds before moving on by itself; 0 = on click")},
             true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                if (!has(p, "type") && !has(p, "direction") && !has(p, "duration") && !has(p, "advance_after"))
                    return Result::failure("give type=, direction=, duration= or advance_after=");
                std::vector<std::size_t> which;
                if (boolParam(p, "all", false)) {
                    for (std::size_t i = 0; i < d.data.slides.size(); ++i) which.push_back(i);
                    if (which.empty()) return Result::failure("the deck has no slides");
                } else {
                    std::size_t i;
                    std::string error;
                    if (!has(p, "slide")) return Result::failure("give slide=<n> or all=true");
                    if (!slideArg(d.data, p, "slide", i, error)) return Result::failure(error);
                    which.push_back(i);
                }
                if (has(p, "duration") && !(numberParam(p, "duration") >= 0.1 && numberParam(p, "duration") <= 10)) return Result::failure("duration must be 0.1..10 seconds");
                if (has(p, "advance_after") && !(numberParam(p, "advance_after") >= 0 && numberParam(p, "advance_after") <= 3600))
                    return Result::failure("advance_after must be 0..3600 seconds");
                for (std::size_t i : which) {
                    Slide& s = d.data.slides[i];
                    if (has(p, "type")) s.transition = stringParam(p, "type") == "none" ? std::string() : stringParam(p, "type");
                    if (has(p, "direction")) s.direction = stringParam(p, "direction");
                    if (has(p, "duration")) s.transitionSeconds = numberParam(p, "duration");
                    if (has(p, "advance_after")) s.advanceAfter = numberParam(p, "advance_after");
                }
                const Slide& s = d.data.slides[which.front()];
                return Result::success((which.size() == 1 ? "slide " + std::to_string(which.front() + 1) : std::to_string(which.size()) + " slides") + ": " +
                                       transitionText(s));
            }));

    reg.add({"office.deck_master", "Slide Master",
             "Change what every slide shares: the footer text, slide numbers, and whether master objects (office.deck_shape, "
             "deck_text_box or deck_image with master=true, e.g. a logo) also show on title and section slides. clear=true removes "
             "the master objects. Without changes it describes the master.",
             {doc(), str("footer", "footer text at the bottom left (empty removes it)"), flag("slide_numbers", "slide numbers bottom right"),
              flag("on_title", "also show the master, footer and numbers on title and section slides"), flag("clear", "remove every master object", false),
              previewSpec()},
             true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                if (has(p, "footer")) d.data.footer = trimCopy(stringParam(p, "footer"));
                if (has(p, "slide_numbers")) d.data.slideNumbers = boolParam(p, "slide_numbers", true);
                if (has(p, "on_title")) d.data.masterOnTitle = boolParam(p, "on_title", false);
                if (boolParam(p, "clear", false)) d.data.master.clear();
                Result r = Result::success(masterText(d.data));
                for (std::size_t i = 0; i < d.data.slides.size(); ++i) {
                    const auto& l = d.data.slides[i].layout;
                    if (d.data.masterOnTitle || (l != "title" && l != "section")) {
                        slidePreview(r, d.data, i, p);
                        break;
                    }
                }
                return r;
            }));

    reg.add({"office.deck_duplicate_slide", "Duplicate Slide", "Copy a slide, with its objects, notes and transition, right after it (or to position to=).",
             {doc(), integer("slide", "slide to copy", true), integer("to", "position of the copy (default: right after the original)")}, true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t i;
                std::string error;
                if (!slideArg(d.data, p, "slide", i, error)) return Result::failure(error);
                std::size_t to = i + 1;
                if (has(p, "to")) {
                    const std::int64_t t = intParam(p, "to");
                    if (t < 1 || t > static_cast<std::int64_t>(d.data.slides.size()) + 1) return Result::failure("to must be 1.." + std::to_string(d.data.slides.size() + 1));
                    to = static_cast<std::size_t>(t - 1);
                }
                Slide copy = d.data.slides[i];
                d.data.slides.insert(d.data.slides.begin() + static_cast<std::ptrdiff_t>(to), std::move(copy));
                return Result::success("copied slide " + std::to_string(i + 1) + " to " + describe(d.data, to), static_cast<std::int64_t>(to + 1))
                    .with("slide", static_cast<std::int64_t>(to + 1))
                    .with("slides", static_cast<std::int64_t>(d.data.slides.size()));
            }));

    reg.add({"office.deck_notes", "Speaker Notes", "List every slide's speaker notes (office.deck_edit_slide notes= changes them).",
             {doc(), fileSpec("slide deck")}, false, true},
            read(session, [](DeckDoc& d, const Params&, const std::string&) -> Result {
                std::string t;
                int with = 0;
                for (std::size_t i = 0; i < d.data.slides.size(); ++i) {
                    const Slide& s = d.data.slides[i];
                    t += "\n" + std::to_string(i + 1) + ". " + (s.title.empty() ? std::string("(no title)") : s.title) + (s.hidden ? " (hidden)" : "") + ": ";
                    if (s.notes.empty()) {
                        t += "(no notes)";
                        continue;
                    }
                    ++with;
                    std::string n = s.notes;
                    std::size_t pos = 0;
                    while ((pos = n.find('\n', pos)) != std::string::npos) n.replace(pos, 1, "\n     "), pos += 6;
                    t += n;
                }
                return Result::success(std::to_string(with) + " of " + std::to_string(d.data.slides.size()) + " slide(s) have notes:" + t)
                    .with("count", static_cast<std::int64_t>(with));
            }));

    reg.add({"office.deck_present", "Present Slide Show",
             "Make the deck a slide show to present from any browser: one .html file with every visible slide, the transitions and "
             "timings, speaker notes (N), a presenter window with the next slide, notes and a timer (P), full screen (F), black or "
             "white screen (B/W), and jumping to a slide by typing its number.",
             {doc(), str("path", "the .html file (default: next to the deck, <name>-show.html)"), integer("width", "slide image width in pixels", false, std::int64_t{1600}),
              flag("loop", "start again after the last slide (a kiosk show with advance_after timings)", false), integer("start", "open at this slide")},
             false, false},
            read(session, [](DeckDoc& d, const Params& p, const std::string& id) -> Result {
                std::string path = has(p, "path") ? stringParam(p, "path") : std::string();
                if (path.empty()) {
                    if (d.path.empty() || d.path == id) return Result::failure(id + " has no file yet; give path=<show.html>");
                    fs::path base = pathFromUtf8(d.path);
                    path = (base.parent_path() / (base.stem().string() + "-show.html")).string();
                }
                const auto visible = std::count_if(d.data.slides.begin(), d.data.slides.end(), [](const Slide& s) { return !s.hidden; });
                if (visible == 0) return Result::failure("no visible slides to show");
                writeBytes(path, writeSlideShow(d.data, static_cast<int>(intParam(p, "width", 1600)), boolParam(p, "loop", false)));
                std::string open = path;
                if (has(p, "start")) open += "#" + std::to_string(intParam(p, "start"));
                return Result::success("slide show of " + std::to_string(visible) + " slide(s) in " + path +
                                           ": open it in a browser. Space or arrow keys move on, F full screen, P presenter window, N notes, ? help.",
                                       path)
                    .with("path", path)
                    .with("open", open);
            }));
}

}  // namespace wetwaresuite::office::deckcmd
