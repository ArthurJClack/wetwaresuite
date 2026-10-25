// The cut tool: the model and its JSON, export planning, the basic commands,
// MCP, and real exports when ffmpeg is installed. Pixels, sound and the editing
// tools are in test_render.cpp and test_edits.cpp.
#include <fstream>

#include "support.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mcp/server.hpp"

namespace cuttest {

namespace {

int count(const std::vector<std::string>& args, const std::string& value) {
    int n = 0;
    for (const auto& a : args) {
        if (a == value) ++n;
    }
    return n;
}

// the value right after `flag` in an argv, or empty
std::string after(const std::vector<std::string>& args, const std::string& flag, int nth = 0) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag && nth-- == 0) return args[i + 1];
    }
    return {};
}

Clip media(const std::string& id, const std::string& src, double in, double out, double start, bool audio = true) {
    Clip c;
    c.id = id;
    c.kind = "video";
    c.source = src;
    c.in = in;
    c.out = out;
    c.start = start;
    c.sourceDuration = 100;
    c.sourceWidth = 1280;
    c.sourceHeight = 720;
    c.sourceFps = 30;
    c.hasAudio = audio;
    c.videoCodec = "h264";
    c.audioCodec = audio ? "aac" : "";
    return c;
}

Timeline basic() {
    Timeline tl;
    tl.settings.width = 1280;
    tl.settings.height = 720;
    tl.settings.fps = 30;
    tl.ensureTrack("v1").clips = {media("c1", "/m/a.mp4", 1, 5, 0), media("c2", "/m/b.mp4", 0, 4, 4)};
    tl.nextClip = 3;
    return tl;
}

bool throws(const std::function<void()>& fn, const std::string& part = {}) {
    try {
        fn();
    } catch (const std::exception& e) {
        return part.empty() || contains(e.what(), part);
    }
    return false;
}

}  // namespace

