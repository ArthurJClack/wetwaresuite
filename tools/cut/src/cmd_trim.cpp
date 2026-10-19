// The trim tools of a pro editor: ripple trim (the rest of the track follows),
// roll (move the cut between two clips), slip (change what a clip shows, not
// where it sits) and slide (move a clip between its neighbours). Each works on
// linked clips too; linked=false on one side makes J and L cuts.
#include <algorithm>
#include <cmath>

#include "internal.hpp"

namespace wetwaresuite::cut {

namespace {

ParamSpec deltaParam(const std::string& what) { return {"delta", ParamType::Number, what, false}; }

std::vector<std::string> members(Timeline& tl, const Params& p) {
    Clip* c = requireClip(tl, p);
    std::vector<std::string> ids{c->id};
    if (boolParam(p, "linked", true)) {
        for (Clip* x : group(tl, *c, true)) {
            if (x->id != c->id) ids.push_back(x->id);
        }
    }
    return ids;
}

void check(const Clip& c) {
    if (std::string err = validate(c); !err.empty()) throw std::runtime_error(c.id + ": " + err);
}

double amount(const Params& p, double current) {
    if (has(p, "to")) return numberParam(p, "to") - current;
    if (has(p, "delta")) return numberParam(p, "delta");
    throw std::runtime_error("give delta (seconds to move the edge) or to (where the edge goes)");
}

}  // namespace

void registerTrimCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cut.ripple_trim", "Ripple Trim",
             "Trim a clip's start or end edge and move everything after it on the track by as much, so no gap opens "
             "and nothing is covered. edge=end with delta=2 makes the clip 2 s longer and pushes the rest later; "
             "edge=start with delta=1 cuts its first second and pulls the rest earlier. Linked clips (its sound) "
             "trim and ripple too unless linked=false.",
             {docParam(), clipParam(), {"edge", ParamType::String, "which edge", false, {"start", "end"}, std::string("end")},
              deltaParam("how far the edge moves on the timeline, in seconds (positive = later)"),
              {"to", ParamType::Number, "or: the timeline time the edge goes to", false}, linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const bool end = stringParam(p, "edge", "end") != "start";
                Clip* c = requireClip(tl, p);
                const std::string first = c->id;
                const double d = amount(p, end ? c->end() : c->start);
                if (std::abs(d) < 1e-6) return Result::failure("the edge is already there");
                for (const auto& id : members(tl, p)) {
                    Track* t = trackOf(tl, id);
                    Clip& x = *tl.findClip(id);
                    if (end) {
                        trimEnd(x, d);
                        rippleTrack(*t, x.start + 1e-3, d, {x.id});
                    } else {
                        trimStart(x, d);
                        x.start -= d;
                        rippleTrack(*t, x.start + 1e-3, -d, {x.id});
                    }
                    check(x);
                }
                return clipResult("ripple-trimmed the " + std::string(end ? "end" : "start") + " of " + first + " by " + num(d) + "s", tl, p, first);
            }));

    reg.add({"cut.roll", "Roll Edit",
             "Move the cut between a clip and the clip right after it (edge=end) or right before it (edge=start): one "
             "gets longer, the other shorter, and nothing else moves. With linked=false only this track's cut moves: "
             "roll the sound's cut earlier for a J cut (the next shot's sound starts first) or later for an L cut.",
             {docParam(), clipParam(), {"edge", ParamType::String, "which of its cuts", false, {"start", "end"}, std::string("end")},
              deltaParam("how far the cut moves, in seconds (positive = later)"),
              {"to", ParamType::Number, "or: the timeline time the cut goes to", false}, linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const bool end = stringParam(p, "edge", "end") != "start";
                Clip* c = requireClip(tl, p);
                const std::string first = c->id;
                const double d = amount(p, end ? c->end() : c->start);
                int rolled = 0;
                for (const auto& id : members(tl, p)) {
                    Track* t = trackOf(tl, id);
                    Clip* x = tl.findClip(id);
                    Clip* left = end ? x : prevTouching(*t, *x);
                    Clip* right = end ? nextTouching(*t, *x) : x;
                    if (!left || !right) {
                        if (id == first) {
                            return Result::failure(first + " has no clip right " + (end ? "after" : "before") + " it on " + t->id +
                                                   " to roll with (use cut.ripple_trim or cut.trim_clip)");
                        }
                        continue;
                    }
                    trimEnd(*left, d);
                    trimStart(*right, d);
                    check(*left);
                    check(*right);
                    ++rolled;
                }
                Result r = clipResult("rolled the cut " + std::string(end ? "after " : "before ") + first + " by " + num(d) + "s" +
                                          (rolled > 1 ? " on " + std::to_string(rolled) + " tracks" : ""),
                                      tl, p, first);
                return r;
            }));

    reg.add({"cut.slip", "Slip Edit",
             "Change which part of the source a clip shows without moving or resizing it: delta=1 shows the source "
             "one second later. Linked clips slip with it unless linked=false.",
             {docParam(), clipParam(), {"delta", ParamType::Number, "source seconds to slip by (positive = later in the source)", true},
              linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                if (!c->timed()) return Result::failure("only video, audio and nested clips can slip; stills and titles show the same thing throughout");
                const std::string first = c->id;
                const double d = numberParam(p, "delta");
                for (const auto& id : members(tl, p)) {
                    Clip& x = *tl.findClip(id);
                    if (!x.timed()) continue;
                    x.in += d;
                    x.out += d;
                    check(x);
                }
                return clipResult("slipped " + first + " by " + num(d) + "s: it now shows " + num(tl.findClip(first)->in) + "-" +
                                      num(tl.findClip(first)->out) + "s of its source",
                                  tl, p, first);
            }));

    reg.add({"cut.slide", "Slide Edit",
             "Move a clip along the timeline between its neighbours: the clip before it gets longer and the one after "
             "it shorter (or the other way), so the total length stays the same and no gap opens. Linked clips slide "
             "too unless linked=false.",
             {docParam(), clipParam(), deltaParam("seconds to move it (positive = later)"),
              {"to", ParamType::Number, "or: the new start", false}, linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                const std::string first = c->id;
                const double d = amount(p, c->start);
                for (const auto& id : members(tl, p)) {
                    Track* t = trackOf(tl, id);
                    Clip* x = tl.findClip(id);
                    Clip* prev = prevTouching(*t, *x);
                    Clip* next = nextTouching(*t, *x);
                    if (prev) trimEnd(*prev, d);
                    if (next) trimStart(*next, d);
                    x->start += d;
                    if (x->start < -kEps) throw std::runtime_error("that would slide " + x->id + " before 0");
                    if (!prev && d < 0) {
                        for (const auto& o : t->clips) {
                            if (o.id != x->id && o.start < x->start && o.end() > x->start + 1e-3) throw std::runtime_error(x->id + " would cover " + o.id);
                        }
                    }
                    if (prev) check(*prev);
                    if (next) check(*next);
                    check(*x);
                }
                return clipResult("slid " + first + " by " + num(d) + "s", tl, p, first);
            }));
}

}  // namespace wetwaresuite::cut
