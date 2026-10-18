#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/cut/cut.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::cut;
namespace fs = std::filesystem;

namespace {

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

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

double num(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return std::nan("");
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return std::nan("");
}

Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
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

void testEscaping() {
    CHECK(escapeOption("a:b'c\\d") == "a\\:b\\'c\\\\d");
    CHECK(escapeGraph("x,y;[z]'\\") == "x\\,y\\;\\[z\\]\\'\\\\");
    // ffmpeg's own documented example of the two levels
    CHECK(filter("drawtext", {{"text", "this is a 'string': may contain one, or more, special characters"}}) ==
          "drawtext=text=this is a \\\\\\'string\\\\\\'\\\\: may contain one\\, or more\\, special characters");
    CHECK(filter("setsar", {{"", "1"}}) == "setsar=1");
    CHECK(filter("anull") == "anull");
    CHECK(filter("scale", {{"w", "640"}, {"h", "-2"}}) == "scale=w=640:h=-2");
    CHECK(xfadeName("crossfade") == "fade");
    CHECK(xfadeName("wipeleft") == "wipeleft");
    CHECK(xfadeName("fadeblack") == "fadeblack");
    CHECK(xfadeName("bogus").empty());
    CHECK(xfadeName("none").empty());
}

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
    bool threw = false;
    try {
        tl.ensureTrack("x1");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    const Track* v1 = tl.track("v1");
    CHECK(tl.previousMedia(*v1, *tl.findClip("c2")) == tl.findClip("c1"));
    CHECK(tl.previousMedia(*v1, *tl.findClip("c1")) == nullptr);
    Clip slow = media("x", "/m/x.mp4", 0, 4, 0);
    slow.speed = 0.5;
    CHECK(near(slow.length(), 8));

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
    Clip title;
    title.id = "c3";
    title.kind = "title";
    title.text = "Hello: \"world\"";
    title.out = 2;
    title.start = 1;
    title.box = true;
    tl.track("v3")->clips.push_back(title);
    tl.nextClip = 4;
    tl.track("v2")->hidden = true;
    const std::string json = toJson(tl);
    auto back = fromJson(json);
    CHECK(back->tracks.size() == 4);
    CHECK(toJson(*back) == json);
    CHECK(back->findClip("c2")->transition == "wipeleft" && near(back->findClip("c2")->cropW, 640));
    CHECK(back->findClip("c3")->text == "Hello: \"world\"" && back->findClip("c3")->box);
    CHECK(back->track("v2")->hidden);
    CHECK(back->nextClip == 4);
    threw = false;
    try {
        fromJson("{\"format\":\"something\"}");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
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
}

void testPlan() {
    Timeline tl = basic();
    // plain cuts of identical sources: stream copy through the concat demuxer
    CHECK(streamCopyBlocker(tl, "mp4").empty());
    RenderRequest rq;
    rq.output = "/out/x.mp4";
    rq.listPath = "/tmp/list.txt";
    RenderPlan plan = planRender(tl, rq);
    CHECK(plan.streamCopy);
    CHECK(plan.concatList == "file '/m/a.mp4'\ninpoint 1\noutpoint 5\nfile '/m/b.mp4'\ninpoint 0\noutpoint 4\n");
    CHECK(after(plan.args, "-f") == "concat" && after(plan.args, "-c") == "copy" && plan.args.back() == "/out/x.mp4");
    CHECK(contains(streamCopyBlocker(tl, "webm"), "differs"));
    rq.output = "/out/x.mov";
    CHECK(planRender(tl, rq).streamCopy);  // mp4 and mov hold the same streams

    // anything else re-encodes with one filter graph
    tl.track("v1")->clips[1].volume = 0.5;
    CHECK(contains(streamCopyBlocker(tl, "mp4"), "c2"));
    rq.output = "/out/x.mp4";
    rq.mode = "copy";
    bool threw = false;
    try {
        planRender(tl, rq);
    } catch (const std::exception& e) {
        threw = contains(e.what(), "stream-copy");
    }
    CHECK(threw);
    rq.mode = "auto";
    plan = planRender(tl, rq);
    CHECK(!plan.streamCopy);
    CHECK(count(plan.args, "-filter_complex") == 1);
    CHECK(after(plan.args, "-ss") == "1" && after(plan.args, "-t") == "4");  // c1 reads source 1..5
    CHECK(after(plan.args, "-c:v") == "libx264" && after(plan.args, "-c:a") == "aac");
    CHECK(after(plan.args, "-t", 2) == "8");  // the output length
    CHECK(contains(plan.graph, "color=c=black:s=1280x720:r=30:d=8"));
    CHECK(contains(plan.graph, "overlay=x=0:y=0:eof_action=pass:enable=gte(t\\,4)*lt(t\\,8)"));
    CHECK(contains(plan.graph, "setpts=PTS+4/TB"));
    CHECK(contains(plan.graph, "volume=0.5"));
    CHECK(contains(plan.graph, "adelay=delays=4000:all=1"));
    CHECK(contains(plan.graph, "amix=inputs=2"));
    CHECK(!contains(plan.graph, "xfade"));

    // a crossfade: c2 overlaps c1 by 1s and the two become one xfade chain
    tl.track("v1")->clips[1].start = 3;
    tl.track("v1")->clips[1].transition = "crossfade";
    tl.track("v1")->clips[1].transitionDuration = 1;
    plan = planRender(tl, rq);
    CHECK(contains(plan.graph, "xfade=transition=fade:duration=1:offset=3"));
    CHECK(contains(plan.graph, "afade=t=out:st=3:d=1"));  // the sound crossfades too
    CHECK(contains(plan.graph, "afade=t=in:st=0:d=1"));
    CHECK(near(plan.duration, 7));
    CHECK(count(plan.args, "-i") == 2);  // each clip read once for picture and sound

    // a frame inside the crossfade decodes only from the start of the overlap
    rq.output = "/out/f.png";
    rq.at = 3.5;
    plan = planRender(tl, rq);
    CHECK(after(plan.args, "-frames:v") == "1");
    CHECK(after(plan.args, "-ss") == "4");  // c1 source 1 + 3s skipped
    CHECK(contains(plan.graph, "offset=0"));
    CHECK(contains(plan.graph, "setpts=PTS+3.5/TB"));  // the background starts at the frame
    CHECK(!contains(plan.graph, "amix") && !contains(plan.graph, "adelay"));
    // a frame outside it reads just the clip on screen
    rq.at = 6;
    plan = planRender(tl, rq);
    CHECK(count(plan.args, "-i") == 1 && after(plan.args, "-i") == "/m/b.mp4");
    CHECK(!contains(plan.graph, "xfade"));

    // speed: picture timestamps and a chain of atempo filters (each 0.5..2)
    Timeline fast = basic();
    fast.track("v1")->clips.resize(1);
    fast.track("v1")->clips[0].speed = 3;
    rq.output = "/out/fast.mp4";
    rq.at = 0;
    plan = planRender(fast, rq);
    CHECK(contains(plan.graph, "setpts=(PTS-STARTPTS)/3"));
    CHECK(contains(plan.graph, "atempo=2,atempo=1.5"));
    CHECK(after(plan.args, "-t", 1) == "1.333333");

    // picture-in-picture, opacity, colour, LUT, fades and a title on top
    Timeline pip = basic();
    Clip small = media("c3", "/m/cam.mp4", 0, 3, 1, false);
    small.x = 900;
    small.y = 40;
    small.width = 340;
    small.opacity = 0.5;
    small.fadeIn = 0.5;
    small.grayscale = true;
    small.contrast = 1.2;
    small.lut = "/luts/warm.cube";
    small.cropX = 0;
    small.cropY = 0;
    small.cropW = 640;
    small.cropH = 360;
    pip.ensureTrack("v2").clips.push_back(small);
    Clip title;
    title.id = "c4";
    title.kind = "title";
    title.text = "It's here: a, b";
    title.start = 0.5;
    title.out = 2;
    title.position = "lower-third";
    title.fadeIn = 0.25;
    title.box = true;
    pip.ensureTrack("v3").clips.push_back(title);
    rq.output = "/out/pip.webm";
    rq.fontFile = "/fonts/Sans.ttf";
    plan = planRender(pip, rq);
    CHECK(after(plan.args, "-c:v") == "libvpx-vp9" && after(plan.args, "-c:a") == "libopus");
    CHECK(contains(plan.graph, "crop=w=640:h=360:x=0:y=0"));
    CHECK(contains(plan.graph, "scale=w=340:h=192:force_original_aspect_ratio=decrease"));
    CHECK(contains(plan.graph, "overlay=x=900:y=40"));
    CHECK(contains(plan.graph, "lut=a=val*0.5"));
    CHECK(contains(plan.graph, "fade=t=in:st=0:d=0.5:alpha=1"));
    CHECK(contains(plan.graph, "hue=s=0"));
    CHECK(contains(plan.graph, "eq=brightness=0:contrast=1.2"));
    CHECK(contains(plan.graph, "lut3d=file=/luts/warm.cube"));
    CHECK(contains(plan.graph, "drawtext=fontfile=/fonts/Sans.ttf:text=It\\\\\\'s here\\\\: a\\, b:expansion=none"));
    CHECK(contains(plan.graph, "x=w*0.06:y=h*0.72:box=1"));
    CHECK(contains(plan.graph, "enable=gte(t\\,0.5)*lt(t\\,2.5)"));
    CHECK(contains(plan.graph, "alpha=clip(if(lt(t\\,0.75)\\,(t-0.5)/0.25\\,1)\\,0\\,1)"));
    CHECK(count(plan.args, "-i") == 3);  // the silent camera clip adds no sound
    // the title draws after (above) every picture below it
    CHECK(plan.graph.find("drawtext") > plan.graph.find("overlay=x=900"));

    // sound only, gif and stills
    rq.output = "/out/a.mp3";
    plan = planRender(pip, rq);
    CHECK(after(plan.args, "-c:a") == "libmp3lame" && !contains(plan.graph, "color="));
    rq.output = "/out/a.gif";
    plan = planRender(pip, rq);
    CHECK(contains(plan.graph, "palettegen") && contains(plan.graph, "paletteuse") && after(plan.args, "-loop") == "0");
    rq.output = "/out/a.png";
    rq.at = 1.5;
    rq.stillWidth = 640;
    plan = planRender(pip, rq);
    CHECK(contains(plan.graph, "scale=w=640:h=360"));
    rq.output = "/out/a.avi";
    threw = false;
    try {
        planRender(pip, rq);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    Timeline empty;
    rq.output = "/out/e.mp4";
    threw = false;
    try {
        planRender(empty, rq);
    } catch (const std::exception& e) {
        threw = contains(e.what(), "empty");
    }
    CHECK(threw);
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
    {
        // the window's timeline: layout and the command each drag becomes
        TimelineLayout layout;
        CHECK(tl->timeline(layout) && layout.docParam == "doc" && layout.tracks.size() == 1 && layout.tracks[0].clips.size() == 2);
        CHECK(layout.tracks[0].clips[0].label == "\"Hello there\"" && layout.tracks[0].clips[0].headroom < 0);
        CHECK(tl->timelineCommand({TimelineEditKind::Move, "c2", "v1", "v1", 2.5}) == "cut.move_clip clip=c2 start=2.5000");
        CHECK(tl->timelineCommand({TimelineEditKind::Move, "c2", "v1", "v2", 2.5}) == "cut.move_clip clip=c2 start=2.5000 track=v2");
        CHECK(tl->timelineCommand({TimelineEditKind::TrimEnd, "c1", "v1", "", 0, 3}) == "cut.trim_clip clip=c1 duration=3.0000");
        CHECK(tl->timelineCommand({TimelineEditKind::TrimStart, "c1", "v1", "", 2}) == "cut.set_clip clip=c1 start=2.0000 duration=3.0000");
        CHECK(tl->timelineCommand({TimelineEditKind::Split, "c1", "v1", "", 0, 0, 3}) == "cut.split_clip clip=c1 at=3.0000");
        CHECK(tl->timelineCommand({TimelineEditKind::Delete, "nope", "v1"}).empty());
        CHECK(run(s, tl->timelineCommand({TimelineEditKind::TrimStart, "c1", "v1", "", 2}) + " doc=cut1").ok);
        CHECK(near(tl->findClip("c1")->start, 2) && near(tl->findClip("c1")->end(), 5));
        CHECK(run(s, "undo").ok && near(tl->findClip("c1")->start, 1));
    }
    CHECK(run(s, "cut.set_clip doc=cut1 clip=c1 text=Changed box=true").ok);
    CHECK(tl->findClip("c1")->text == "Changed" && tl->findClip("c1")->box);
    CHECK(!run(s, "cut.set_clip doc=cut1 clip=c1 fade_in=3 fade_out=3").ok);  // longer than the clip
    CHECK(!run(s, "cut.set_clip doc=cut1 clip=c1 speed=2").ok);  // titles have no speed
    CHECK(!run(s, "cut.set_clip doc=cut1 clip=c9 opacity=1").ok);
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
    CHECK(!run(s, "cut.transition doc=cut1 clip=c1").ok);  // titles don't take transitions
    CHECK(!run(s, "cut.add_clip doc=cut1 path=/no/such/file.mp4").ok);
    Result info = run(s, "cut.info doc=cut1");
    CHECK(info.ok && contains(info.message, "c3 title") && num(info, "clips") == 3);

    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_cut_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "edit.wwcut").string();
    CHECK(run(s, "cut.save doc=cut1 path=\"" + path + "\"").ok);
    CHECK(contains(toJson(*tl), "\"format\": \"wwcut\""));
    CHECK(!run(s, "cut.save doc=cut1 path=\"" + (dir / "x.mp4").string() + "\"").ok);
    r = run(s, "cut.open path=\"" + path + "\"");
    CHECK(r.ok && toText(r.value) == "cut2");
    auto tl2 = s.workspace().getAs<Timeline>("cut2");
    CHECK(tl2 && toJson(*tl2) == toJson(*tl));
    CHECK(run(s, "cut.close doc=cut2").ok && !s.workspace().get("cut2"));
    CHECK(run(s, "undo").ok && s.workspace().get("cut2"));
    // an .achcut file from before the rename opens
    const fs::path legacy = dir / "old.achcut";
    fs::copy_file(path, legacy, fs::copy_options::overwrite_existing);
    CHECK(run(s, "cut.open path=\"" + legacy.string() + "\"").ok);
    fs::remove_all(dir);
}

// Real renders, only when ffmpeg is installed.
void testWithFfmpeg() {
    if (!findProgram("ffmpeg") || !findProgram("ffprobe")) {
        std::printf("ffmpeg not found: skipping render tests\n");
        return;
    }
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_cut_media";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string ff = findProgram("ffmpeg")->string();
    const std::string a = (dir / "a.mp4").string(), b = (dir / "b.mp4").string(), png = (dir / "logo.png").string(),
                      srt = (dir / "subs.srt").string();
    CHECK(runProgram({ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=320x240:r=25:d=3", "-f", "lavfi",
                      "-i", "sine=f=440:d=3", "-c:v", "libx264", "-g", "25", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", a})
              .exitCode == 0);
    CHECK(runProgram({ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "smptebars=s=320x240:r=25:d=3", "-f", "lavfi",
                      "-i", "sine=f=660:d=3", "-c:v", "libx264", "-g", "25", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", b})
              .exitCode == 0);
    CHECK(runProgram({ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=c=red:s=64x64", "-frames:v", "1", png}).exitCode == 0);
    {
        std::ofstream out(srt);
        out << "1\n00:00:00,200 --> 00:00:01,500\nHello\n";
    }

    Session s;
    registerCommands(s);
    Result r = run(s, "cut.probe path=\"" + a + "\"");
    CHECK(r.ok && near(num(r, "duration"), 3, 0.1) && num(r, "width") == 320 && near(num(r, "fps"), 25));
    CHECK(run(s, "cut.new width=320 height=240 fps=25").ok);
    CHECK(run(s, "cut.add_clip doc=cut1 path=\"" + a + "\" out=2").ok);
    CHECK(run(s, "cut.add_clip doc=cut1 path=\"" + b + "\" out=2").ok);
    CHECK(!run(s, "cut.add_clip doc=cut1 path=\"" + a + "\" out=9").ok);  // past the end of the source
    r = run(s, "cut.transition doc=cut1 clip=c2 type=crossfade duration=0.5 preview=true");
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    auto tl = s.workspace().getAs<Timeline>("cut1");
    CHECK(near(tl->findClip("c2")->start, 1.5) && near(tl->duration(), 3.5));
    CHECK(run(s, "cut.add_image doc=cut1 path=\"" + png + "\" track=v2 start=0 duration=1 x=10 y=10 width=32").ok);
    CHECK(run(s, "cut.add_title doc=cut1 text=\"Title, with: punctuation\" start=0 duration=1.5 font_size=24").ok);
    r = run(s, "cut.preview doc=cut1 t=1.75 width=160");
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].bytes.substr(1, 3) == "PNG");

    const std::string mp4 = (dir / "out.mp4").string();
    r = run(s, "cut.render doc=cut1 output=\"" + mp4 + "\" quality=draft");
    CHECK(r.ok && toText(*r.field("mode")) == "encode" && near(num(r, "duration"), 3.5, 0.1));
    CHECK(!run(s, "cut.render doc=cut1 output=\"" + mp4 + "\"").ok);  // exists
    CHECK(run(s, "cut.render doc=cut1 output=\"" + mp4 + "\" overwrite=true mode=encode quality=draft subtitles=\"" + srt + "\"").ok);
    r = run(s, "cut.render doc=cut1 output=\"" + (dir / "frame.png").string() + "\" at=1");
    CHECK(r.ok && num(r, "width") == 320);
    r = run(s, "cut.render doc=cut1 output=\"" + (dir / "sound.mp3").string() + "\"");
    CHECK(r.ok && near(num(r, "duration"), 3.5, 0.2));

    // plain cuts stream-copy
    CHECK(run(s, "cut.new width=320 height=240 fps=25").ok);
    CHECK(run(s, "cut.add_clip doc=cut2 path=\"" + a + "\" in=0 out=1").ok);
    CHECK(run(s, "cut.add_clip doc=cut2 path=\"" + a + "\" in=2 out=3").ok);
    r = run(s, "cut.render doc=cut2 output=\"" + (dir / "copy.mp4").string() + "\"");
    CHECK(r.ok && toText(*r.field("mode")) == "copy" && near(num(r, "duration"), 2, 0.2));

    // one-shot helpers
    r = run(s, "cut.concat_files inputs=\"" + a + "," + b + "\" output=\"" + (dir / "joined.mp4").string() + "\"");
    CHECK(r.ok && near(num(r, "duration"), 6, 0.2) && toText(*r.field("mode")) == "copy");
    r = run(s, "cut.trim_file input=\"" + a + "\" output=\"" + (dir / "trim.mp4").string() + "\" start=1 end=2");
    CHECK(r.ok && near(num(r, "duration"), 1, 0.1));
    r = run(s, "cut.extract_audio input=\"" + a + "\" output=\"" + (dir / "a.m4a").string() + "\"");
    CHECK(r.ok && near(num(r, "duration"), 3, 0.1));
    r = run(s, "cut.thumbnails input=\"" + a + "\" output=\"" + (dir / "sheet.png").string() + "\" columns=3 rows=2 width=100 preview=true");
    CHECK(r.ok && num(r, "width") == 3 * 100 + 4 * 4 && r.attachments.size() == 1);
    r = run(s, "cut.gif input=\"" + a + "\" output=\"" + (dir / "a.gif").string() + "\" start=1 duration=1 fps=10 width=160");
    CHECK(r.ok && num(r, "width") == 160);
    r = run(s, "cut.speed input=\"" + a + "\" output=\"" + (dir / "fast.mp4").string() + "\" factor=2 quality=draft");
    CHECK(r.ok && near(num(r, "duration"), 1.5, 0.1));
    r = run(s, "cut.mute input=\"" + a + "\" output=\"" + (dir / "mute.mp4").string() + "\"");
    CHECK(r.ok);
    r = run(s, "cut.probe path=\"" + (dir / "mute.mp4").string() + "\"");
    CHECK(r.ok && toText(*r.field("has_audio")) == "false");
    r = run(s, "cut.subtitles input=\"" + a + "\" subtitles=\"" + srt + "\" output=\"" + (dir / "subs.mp4").string() + "\" quality=draft");
    CHECK(r.ok && near(num(r, "duration"), 3, 0.1));
    fs::remove_all(dir);
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
                             "cut_thumbnails", "cut_subtitles", "cut_split_clip", "cut_set_clip", "cut_filter"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string r = call("cut_new", R"({"width":640,"height":360})");
    CHECK(contains(r, "\"doc\":\"cut1\""));
    r = call("cut_add_title", R"({"doc":"cut1","text":"Hi","duration":2,"position":"top"})");
    CHECK(contains(r, "\"isError\":false") && contains(r, "\"clip\":\"c1\""));
    if (findProgram("ffmpeg")) {
        r = call("cut_preview", R"({"doc":"cut1","t":1})");
        CHECK(contains(r, "\"type\":\"image\""));
    }
    r = call("undo", "{}");
    CHECK(contains(r, "undone"));
    CHECK(session.workspace().getAs<Timeline>("cut1")->duration() == 0);
    r = call("cut_preview", R"({"doc":"cut1","t":0})");
    CHECK(contains(r, "\"isError\":true"));
}

}  // namespace

int main() {
    testEscaping();
    testModel();
    testPlan();
    testCommands();
    testWithFfmpeg();
    testMcp();
    return check::summary("cut");
}