void testModel() {
    Timeline tl = basic();
    CHECK(near(tl.duration(), 8));
    CHECK(tl.findClip("c2") && tl.findClip("c2")->start == 4);
    CHECK(near(tl.trackEnd("v1"), 8));
    Track& v3 = tl.ensureTrack("v3");
    CHECK(v3.kind == "video");
    tl.ensureTrack("a1");
    tl.ensureTrack("v2");
    CHECK(tl.tracks.size() == 4 && tl.tracks[0].id == "v1" && tl.tracks[1].id == "v2" && tl.tracks[2].id == "v3" && tl.tracks[3].id == "a1");
    CHECK(throws([&] { tl.ensureTrack("x1"); }));
    const Track* v1 = tl.track("v1");
    CHECK(tl.previousMedia(*v1, *tl.findClip("c2")) == tl.findClip("c1"));
    CHECK(tl.previousMedia(*v1, *tl.findClip("c1")) == nullptr);
    Clip slow = media("x", "/m/x.mp4", 0, 4, 0);
    slow.speed = 0.5;
    CHECK(near(slow.length(), 8));
    CHECK(near(slow.sourceTime(2), 1));
    slow.reverse = true;
    CHECK(near(slow.sourceTime(2), 3, 1e-3));
    slow.hold = true;
    CHECK(near(slow.sourceTime(5), 0));

    // keyframes interpolate with easing, and hold steps
    std::vector<Key> keys = {{0, 0, "linear"}, {2, 10, "hold"}, {4, 20, "linear"}};
    CHECK(near(evaluateKeys(keys, 1), 5) && near(evaluateKeys(keys, 3), 10) && near(evaluateKeys(keys, 9), 20) && near(evaluateKeys(keys, -1), 0));
    keys[0].ease = "ease_in_out";
    CHECK(near(evaluateKeys(keys, 1), 5, 1e-6) && evaluateKeys(keys, 0.5) < 2.5);
    Clip anim;
    anim.keys["opacity"] = {{0, 0}, {1, 1}};
    CHECK(near(anim.value("opacity", 0.5), 0.5) && near(anim.value("scale", 0.5), 1));
    CHECK(propertyField(anim, "rotation") && !propertyField(anim, "nope"));

    // solo and mute decide what plays
    Timeline st = basic();
    st.ensureTrack("a1");
    st.ensureTrack("a2").solo = true;
    CHECK(!st.sounds(*st.track("a1")) && st.sounds(*st.track("a2")) && st.sounds(*st.track("v1")));

    // JSON round trip keeps every property
    Clip& c2 = tl.track("v1")->clips[1];
    c2.transition = "wipeleft";
    c2.transitionDuration = 0.5;
    c2.brightness = 0.1;
    c2.cropX = 10;
    c2.cropY = 20;
    c2.cropW = 640;
    c2.cropH = 360;
    c2.width = 320;
    c2.wheelGain = {0.1, 0, -0.1, 0.2};
    c2.curve = {0, 0, 0.5, 0.6, 1, 1};
    c2.keyColor = "#00ff00";
    c2.blend = "screen";
    c2.reverse = true;
    c2.link = "L1";
    c2.keys["x"] = {{0, 10, "ease_out"}, {1, 20}};
    Clip title;
    title.id = "c3";
    title.kind = "title";
    title.text = "Hello: \"world\"";
    title.out = 2;
    title.start = 1;
    title.box = true;
    title.outline = 3;
    title.animIn = "typewriter";
    title.style = "lower_third";
    title.subtitle = "sub";
    title.caption = true;
    tl.track("v3")->clips.push_back(title);
    tl.nextClip = 4;
    tl.track("v2")->hidden = true;
    tl.track("v2")->locked = true;
    tl.track("a1")->volume = 0.5;
    tl.markers.push_back({"m1", 2, 0, "beat", "red"});
    tl.nextMarker = 2;
    tl.inPoint = 1;
    tl.outPoint = 3;
    tl.masterVolume = 0.8;
    auto nest = std::make_shared<Timeline>(basic());
    Clip n;
    n.id = "c9";
    n.kind = "nest";
    n.source = "Nest";
    n.out = 2;
    n.start = 3;
    n.nested = nest;
    tl.track("v3")->clips.push_back(n);
    tl.nextClip = 10;
    tl.nextLink = 2;
    const std::string json = toJson(tl);
    auto back = fromJson(json);
    CHECK(back->tracks.size() == 4);
    CHECK(toJson(*back) == json);
    CHECK(back->findClip("c2")->transition == "wipeleft" && near(back->findClip("c2")->cropW, 640));
    CHECK(back->findClip("c2")->wheelGain == c2.wheelGain && back->findClip("c2")->curve == c2.curve && back->findClip("c2")->reverse);
    CHECK(back->findClip("c2")->keys.at("x").size() == 2 && back->findClip("c2")->keys.at("x")[0].ease == "ease_out");
    CHECK(back->findClip("c3")->text == "Hello: \"world\"" && back->findClip("c3")->box && back->findClip("c3")->caption);
    CHECK(back->findClip("c9")->nested && back->findClip("c9")->nested->tracks.size() == 1);
    CHECK(back->track("v2")->hidden && back->track("v2")->locked && near(back->track("a1")->volume, 0.5));
    CHECK(back->markers.size() == 1 && back->markers[0].name == "beat" && near(back->inPoint, 1) && near(back->masterVolume, 0.8));
    CHECK(back->nextClip == 10);
    CHECK(throws([] { fromJson("{\"format\":\"something\"}"); }));
    // relative sources resolve against the project's folder
    auto rel = fromJson(R"({"format":"wwcut","tracks":[{"id":"v1","clips":[{"id":"c1","kind":"video","source":"clip.mp4","in":0,"out":1}]}]})",
                        fs::path("/proj"));
    CHECK(fs::path(rel->findClip("c1")->source) == fs::path("/proj") / "clip.mp4");
    CHECK(rel->nextClip == 2);
    // timelines saved before the rename ("achcut") still load, CRLF line ends too
    auto old = fromJson("{\r\n\"format\": \"achcut\",\r\n\"tracks\": []\r\n}\r\n");
    CHECK(old != nullptr);

    MediaInfo m = parseProbe(R"({"streams":[{"codec_type":"video","codec_name":"h264","width":1920,"height":1080,"avg_frame_rate":"30000/1001"},
        {"codec_type":"audio","codec_name":"aac","sample_rate":"48000","channels":2}],"format":{"format_name":"mov,mp4","duration":"12.5"}})");
    CHECK(m.hasVideo && m.hasAudio && !m.still);
    CHECK(m.width == 1920 && near(m.fps, 29.97, 0.01) && near(m.duration, 12.5));
    CHECK(m.streams.size() == 2 && m.streams[1].sampleRate == 48000 && m.streams[1].channels == 2);
    MediaInfo png = parseProbe(R"({"streams":[{"codec_type":"video","codec_name":"png","width":64,"height":32,"r_frame_rate":"25/1"}],
        "format":{"format_name":"png_pipe"}})");
    CHECK(png.still && png.width == 64);

    // captions
    auto cues = parseCaptions("\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hello</i>\r\nthere\r\n\r\n2\n00:01:00.000 --> 00:01:01.000\nBye\n");
    CHECK(cues.size() == 2 && near(cues[0].start, 1) && near(cues[0].end, 2.5) && cues[0].text == "Hello\nthere" && near(cues[1].start, 60));
    auto vtt = parseCaptions("WEBVTT\n\n00:01.500 --> 00:02.000 align:start\nHi\n");
    CHECK(vtt.size() == 1 && near(vtt[0].start, 1.5));
    CHECK(contains(writeSrt(cues), "00:00:01,000 --> 00:00:02,500\nHello\nthere"));
    CHECK(contains(writeVtt(cues), "WEBVTT") && contains(writeVtt(cues), "00:01:00.000 --> 00:01:01.000"));
    CHECK(parseCaptions(writeSrt(cues)).size() == 2);
    CHECK(throws([] { parseCaptions("nothing here"); }));
}

