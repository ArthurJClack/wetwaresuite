#include "wetwaresuite/playback/player.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "ma_config.hpp"
#include "miniaudio.h"

namespace wetwaresuite::playback {

namespace {

constexpr int kMaxDeviceChannels = 8;

bool forcedSilent() {
    const char* v = std::getenv("WETWARESUITE_AUDIO");
    return v && (std::string(v) == "null" || std::string(v) == "none" || std::string(v) == "silent");
}

// Linux with no sound card and no sound server (containers, CI, headless servers):
// go straight to silent instead of letting ALSA print a screenful of errors.
bool noSoundHere() {
#if defined(__linux__)
    std::error_code ec;
    if (std::filesystem::exists("/dev/snd", ec)) return false;
    if (std::getenv("PULSE_SERVER")) return false;
    if (const char* run = std::getenv("XDG_RUNTIME_DIR")) {
        const std::filesystem::path dir = run;
        if (std::filesystem::exists(dir / "pulse" / "native", ec) || std::filesystem::exists(dir / "pipewire-0", ec)) return false;
    }
    return true;
#else
    return false;
#endif
}

}  // namespace

struct Player::Impl {
    // what's playing; guarded by mutex (the audio thread only try_locks it)
    mutable std::mutex mutex;
    std::shared_ptr<const audio::Buffer> buffer;
    std::size_t begin = 0, end = 0;
    std::atomic<std::size_t> cursor{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> loop{false};
    std::atomic<float> volume{1.0f};
    std::string source;

    // the device
    ma_context context{};
    bool contextReady = false;
    bool silent = false;
    ma_device device{};
    bool deviceReady = false;
    int deviceRate = 0, deviceChannels = 0;
    std::string deviceName;
    std::string wantedDevice = "default";
    ma_device_id wantedId{};
    bool hasWantedId = false;

    ~Impl() {
        if (deviceReady) ma_device_uninit(&device);
        if (contextReady) ma_context_uninit(&context);
    }

    static void callback(ma_device* dev, void* out, const void*, ma_uint32 frameCount) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        auto* dst = static_cast<float*>(out);
        const ma_uint32 outCh = dev->playback.channels;
        std::fill(dst, dst + static_cast<std::size_t>(frameCount) * outCh, 0.0f);
        if (!self->playing.load() || self->paused.load()) return;
        std::unique_lock lock(self->mutex, std::try_to_lock);
        if (!lock.owns_lock() || !self->buffer) return;
        const audio::Buffer& b = *self->buffer;
        const int inCh = b.channelCount();
        const float gain = self->volume.load();
        std::size_t pos = self->cursor.load();
        for (ma_uint32 i = 0; i < frameCount; ++i) {
            if (pos >= self->end) {
                if (self->loop.load() && self->end > self->begin) {
                    pos = self->begin;
                } else {
                    self->playing.store(false);
                    break;
                }
            }
            float* frame = dst + static_cast<std::size_t>(i) * outCh;
            if (inCh == 1) {
                const float v = b.channels[0][pos] * gain;
                for (ma_uint32 c = 0; c < outCh; ++c) frame[c] = v;
            } else {
                for (ma_uint32 c = 0; c < outCh; ++c) frame[c] = c < static_cast<ma_uint32>(inCh) ? b.channels[c][pos] * gain : 0.0f;
            }
            ++pos;
        }
        self->cursor.store(pos);
    }

    bool ensureContext(std::string& error) {
        if (contextReady) return true;
        if (!forcedSilent() && !noSoundHere() && ma_context_init(nullptr, 0, nullptr, &context) == MA_SUCCESS) {
            contextReady = true;
            silent = false;
            return true;
        }
        return openSilentContext(error);
    }

    bool openSilentContext(std::string& error) {
        if (contextReady) ma_context_uninit(&context);
        contextReady = false;
        ma_backend nullBackend = ma_backend_null;
        if (ma_context_init(&nullBackend, 1, nullptr, &context) != MA_SUCCESS) {
            error = "no audio output available";
            return false;
        }
        contextReady = true;
        silent = true;
        return true;
    }

    bool openDevice(int rate, int channels, std::string& error) {
        if (deviceReady && rate == deviceRate && channels == deviceChannels) return true;
        if (deviceReady) {
            ma_device_uninit(&device);
            deviceReady = false;
        }
        if (!ensureContext(error)) return false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
            cfg.playback.format = ma_format_f32;
            cfg.playback.channels = static_cast<ma_uint32>(channels);
            cfg.sampleRate = static_cast<ma_uint32>(rate);
            cfg.dataCallback = &Impl::callback;
            cfg.pUserData = this;
            cfg.performanceProfile = ma_performance_profile_conservative;  // bigger buffers, easy on weak CPUs
            if (hasWantedId && !silent) cfg.playback.pDeviceID = &wantedId;
            if (ma_device_init(&context, &cfg, &device) == MA_SUCCESS && ma_device_start(&device) == MA_SUCCESS) {
                deviceReady = true;
                deviceRate = rate;
                deviceChannels = channels;
                deviceName = silent ? "silent (no sound device)" : device.playback.name;
                return true;
            }
            ma_device_uninit(&device);
            // a real backend that can't open a device: carry on silently
            if (silent || !openSilentContext(error)) break;
        }
        if (error.empty()) error = "could not open an audio output device";
        return false;
    }
};

