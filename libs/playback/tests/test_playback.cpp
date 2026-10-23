// Runs on the silent device (WETWARESUITE_AUDIO=null), which still moves the
// playhead in real time, so it passes on machines with no sound card.
#include <chrono>
#include <cstdlib>
#include <thread>

#include "wetwaresuite/playback/commands.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::playback;

namespace {

std::shared_ptr<const audio::Buffer> tone(double seconds, int channels = 2) {
    auto b = std::make_shared<audio::Buffer>(channels, static_cast<std::size_t>(seconds * 48000), 48000);
    for (auto& c : b->channels)
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = 0.1f * static_cast<float>((i % 100) / 50.0 - 1.0);
    return b;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// the silent device moves in chunks, so poll instead of guessing a sleep
template <typename F>
bool within(int ms, F f) {
    for (int t = 0; t < ms; t += 5) {
        if (f()) return true;
        sleepMs(5);
    }
    return f();
}

}  // namespace

int main() {
#ifdef _WIN32
    _putenv_s("WETWARESUITE_AUDIO", "null");
#else
    setenv("WETWARESUITE_AUDIO", "null", 1);
#endif
    Player& p = Player::instance();
    std::string error;

    // nothing to play
    CHECK(!p.play({}, error) && !error.empty());
    CHECK(p.status().source.empty() && !p.status().playing);

    // plays, moves, finishes
    error.clear();
    CHECK(p.play({tone(0.3), 0, -1, false, "beep"}, error));
    CHECK(error.empty());
    CHECK(within(1000, [&] { return p.status().position > 0.0; }));
    Status s = p.status();
    CHECK(s.playing && s.silent && s.source == "beep" && s.position < 0.3);
    CHECK(p.wait(5) && !p.status().playing);
    CHECK(p.status().position >= 0.29);

    // range, pause holds the playhead, resume carries on
    CHECK(p.play({tone(2.0), 0.5, 1.5, false, "range"}, error));
    s = p.status();
    CHECK(s.start > 0.49 && s.start < 0.51 && s.end > 1.49 && s.end < 1.51 && s.position >= 0.5);
    p.pause();
    const double held = p.status().position;
    sleepMs(100);
    CHECK(p.status().paused && !p.status().playing && p.status().position == held);
    p.resume();
    CHECK(within(1000, [&] { return p.status().position > held; }));
    CHECK(p.status().playing);
    p.seek(1.4);
    CHECK(p.status().position >= 1.39);
    CHECK(p.wait(5));
    CHECK(p.status().position <= 1.51);

    // looping never finishes until stopped; wait refuses to block on it
    CHECK(p.play({tone(0.1, 1), 0, -1, true, "loop"}, error));
    CHECK(within(2000, [&] { return p.status().position > 0.05; }));
    sleepMs(150);  // past the end at least once
    CHECK(p.status().playing && p.status().loop);
    CHECK(!p.wait(0.05));
    p.stop();
    CHECK(!p.status().playing);

    // volume is kept, mono plays on a stereo device
    p.setVolume(0.5);
    CHECK(p.status().volume > 0.49 && p.status().volume < 0.51);
    CHECK(p.devices().front() == "default");
    CHECK(p.selectDevice("default", error));
    CHECK(!p.selectDevice("no such device", error));

    // the commands
    Session session;
    registerTransportCommands(session, "audio");
    for (const char* id : {"audio.pause", "audio.resume", "audio.stop", "audio.seek", "audio.playhead", "audio.volume", "audio.loop",
                           "audio.wait", "audio.devices", "audio.output_device"})
        CHECK(session.registry().find(id) != nullptr);
    Result r = startPlayback(tone(0.2), "cmd", {{"wait", true}, {"volume", -6.0}});
    CHECK(r.ok && r.field("playing") && std::get<bool>(*r.field("playing")) == false);
    CHECK(std::get<std::string>(*r.field("source")) == "cmd");
    CHECK(std::get<double>(*r.field("volume_db")) > -6.1 && std::get<double>(*r.field("volume_db")) < -5.9);
    r = startPlayback(tone(1.0), "cmd2", {{"loop", true}});
    CHECK(r.ok && std::get<bool>(*r.field("loop")));
    r = session.run("audio.stop", {});
    CHECK(r.ok && !std::get<bool>(*r.field("playing")));
    r = session.run("audio.playhead", {});
    CHECK(r.ok && r.message.find("stopped cmd2") != std::string::npos);
    CHECK(!session.run("audio.seek", {}).ok);  // seconds is required
    CHECK(startPlayback(tone(1.0), "x", {{"start", 5.0}}).ok == false);  // past the end

    finishBeforeExit();  // returns at once: nothing playing
    return check::summary("playback");
}