void testPlan() {
    Timeline tl = basic();
    ExportRequest rq;
    rq.output = "/out/x.mp4";
    rq.listPath = "/tmp/list.txt";
    rq.audioPath = "/tmp/mix.wav";
    CHECK(streamCopyBlocker(tl, "mp4").empty());
    EncodePlan plan = planExport(tl, rq, true);
    CHECK(plan.streamCopy);
    CHECK(plan.concatList == "file '/m/a.mp4'\ninpoint 1\noutpoint 5\nfile '/m/b.mp4'\ninpoint 0\noutpoint 4\n");
    CHECK(after(plan.args, "-f") == "concat" && after(plan.args, "-c") == "copy" && plan.args.back() == "/out/x.mp4");
    CHECK(contains(streamCopyBlocker(tl, "webm"), "differs"));
    rq.output = "/out/x.mov";
    CHECK(planExport(tl, rq, true).streamCopy);  // mp4 and mov hold the same streams
    tl.track("v1")->clips[1].volume = 0.5;
    CHECK(contains(streamCopyBlocker(tl, "mp4"), "c2"));
    rq.mode = "copy";
    CHECK(throws([&] { planExport(tl, rq, true); }, "stream-copy"));

    // encoding: composited frames on stdin plus the mixed sound
    rq.mode = "auto";
    rq.output = "/out/x.mp4";
    plan = planExport(tl, rq, true);
    CHECK(!plan.streamCopy && plan.video && plan.audio);
    CHECK(after(plan.args, "-f") == "rawvideo" && after(plan.args, "-pix_fmt") == "rgba" && after(plan.args, "-s") == "1280x720");
    CHECK(after(plan.args, "-i") == "pipe:0" && after(plan.args, "-i", 1) == "/tmp/mix.wav");
    CHECK(after(plan.args, "-c:v") == "libx264" && after(plan.args, "-c:a") == "aac" && after(plan.args, "-t") == "8");
    CHECK(plan.frames == 240 && near(plan.fps, 30));
    CHECK(!contains(plan.args.back(), "pipe"));
    // no sound: no audio input
    plan = planExport(tl, rq, false);
    CHECK(count(plan.args, "-i") == 1 && after(plan.args, "-c:a").empty());

    // a range
    rq.start = 2;
    rq.end = 5;
    plan = planExport(tl, rq, true);
    CHECK(near(plan.start, 2) && near(plan.duration, 3) && plan.frames == 90);
    rq.start = 0;
    rq.end = -1;

    // presets
    CHECK(findPreset("shorts") && findPreset("shorts")->name == "youtube_shorts" && findPreset("4k")->width == 3840 && !findPreset("nope"));
    CHECK(findPreset("Reels")->name == "instagram_reels" && findPreset("twitter")->name == "x" && findPreset("9:16")->name == "vertical");
    CHECK(presets().size() >= 8);
    rq.preset = "vertical";
    plan = planExport(tl, rq, true);
    CHECK(plan.width == 1080 && plan.height == 1920 && plan.reframe == "fill");
    rq.preset = "youtube_4k";
    plan = planExport(tl, rq, true);
    CHECK(plan.width == 3840 && plan.height == 2160);
    rq.preset = "prores";
    rq.output = "/out/x.mov";
    plan = planExport(tl, rq, true);
    CHECK(after(plan.args, "-c:v") == "prores_ks" && after(plan.args, "-c:a") == "pcm_s16le");
    rq.preset = "nope";
    CHECK(throws([&] { planExport(tl, rq, true); }, "preset"));

    // platform limits: length, frame rate and file size
    rq.output = "/out/x.mp4";
    rq.preset = "discord";
    plan = planExport(tl, rq, true);  // 8 s fits 10 MB easily: full 720p, capped bitrate
    CHECK(plan.width == 1280 && plan.height == 720 && near(plan.maxBytes, 10e6) && plan.videoKbps == 9072);
    CHECK(after(plan.args, "-maxrate") == "9072k" && after(plan.args, "-b:a") == "128k");
    Timeline longer = tl;
    longer.track("v1")->clips[1].out = 296;  // 5 minutes
    plan = planExport(longer, rq, true);
    CHECK(plan.width == 640 && plan.height == 360 && after(plan.args, "-maxrate") == "181k" && after(plan.args, "-b:a") == "64k");
    rq.shrink = 0.5;  // a retry after missing the limit
    CHECK(throws([&] { planExport(longer, rq, true); }, "can't fit"));
    rq.shrink = 1;
    longer.track("v1")->clips[1].out = 1000;
    CHECK(throws([&] { planExport(longer, rq, true); }, "can't fit"));
    rq.preset = "tiktok";
    longer.track("v1")->clips[1].out = 700;
    CHECK(throws([&] { planExport(longer, rq, true); }, "up to 10:00"));
    rq.end = 590;  // a part that fits
    CHECK(planExport(longer, rq, true).height == 1920);
    rq.end = -1;
    tl.settings.fps = 60;
    rq.preset = "x";
    CHECK(near(planExport(tl, rq, true).fps, 40));
    rq.preset = "youtube_shorts";
    CHECK(near(planExport(tl, rq, true).fps, 60));
    tl.settings.fps = 30;
    rq.output = "/out/a.gif";
    rq.preset = "gif_discord";
    plan = planExport(tl, rq, true);
    CHECK(plan.width == 480 && near(plan.fps, 15) && near(plan.maxBytes, 10e6) && plan.videoKbps == 0);
    rq.shrink = 0.5;
    plan = planExport(tl, rq, true);
    CHECK(plan.width == 240 && near(plan.fps, 11));
    rq.shrink = 1;
    CHECK(presetLimits(*findPreset("tiktok")) == "up to 10:00, 287 MB, 60 fps" && presetLimits(*findPreset("prores")).empty());
    rq.preset.clear();

    // gif, webm, sound only
    rq.output = "/out/a.gif";
    plan = planExport(tl, rq, true);
    CHECK(plan.width == 480 && plan.height == 270 && near(plan.fps, 15) && !plan.audio);
    CHECK(contains(after(plan.args, "-filter_complex"), "palettegen") && after(plan.args, "-loop") == "0");
    rq.output = "/out/a.webm";
    plan = planExport(tl, rq, true);
    CHECK(after(plan.args, "-c:v") == "libvpx-vp9" && after(plan.args, "-c:a") == "libopus");
    rq.output = "/out/a.mp3";
    plan = planExport(tl, rq, true);
    CHECK(!plan.video && after(plan.args, "-i") == "/tmp/mix.wav" && after(plan.args, "-c:a") == "libmp3lame");
    CHECK(throws([&] { planExport(tl, rq, false); }, "no sound"));
    rq.output = "/out/a.avi";
    CHECK(throws([&] { planExport(tl, rq, true); }));
    Timeline empty;
    rq.output = "/out/e.mp4";
    CHECK(throws([&] { planExport(empty, rq, true); }, "empty"));
}

