//go:build linux && !android && cgo

#include "internal.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/keys.h>
#include <spa/param/video/format-utils.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

namespace qui::sys {

namespace {

constexpr double kStallSeconds = 15;
constexpr double kReconnectCooldown = 20;

void ensure_pipewire() {
  static std::once_flag once;
  std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

double monotonic() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

class VideoStream {
 public:
  VideoStream(int fd, uint32_t node, int fps, std::string name) : fd_(fd), node_(node), fps_(fps > 0 ? fps : 60), name_(std::move(name)) {}
  ~VideoStream() { disconnect(); }

  bool connect() {
    ensure_pipewire();
    loop_ = pw_thread_loop_new(("qui-video-" + name_).c_str(), nullptr);
    if (!loop_ || pw_thread_loop_start(loop_) < 0) return fail("could not start the PipeWire loop"), false;
    pw_thread_loop_lock(loop_);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    const int duplicate = dup(fd_);
    core_ = context_ && duplicate >= 0 ? pw_context_connect_fd(context_, duplicate, nullptr, 0) : nullptr;
    if (!core_) {
      pw_thread_loop_unlock(loop_);
      disconnect();
      return fail("could not connect to the PipeWire remote"), false;
    }
    pw_properties *properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
    stream_ = pw_stream_new(core_, ("qui-" + name_).c_str(), properties);
    static const pw_stream_events events = [] {
      pw_stream_events e{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.state_changed = on_state;
      e.param_changed = on_param;
      e.remove_buffer = on_remove;
      e.process = on_process;
      return e;
    }();
    spa_zero(listener_);
    pw_stream_add_listener(stream_, &listener_, &events, this);

    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_video_info_raw info{};
    info.format = SPA_VIDEO_FORMAT_BGRx;
    info.max_framerate = SPA_FRACTION(static_cast<uint32_t>(fps_), 1);
    const spa_pod *params[] = {spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    const int result = pw_stream_connect(stream_, PW_DIRECTION_INPUT, node_, static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    pw_thread_loop_unlock(loop_);
    if (result < 0) {
      disconnect();
      return fail("could not connect the video stream"), false;
    }
    last_frame_ = monotonic();
    return true;
  }

  void disconnect() {
    if (loop_) pw_thread_loop_stop(loop_);
    if (stream_) pw_stream_destroy(stream_);
    if (core_) pw_core_disconnect(core_);
    if (context_) pw_context_destroy(context_);
    if (loop_) pw_thread_loop_destroy(loop_);
    stream_ = nullptr;
    core_ = nullptr;
    context_ = nullptr;
    loop_ = nullptr;
    pending_ = nullptr;
  }

  int next(qsys_frame *out, int timeout_ms) {
    {
      std::unique_lock lock(mutex_);
      if (!pending_) cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return pending_ != nullptr; });
      if (!pending_) {
        lock.unlock();
        recover_if_stalled();
        return 0;
      }
    }
    // Copied under the loop lock because a renegotiation unmaps the buffers.
    pw_thread_loop_lock(loop_);
    std::unique_lock lock(mutex_);
    pw_buffer *buffer = std::exchange(pending_, nullptr);
    if (buffer) {
      const spa_data &plane = buffer->buffer->datas[0];
      const size_t size = static_cast<size_t>(frame_.stride) * static_cast<size_t>(frame_.height);
      pixels_.resize(size);
      std::memcpy(pixels_.data(), static_cast<const uint8_t *>(plane.data) + plane.chunk->offset, size);
      pw_stream_queue_buffer(stream_, buffer);
    }
    const qsys_frame frame = frame_;
    lock.unlock();
    pw_thread_loop_unlock(loop_);
    if (!buffer) return 0;
    *out = frame;
    out->data = pixels_.data();
    return 1;
  }

 private:
  void recover_if_stalled() {
    const double now = monotonic();
    if (now - last_frame_.load() < kStallSeconds || state_.load() == PW_STREAM_STATE_PAUSED || now - last_attempt_ < kReconnectCooldown) return;
    last_attempt_ = now;
    disconnect();
    connect();
  }

  static void on_state(void *data, pw_stream_state, pw_stream_state state, const char *) { static_cast<VideoStream *>(data)->state_ = state; }

  static void on_param(void *data, uint32_t id, const spa_pod *param) {
    auto *self = static_cast<VideoStream *>(data);
    spa_video_info_raw info{};
    if (id != SPA_PARAM_Format || !param || spa_format_video_raw_parse(param, &info) < 0) return;
    std::lock_guard lock(self->mutex_);
    self->width_ = static_cast<int>(info.size.width);
    self->height_ = static_cast<int>(info.size.height);
  }

  static void on_remove(void *data, pw_buffer *buffer) {
    auto *self = static_cast<VideoStream *>(data);
    std::lock_guard lock(self->mutex_);
    if (self->pending_ == buffer) self->pending_ = nullptr;
  }

  static void on_process(void *data) {
    auto *self = static_cast<VideoStream *>(data);
    pw_buffer *newest = nullptr;
    while (pw_buffer *buffer = pw_stream_dequeue_buffer(self->stream_)) {
      if (newest) pw_stream_queue_buffer(self->stream_, newest);
      newest = buffer;
    }
    if (!newest) return;
    const spa_buffer *buffer = newest->buffer;
    const spa_data *plane = buffer->n_datas > 0 ? &buffer->datas[0] : nullptr;
    bool valid = plane && plane->data && plane->chunk && !(plane->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) && plane->chunk->offset <= plane->maxsize &&
                 plane->chunk->size <= plane->maxsize - plane->chunk->offset;
    int64_t stride = 0;
    if (valid) {
      const int64_t width = self->width_, height = self->height_;
      stride = plane->chunk->stride ? plane->chunk->stride : width * 4;
      valid = width > 0 && height > 0 && stride >= width * 4 && stride * height <= plane->chunk->size;
    }
    if (!valid) {
      pw_stream_queue_buffer(self->stream_, newest);
      return;
    }
    {
      std::lock_guard lock(self->mutex_);
      if (self->pending_) pw_stream_queue_buffer(self->stream_, self->pending_);
      self->pending_ = newest;
      ++self->sequence_;
      self->frame_ = {nullptr, self->width_, self->height_, static_cast<int>(stride), self->sequence_, static_cast<double>(pw_stream_get_nsec(self->stream_)) / 1e9};
    }
    self->last_frame_ = monotonic();
    self->cv_.notify_one();
  }

  int fd_;
  uint32_t node_;
  int fps_;
  std::string name_;
  pw_thread_loop *loop_ = nullptr;
  pw_context *context_ = nullptr;
  pw_core *core_ = nullptr;
  pw_stream *stream_ = nullptr;
  spa_hook listener_{};
  std::mutex mutex_;
  std::condition_variable cv_;
  pw_buffer *pending_ = nullptr;
  qsys_frame frame_{};
  std::vector<uint8_t> pixels_;
  int width_ = 0, height_ = 0;
  uint64_t sequence_ = 0;
  double last_attempt_ = 0;
  std::atomic<double> last_frame_{0};
  std::atomic<int> state_{PW_STREAM_STATE_UNCONNECTED};
};

}  // namespace

}  // namespace qui::sys

using namespace qui::sys;

struct qsys_screen {
  PortalSession session;
  std::vector<std::unique_ptr<VideoStream>> streams;
};

struct qsys_audio {
  int rate = 48000, channels = 2;
  pw_thread_loop *loop = nullptr;
  pw_stream *stream = nullptr;
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<float> ring;
  size_t read_at = 0, available = 0;
  double end_time = 0;
  bool failed = false;
};

namespace {

void audio_state(void *data, pw_stream_state, pw_stream_state state, const char *) {
  auto *audio = static_cast<qsys_audio *>(data);
  if (state != PW_STREAM_STATE_ERROR) return;
  std::lock_guard lock(audio->mutex);
  audio->failed = true;
  audio->cv.notify_all();
}

void audio_process(void *data) {
  auto *audio = static_cast<qsys_audio *>(data);
  pw_buffer *buffer = pw_stream_dequeue_buffer(audio->stream);
  if (!buffer) return;
  const spa_data &plane = buffer->buffer->datas[0];
  if (buffer->buffer->n_datas > 0 && plane.data && plane.chunk && plane.chunk->size > 0 && plane.chunk->offset <= plane.maxsize &&
      plane.chunk->size <= plane.maxsize - plane.chunk->offset && !(plane.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
    const auto *samples = reinterpret_cast<const float *>(static_cast<const uint8_t *>(plane.data) + plane.chunk->offset);
    const size_t count = plane.chunk->size / sizeof(float);
    const double seconds = static_cast<double>(count) / audio->channels / audio->rate;
    const double start = buffer->time > 0 ? static_cast<double>(buffer->time) / 1e9 : static_cast<double>(pw_stream_get_nsec(audio->stream)) / 1e9 - seconds;
    std::lock_guard lock(audio->mutex);
    for (size_t i = 0; i < count; ++i) {
      audio->ring[(audio->read_at + audio->available) % audio->ring.size()] = samples[i];
      if (audio->available < audio->ring.size()) ++audio->available;
      else audio->read_at = (audio->read_at + 1) % audio->ring.size();
    }
    audio->end_time = start + seconds;
    audio->cv.notify_one();
  }
  pw_stream_queue_buffer(audio->stream, buffer);
}

}  // namespace

extern "C" {

qsys_screen *qsys_screen_open(const qsys_screen_options *options) {
  auto screen = std::make_unique<qsys_screen>();
  if (!portal_start_screen(*options, screen->session)) {
    portal_stop(screen->session);
    return nullptr;
  }
  for (size_t i = 0; i < screen->session.streams.size(); ++i) {
    auto stream = std::make_unique<VideoStream>(screen->session.pipewire_fd, screen->session.streams[i].node_id, options->fps, std::to_string(i));
    if (!stream->connect()) {
      screen->streams.clear();
      portal_stop(screen->session);
      return nullptr;
    }
    screen->streams.push_back(std::move(stream));
  }
  return screen.release();
}

void qsys_screen_close(qsys_screen *screen) {
  if (!screen) return;
  screen->streams.clear();
  portal_stop(screen->session);
  delete screen;
}

int qsys_screen_count(const qsys_screen *screen) { return static_cast<int>(screen->session.streams.size()); }

void qsys_screen_info(const qsys_screen *screen, int stream, qsys_stream_info *out) { *out = screen->session.streams[static_cast<size_t>(stream)]; }

int qsys_screen_had_permission(const qsys_screen *screen) { return screen->session.had_permission; }

int qsys_screen_next(qsys_screen *screen, int stream, qsys_frame *out, int timeout_ms) {
  portal_pump();
  if (screen->session.closed) return -1;
  if (stream < 0 || stream >= static_cast<int>(screen->streams.size())) return fail("no such stream");
  return screen->streams[static_cast<size_t>(stream)]->next(out, timeout_ms);
}

namespace {

class SourceProbe {
 public:
  bool collect(std::vector<qsys_audio_source> &out) {
    loop_ = pw_thread_loop_new("qui-sources", nullptr);
    if (!loop_ || pw_thread_loop_start(loop_) < 0) {
      if (loop_) pw_thread_loop_destroy(loop_);
      fail("could not start the PipeWire loop");
      return false;
    }
    pw_thread_loop_lock(loop_);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    core_ = context_ ? pw_context_connect(context_, nullptr, 0) : nullptr;
    registry_ = core_ ? pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0) : nullptr;
    if (!registry_) {
      pw_thread_loop_unlock(loop_);
      teardown();
      fail("could not reach PipeWire");
      return false;
    }

    static const pw_core_events core_events = [] {
      pw_core_events e{};
      e.version = PW_VERSION_CORE_EVENTS;
      e.done = [](void *data, uint32_t id, int) {
        auto *self = static_cast<SourceProbe *>(data);
        if (id != PW_ID_CORE) return;
        self->done_ = true;
        pw_thread_loop_signal(self->loop_, false);
      };
      return e;
    }();
    static const pw_registry_events registry_events = [] {
      pw_registry_events e{};
      e.version = PW_VERSION_REGISTRY_EVENTS;
      e.global = [](void *data, uint32_t, uint32_t, const char *, uint32_t, const spa_dict *properties) {
        static_cast<SourceProbe *>(data)->add(properties);
      };
      return e;
    }();
    spa_zero(core_hook_);
    spa_zero(registry_hook_);
    pw_core_add_listener(core_, &core_hook_, &core_events, this);
    pw_registry_add_listener(registry_, &registry_hook_, &registry_events, this);

    pw_core_sync(core_, PW_ID_CORE, 0);
    while (!done_) pw_thread_loop_wait(loop_);
    pw_thread_loop_unlock(loop_);

    out = std::move(found_);
    teardown();
    return true;
  }

 private:
  void add(const spa_dict *properties) {
    const char *klass = properties ? spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS) : nullptr;
    if (!klass || std::strncmp(klass, "Audio/Source", 12) != 0) return;
    const char *name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    const char *description = spa_dict_lookup(properties, PW_KEY_NODE_DESCRIPTION);
    qsys_audio_source source{};
    std::snprintf(source.name, sizeof(source.name), "%s", name ? name : "");
    std::snprintf(source.description, sizeof(source.description), "%s", description && *description ? description : (name ? name : ""));
    if (!*source.name) return;
    source.is_monitor = std::strstr(source.name, ".monitor") != nullptr;
    for (const qsys_audio_source &seen : found_)
      if (std::strcmp(seen.name, source.name) == 0) return;
    found_.push_back(source);
  }

  void teardown() {
    if (!loop_) return;
    if (registry_) {
      spa_hook_remove(&registry_hook_);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry_));
      registry_ = nullptr;
    }
    if (core_) {
      spa_hook_remove(&core_hook_);
      pw_core_disconnect(core_);
      core_ = nullptr;
    }
    if (context_) {
      pw_context_destroy(context_);
      context_ = nullptr;
    }
    pw_thread_loop_stop(loop_);
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }

