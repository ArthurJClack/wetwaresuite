// The basic edits: move, trim, split, razor, delete (with ripple), close gaps,
// set any property, and linking a picture with its sound.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "internal.hpp"

namespace wetwaresuite::cut {

namespace {

bool timingParams(const Params& p) {
    for (const char* k : {"start", "in", "out", "duration", "speed", "reverse"}) {
        if (has(p, k)) return true;
    }
    return false;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}

}  // namespace

void registerEditCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cut.move_clip", "Move Clip",
             "Move a clip to a new start time and/or another track of the same kind. Its linked clips (a video's "
             "sound) move by as much unless linked=false.",
             {docParam(), clipParam(), {"start", ParamType::Number, "new start on the timeline, in seconds", false},
              {"track", ParamType::String, "move to this track (v<n> for pictures, a<n> for audio)", false}, linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                if (!has(p, "start") && !has(p, "track")) return Result::failure("give a new start and/or track");
                const std::string id = c->id;
                const double delta = has(p, "start") ? numberParam(p, "start") - c->start : 0;
                for (Clip* x : group(tl, *c, boolParam(p, "linked", true))) {
                    x->start += delta;
                    if (x->start < -kEps) return Result::failure("that would move " + x->id + " before 0");
                    x->start = std::max(0.0, x->start);
                }
                Track* owner = trackOf(tl, id);
                std::string where = owner->id;
                if (has(p, "track") && stringParam(p, "track") != owner->id) {
                    const std::string from = owner->id;
                    Track& dest = tl.ensureTrack(stringParam(p, "track"));
                    owner = tl.track(from);  // ensureTrack may have moved the tracks
                    Clip moved = *tl.findClip(id);
                    if ((dest.kind == "audio") != (moved.kind == "audio")) return Result::failure("a " + moved.kind + " clip can't go on " + dest.id);
                    owner->clips.erase(std::find_if(owner->clips.begin(), owner->clips.end(), [&](const Clip& x) { return x.id == id; }));
                    dest.clips.push_back(moved);
                    where = dest.id;
                }
                return clipResult("moved " + id + " to " + where + " at " + num(tl.findClip(id)->start) + "s", tl, p, id);
            }));

    reg.add({"cut.trim_clip", "Trim Clip",
             "Change which part of the source a clip plays: in and out are source times in seconds, or give duration "
             "(timeline seconds). The clip keeps its start; stills and titles take duration only. Linked clips trim "
             "with it unless linked=false. For edits that move the neighbours too see cut.ripple_trim, cut.roll, "
             "cut.slip and cut.slide.",
             {docParam(), clipParam(), propSpecs({"in"})[0], propSpecs({"out"})[0], propSpecs({"duration"})[0], linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                if (!has(p, "in") && !has(p, "out") && !has(p, "duration")) return Result::failure("give in, out or duration");
                const Clip before = *c;
                Clip copy = *c;
                if (std::string err = applyProps(copy, p); !err.empty()) return Result::failure(err);
                if (std::string err = validate(copy); !err.empty()) return Result::failure(err);
                *c = copy;
                if (boolParam(p, "linked", true)) followLinked(tl, before, copy);
                return clipResult("trimmed " + copy.id + " to " + num(copy.length()) + "s", tl, p, copy.id);
            }));

    reg.add({"cut.split_clip", "Split Clip",
             "Cut a clip in two at a timeline time (default: the playhead). The first part keeps the id; the second "
             "gets a new one (returned). Linked clips are cut at the same time unless linked=false.",
             {docParam(), clipParam(), {"at", ParamType::Number, "timeline time to cut at, in seconds (default the playhead)", false},
              linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                const double at = numberParam(p, "at", tl.cursor);
                const std::string first = c->id;
                std::vector<std::pair<std::string, std::string>> parts;
                std::vector<std::string> ids;
                for (Clip* x : group(tl, *c, boolParam(p, "linked", true))) ids.push_back(x->id);
                std::string second;
                for (const auto& id : ids) {
                    Clip* x = tl.findClip(id);
                    if (id != first && (at <= x->start + 1e-3 || at >= x->end() - 1e-3)) continue;  // a linked clip not under the cut
                    const std::string link = x->link;
                    const std::string s = splitClip(tl, *trackOf(tl, id), id, at);
                    parts.push_back({link, s});
                    if (id == first) second = s;
                }
                relinkParts(tl, parts);
                Result r = clipResult("split " + first + " at " + num(at) + "s into " + first + " and " + second +
                                          (parts.size() > 1 ? " (and " + std::to_string(parts.size() - 1) + " linked)" : ""),
                                      tl, p, second);
                r.with("first", first).with("second", second);
                return r;
            }));

    reg.add({"cut.razor", "Razor",
             "Cut through every track at one time (default: the playhead), like the razor tool across all tracks: "
             "each clip under it is split in two. tracks limits it (e.g. v1,a1); locked tracks are left alone.",
             {docParam(), {"at", ParamType::Number, "timeline time, in seconds (default the playhead)", false},
              {"tracks", ParamType::String, "only these tracks, comma separated (default every unlocked track)", false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const double at = numberParam(p, "at", tl.cursor);
                std::vector<std::pair<std::string, std::string>> parts;
                std::vector<std::string> made;
                for (const auto& tid : targetTracks(tl, stringParam(p, "tracks"))) {
                    std::vector<std::string> under;
                    for (const auto& c : tl.track(tid)->clips) {
                        if (at > c.start + 1e-3 && at < c.end() - 1e-3) under.push_back(c.id);
                    }
                    for (const auto& id : under) {
                        const std::string link = tl.findClip(id)->link;
                        const std::string s = splitClip(tl, *tl.track(tid), id, at);
                        parts.push_back({link, s});
                        made.push_back(s);
                    }
                }
                if (made.empty()) return Result::failure("no clip is under " + num(at) + "s on those tracks");
                relinkParts(tl, parts);
                Result r = Result::success("cut " + std::to_string(made.size()) + " clip" + (made.size() == 1 ? "" : "s") + " at " + num(at) +
                                           "s; the right-hand parts are " + joined(made));
                return r.with("doc", stringParam(p, "doc")).with("at", at).with("clips", joined(made))
                    .with("count", static_cast<std::int64_t>(made.size()));
            }));

    reg.add({"cut.delete_clip", "Delete Clip",
             "Remove a clip (and its linked clips unless linked=false). With ripple=true the clips after it move left "
             "to close the gap (ripple delete); sync=true also pulls every other unlocked track along so everything "
             "stays in sync.",
             {docParam(), clipParam(), {"ripple", ParamType::Bool, "close the gap it leaves", false, {}, false},
              {"sync", ParamType::Bool, "with ripple: move the other unlocked tracks too", false, {}, false}, linkedParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                struct Gone {
                    std::string id, track;
                    double start, end, shift;
                };
                std::vector<Gone> gone;
                for (Clip* x : group(tl, *c, boolParam(p, "linked", true))) {
                    Track* t = trackOf(tl, x->id);
                    gone.push_back({x->id, t->id, x->start, x->end(), x->length() - incomingOverlap(tl, *t, *x)});
                }
                for (const auto& g : gone) removeClip(tl, *tl.track(g.track), g.id);
                std::string text = "deleted " + gone.front().id;
                if (gone.size() > 1) text += " and " + std::to_string(gone.size() - 1) + " linked";
                if (boolParam(p, "ripple", false)) {
                    std::set<std::string> touched;
                    for (const auto& g : gone) {
                        rippleTrack(*tl.track(g.track), g.start, -g.shift);
                        touched.insert(g.track);
                    }
                    if (boolParam(p, "sync", false)) {
                        const Gone& g = gone.front();
                        for (auto& t : tl.tracks) {
                            if (t.locked || touched.count(t.id)) continue;
                            const bool blocked = std::any_of(t.clips.begin(), t.clips.end(),
                                                             [&](const Clip& x) { return x.start < g.end - 1e-3 && x.end() > g.start + 1e-3; });
                            if (blocked) {
                                text += "; " + t.id + " has a clip in the way, left as it is";
                                continue;
                            }
                            rippleTrack(t, g.end, -g.shift);
                        }
                    }
                    text += ", closing the gap";
                }
                Result r = Result::success(text + " (timeline " + num(tl.duration()) + "s)");
                return r.with("doc", stringParam(p, "doc")).with("duration", tl.duration());
            }));

    reg.add({"cut.delete_gap", "Delete Gap",
             "Close the empty space on a track at a time (default: the playhead): everything after it on that track "
             "(with linked clips) moves left. all_tracks=true moves every unlocked track that's empty there too.",
             {docParam(), {"track", ParamType::String, "track with the gap", false, {}, std::string("v1")},
              {"at", ParamType::Number, "a time inside the gap (default the playhead)", false},
              {"all_tracks", ParamType::Bool, "close it on every unlocked track that's empty there", false, {}, false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const std::string tid = stringParam(p, "track", "v1");
                Track* t = tl.track(tid);
                if (!t) return Result::failure("no track called '" + tid + "'");
                const double at = numberParam(p, "at", tl.cursor);
                double from = 0, to = -1;
                for (const auto& c : t->clips) {
                    if (c.start <= at + 1e-4 && c.end() > at + 1e-4) return Result::failure("there's a clip (" + c.id + ") at " + num(at) + "s, not a gap");
                    if (c.end() <= at + 1e-4) from = std::max(from, c.end());
                    if (c.start > at - 1e-4 && (to < 0 || c.start < to)) to = c.start;
                }
                if (to < 0 || to - from < 1e-3) return Result::failure("there's no gap at " + num(at) + "s on " + tid);
                const double len = to - from;
                std::set<std::string> moved;
                auto shift = [&](Clip& x) {
                    if (moved.insert(x.id).second) x.start = std::max(0.0, x.start - len);
                };
                if (boolParam(p, "all_tracks", false)) {
                    for (auto& tr : tl.tracks) {
                        if (tr.locked) continue;
                        const bool busy = std::any_of(tr.clips.begin(), tr.clips.end(), [&](const Clip& x) { return x.start < to - 1e-3 && x.end() > from + 1e-3; });
                        if (busy) continue;
                        for (auto& x : tr.clips) {
                            if (x.start >= to - 1e-4) shift(x);
                        }
                    }
                } else {
                    std::vector<std::string> ids;
                    for (const auto& x : t->clips) {
                        if (x.start >= to - 1e-4) ids.push_back(x.id);
                    }
                    for (const auto& id : ids) {
                        for (Clip* x : group(tl, *tl.findClip(id), true)) shift(*x);
                    }
                }
                Result r = Result::success("closed the " + num(len) + "s gap at " + num(from) + "s on " + tid + " (timeline " + num(tl.duration()) + "s)");
                return r.with("doc", stringParam(p, "doc")).with("gap", len).with("duration", tl.duration());
            }));

    {
        std::vector<ParamSpec> s{docParam(), clipParam()};
        for (auto& x : propSpecs(allPropNames())) s.push_back(x);
        s.push_back(linkedParam());
        s.push_back(previewParam());
        reg.add({"cut.set_clip", "Set Clip",
                 "Change any property of a clip: timing (start, in, out, duration, speed, reverse), sound (volume, "
                 "mute, pan), fades, opacity and blend mode, the picture box and transform (x, y, width, height, fit, "
                 "crop, scale, rotation, edge crops, flips), colour correction, effects, chroma key and, for titles, "
                 "text and styling. Only the given ones change. Timing changes follow on linked clips.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    Clip* c = requireClip(tl, p);
                    const Clip before = *c;
                    Clip copy = *c;
                    if (std::string err = applyProps(copy, p); !err.empty()) return Result::failure(err);
                    // trimming the start edge (start and in together): keyframes stay with the media
                    if (has(p, "start") && has(p, "in") && copy.timed()) shiftKeys(copy, -(copy.start - before.start));
                    if (std::string err = validate(copy); !err.empty()) return Result::failure(err);
                    *c = copy;
                    if (timingParams(p) && boolParam(p, "linked", true)) followLinked(tl, before, copy);
                    return clipResult("updated " + copy.id, tl, p, copy.id);
                }));
    }

    reg.add({"cut.link", "Link Clips",
             "Link clips (usually a picture and a sound) so moving, trimming, cutting and deleting one changes them all.",
             {docParam(), {"clips", ParamType::String, "two or more clip ids, comma separated", true}}, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                auto ids = splitList(stringParam(p, "clips"));
                if (ids.size() < 2) return Result::failure("give two or more clips to link, e.g. clips=c1,c2");
                const std::string link = tl.newLinkId();
                for (const auto& id : ids) {
                    Clip* c = tl.findClip(id);
                    if (!c) return Result::failure("no clip called '" + id + "'");
                    // a clip leaving another link leaves that link's other clips alone
                    c->link = link;
                }
                Result r = Result::success("linked " + joined(ids));
                return r.with("doc", stringParam(p, "doc")).with("link", link);
            }));

    reg.add({"cut.unlink", "Unlink Clips",
             "Break the link between a clip and its linked clips (a video and its sound), so each can be moved, "
             "trimmed and deleted on its own. The video stays silent: its sound is the separate audio clip.",
             {docParam(), clipParam()}, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                if (c->link.empty()) return Result::failure(c->id + " isn't linked to anything");
                std::vector<std::string> ids;
                for (Clip* x : group(tl, *c, true)) {
                    ids.push_back(x->id);
                    x->link.clear();
                }
                Result r = Result::success("unlinked " + joined(ids));
                return r.with("doc", stringParam(p, "doc")).with("clips", joined(ids));
            }));
}

}  // namespace wetwaresuite::cut