// Edits that don't need ffmpeg: titles, timing, undo, saving.
void testCommands() {
    Session s;
    registerCommands(s);
    CHECK(run(s, "cut.new width=1080 height=1920 fps=25").ok);
    CHECK(!run(s, "cut.new width=1081").ok);
    Result r = run(s, "cut.add_title doc=cut1 text=\"Hello there\" start=1 duration=4 font_size=90 color=yellow fade_in=0.5");
    CHECK(r.ok && toText(r.value) == "c1" && near(num(r, "end"), 5));
    r = run(s, "cut.add_title doc=cut1 text=Second start=2 duration=1 x=100 y=200");
    CHECK(r.ok && toText(r.value) == "c2");
    auto tl = s.workspace().getAs<Timeline>("cut1");
    CHECK(tl->track("v1") && tl->track("v1")->clips.size() == 2);  // titles share the titles track
    CHECK(tl->findClip("c2")->position == "custom");
    CHECK(near(tl->duration(), 5));
    CHECK(s.registry().find("cut.play") && run(s, "cut.stop").ok && run(s, "cut.playhead").ok);
    {
        // the window's timeline: layout and the command each drag becomes
        TimelineLayout layout;
        CHECK(tl->timeline(layout) && layout.docParam == "doc" && layout.tracks.size() == 1 && layout.tracks[0].clips.size() == 2);
        CHECK(layout.tracks[0].clips[0].label == "\"Hello there\"" && layout.tracks[0].clips[0].headroom < 0);
        CHECK(layout.tools && layout.tracks[0].toggles.size() == 4 && layout.tracks[0].toggles[0].name == "locked");
        CHECK(tl->timelineCommand({TimelineEditKind::Move, "c2", "v1", "v1", 2.5}) == "cut.move_clip clip=c2 start=2.5000");
        CHECK(tl->timelineCommand({TimelineEditKind::Move, "c2", "v1", "v2", 2.5}) == "cut.move_clip clip=c2 start=2.5000 track=v2");
        CHECK(tl->timelineCommand({TimelineEditKind::TrimEnd, "c1", "v1", "", 0, 3}) == "cut.trim_clip clip=c1 duration=3.0000");
        CHECK(tl->timelineCommand({TimelineEditKind::TrimStart, "c1", "v1", "", 2}) == "cut.set_clip clip=c1 start=2.0000 duration=3.0000");
        CHECK(tl->timelineCommand({TimelineEditKind::Split, "c1", "v1", "", 0, 0, 3}) == "cut.split_clip clip=c1 at=3.0000");
        CHECK(tl->timelineCommand({TimelineEditKind::Delete, "nope", "v1"}).empty());
        CHECK(tl->timelineCommand({TimelineEditKind::Razor, "", "", "", 0, 0, 2}) == "cut.razor at=2.0000");
        CHECK(tl->timelineCommand({TimelineEditKind::RippleDelete, "c1", "v1"}) == "cut.delete_clip clip=c1 ripple=true");
        CHECK(tl->timelineCommand({TimelineEditKind::Cursor, "", "", "", 0, 0, 1.5}) == "cut.set_cursor t=1.5000");
        CHECK(tl->timelineCommand({TimelineEditKind::Marker, "", "", "", 0, 0, 1}) == "cut.add_marker t=1.0000");
        CHECK(tl->timelineCommand({TimelineEditKind::InPoint, "", "", "", 0, 0, 1}) == "cut.set_in_out in=1.0000");
        wetwaresuite::TimelineEdit toggle{TimelineEditKind::Toggle, "", "v1"};
        toggle.name = "locked";
        CHECK(tl->timelineCommand(toggle) == "cut.set_track track=v1 locked=true");
        CHECK(run(s, tl->timelineCommand({TimelineEditKind::TrimStart, "c1", "v1", "", 2}) + " doc=cut1").ok);
        CHECK(near(tl->findClip("c1")->start, 2) && near(tl->findClip("c1")->end(), 5));
        CHECK(run(s, "undo").ok && near(tl->findClip("c1")->start, 1));
        CHECK(run(s, tl->timelineCommand({TimelineEditKind::Cursor, "", "", "", 0, 0, 1.5}) + " doc=cut1").ok && near(tl->cursor, 1.5));
        TimelineLayout again;
        tl->timeline(again);
        CHECK(near(again.cursor, 1.5));
    }
    CHECK(run(s, "cut.set_clip doc=cut1 clip=c1 text=Changed box=true").ok);
    CHECK(tl->findClip("c1")->text == "Changed" && tl->findClip("c1")->box);
    CHECK(!run(s, "cut.set_clip doc=cut1 clip=c1 fade_in=3 fade_out=3").ok);  // longer than the clip
    CHECK(!run(s, "cut.set_clip doc=cut1 clip=c1 speed=2").ok);  // titles have no speed
    CHECK(!run(s, "cut.set_clip doc=cut1 clip=c9 opacity=1").ok);
    CHECK(!run(s, "cut.set_clip doc=cut1 clip=c1 blend=sparkly").ok);
    CHECK(!run(s, "cut.set_clip doc=cut1 clip=c1 anim_in=explode").ok);
    CHECK(run(s, "cut.split_clip doc=cut1 clip=c1 at=3").ok);
    CHECK(near(tl->findClip("c1")->end(), 3) && near(tl->findClip("c3")->start, 3) && near(tl->findClip("c3")->end(), 5));
    CHECK(near(tl->findClip("c3")->fadeIn, 0));
    CHECK(!run(s, "cut.split_clip doc=cut1 clip=c1 at=4").ok);
    CHECK(run(s, "cut.move_clip doc=cut1 clip=c3 start=10").ok);
    CHECK(near(tl->duration(), 12));
    CHECK(run(s, "undo").ok);
    CHECK(near(tl->duration(), 5));
    CHECK(run(s, "cut.trim_clip doc=cut1 clip=c3 duration=0.5").ok);
    CHECK(near(tl->findClip("c3")->end(), 3.5));
    CHECK(run(s, "cut.delete_clip doc=cut1 clip=c2").ok);
    CHECK(!tl->findClip("c2"));
    CHECK(run(s, "undo").ok && tl->findClip("c2"));
    CHECK(run(s, "cut.settings doc=cut1 width=720 height=1280 background=#112233").ok);
    CHECK(tl->settings.width == 720 && tl->settings.background == "#112233");
    CHECK(!run(s, "cut.settings doc=cut1 fps=0").ok && near(tl->settings.fps, 25));
    CHECK(!run(s, "cut.settings doc=cut1 background=nocolour").ok);
    CHECK(!run(s, "cut.transition doc=cut1 clip=c1").ok);  // titles don't take transitions
    CHECK(!run(s, "cut.add_clip doc=cut1 path=/no/such/file.mp4").ok);
    Result info = run(s, "cut.info doc=cut1");
    CHECK(info.ok && contains(info.message, "c3 title") && num(info, "clips") == 3);

    // the playhead
    CHECK(run(s, "cut.set_cursor doc=cut1 t=0").ok);
    CHECK(run(s, "cut.set_cursor doc=cut1 to=next_edit").ok && near(tl->cursor, 1));
    CHECK(run(s, "cut.set_cursor doc=cut1 to=next_edit").ok && near(tl->cursor, 2));
    CHECK(run(s, "cut.set_cursor doc=cut1 frames=5").ok && near(tl->cursor, 2.2));
    CHECK(run(s, "cut.set_cursor doc=cut1 to=end").ok && near(tl->cursor, tl->duration()));
    CHECK(!run(s, "cut.set_cursor doc=cut1 to=next_marker").ok);

    const fs::path dir = tempDir("commands");
    const std::string path = (dir / "edit.wwcut").string();
    CHECK(run(s, "cut.save doc=cut1 path=" + q(path)).ok);
    CHECK(contains(toJson(*tl), "\"format\": \"wwcut\""));
    CHECK(!run(s, "cut.save doc=cut1 path=" + q(dir / "x.mp4")).ok);
    r = run(s, "cut.open path=" + q(path));
    CHECK(r.ok && toText(r.value) == "cut2");
    auto tl2 = s.workspace().getAs<Timeline>("cut2");
    CHECK(tl2 && toJson(*tl2) == toJson(*tl));
    CHECK(run(s, "cut.close doc=cut2").ok && !s.workspace().get("cut2"));
    CHECK(run(s, "undo").ok && s.workspace().get("cut2"));
    // an .achcut file from before the rename opens
    const fs::path legacy = dir / "old.achcut";
    fs::copy_file(path, legacy, fs::copy_options::overwrite_existing);
    CHECK(run(s, "cut.open path=" + q(legacy)).ok);
    // presets list
    r = run(s, "cut.presets");
    CHECK(r.ok && contains(r.message, "youtube_1080p") && contains(text(r, "presets"), "vertical"));
    std::error_code ec;
    fs::remove_all(dir, ec);  // windows: a cached decoder may still hold a source open
}

