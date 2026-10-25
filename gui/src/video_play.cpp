// A timeline's moving picture while it plays: the document hands out a
// FramePlayer (a snapshot of the edit) and a worker thread draws the frame the
// sound has reached, so the window never waits on decoding. Paused, the last
// frame stays; stopped, the canvas goes back to the playhead's preview.
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

void VideoPlayback::request(double seconds) {
    {
        std::lock_guard<std::mutex> g(lock_);
        if (seconds == want_) return;
        want_ = seconds;
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

void VideoPlayback::work() {
    std::vector<std::uint8_t> buf;
    std::unique_lock<std::mutex> lk(lock_);
    while (true) {
        wake_.wait(lk, [this] { return stop_ || (want_ >= 0 && want_ != drawn_); });
        if (stop_) return;
        const double t = want_;
        lk.unlock();
        int w = 0, h = 0;
        const bool ok = player_ && player_->frame(t, maxWidth_, w, h, buf);
        lk.lock();
        drawn_ = t;
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
    if (!doc || doc->kind() != "timeline" || !timelinePlaying()) {
        if (video_) {
            video_.reset();
            canvas_.dirty = true;  // back to the playhead's frame
        }
        return false;
    }
    if (!video_ || video_->doc() != currentDoc_) {
        auto player = doc->player();
        if (!player) return false;
        video_ = std::make_unique<VideoPlayback>(currentDoc_, std::move(player), 960);  // the size previews are drawn at
    }
    video_->request(transport_.position);
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    if (video_->take(rgba, w, h) && w > 0 && h > 0) canvas_.texture.upload(renderer_, rgba.data(), w, h);
    canvas_.dirty = false;
    canvas_.shownDoc = currentDoc_;
    return canvas_.texture.tex != nullptr;
}

}  // namespace wetwaresuite::gui