  pw_thread_loop *loop_ = nullptr;
  pw_context *context_ = nullptr;
  pw_core *core_ = nullptr;
  pw_registry *registry_ = nullptr;
  spa_hook core_hook_{}, registry_hook_{};
  bool done_ = false;
  std::vector<qsys_audio_source> found_;
};

}  // namespace

extern "C" int qsys_audio_sources(qsys_audio_source *out, int max) {
  if (!out || max <= 0) return qui::sys::fail("no room for the sources");
  ensure_pipewire();
  std::vector<qsys_audio_source> found;
  SourceProbe probe;
  if (!probe.collect(found)) return -1;
  const size_t count = std::min<size_t>(static_cast<size_t>(max), found.size());
  std::copy(found.begin(), found.begin() + static_cast<long>(count), out);
  return static_cast<int>(count);
}

qsys_audio *qsys_audio_open(const qsys_audio_options *options) {
  ensure_pipewire();
  auto audio = std::make_unique<qsys_audio>();
  if (options->rate > 0) audio->rate = options->rate;
  if (options->channels > 0) audio->channels = std::min(options->channels, 2);
  audio->ring.resize(static_cast<size_t>(audio->rate) * static_cast<size_t>(audio->channels) * 2);

  audio->loop = pw_thread_loop_new("qui-audio", nullptr);
  if (!audio->loop || pw_thread_loop_start(audio->loop) < 0) {
    fail("could not start the PipeWire loop");
    if (audio->loop) pw_thread_loop_destroy(audio->loop);
    return nullptr;
  }
  pw_thread_loop_lock(audio->loop);
  pw_properties *properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, options->system ? "Music" : "Communication", nullptr);
  if (options->system) pw_properties_set(properties, PW_KEY_STREAM_CAPTURE_SINK, "true");
  if (options->target && *options->target) pw_properties_set(properties, PW_KEY_TARGET_OBJECT, options->target);
  static const pw_stream_events events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = audio_state;
    e.process = audio_process;
    return e;
  }();
  audio->stream = pw_stream_new_simple(pw_thread_loop_get_loop(audio->loop), options->name && *options->name ? options->name : "qui-audio", properties, &events, audio.get());

  uint8_t buffer[1024];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = static_cast<uint32_t>(audio->rate);
  info.channels = static_cast<uint32_t>(audio->channels);
  if (audio->channels == 1) {
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  } else {
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
  }
  const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
  const int result = audio->stream ? pw_stream_connect(audio->stream, PW_DIRECTION_INPUT, PW_ID_ANY, static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1) : -1;
  pw_thread_loop_unlock(audio->loop);
  if (result < 0) {
    fail("could not connect the audio stream");
    qsys_audio_close(audio.release());
    return nullptr;
  }
  return audio.release();
}

void qsys_audio_close(qsys_audio *audio) {
  if (!audio) return;
  if (audio->loop) pw_thread_loop_stop(audio->loop);
  if (audio->stream) pw_stream_destroy(audio->stream);
  if (audio->loop) pw_thread_loop_destroy(audio->loop);
  delete audio;
}

int qsys_audio_read(qsys_audio *audio, float *out, int max_frames, int timeout_ms, double *time) {
  std::unique_lock lock(audio->mutex);
  const auto channels = static_cast<size_t>(audio->channels);
  audio->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return audio->available >= channels || audio->failed; });
  if (audio->failed && audio->available < channels) return -1;
  const size_t frames = std::min(audio->available / channels, static_cast<size_t>(std::max(0, max_frames)));
  if (time) *time = audio->end_time - static_cast<double>(audio->available / channels) / audio->rate;
  for (size_t i = 0; i < frames * channels; ++i) {
    out[i] = audio->ring[audio->read_at];
    audio->read_at = (audio->read_at + 1) % audio->ring.size();
  }
  audio->available -= frames * channels;
  return static_cast<int>(frames);
}

}  // extern "C"