// Real exports, only when ffmpeg is installed.
void testWithFfmpeg() {
    if (!findProgram("ffmpeg") || !findProgram("ffprobe")) {
        std::printf("ffmpeg not found: skipping export tests\n");
        return;
    }
    const fs::path dir = tempDir("media");
    const std::string ff = findProgram("ffmpeg")->string();
    const std::string a = (dir / "a.mp4").string(), b = (dir / "b.mp4").string(), srt = (dir / "subs.srt").string();
    const std::string png = solidPng(dir / "logo.png", {255, 0, 0, 255}, 64, 64);
    CHECK(runProgram({ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=320x240:r=25:d=3", "-f", "lavfi",
                      "-i", "sine=f=440:d=3", "-c:v", "libx264", "-g", "25", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", a})
              .exitCode == 0);
    CHECK(runProgram({ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "smptebars=s=320x240:r=25:d=3", "-f", "lavfi",
                      "-i", "sine=f=660:d=3", "-c:v", "libx264", "-g", "25", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", b})
              .exitCode == 0);
    {
        std::ofstream out(srt);
        out << "1\n00:00:00,200 --> 00:00:01,500\nHello\n";
    }

    Session s;
    registerCommands(s);
    Result r = run(s, "cut.probe path=" + q(a));
    CHECK(r.ok && near(num(r, "duration"), 3, 0.1) && num(r, "width") == 320 && near(num(r, "fps"), 25));
    CHECK(run(s, "cut.new width=320 height=240 fps=25").ok);
    r = run(s, "cut.add_clip doc=cut1 path=" + q(a) + " out=2");
    CHECK(r.ok && text(r, "sound") == "c2");  // its sound is linked on a1
    CHECK(run(s, "cut.add_clip doc=cut1 path=" + q(b) + " out=2").ok);
    auto tl = s.workspace().getAs<Timeline>("cut1");
    CHECK(tl->track("a1") && tl->track("a1")->clips.size() == 2 && !tl->findClip("c1")->ownSound && tl->findClip("c1")->link == tl->findClip("c2")->link);
    CHECK(!run(s, "cut.add_clip doc=cut1 path=" + q(a) + " out=9").ok);  // past the end of the source
    r = run(s, "cut.transition doc=cut1 clip=c3 type=crossfade duration=0.5 preview=true");
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    CHECK(near(tl->findClip("c3")->start, 1.5) && near(tl->duration(), 3.5));
    CHECK(near(tl->findClip("c4")->start, 1.5) && tl->findClip("c4")->transition == "crossfade");  // the sound crossfades too
    CHECK(run(s, "cut.add_image doc=cut1 path=" + q(png) + " track=v2 start=0 duration=1 x=10 y=10 width=32").ok);
    CHECK(run(s, "cut.add_title doc=cut1 text=\"Title, with: punctuation\" start=0 duration=1.5 font_size=24").ok);
    r = run(s, "cut.preview doc=cut1 t=1.75 width=160");
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].bytes.substr(1, 3) == "PNG");
    // a decoded frame is the test pattern, not black
    raster::Image f = frame(*tl, 0.5, 160);
    int lit = 0;
    for (int x = 0; x < f.width(); x += 4) lit += px(f, x, f.height() / 2).r + px(f, x, f.height() / 2).g > 60;
    CHECK(lit > 10);

    r = run(s, "cut.play doc=cut1 start=0 end=0.2 wait=true");
    CHECK(r.ok && text(r, "source") == "cut1" && near(num(r, "length"), 3.5, 0.15));
    const std::string mp4 = (dir / "out.mp4").string();
    r = run(s, "cut.render doc=cut1 output=" + q(mp4) + " quality=draft");
    CHECK(r.ok && text(r, "mode") == "encode" && near(num(r, "duration"), 3.5, 0.1));
    CHECK(!run(s, "cut.render doc=cut1 output=" + q(mp4)).ok);  // exists
    CHECK(run(s, "cut.render doc=cut1 output=" + q(mp4) + " overwrite=true mode=encode quality=draft subtitles=" + q(srt)).ok);
    r = run(s, "cut.render doc=cut1 output=" + q(dir / "vertical.mp4") + " preset=vertical quality=draft start=1 end=2");
    CHECK(r.ok && num(r, "width") == 1080 && num(r, "height") == 1920 && near(num(r, "duration"), 1, 0.1));
    r = run(s, "cut.render doc=cut1 output=" + q(dir / "frame.png") + " at=1");
    CHECK(r.ok && num(r, "width") == 320);
    r = run(s, "cut.render doc=cut1 output=" + q(dir / "sound.mp3"));
    CHECK(r.ok && near(num(r, "duration"), 3.5, 0.2));
    r = run(s, "cut.render doc=cut1 output=" + q(dir / "a.gif") + " end=1");
    CHECK(r.ok && num(r, "width") == 480);

    // plain cuts stream-copy
    CHECK(run(s, "cut.new width=320 height=240 fps=25").ok);
    CHECK(run(s, "cut.add_clip doc=cut2 path=" + q(a) + " in=0 out=1").ok);
    CHECK(run(s, "cut.add_clip doc=cut2 path=" + q(a) + " in=2 out=3").ok);
    r = run(s, "cut.render doc=cut2 output=" + q(dir / "copy.mp4"));
    CHECK(r.ok && text(r, "mode") == "copy" && near(num(r, "duration"), 2, 0.2));
    // speed with the pitch kept, reverse: through the mixer and compositor
    CHECK(run(s, "cut.clip_speed doc=cut2 clip=c1 speed=2").ok);
    CHECK(run(s, "cut.clip_speed doc=cut2 clip=c3 reverse=true").ok);
    r = run(s, "cut.render doc=cut2 output=" + q(dir / "speed.mp4") + " quality=draft");
    CHECK(r.ok && text(r, "mode") == "encode" && near(num(r, "duration"), 1.5, 0.15));

    // one-shot helpers
    r = run(s, "cut.concat_files inputs=\"" + a + "," + b + "\" output=" + q(dir / "joined.mp4"));
    CHECK(r.ok && near(num(r, "duration"), 6, 0.2) && text(r, "mode") == "copy");
    r = run(s, "cut.trim_file input=" + q(a) + " output=" + q(dir / "trim.mp4") + " start=1 end=2");
    CHECK(r.ok && near(num(r, "duration"), 1, 0.1));
    r = run(s, "cut.extract_audio input=" + q(a) + " output=" + q(dir / "a.m4a"));
    CHECK(r.ok && near(num(r, "duration"), 3, 0.1));
    r = run(s, "cut.thumbnails input=" + q(a) + " output=" + q(dir / "sheet.png") + " columns=3 rows=2 width=100 preview=true");
    CHECK(r.ok && num(r, "width") == 3 * 100 + 4 * 4 && r.attachments.size() == 1);
    r = run(s, "cut.gif input=" + q(a) + " output=" + q(dir / "b.gif") + " start=1 duration=1 fps=10 width=160");
    CHECK(r.ok && num(r, "width") == 160);
    r = run(s, "cut.speed input=" + q(a) + " output=" + q(dir / "fast.mp4") + " factor=2 quality=draft");
    CHECK(r.ok && near(num(r, "duration"), 1.5, 0.1));
    r = run(s, "cut.mute input=" + q(a) + " output=" + q(dir / "mute.mp4"));
    CHECK(r.ok);
    r = run(s, "cut.probe path=" + q(dir / "mute.mp4"));
    CHECK(r.ok && text(r, "has_audio") == "false");
    r = run(s, "cut.subtitles input=" + q(a) + " subtitles=" + q(srt) + " output=" + q(dir / "subs.mp4") + " quality=draft");
    CHECK(r.ok && near(num(r, "duration"), 3, 0.1));
    std::error_code ec;
    fs::remove_all(dir, ec);  // windows: a cached decoder may still hold a source open
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server
            .handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}")
            .value_or("");
    };
    std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"cut_new", "cut_add_clip", "cut_add_title", "cut_transition", "cut_render", "cut_preview", "cut_probe",
                             "cut_thumbnails", "cut_subtitles", "cut_split_clip", "cut_set_clip", "cut_filter", "cut_razor", "cut_ripple_trim",
                             "cut_roll", "cut_slip", "cut_slide", "cut_insert", "cut_overwrite", "cut_lift", "cut_extract", "cut_clip_speed",
                             "cut_freeze_frame", "cut_add_marker", "cut_set_in_out", "cut_nest", "cut_unnest", "cut_keyframe", "cut_transform",
                             "cut_chroma_key", "cut_effect", "cut_add_lower_third", "cut_import_captions", "cut_export_captions",
                             "cut_normalize", "cut_loudness", "cut_export_frame", "cut_presets", "cut_link", "cut_unlink", "cut_set_cursor",
                             "cut_delete_gap", "cut_add_color", "cut_add_caption"}) {
        if (list.find(std::string("\"") + tool + "\"") == std::string::npos) std::printf("  missing tool %s\n", tool);
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string r = call("cut_new", R"({"width":640,"height":360})");
    CHECK(contains(r, "\"doc\":\"cut1\""));
    r = call("cut_add_title", R"({"doc":"cut1","text":"Hi","duration":2,"position":"top"})");
    CHECK(contains(r, "\"isError\":false") && contains(r, "\"clip\":\"c1\""));
    r = call("cut_preview", R"({"doc":"cut1","t":1})");  // the compositor needs no ffmpeg
    CHECK(contains(r, "\"type\":\"image\""));
    r = call("undo", "{}");
    CHECK(contains(r, "undone"));
    CHECK(session.workspace().getAs<Timeline>("cut1")->duration() == 0);
    r = call("cut_preview", R"({"doc":"cut1","t":0})");
    CHECK(contains(r, "\"isError\":true"));
}

}  // namespace cuttest

int main() {
    using namespace cuttest;
    setVideoFrameSource({});
    testModel();
    testPlan();
    testCommands();
    testRender();
    testMix();
    testEdits();
    testSequence();
    testWithFfmpeg();
    testMcp();
    return check::summary("cut");
}