Player& Player::instance() {
    static Player player;
    return player;
}

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;

bool Player::play(const PlayRequest& rq, std::string& error) {
    if (!rq.buffer || rq.buffer->empty() || rq.buffer->channelCount() == 0) {
        error = "nothing to play (empty audio)";
        return false;
    }
    const audio::Buffer& b = *rq.buffer;
    const audio::Range r = audio::rangeOf(b, std::max(0.0, rq.start), rq.end);
    if (r.size() == 0) {
        error = "the range to play is empty";
        return false;
    }
    stop();
    const int channels = std::min(std::max(b.channelCount(), 2), kMaxDeviceChannels);
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->openDevice(b.rate, channels, error)) return false;
        impl_->buffer = rq.buffer;
        impl_->begin = r.begin;
        impl_->end = r.end;
        impl_->cursor.store(r.begin);
        impl_->loop.store(rq.loop);
        impl_->source = rq.source;
        impl_->paused.store(false);
        impl_->playing.store(true);
    }
    return true;
}

void Player::pause() {
    std::lock_guard lock(impl_->mutex);  // an audio callback in flight finishes first, so the playhead holds still
    if (impl_->playing.load()) impl_->paused.store(true);
}

void Player::resume() { impl_->paused.store(false); }

void Player::stop() {
    std::lock_guard lock(impl_->mutex);
    impl_->playing.store(false);
    impl_->paused.store(false);
}

void Player::seek(double seconds) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->buffer) return;
    const std::size_t f = impl_->buffer->toFrame(std::max(0.0, seconds));
    impl_->cursor.store(std::clamp(f, impl_->begin, impl_->end));
}

void Player::setVolume(double gain) { impl_->volume.store(static_cast<float>(std::clamp(gain, 0.0, 4.0))); }

void Player::setLoop(bool loop) { impl_->loop.store(loop); }

Status Player::status() const {
    std::lock_guard lock(impl_->mutex);
    Status s;
    s.paused = impl_->paused.load() && impl_->playing.load();
    s.playing = impl_->playing.load() && !impl_->paused.load();
    s.loop = impl_->loop.load();
    s.volume = impl_->volume.load();
    s.device = impl_->deviceReady ? impl_->deviceName : std::string("not opened yet");
    s.silent = impl_->silent;
    if (impl_->buffer) {
        const audio::Buffer& b = *impl_->buffer;
        const double rate = b.rate > 0 ? b.rate : 1;
        s.position = static_cast<double>(impl_->cursor.load()) / rate;
        s.start = static_cast<double>(impl_->begin) / rate;
        s.end = static_cast<double>(impl_->end) / rate;
        s.length = b.seconds();
        s.rate = b.rate;
        s.channels = b.channelCount();
        s.source = impl_->source;
    }
    return s;
}

bool Player::wait(double timeoutSeconds) {
    using clock = std::chrono::steady_clock;
    const auto until = clock::now() + std::chrono::duration<double>(timeoutSeconds < 0 ? 1e9 : timeoutSeconds);
    while (impl_->playing.load()) {
        if ((impl_->loop.load() && timeoutSeconds < 0) || clock::now() >= until) return false;  // a loop never ends by itself
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

std::vector<std::string> Player::devices() {
    std::vector<std::string> out = {"default"};
    std::lock_guard lock(impl_->mutex);
    std::string error;
    if (!impl_->ensureContext(error) || impl_->silent) return out;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&impl_->context, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return out;
    for (ma_uint32 i = 0; i < count; ++i) {
        const std::string name = infos[i].name;
        if (name.rfind("Discard all samples", 0) != 0) out.push_back(name);  // ALSA's "null" sink
    }
    return out;
}

bool Player::selectDevice(const std::string& name, std::string& error) {
    stop();
    std::lock_guard lock(impl_->mutex);
    if (name.empty() || name == "default") {
        impl_->hasWantedId = false;
    } else {
        if (!impl_->ensureContext(error)) return false;
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        bool found = false;
        if (!impl_->silent && ma_context_get_devices(&impl_->context, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; ++i) {
                if (name == infos[i].name) {
                    impl_->wantedId = infos[i].id;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            error = "no output device called '" + name + "' (see play.devices)";
            return false;
        }
        impl_->hasWantedId = true;
    }
    impl_->wantedDevice = name.empty() ? "default" : name;
    if (impl_->deviceReady) {
        ma_device_uninit(&impl_->device);
        impl_->deviceReady = false;
    }
    return true;
}

void finishBeforeExit() {
    Player& p = Player::instance();
    if (p.status().loop) return;
    p.wait(-1);
}

}  // namespace wetwaresuite::playback
