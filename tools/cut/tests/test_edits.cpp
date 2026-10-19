// The editing tools: linked clips, move/trim/split/razor/delete with ripple,
// ripple trim, roll (J/L cuts), slip, slide, track lock, insert/overwrite,
// lift/extract, markers, in/out, speed, nesting. Built from mattes, stills and
// generated WAVs so no ffmpeg is needed.
#include "support.hpp"

namespace cuttest {

namespace {

struct Fixture {
    Session s;
    std::shared_ptr<Timeline> tl;
    fs::path dir;
    std::string tone, red;
    // v1: c1 red 0-4, c2 blue 4-8; a1: c3 0-4, c4 4-8; c1+c3 and c2+c4 linked
    Fixture() {
        registerCommands(s);
        dir = tempDir("edits");
        tone = writeTone(dir / "tone.wav", 10, 220, 0.4);
        red = solidPng(dir / "red.png", {255, 0, 0, 255});
        run(s, "cut.new width=64 height=36 fps=10");
        tl = s.workspace().getAs<Timeline>("cut1");
        run(s, "cut.add_color doc=cut1 color=red duration=4");
        run(s, "cut.add_color doc=cut1 color=blue duration=4");
        run(s, "cut.add_audio doc=cut1 path=" + q(tone) + " duration=4");
        run(s, "cut.add_audio doc=cut1 path=" + q(tone) + " in=2 duration=4");  // c4 has 2s of handle before it
        run(s, "cut.link doc=cut1 clips=c1,c3");
        run(s, "cut.link doc=cut1 clips=c2,c4");
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Result go(const std::string& args) {
        const auto sp = args.find(' ');
        return run(s, args.substr(0, sp) + " doc=cut1" + (sp == std::string::npos ? std::string() : args.substr(sp)));
    }
    const Clip& c(const std::string& id) {
        static const Clip none;
        const Clip* x = tl->findClip(id);
        return x ? *x : none;
    }
    bool undo() { return run(s, "undo").ok; }
};

}  // namespace

void testEdits() {
    Fixture f;
    auto& tl = *f.tl;
    CHECK(near(tl.duration(), 8) && !f.c("c1").link.empty() && f.c("c1").link == f.c("c3").link && f.c("c2").link != f.c("c1").link);
    std::string before = toJson(tl);

    // linked moves and trims
    CHECK(f.go("cut.move_clip clip=c1 start=1").ok && near(f.c("c3").start, 1));
    CHECK(f.undo() && toJson(tl) == before);
    CHECK(f.go("cut.move_clip clip=c1 start=1 linked=false").ok && near(f.c("c3").start, 0));
    CHECK(f.undo());
    CHECK(f.go("cut.trim_clip clip=c3 duration=3").ok && near(f.c("c3").length(), 3) && near(f.c("c1").length(), 3));
    CHECK(f.undo());
    CHECK(f.go("cut.set_clip clip=c3 start=0.5").ok && near(f.c("c1").start, 0.5));
    CHECK(f.undo());

    // split and razor keep the right-hand parts linked to each other
    Result r = f.go("cut.split_clip clip=c1 at=2");
    CHECK(r.ok && text(r, "second") == "c5");
    CHECK(near(f.c("c1").end(), 2) && near(f.c("c5").start, 2) && f.c("c3").end() < 2.01 && f.c("c6").kind == "audio");
    CHECK(f.c("c5").link == f.c("c6").link && f.c("c5").link != f.c("c1").link && f.c("c1").link == f.c("c3").link);
    CHECK(f.undo() && toJson(tl) == before);
    CHECK(f.go("cut.set_cursor t=6").ok);
    r = f.go("cut.razor tracks=v1");
    CHECK(r.ok && num(r, "count") == 1);
    CHECK(f.undo());
    r = f.go("cut.razor at=6");
    CHECK(r.ok && num(r, "count") == 2 && tl.track("v1")->clips.size() == 3 && tl.track("a1")->clips.size() == 3);
    CHECK(f.c("c5").link == f.c("c6").link);
    CHECK(f.undo());
    CHECK(!f.go("cut.razor at=20").ok);

    // ripple delete takes the linked sound and closes the gap on both tracks
    CHECK(f.go("cut.delete_clip clip=c1 ripple=true").ok);
    CHECK(!tl.findClip("c3") && near(f.c("c2").start, 0) && near(f.c("c4").start, 0) && near(tl.duration(), 4));
    CHECK(f.undo() && toJson(tl) == before);
    CHECK(f.go("cut.delete_clip clip=c1 linked=false").ok && tl.findClip("c3"));
    CHECK(f.undo());
    // delete, then delete the gap
    CHECK(f.go("cut.delete_clip clip=c1").ok && near(tl.duration(), 8));
    CHECK(f.go("cut.delete_gap track=v1 at=1").ok && near(f.c("c2").start, 0) && near(f.c("c4").start, 0));
    CHECK(!f.go("cut.delete_gap track=v1 at=1").ok);  // no gap there now
    CHECK(f.undo() && f.undo() && toJson(tl) == before);
    // sync ripple: other tracks follow
    CHECK(f.go("cut.add_color color=green track=v2 start=5 duration=1").ok);  // c5
    CHECK(f.go("cut.delete_clip clip=c1 ripple=true sync=true").ok && near(f.c("c5").start, 1));
    CHECK(f.undo() && f.undo() && toJson(tl) == before);

    // a locked track refuses edits, also through links
    CHECK(f.go("cut.set_track track=a1 locked=true").ok);
    r = f.go("cut.move_clip clip=c1 start=1");
    CHECK(!r.ok && contains(r.message, "locked") && near(f.c("c1").start, 0));
    CHECK(f.go("cut.move_clip clip=c1 start=1 linked=false").ok);
    CHECK(f.go("cut.razor at=2").ok && tl.track("a1")->clips.size() == 2);  // the locked track is left alone
    CHECK(f.undo() && f.undo());
    CHECK(f.go("cut.set_track track=a1 locked=false").ok);
    TimelineLayout layout;
    tl.timeline(layout);
    CHECK(layout.tracks.size() == 2 && !layout.tracks[1].toggles[0].on);

    // ripple trims
    CHECK(f.go("cut.ripple_trim clip=c1 edge=end delta=1").ok);
    CHECK(near(f.c("c1").length(), 5) && near(f.c("c2").start, 5) && near(f.c("c3").length(), 5) && near(f.c("c4").start, 5) && near(tl.duration(), 9));
    CHECK(f.undo());
    CHECK(f.go("cut.ripple_trim clip=c3 edge=start delta=1").ok);
    CHECK(near(f.c("c3").start, 0) && near(f.c("c3").in, 1) && near(f.c("c3").length(), 3) && near(f.c("c1").length(), 3) && near(f.c("c2").start, 3) &&
          near(f.c("c4").start, 3));
    CHECK(f.undo());
    CHECK(f.go("cut.ripple_trim clip=c1 to=3").ok && near(f.c("c2").start, 3));
    CHECK(f.undo());
    CHECK(!f.go("cut.ripple_trim clip=c3 edge=start delta=5").ok);  // nothing left

    // roll: the cut moves, the total stays
    CHECK(f.go("cut.roll clip=c1 delta=1").ok);
    CHECK(near(f.c("c1").end(), 5) && near(f.c("c2").start, 5) && near(f.c("c2").length(), 3) && near(f.c("c3").end(), 5) && near(f.c("c4").start, 5) &&
          near(f.c("c4").in, 3) && near(tl.duration(), 8));
    CHECK(f.undo());
    CHECK(f.go("cut.roll clip=c2 edge=start delta=-1").ok && near(f.c("c1").end(), 3) && near(f.c("c2").start, 3));
    CHECK(f.undo());
    // a J cut: the next shot's sound starts a second early, the picture cut stays
    CHECK(f.go("cut.roll clip=c3 delta=-1 linked=false").ok);
    CHECK(near(f.c("c3").end(), 3) && near(f.c("c4").start, 3) && near(f.c("c1").end(), 4) && near(f.c("c2").start, 4));
    CHECK(f.undo());
    CHECK(!f.go("cut.roll clip=c2 delta=1").ok);  // nothing after it

    // slip: same place, other part of the source
    CHECK(f.go("cut.slip clip=c3 delta=2").ok && near(f.c("c3").in, 2) && near(f.c("c3").start, 0) && near(f.c("c3").length(), 4));
    CHECK(!f.go("cut.slip clip=c3 delta=-5").ok);   // before the source starts
    CHECK(!f.go("cut.slip clip=c4 delta=5").ok);    // past its end
    CHECK(!f.go("cut.slip clip=c1 delta=1").ok);    // a matte has nothing to slip
    CHECK(f.undo());

    // slide: the neighbours give and take
    CHECK(f.go("cut.add_color color=green duration=2").ok);  // c5 at 8-10
    CHECK(f.go("cut.slide clip=c2 delta=-1 linked=false").ok);
    CHECK(near(f.c("c1").end(), 3) && near(f.c("c2").start, 3) && near(f.c("c2").length(), 4) && near(f.c("c5").start, 7) && near(f.c("c5").length(), 3) &&
          near(tl.duration(), 10));
    CHECK(f.undo() && f.undo());

    // unlink: each goes its own way
    CHECK(f.go("cut.unlink clip=c1").ok && f.c("c1").link.empty() && f.c("c3").link.empty());
    CHECK(f.go("cut.move_clip clip=c1 start=1").ok && near(f.c("c3").start, 0));
    CHECK(!f.go("cut.unlink clip=c1").ok);
    CHECK(f.undo() && f.undo() && toJson(tl) == before);
}

void testSequence() {
    Fixture f;
    auto& tl = *f.tl;
    const std::string before = toJson(tl);

    // insert at the playhead pushes every track
    CHECK(f.go("cut.set_cursor t=2").ok);
    Result r = f.go("cut.insert path=" + q(f.red) + " duration=1");
    CHECK(r.ok && contains(r.message, "inserted"));
    const std::string ins = toText(r.value);
    CHECK(near(f.c(ins).start, 2) && near(tl.duration(), 9) && tl.track("v1")->clips.size() == 4 && tl.track("a1")->clips.size() == 3);
    CHECK(near(f.c("c2").start, 5) && near(f.c("c4").start, 5) && near(tl.cursor, 3));
    CHECK(f.undo() && toJson(tl) == before);
    // overwrite replaces just that stretch
    CHECK(f.go("cut.set_cursor t=2").ok);
    r = f.go("cut.overwrite path=" + q(f.red) + " duration=1");
    CHECK(r.ok && near(tl.duration(), 8) && tl.track("v1")->clips.size() == 4 && near(f.c("c2").start, 4));
    CHECK(f.undo());
    r = f.go("cut.overwrite path=" + q(f.tone) + " at=1 duration=2");  // a sound file goes on a1
    CHECK(r.ok && trackOfClip(tl, toText(r.value)) == "a1" && tl.track("a1")->clips.size() == 4);
    CHECK(f.undo() && toJson(tl) == before);

    // in/out, lift and extract
    CHECK(!f.go("cut.lift tracks=v1").ok);  // no in/out yet
    CHECK(f.go("cut.set_in_out in=2 out=6").ok && near(tl.inPoint, 2) && near(tl.outPoint, 6));
    TimelineLayout layout;
    tl.timeline(layout);
    CHECK(near(layout.inPoint, 2) && near(layout.outPoint, 6));
    CHECK(f.go("cut.lift").ok && near(tl.duration(), 8) && near(f.c("c1").end(), 2) && tl.track("v1")->clips.size() == 2);
    CHECK(f.undo());
    CHECK(f.go("cut.extract").ok && near(tl.duration(), 4) && tl.track("v1")->clips.size() == 2 && tl.track("a1")->clips.size() == 2);
    CHECK(f.c(tl.track("v1")->clips[1].id).color == "blue" && near(tl.track("v1")->clips[1].start, 2));
    CHECK(f.undo());
    CHECK(f.go("cut.extract tracks=a1 from=0 to=1").ok && near(f.c("c1").length(), 4));
    CHECK(near(tl.track("a1")->clips[0].start, 0) && near(tl.track("a1")->clips[0].length(), 3) && near(tl.track("a1")->clips[0].in, 1));
    CHECK(f.undo());
    CHECK(f.go("cut.set_in_out in=7").ok && tl.outPoint < 0);  // a new in after the out drops the out
    CHECK(f.go("cut.set_in_out clear=true").ok && tl.inPoint < 0);
    CHECK(f.undo() && f.undo() && f.undo());

    // markers
    r = f.go("cut.add_marker t=1 name=Intro");
    CHECK(r.ok && toText(r.value) == "m1");
    CHECK(f.go("cut.add_marker t=3 color=red").ok && !f.go("cut.add_marker t=3 color=plaid").ok);
    CHECK(f.go("cut.set_cursor t=0").ok && f.go("cut.set_cursor to=next_marker").ok && near(tl.cursor, 1));
    CHECK(f.go("cut.set_cursor to=next_marker").ok && near(tl.cursor, 3) && f.go("cut.set_cursor to=prev_marker").ok && near(tl.cursor, 1));
    r = f.go("cut.markers");
    CHECK(r.ok && num(r, "count") == 2 && contains(r.message, "Intro"));
    CHECK(f.go("cut.set_marker marker=m1 t=5 name=Later").ok && tl.markers.back().id == "m1");
    tl.timeline(layout);
    CHECK(layout.markers.size() == 2 && layout.markers[1].name == "Later");
    CHECK(f.go("cut.remove_marker marker=m2").ok && tl.markers.size() == 1 && !f.go("cut.remove_marker marker=m2").ok);
    CHECK(f.go("cut.remove_marker all=true").ok && tl.markers.empty());

    // speed on the sound, rippling the rest of its track
    CHECK(f.go("cut.clip_speed clip=c3 speed=2").ok && near(f.c("c3").length(), 2) && near(f.c("c4").start, 2));
    CHECK(f.go("cut.clip_speed clip=c3 duration=8").ok && near(f.c("c3").speed, 0.5) && near(f.c("c4").start, 8));
    CHECK(!f.go("cut.clip_speed clip=c1 speed=2").ok);  // a matte has no speed
    CHECK(f.undo() && f.undo());

    // a linked transition crossfades the sound too and keeps it in sync
    CHECK(f.go("cut.transition clip=c2 type=dissolve duration=1").ok);
    CHECK(near(f.c("c2").start, 3) && near(f.c("c4").start, 3) && f.c("c4").transition == "crossfade" && near(tl.duration(), 7));
    CHECK(f.undo());

    // nest a picture and its sound, then unnest
    r = f.go("cut.nest clips=c1");
    CHECK(r.ok);
    const std::string nest = toText(r.value);
    CHECK(f.c(nest).kind == "nest" && !tl.findClip("c1") && !tl.findClip("c3") && near(f.c(nest).length(), 4) && trackOfClip(tl, nest) == "v1");
    CHECK(close(px(frame(tl, 1), 32, 18), 255, 0, 0, 2));
    audio::Buffer m = mixAudio(tl, 0, 4);
    CHECK(rms(m, 0, 96000) > 0.2);  // the nest plays the sound inside
    const std::string saved = toJson(tl);
    CHECK(toJson(*fromJson(saved)) == saved);
    CHECK(f.go("cut.unnest clip=" + nest).ok && !tl.findClip(nest) && tl.track("v1")->clips.size() == 2 && tl.track("a1")->clips.size() == 2);
    CHECK(close(px(frame(tl, 1), 32, 18), 255, 0, 0, 2));
    CHECK(f.undo() && f.undo() && toJson(tl) == before);
}

}  // namespace cuttest
