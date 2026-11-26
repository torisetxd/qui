#pragma once

#include "qui_sys.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace qui::sys {

inline void check(int status) {
  if (status != 0) throw std::runtime_error(qsys_last_error());
}

class Screen {
 public:
  explicit Screen(const qsys_screen_options &options) : p_(qsys_screen_open(&options)) {
    if (!p_) throw std::runtime_error(qsys_last_error());
  }

  bool had_permission() const { return qsys_screen_had_permission(p_.get()); }
  int count() const { return qsys_screen_count(p_.get()); }
  qsys_stream_info info(int stream) const {
    qsys_stream_info out{};
    qsys_screen_info(p_.get(), stream, &out);
    return out;
  }

  /* Empty on timeout. Throws once the session has ended. */
  std::optional<qsys_frame> next(int stream, std::chrono::milliseconds timeout) {
    qsys_frame frame{};
    const int status = qsys_screen_next(p_.get(), stream, &frame, static_cast<int>(timeout.count()));
    if (status < 0) throw std::runtime_error("screen capture session ended");
    return status ? std::optional(frame) : std::nullopt;
  }

 private:
  struct Close {
    void operator()(qsys_screen *screen) const { qsys_screen_close(screen); }
  };
  std::unique_ptr<qsys_screen, Close> p_;
};

/* A capture source to pass as qsys_audio_options::target. */
struct AudioSource {
  std::string name, description;
  bool monitor = false;
};

inline std::vector<AudioSource> audio_sources() {
  std::vector<qsys_audio_source> buffer(64);
  const int count = qsys_audio_sources(buffer.data(), static_cast<int>(buffer.size()));
  if (count < 0) throw std::runtime_error(qsys_last_error());
  std::vector<AudioSource> out;
  out.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) out.push_back({buffer[static_cast<size_t>(i)].name, buffer[static_cast<size_t>(i)].description, buffer[static_cast<size_t>(i)].is_monitor != 0});
  return out;
}

/* Global keys through the portal. The keys arrive on an internal thread, so
 * wake the UI from the callback instead of touching it there. */
class Shortcuts {
 public:
  struct Entry {
    std::string id, description, preferred_trigger;
  };

  Shortcuts(const std::string &app_id, const std::vector<Entry> &entries, std::function<void(const std::string &)> on)
      : on_(std::move(on)) {
    std::vector<qsys_shortcut> raw;
    raw.reserve(entries.size());
    for (const Entry &entry : entries) raw.push_back({entry.id.c_str(), entry.description.c_str(), entry.preferred_trigger.c_str()});
    p_.reset(qsys_shortcuts_open(app_id.c_str(), raw.data(), static_cast<int>(raw.size()), &Shortcuts::dispatch, this));
    if (!p_) throw std::runtime_error(qsys_last_error());
  }

 private:
  static void dispatch(const char *id, void *user) {
    auto *self = static_cast<Shortcuts *>(user);
    if (self->on_) self->on_(id);
  }
  struct Close {
    void operator()(qsys_shortcuts *shortcuts) const { qsys_shortcuts_close(shortcuts); }
  };
  std::function<void(const std::string &)> on_;
  std::unique_ptr<qsys_shortcuts, Close> p_;
};

class Audio {
 public:
  explicit Audio(const qsys_audio_options &options = {}) : channels_(options.channels > 0 && options.channels <= 2 ? options.channels : 2), p_(qsys_audio_open(&options)) {
    if (!p_) throw std::runtime_error(qsys_last_error());
  }

  /* Returns frames read, 0 on timeout. Throws once the stream has failed. */
  int read(std::span<float> out, std::chrono::milliseconds timeout, double *time = nullptr) {
    const int frames = qsys_audio_read(p_.get(), out.data(), static_cast<int>(out.size()) / channels_, static_cast<int>(timeout.count()), time);
    if (frames < 0) throw std::runtime_error("audio capture stream failed");
    return frames;
  }

 private:
  struct Close {
    void operator()(qsys_audio *audio) const { qsys_audio_close(audio); }
  };
  int channels_;
  std::unique_ptr<qsys_audio, Close> p_;
};

}  // namespace qui::sys
