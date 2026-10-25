// A timeline's picture: the document hands out a FramePlayer (a snapshot of the
// edit) and a worker thread draws the frame wanted (the one the sound has
// reached while playing, the one under the mouse while the playhead is dragged,
// else the playhead's), so the window never waits on decoding. Dragging asks for
// quick nearby frames, then the exact one once the mouse rests.
#include <algorithm>

#include "shell.hpp"

namespace wetwaresuite::gui {

VideoPlayback::VideoPlayback(std::string doc, std::unique_ptr<FramePlayer> player, int maxWidth)
    : doc_(std::move(doc)), player_(std::move(player)), maxWidth_(maxWidth), worker_([this] { work(); }) {}

VideoPlayback::~VideoPlayback() {
    {
        std::lock_guard<std::mutex> g(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

void VideoPlayback::request(double seconds, bool fast) {
    {
        std::lock_guard<std::mutex> g(lock_);
        if (seconds == want_ && fast == wantFast_) return;
        want_ = seconds;
        wantFast_ = fast;
    }
    wake_.notify_all();
}

bool VideoPlayback::take(std::vector<std::uint8_t>& rgba, int& width, int& height) {
    std::lock_guard<std::mutex> g(lock_);
    if (!fresh_) return false;
    fresh_ = false;
    rgba.swap(rgba_);
    width = width_;
    height = height_;
    return true;
}

bool VideoPlayback::pending() {
    std::lock_guard<std::mutex> g(lock_);
    return fresh_ || (want_ >= 0 && (want_ != drawn_ || (drawnFast_ && !wantFast_)));
}

void VideoPlayback::work() {
    std::vector<std::uint8_t> buf;
    std::unique_lock<std::mutex> lk(lock_);
    while (true) {
        // a quick frame is redrawn exactly once the exact one is wanted
        wake_.wait(lk, [this] { return stop_ || (want_ >= 0 && (want_ != drawn_ || (drawnFast_ && !wantFast_))); });
        if (stop_) return;
        const double t = want_;
        const bool fast = wantFast_;
        lk.unlock();
        int w = 0, h = 0;
        const bool ok = player_ && (fast ? player_->scrub(t, maxWidth_, w, h, buf) : player_->frame(t, maxWidth_, w, h, buf));
        lk.lock();
        drawn_ = t;
        drawnFast_ = fast;
        if (ok) {
            rgba_.swap(buf);
            width_ = w;
            height_ = h;
            fresh_ = true;
        }
    }
}

bool Shell::timelinePlaying() const {
    const Transport& t = transport_;
    return !currentDoc_.empty() && t.doc == currentDoc_ && t.source == currentDoc_ && (t.playing || t.paused);
}

bool Shell::videoPlaybackFrame() {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || doc->kind() != "timeline") {
        video_.reset();
        scrubTime_ = -1;
        return false;
    }
    const bool playing = timelinePlaying();
    const bool live = playing || scrubTime_ >= 0;
    if (liveVideo_ && !live) canvas_.dirty = true;  // stopped: back to the playhead's own frame
    liveVideo_ = live;
    if (!video_ && live) {
        auto player = doc->player();
        if (!player) return false;
        video_ = std::make_unique<VideoPlayback>(currentDoc_, std::move(player), 960);  // the size previews are drawn at
    }
    if (!video_) return false;
    if (video_->doc() != currentDoc_) {
        video_.reset();
        canvas_.dirty = true;
        return false;
    }
    // while the mouse keeps moving, quick frames; resting 150 ms, the exact one
    if (playing) video_->request(transport_.position);
    else if (scrubTime_ >= 0) video_->request(scrubTime_, SDL_GetTicks() - scrubMovedAt_ < 150);
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    if (video_->take(rgba, w, h) && w > 0 && h > 0) canvas_.texture.upload(renderer_, rgba.data(), w, h);
    if (!playing && scrubTime_ < 0) return false;  // a still: refreshCanvas decides when to draw a new one
    canvas_.dirty = false;
    canvas_.shownDoc = currentDoc_;
    return canvas_.texture.tex != nullptr;
}

bool Shell::timelineCanvas(const std::shared_ptr<Document>& doc) {
    if (doc->kind() != "timeline") return false;
    TimelineLayout layout;
    auto player = doc->player();
    if (!player || !doc->timeline(layout)) return false;
    // a fresh snapshot of the edit; the old picture stays up until the new one is drawn
    video_ = std::make_unique<VideoPlayback>(currentDoc_, std::move(player), 960);
    video_->request(std::max(0.0, layout.cursor));
    return true;
}

}  // namespace wetwaresuite::gui
