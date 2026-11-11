//go:build cgo

#include "engine.hpp"
#include "qui_c.h"

#include <SDL3/SDL.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#if defined(__ANDROID__)
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace {

using namespace qui;

constexpr float kDragThreshold = 4, kTouchSlop = 8;
constexpr size_t kResultsHeader = 64, kResultsSlot = 56;
constexpr float kWheelPixels = 110, kSmoothRate = 13, kSmoothCap = 2600;
constexpr double kCaretBlink = 0.53;
constexpr double kLongPress = 0.5, kDoubleTap = 0.3, kPressDelay = 0.08, kVelocityWindow = 0.1, kFlingStep = 1.0 / 60.0;
constexpr float kDoubleTapSlop = 24, kFlingMin = 120, kFlingStop = 24, kFlingDecay = 3, kFlingMax = 6000;
constexpr double kNever = std::numeric_limits<double>::infinity();

std::string g_app_id;
bool g_borderless = false;

constexpr uint32_t kKeyReleased = 0x100;

uint32_t key_from_sdl(SDL_Keycode key) {
  switch (key) {
    case SDLK_LEFT: return KeyLeft;
    case SDLK_RIGHT: return KeyRight;
    case SDLK_UP: return KeyUp;
    case SDLK_DOWN: return KeyDown;
    case SDLK_HOME: return KeyHome;
    case SDLK_END: return KeyEnd;
    case SDLK_PAGEUP: return KeyPageUp;
    case SDLK_PAGEDOWN: return KeyPageDown;
    case SDLK_KP_ENTER: return KeyEnter;
    case SDLK_AC_BACK: return KeyEscape;
    case SDLK_LSHIFT: return KeyLeftShift;
    case SDLK_RSHIFT: return KeyRightShift;
    case SDLK_LCTRL: return KeyLeftCtrl;
    case SDLK_RCTRL: return KeyRightCtrl;
    case SDLK_LALT: return KeyLeftAlt;
    case SDLK_RALT: return KeyRightAlt;
    case SDLK_LGUI: return KeyLeftMeta;
    case SDLK_RGUI: return KeyRightMeta;
    default: break;
  }
  if (key >= SDLK_F1 && key <= SDLK_F12) return KeyF1 + (key - SDLK_F1);
  if (key & SDLK_SCANCODE_MASK) return 0;
  return key;
}

uint32_t mods_from_sdl(SDL_Keymod mods) {
  uint32_t out = 0;
  if (mods & SDL_KMOD_SHIFT) out |= ModShift;
  if (mods & SDL_KMOD_CTRL) out |= ModCtrl;
  if (mods & SDL_KMOD_ALT) out |= ModAlt;
  if (mods & SDL_KMOD_GUI) out |= ModMeta;
  return out;
}

uint32_t key_from_name(std::string_view name) {
  static const std::pair<std::string_view, uint32_t> names[] = {
      {"backspace", KeyBackspace}, {"tab", KeyTab}, {"enter", KeyEnter}, {"escape", KeyEscape}, {"space", KeySpace},
      {"delete", KeyDelete}, {"left", KeyLeft}, {"right", KeyRight}, {"up", KeyUp}, {"down", KeyDown},
      {"home", KeyHome}, {"end", KeyEnd}, {"pageup", KeyPageUp}, {"pagedown", KeyPageDown}};
  for (const auto &[text, code] : names)
    if (text == name) return code;
  return name.size() == 1 ? static_cast<uint32_t>(static_cast<unsigned char>(name[0])) : 0;
}

std::string env(const char *name) {
  const char *value = std::getenv(name);
  return value ? value : "";
}

uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc = 0xffffffffu) {
  static const auto table = [] {
    std::vector<uint32_t> t(256);
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return crc;
}

void put_be32(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data) {
  put_be32(out, static_cast<uint32_t>(data.size()));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  put_be32(out, ~crc32(out.data() + start, out.size() - start));
}

bool write_png(const std::string &path, const uint32_t *pixels, int width, int height) {
  std::vector<uint8_t> raw;
  raw.reserve((static_cast<size_t>(width) * 4 + 1) * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    const auto *row = reinterpret_cast<const uint8_t *>(pixels + static_cast<size_t>(y) * width);
    raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4);
  }
  std::vector<uint8_t> z{0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (const uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t at = 0; at < raw.size();) {
    const size_t n = std::min<size_t>(raw.size() - at, 65535);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n));
    z.push_back(static_cast<uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + static_cast<long>(at), raw.begin() + static_cast<long>(at + n));
    at += n;
  }
  put_be32(z, (b << 16) | a);

  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::vector<uint8_t> header;
  put_be32(header, static_cast<uint32_t>(width));
  put_be32(header, static_cast<uint32_t>(height));
  header.insert(header.end(), {8, 6, 0, 0, 0});
  chunk(png, "IHDR", header);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", {});
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
  return file.good();
}

int headless_frames() {
  const std::string value = env("QUI_FRAMES");
  return value.empty() ? 4 : std::atoi(value.c_str());
}

void *gl_proc(const char *name) { return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name)); }

#if defined(__ANDROID__)
void *pump_stdio_to_logcat(void *arg) {
  const int fd = *static_cast<int *>(arg);
  std::string pending;
  char chunk[1024];
  for (ssize_t n; (n = read(fd, chunk, sizeof(chunk))) > 0;) {
    pending.append(chunk, static_cast<size_t>(n));
    for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1))
      __android_log_write(ANDROID_LOG_INFO, "qui", pending.substr(0, end).c_str());
  }
  return nullptr;
}

// QuiActivity answers this message by switching the system bar icons between
// dark (param 1, for a light background) and light.
constexpr Uint32 kLightBarsCommand = 0x8001;

void move_task_to_back() {
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return;
  jclass type = env->GetObjectClass(activity);
  env->CallBooleanMethod(activity, env->GetMethodID(type, "moveTaskToBack", "(Z)Z"), JNI_TRUE);
  env->DeleteLocalRef(type);
  env->DeleteLocalRef(activity);
}

void capture_stdio() {
  static int pipes[2];
  static bool started = false;
  if (started || pipe(pipes) != 0) return;
  started = true;
  dup2(pipes[1], 1);
  dup2(pipes[1], 2);
  setvbuf(stderr, nullptr, _IOLBF, 0);
  pthread_t thread;
  if (pthread_create(&thread, nullptr, pump_stdio_to_logcat, &pipes[0]) == 0) pthread_detach(thread);
}
#endif

}  // namespace

struct qui_app {
  std::mutex picked_lock;
  std::deque<std::string> picked;
  std::string picked_out;
  std::filesystem::path paste_dir;
  int pastes = 0;
  Engine engine;
  std::unique_ptr<Backend> backend;
  CpuBackend *cpu = nullptr;
  SDL_Window *window = nullptr;
  SDL_GLContext gl = nullptr;

  bool headless = false, sdl_ready = false, quit = false, dirty = true, force_full = true, window_focused = true, stop_text_pending = false;
  int frames = 0;
  int pixel_w = 0, pixel_h = 0;
  float dpr = 1, logical_w = 0, logical_h = 0;
  float inset_top = 0, inset_right = 0, inset_bottom = 0, inset_left = 0;
  std::string shot_path, tree_path, wire_path;
  std::vector<uint8_t> last_wire, results;
  std::vector<uint32_t> capture;
  std::vector<std::string> drive;
  size_t drive_at = 0;

  float pointer_x = -1, pointer_y = -1;
  float press_x = 0, press_y = 0;
  float drag_dx = 0, drag_dy = 0;
  uint64_t hovered = 0, pressed = 0, focused = 0, clicked = 0, right_clicked = 0;
  uint64_t menu_return = 0;
  bool menu_open = false;
  uint64_t drag_start = 0, drag_end = 0, scrollbar_hovered = 0, scrollbar_dragged = 0;
  float scrollbar_grab = 0;
  bool back_pending = false, back_consumed = false;
  bool dragging = false, focus_ring = false, selecting = false, caret_on = true, text_cursor = false, pointer_down = false, pointer_tapped = false;
  bool text_selecting = false, files_hovering = false;
  float engine_ms = 0;
  std::vector<uint32_t> keys;
  std::string clipboard, text_blob;
  SDL_Rect last_ime_area{};
  SDL_Cursor *cursor_text = nullptr;

  enum class Track : uint8_t { None, Pending, Press, Pan, Scroll, Pinch, Done };
  struct Link {
    uint64_t id;
    uint8_t gestures;
    bool scroll;
  };
  struct Sample {
    double time;
    float x, y;
  };
  struct Finger {
    SDL_FingerID id;
    float x, y;
  };

  Track track = Track::None;
  bool track_touch = false, input_touch = false;
  std::vector<Link> chain;
  std::vector<Sample> samples;
  std::vector<Finger> fingers;
  uint64_t gesture_owner = 0, pan_start = 0, pan_end = 0, pinch_once = 0, long_pressed = 0, double_tapped = 0, last_tap_id = 0, fling_id = 0;
  float vel_x = 0, vel_y = 0, pinch_scale = 1, pinch_spread = 0, last_tap_x = 0, last_tap_y = 0, fling_v = 0;
  double long_press_at = kNever, press_show_at = 0, last_tap_at = -1, fling_next = 0, fling_last = 0, drive_clock = 0;
  double smooth_next = 0, smooth_last = 0;
  double ease_next = 0;
  double trimmed_at = 0;
  bool hit_drag = false;
  double drag_click_at = 0;
  uint64_t smooth_id = 0;
  float smooth_left = 0;
  bool smooth_wheel = false;

  double next_tick = 0, blink_at = 0;
  // Android can drop the first frames presented to a surface it just
  // recreated, so a resume redraws a few times while it settles.
  int settle_frames = 0;
  double settle_next = 0;
  int light_bars = -1;
  double last_frame_time = 0;
  float dt = 1.f / 60.f;

  double now() const { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }
  double clock() const { return headless ? drive_clock : now(); }
  double event_time(const SDL_Event &e) const { return headless ? drive_clock : static_cast<double>(e.common.timestamp) * 1e-9; }
  uint64_t active_pan() const { return track == Track::Pan ? gesture_owner : 0; }
  uint64_t active_pinch() const { return track == Track::Pinch ? gesture_owner : pinch_once; }
  bool press_hidden() const { return press_show_at > 0 && pressed; }
  uint64_t shown_pressed() const { return press_hidden() ? 0 : pressed; }
  uint64_t shown_hovered() const { return press_hidden() ? 0 : hovered; }

  void read_size() {
    if (headless && !window) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    pixel_w = std::max(1, w);
    pixel_h = std::max(1, h);
    if (env("QUI_DPR").empty()) dpr = std::max(0.25f, SDL_GetWindowDisplayScale(window));
    logical_w = static_cast<float>(pixel_w) / dpr;
    logical_h = static_cast<float>(pixel_h) / dpr;
    SDL_Rect safe{};
    int window_w = 0, window_h = 0;
    if (SDL_GetWindowSafeArea(window, &safe) && SDL_GetWindowSize(window, &window_w, &window_h)) {
      const float scale = SDL_GetWindowPixelDensity(window) / dpr;
      inset_left = static_cast<float>(safe.x) * scale;
      inset_top = static_cast<float>(safe.y) * scale;
      inset_right = static_cast<float>(window_w - safe.x - safe.w) * scale;
      inset_bottom = static_cast<float>(window_h - safe.y - safe.h) * scale;
    }
  }

  bool create_gl(Uint64 flags, const char *title, int width, int height) {
#if defined(_WIN32)
    bool es = false;
#else
    bool es = true;
#endif
    const auto configure = [&](bool use_es) {
      es = use_es;
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, use_es ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, use_es ? 0 : 3);
    };
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    configure(es);
    window = SDL_CreateWindow(title, width, height, flags | SDL_WINDOW_OPENGL);
    if (!window) {
      std::fprintf(stderr, "qui: OpenGL is unavailable (%s), using the CPU renderer\n", SDL_GetError());
      return false;
    }
    gl = SDL_GL_CreateContext(window);
    if (!gl) {
      configure(!es);
      gl = SDL_GL_CreateContext(window);
    }
    if (gl) {
      SDL_GL_MakeCurrent(window, gl);
      SDL_GL_SetSwapInterval(1);
      backend = make_gl_backend(gl_proc, headless, es);
    }
    if (backend) return true;
    std::fprintf(stderr, "qui: OpenGL is unavailable (%s), using the CPU renderer\n", SDL_GetError());
    if (gl) SDL_GL_DestroyContext(gl);
    gl = nullptr;
    SDL_DestroyWindow(window);
    window = nullptr;
    return false;
  }

  void use_cpu() {
    auto backend_cpu = make_cpu_backend();
    cpu = backend_cpu.get();
    backend = std::move(backend_cpu);
  }

  void open(const char *title, int width, int height) {
    shot_path = env("QUI_SHOT");
    tree_path = env("QUI_TREE");
    wire_path = env("QUI_WIRE");
    headless = !shot_path.empty() || !tree_path.empty() || !wire_path.empty();
    engine.async_images = !headless;
    engine.on_async = [] {
      SDL_Event event{};
      event.type = SDL_EVENT_USER;
      SDL_PushEvent(&event);
    };
    std::string requested = env("QUI_BACKEND");
    if (const std::string size = env("QUI_SIZE"); !size.empty()) std::sscanf(size.c_str(), "%dx%d", &width, &height);
    if (const std::string scale = env("QUI_DPR"); !scale.empty()) dpr = std::stof(scale);
    std::string steps = env("QUI_DRIVE");
    for (size_t at = 0; at < steps.size();) {
      const size_t end = std::min(steps.find(';', at), steps.size());
      if (end > at) {
        std::string step = steps.substr(at, end - at);
        if (int frames_to_wait = 0; std::sscanf(step.c_str(), "wait:%d", &frames_to_wait) == 1)
          drive.insert(drive.end(), static_cast<size_t>(std::max(1, (frames_to_wait + 1) / 2)), "wait");
        else
          drive.push_back(std::move(step));
      }
      at = end + 1;
    }

    if (headless && requested != "gl") {
      pixel_w = static_cast<int>(std::lround(static_cast<float>(width) * dpr));
      pixel_h = static_cast<int>(std::lround(static_cast<float>(height) * dpr));
      logical_w = static_cast<float>(width);
      logical_h = static_cast<float>(height);
      use_cpu();
      return;
    }

    if (!g_app_id.empty()) SDL_SetAppMetadata(title, nullptr, g_app_id.c_str());
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::fprintf(stderr, "qui: could not start SDL: %s\n", SDL_GetError());
      quit = true;
      use_cpu();
      return;
    }
    sdl_ready = true;
    Uint64 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (headless) flags |= SDL_WINDOW_HIDDEN;
#if !defined(__ANDROID__)
    if (g_borderless) flags |= SDL_WINDOW_BORDERLESS;
#endif
    if (requested == "cpu" || !create_gl(flags, title, width, height)) {
      window = SDL_CreateWindow(title, width, height, flags);
      if (!window) {
        std::fprintf(stderr, "qui: could not create a window: %s\n", SDL_GetError());
        quit = true;
      }
      use_cpu();
    }
    if (window) read_size();
#if !defined(__ANDROID__)
    if (window && g_borderless) SDL_SetWindowHitTest(window, window_hit_test, this);
#endif
    if (window && headless) {
      if (!env("QUI_DPR").empty()) {
        pixel_w = static_cast<int>(std::lround(static_cast<float>(width) * dpr));
        pixel_h = static_cast<int>(std::lround(static_cast<float>(height) * dpr));
      } else {
        pixel_w = width;
        pixel_h = height;
        dpr = 1;
      }
      logical_w = static_cast<float>(width);
      logical_h = static_cast<float>(height);
    }
  }

  // Resize edges, then the app's Drag boxes, except where a hit box inside one
  // takes the press.
  static SDL_HitTestResult SDLCALL window_hit_test(SDL_Window *win, const SDL_Point *area, void *data) {
    auto *app = static_cast<qui_app *>(data);
    app->hit_drag = false;
    int w = 0, h = 0;
    if (!SDL_GetWindowSize(win, &w, &h)) return SDL_HITTEST_NORMAL;
    if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
      constexpr int edge = 6;
      const bool left = area->x < edge, right = area->x >= w - edge, top = area->y < edge, bottom = area->y >= h - edge;
      if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
      if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
      if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
      if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
      if (top) return SDL_HITTEST_RESIZE_TOP;
      if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
      if (left) return SDL_HITTEST_RESIZE_LEFT;
      if (right) return SDL_HITTEST_RESIZE_RIGHT;
    }
    const float scale = SDL_GetWindowPixelDensity(win) / app->dpr;
    const float x = static_cast<float>(area->x) * scale, y = static_cast<float>(area->y) * scale;
    const uint64_t hit = app->hit_test(x, y, false);
    for (const Engine::DragArea &drag : app->engine.drags) {
      if (x < drag.x || x >= drag.x + drag.w || y < drag.y || y >= drag.y + drag.h) continue;
      if (hit && hit != drag.id) return SDL_HITTEST_NORMAL;
      app->hit_drag = true;
      return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
  }

  void window_action(int action) {
    if (!window) return;
    if (action == 0) SDL_MinimizeWindow(window);
    if (action == 1) {
      if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window);
      else SDL_MaximizeWindow(window);
    }
    if (action == 2) quit = true;
  }

  static bool inside(const HitEntry &hit, float x, float y) {
    return x >= hit.x && x < hit.x + hit.w && y >= hit.y && y < hit.y + hit.h && x >= hit.cx0 && x < hit.cx1 && y >= hit.cy0 && y < hit.cy1;
  }

  uint64_t hit_test(float x, float y, bool scroll) const {
    const auto &hits = engine.hits;
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
      if (!scroll && !it->press) continue;
      if (!inside(*it, x, y)) continue;
      if (!scroll || it->scroll) return it->id;
      return it->scroll_parent >= 0 ? hits[static_cast<size_t>(it->scroll_parent)].id : 0;
    }
    return 0;
  }

  const HitEntry *scrollbar_at(float x, float y) const {
    const auto &hits = engine.hits;
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
      if ((!it->press && !it->scroll) || !inside(*it, x, y)) continue;
      return it->scrollbar ? &*it : nullptr;
    }
    return nullptr;
  }

  void drag_scrollbar() {
    const auto &hits = engine.hits;
    const auto bar = std::find_if(hits.rbegin(), hits.rend(), [&](const HitEntry &e) { return e.scrollbar && e.id == scrollbar_dragged; });
    if (bar == hits.rend()) return;
    const float room = bar->h - bar->thumb_h;
    const float fraction = room > 0 ? std::clamp((pointer_y - bar->y - scrollbar_grab) / room, 0.f, 1.f) : 0;
    if (const auto input = engine.inputs.find(bar->id); input != engine.inputs.end()) {
      input->second.scroll_y = fraction * input->second.scroll_limit_y;
    } else if (const auto list = engine.lists.find(bar->id); list != engine.lists.end()) {
      list->second.jump(fraction);
    } else if (const auto scroll = engine.scrolls.find(bar->id); scroll != engine.scrolls.end()) {
      scroll->second.offset = fraction * std::max(0.f, scroll->second.content - scroll->second.viewport);
    }
    dirty = true;
  }

  void update_hover() {
    const HitEntry *bar = input_touch ? nullptr : scrollbar_at(pointer_x, pointer_y);
    if (const uint64_t bar_id = bar ? bar->id : 0; bar_id != scrollbar_hovered) {
      scrollbar_hovered = bar_id;
      dirty = true;
    }
    const bool lifted = input_touch && track != Track::Pending && track != Track::Press;
    const uint64_t id = bar || lifted ? 0 : hit_test(pointer_x, pointer_y, false);
    if (id != hovered) {
      hovered = id;
      dirty = true;
      const bool over_text = input_of(id) != nullptr;
      if (over_text != text_cursor && sdl_ready && window) {
        text_cursor = over_text;
        if (over_text && !cursor_text) cursor_text = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
        SDL_SetCursor(over_text ? cursor_text : SDL_GetDefaultCursor());
      }
    }
  }

  InputState *input_of(uint64_t id) {
    if (!id) return nullptr;
    const auto it = engine.inputs.find(id);
    return it != engine.inputs.end() && it->second.frame == engine.frame_number ? &it->second : nullptr;
  }

  InputState *focused_input() { return input_of(focused); }

  void restart_blink() {
    caret_on = true;
    blink_at = now() + kCaretBlink;
  }

  void set_focus(uint64_t id, bool keyboard) {
    if (id == focused && keyboard == focus_ring) return;
    const bool changed = id != focused;
    focused = id;
    focus_ring = keyboard;
    dirty = true;
    restart_blink();
    if (!changed || !sdl_ready || !window) return;
    stop_text_pending = !focused_input();
    if (!stop_text_pending) SDL_StartTextInput(window);
  }

  void focus_step(bool backwards) {
    std::vector<uint64_t> order;
    for (const HitEntry &hit : engine.hits)
      if (hit.focusable) order.push_back(hit.id);
    if (order.empty()) return;
    const auto at = std::find(order.begin(), order.end(), focused);
    size_t index = 0;
    if (at == order.end()) index = backwards ? order.size() - 1 : 0;
    else if (backwards) index = (static_cast<size_t>(at - order.begin()) + order.size() - 1) % order.size();
    else index = (static_cast<size_t>(at - order.begin()) + 1) % order.size();
    set_focus(order[index], true);
    if (InputState *state = focused_input(); state && !state->multiline) {
      state->anchor = 0;
      state->caret = state->text.size();
      engine.refresh_input(*state);
    }
  }

  void set_clipboard(std::string_view text) {
    clipboard.assign(text);
    if (sdl_ready) SDL_SetClipboardText(clipboard.c_str());
  }

  std::string read_clipboard() {
    if (!sdl_ready) return clipboard;
    char *text = SDL_GetClipboardText();
    std::string out = text ? text : "";
    SDL_free(text);
    return out;
  }

  static std::string clipboard_data(const char *mime) {
    size_t size = 0;
    void *data = SDL_GetClipboardData(mime, &size);
    std::string out = data ? std::string(static_cast<const char *>(data), size) : std::string();
    SDL_free(data);
    return out;
  }

  static std::string file_uri_path(std::string_view uri) {
    if (!uri.starts_with("file://")) return {};
    uri.remove_prefix(7);
    const size_t slash = uri.find('/');
    if (slash == std::string_view::npos) return {};
    uri.remove_prefix(slash);
    std::string path;
    for (size_t i = 0; i < uri.size(); ++i) {
      if (uri[i] == '%' && i + 2 < uri.size()) {
        const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        const int high = hex(uri[i + 1]), low = hex(uri[i + 2]);
        if (high >= 0 && low >= 0) {
          path += static_cast<char>(high * 16 + low);
          i += 2;
          continue;
        }
      }
      path += uri[i];
    }
    return path;
  }

  // Files copied in a file manager, or a copied image with no text beside it,
  // paste the way a drop arrives: through picked_files. Pasted images are
  // written under a temp directory that close() removes.
  bool paste_files() {
    if (!sdl_ready) return false;
    std::vector<std::string> paths;
    for (const char *mime : {"text/uri-list", "x-special/gnome-copied-files"}) {
      if (!paths.empty() || !SDL_HasClipboardData(mime)) continue;
      const std::string list = clipboard_data(mime);
      size_t start = 0;
      while (start < list.size()) {
        size_t end = list.find('\n', start);
        if (end == std::string::npos) end = list.size();
        std::string_view line(list.data() + start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) line.remove_suffix(1);
        if (std::string path = file_uri_path(line); !path.empty()) paths.push_back(std::move(path));
        start = end + 1;
      }
    }
    if (paths.empty() && !SDL_HasClipboardText()) {
      static constexpr std::pair<const char *, const char *> images[] = {
          {"image/png", "png"}, {"image/jpeg", "jpg"}, {"image/gif", "gif"}, {"image/webp", "webp"}};
      for (const auto &[mime, extension] : images) {
        if (!SDL_HasClipboardData(mime)) continue;
        const std::string bytes = clipboard_data(mime);
        if (bytes.empty()) break;
        std::error_code error;
        if (paste_dir.empty()) {
          const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
          paste_dir = std::filesystem::temp_directory_path(error) / ("qui-paste-" + std::to_string(stamp));
          if (error) {
            paste_dir.clear();
            break;
          }
        }
        const auto dir = paste_dir / std::to_string(++pastes);
        std::filesystem::create_directories(dir, error);
        const auto path = dir / (std::string("image.") + extension);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (error || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) break;
        paths.push_back(path.string());
        break;
      }
    }
    if (paths.empty()) return false;
    {
      std::lock_guard lock(picked_lock);
      for (auto &path : paths) picked.push_back(std::move(path));
    }
    dirty = true;
    return true;
  }

  void input_touched(InputState &state) {
    engine.refresh_input(state);
    restart_blink();
    dirty = true;
  }

  void move_caret(InputState &state, size_t to, bool extend) {
    state.caret = to;
    if (!extend) state.anchor = to;
    state.last_edit = EditKind::None;
    input_touched(state);
  }

  void insert_text(InputState &state, std::string_view raw, EditKind kind) {
    const std::string text = clean_input(raw, state.multiline);
    if (text.empty() && !state.has_selection()) return;
    state.replace_range(state.selection_start(), state.selection_end(), text, kind);
    input_touched(state);
  }

  void erase_to(InputState &state, size_t to) {
    if (state.has_selection()) {
      state.replace_range(state.selection_start(), state.selection_end(), "", EditKind::Other);
    } else if (to != state.caret) {
      state.replace_range(std::min(to, state.caret), std::max(to, state.caret), "", EditKind::Erase);
    }
    input_touched(state);
  }

  bool input_key(InputState &state, uint32_t key, uint32_t mods) {
    if (!state.preedit.empty()) return true;
    if (mods & (ModAlt | ModMeta)) return false;
    const bool ctrl = mods & ModCtrl, shift = mods & ModShift;
    const std::string_view text = state.text;
    switch (key) {
      case KeyLeft:
        if (state.has_selection() && !shift) move_caret(state, state.selection_start(), false);
        else {
          state.goal_x = -1;
          move_caret(state, ctrl ? previous_word(text, state.caret) : previous_boundary(text, state.caret), shift);
        }
        return true;
      case KeyRight:
        if (state.has_selection() && !shift) move_caret(state, state.selection_end(), false);
        else {
          state.goal_x = -1;
          move_caret(state, ctrl ? next_word(text, state.caret) : next_boundary(text, state.caret), shift);
        }
        return true;
      case KeyUp:
      case KeyDown:
        if (state.passthrough_nav || !state.multiline || text.empty()) return false;
        move_caret(state, engine.input_vertical(state, key == KeyUp ? -1 : 1), shift);
        return true;
      case KeyHome:
        state.goal_x = -1;
        move_caret(state, ctrl ? 0 : engine.input_line_edge(state, false), shift);
        return true;
      case KeyEnd:
        state.goal_x = -1;
        move_caret(state, ctrl ? state.text.size() : engine.input_line_edge(state, true), shift);
        return true;
      case KeyBackspace:
        erase_to(state, ctrl ? previous_word(text, state.caret) : previous_boundary(text, state.caret));
        return true;
      case KeyDelete:
        erase_to(state, ctrl ? next_word(text, state.caret) : next_boundary(text, state.caret));
        return true;
      case KeyEnter: {
        const bool submit = !state.multiline || (state.submit_on_enter ? !shift : ctrl);
        if (submit) {
          state.submitted = true;
          dirty = true;
          if (!state.multiline && sdl_ready && SDL_HasScreenKeyboardSupport()) set_focus(0, false);
        } else {
          insert_text(state, "\n", EditKind::Other);
        }
        return true;
      }
      case KeyEscape:
        return false;
      default:
        break;
    }
    if (ctrl) {
      switch (key) {
        case 'a':
          state.anchor = 0;
          state.caret = state.text.size();
          state.goal_x = -1;
          input_touched(state);
          return true;
        case 'c':
          if (state.has_selection() && !state.password) set_clipboard(state.selected());
          return true;
        case 'x':
          if (state.has_selection() && !state.password) {
            set_clipboard(state.selected());
            erase_to(state, state.caret);
          }
          return true;
        case 'v':
          if (shift || state.password || !paste_files()) insert_text(state, read_clipboard(), EditKind::Other);
          return true;
        case 'z':
          if (shift) state.restore(state.redo, state.undo);
          else state.restore(state.undo, state.redo);
          input_touched(state);
          return true;
        case 'y':
          state.restore(state.redo, state.undo);
          input_touched(state);
          return true;
        default:
          return false;
      }
    }
    return key < KeyLeft;
  }

  void on_key(uint32_t key, uint32_t mods) {
    if (!key) return;
    if (key == KeyTab && !(mods & (ModCtrl | ModAlt | ModMeta))) {
      if (InputState *state = focused_input(); !state || !state->passthrough_nav) {
        focus_step(mods & ModShift);
        return;
      }
      keys.push_back(key);
      keys.push_back(mods);
      dirty = true;
      return;
    }
    if (InputState *state = focused_input()) {
      if (input_key(*state, key, mods)) return;
    } else if (key == 'c' && (mods & ModCtrl) && !engine.selection.empty()) {
      set_clipboard(engine.selected_text());
    } else if (focused && !mods && (key == KeyEnter || key == KeySpace)) {
      const auto hit = std::find_if(engine.hits.begin(), engine.hits.end(), [&](const HitEntry &e) { return e.id == focused && e.focusable && e.role != RoleSlider; });
      if (hit != engine.hits.end()) {
        clicked = focused;
        dirty = true;
        return;
      }
    }
    keys.push_back(key);
    keys.push_back(mods);
    dirty = true;
  }

  void set_preedit(std::string_view text, size_t cursor) {
    InputState *state = focused_input();
    if (!state || state->password) return;
    state->preedit.assign(text);
    state->preedit_cursor = std::min(cursor, state->preedit.size());
    input_touched(*state);
  }

  void start_text_selection(int clicks) {
    const bool on_link = std::any_of(engine.hits.begin(), engine.hits.end(), [&](const HitEntry &e) { return e.link && e.id == hovered; });
    uint64_t root = 0;
    TextPoint at;
    if (!on_link && engine.text_at(pointer_x, pointer_y, 0, root, at)) {
      if (clicks >= 2) {
        engine.select_word(root, at, clicks >= 3);
      } else if ((SDL_GetModState() & SDL_KMOD_SHIFT) && engine.selection.root == root) {
        engine.selection.focus = at;
        text_selecting = true;
      } else {
        engine.selection = {root, at, at, true};
        text_selecting = true;
      }
      dirty = true;
    } else if (!hovered && engine.selection.root) {
      engine.selection = {};
      dirty = true;
    }
  }

  void pointer_select(InputState &state, int clicks, bool extend) {
    const size_t byte = engine.input_byte_at(state, pointer_x, pointer_y);
    if (clicks == 2) {
      word_range(state.text, byte, state.anchor, state.caret);
    } else if (clicks >= 3) {
      state.anchor = 0;
      state.caret = state.text.size();
    } else {
      state.caret = byte;
      if (!extend) state.anchor = byte;
      selecting = true;
    }
    state.goal_x = -1;
    state.last_edit = EditKind::None;
    input_touched(state);
  }

  bool can_scroll(uint64_t id, float delta) const {
    if (const auto input = engine.inputs.find(id); input != engine.inputs.end())
      return delta < 0 ? input->second.scroll_y > 0 : input->second.scroll_y < input->second.scroll_limit_y;
    if (const auto list = engine.lists.find(id); list != engine.lists.end())
      return delta < 0 ? list->second.anchor > 0 || list->second.offset > 0 : !list->second.stuck;
    if (const auto scroll = engine.scrolls.find(id); scroll != engine.scrolls.end())
      return delta < 0 ? scroll->second.offset > 0 : scroll->second.offset < scroll->second.content - scroll->second.viewport;
    return false;
  }

  bool scroll_content(uint64_t id, float delta) {
    if (!can_scroll(id, delta)) return false;
    if (const auto input = engine.inputs.find(id); input != engine.inputs.end()) {
      input->second.scroll_y = std::clamp(input->second.scroll_y + delta * dpr, 0.f, input->second.scroll_limit_y);
    } else if (const auto list = engine.lists.find(id); list != engine.lists.end()) {
      list->second.gliding = false;
      list->second.scroll_by(delta);
      if (delta > 0 && list->second.bottom && list->second.anchor + 1 >= list->second.count) list->second.stuck = true;
    } else if (const auto scroll = engine.scrolls.find(id); scroll != engine.scrolls.end()) {
      scroll->second.offset = std::clamp(scroll->second.offset + delta, 0.f, std::max(0.f, scroll->second.content - scroll->second.viewport));
    }
    dirty = true;
    return true;
  }

  void capture_chain() {
    chain.clear();
    const auto &hits = engine.hits;
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
      if (it->scrollbar || !inside(*it, pointer_x, pointer_y)) continue;
      for (auto i = static_cast<int32_t>(hits.rend() - it) - 1; i >= 0; i = hits[static_cast<size_t>(i)].parent) {
        const HitEntry &hit = hits[static_cast<size_t>(i)];
        chain.push_back({hit.id, hit.gestures, hit.scroll});
      }
      return;
    }
  }

  void wheel(float amount) {
    fling_id = 0;
    if (SDL_GetModState() & SDL_KMOD_CTRL) {
      capture_chain();
      const auto link = std::find_if(chain.begin(), chain.end(), [](const Link &l) { return (l.gestures & GesturePinch) && !(l.gestures & GestureTouchOnly); });
      if (link != chain.end()) {
        pinch_once = link->id;
        pinch_scale *= std::pow(1.1f, amount);
        dirty = true;
        return;
      }
    }
    const uint64_t id = hit_test(pointer_x, pointer_y, true);
    if (!id) return;
    const float delta = -amount * kWheelPixels;
    // Touchpads send fractional steps at a high rate and already feel smooth.
    if (!smooth_wheel || amount != std::round(amount)) {
      smooth_id = 0;
      scroll_content(id, delta);
      return;
    }
    if (id != smooth_id || (smooth_left > 0) != (delta > 0)) smooth_left = 0;
    if (!smooth_id || id != smooth_id) smooth_last = smooth_next = clock();
    smooth_id = id;
    smooth_left = std::clamp(smooth_left + delta, -kSmoothCap, kSmoothCap);
    dirty = true;
  }

  void step_smooth(float elapsed) {
    if (elapsed <= 0) return;
    float step = smooth_left * (1 - std::exp(-kSmoothRate * elapsed));
    smooth_left -= step;
    const bool done = std::abs(smooth_left) < 0.5f;
    if (done) step += smooth_left;
    if (!scroll_content(smooth_id, step) || done) {
      smooth_id = 0;
      smooth_left = 0;
    }
    dirty = true;
  }

  void begin_track(double time, bool touch) {
    track = Track::Pending;
    track_touch = touch;
    press_x = pointer_x;
    press_y = pointer_y;
    samples.assign(1, {time, pointer_x, pointer_y});
    capture_chain();
  }

  void push_sample(double time) {
    if (samples.size() >= 32) samples.erase(samples.begin());
    samples.push_back({time, pointer_x, pointer_y});
  }

  void measure_velocity() {
    vel_x = vel_y = 0;
    if (samples.size() < 2) return;
    const Sample &last = samples.back();
    auto first = samples.end() - 1;
    while (first != samples.begin() && last.time - (first - 1)->time <= kVelocityWindow) --first;
    const auto span = static_cast<float>(last.time - first->time);
    if (span < 0.001f) return;
    vel_x = (last.x - first->x) / span;
    vel_y = (last.y - first->y) / span;
  }

  void claim_gesture(Track kind, uint64_t id) {
    track = kind;
    gesture_owner = id;
    pressed = 0;
    dragging = false;
    long_press_at = kNever;
    dirty = true;
  }

  void claim(float total_x, float total_y) {
    long_press_at = kNever;
    if (text_selecting) {
      track = Track::Done;
      pressed = 0;
      dirty = true;
      return;
    }
    const bool horizontal = std::abs(total_x) > std::abs(total_y);
    const uint8_t axis = horizontal ? GesturePanX : GesturePanY;
    if (!selecting) {
      for (const Link &link : chain) {
        if ((link.gestures & axis) && (track_touch || !(link.gestures & GestureTouchOnly))) {
          claim_gesture(Track::Pan, link.id);
          pan_start = link.id;
          return;
        }
        if (track_touch && !horizontal && link.scroll && can_scroll(link.id, -total_y)) {
          claim_gesture(Track::Scroll, link.id);
          return;
        }
      }
    }
    track = Track::Press;
    if (pressed) {
      dragging = true;
      drag_start = pressed;
    }
  }

  void track_move(double time, float dx, float dy) {
    if (track == Track::None || track == Track::Done || track == Track::Pinch) return;
    push_sample(time);
    const float total_x = pointer_x - press_x, total_y = pointer_y - press_y;
    if (track == Track::Pending && std::hypot(total_x, total_y) >= (track_touch ? kTouchSlop : kDragThreshold)) claim(total_x, total_y);
    if (track == Track::Scroll) {
      scroll_content(gesture_owner, -dy);
    } else if (track == Track::Pan || dragging) {
      drag_dx += dx;
      drag_dy += dy;
      if (track == Track::Pan) measure_velocity();
      dirty = true;
    }
  }

  void finish_track(double time, bool canceled) {
    if (track == Track::None) return;
    push_sample(time);
    measure_velocity();
    if (canceled) vel_x = vel_y = 0;
    if (track == Track::Pan) {
      pan_end = gesture_owner;
      dirty = true;
    } else if (track == Track::Scroll && std::abs(vel_y) >= kFlingMin) {
      fling_id = gesture_owner;
      fling_v = std::clamp(-vel_y, -kFlingMax, kFlingMax);
      fling_last = fling_next = clock();
      dirty = true;
    }
    track = Track::None;
    gesture_owner = 0;
    long_press_at = kNever;
    press_show_at = 0;
  }

  void step_fling(float elapsed) {
    const float before = fling_v;
    fling_v *= std::exp(-kFlingDecay * elapsed);
    if (!scroll_content(fling_id, (before + fling_v) * 0.5f * elapsed) || std::abs(fling_v) < kFlingStop) fling_id = 0;
    dirty = true;
  }

  void fire_long_press() {
    long_press_at = kNever;
    if (!pressed) return;
    long_pressed = right_clicked = pressed;
    if (InputState *state = input_of(pressed)) {
      set_focus(pressed, false);
      pointer_select(*state, 2, false);
    }
    pressed = 0;
    track = Track::Done;
    dirty = true;
  }

  void run_timers() {
    const double t = clock();
    if (t >= long_press_at) fire_long_press();
    if (press_show_at > 0 && t >= press_show_at) {
      press_show_at = 0;
      dirty = true;
    }
    if (fling_id && t >= fling_next) {
      step_fling(static_cast<float>(std::min(t - fling_last, 0.05)));
      fling_last = t;
      fling_next = t + (cpu ? kFlingStep : 0);
    }
    if (smooth_id && t >= smooth_next) {
      step_smooth(static_cast<float>(std::min(t - smooth_last, 0.05)));
      smooth_last = t;
      smooth_next = t + (cpu ? kFlingStep : 0);
    }
    if (ease_next > 0 && t >= ease_next) {
      ease_next = 0;
      dirty = true;
    }
  }

  bool note_tap(double time, uint64_t id) {
    const bool second = id == last_tap_id && time - last_tap_at <= kDoubleTap && std::hypot(pointer_x - last_tap_x, pointer_y - last_tap_y) <= kDoubleTapSlop;
    if (second) double_tapped = id;
    last_tap_id = second ? 0 : id;
    last_tap_at = time;
    last_tap_x = pointer_x;
    last_tap_y = pointer_y;
    return second;
  }

  void touch_tap(double time) {
    const uint64_t target = pressed && pressed == hovered ? pressed : 0;
    set_focus(target, false);
    if (!target) return;
    clicked = target;
    const bool second = note_tap(time, target);
    if (InputState *state = input_of(target)) {
      pointer_select(*state, second ? 2 : 1, false);
      selecting = false;
      if (sdl_ready && window) SDL_StartTextInput(window);
    }
  }

  float finger_spread() const { return fingers.size() < 2 ? 0 : std::hypot(fingers[0].x - fingers[1].x, fingers[0].y - fingers[1].y); }

  void finger_down(const SDL_TouchFingerEvent &finger, double time) {
    const float x = finger.x * logical_w, y = finger.y * logical_h;
    fingers.push_back({finger.fingerID, x, y});
    const bool stopped_fling = fling_id != 0;
    fling_id = smooth_id = 0;
    if (fingers.size() > 1) {
      if (fingers.size() > 2 || (track != Track::Pending && track != Track::Press)) return;
      const auto link = std::find_if(chain.begin(), chain.end(), [](const Link &l) { return l.gestures & GesturePinch; });
      if (link == chain.end()) return;
      claim_gesture(Track::Pinch, link->id);
      pinch_spread = finger_spread();
      pointer_x = (fingers[0].x + fingers[1].x) / 2;
      pointer_y = (fingers[0].y + fingers[1].y) / 2;
      return;
    }
    input_touch = true;
    pointer_x = x;
    pointer_y = y;
    pointer_down = pointer_tapped = true;
    begin_track(time, true);
    update_hover();
    pressed = stopped_fling ? 0 : hovered;
    dirty = true;
    if (pressed) long_press_at = time + kLongPress;
    const bool in_scroller = std::any_of(chain.begin(), chain.end(), [](const Link &l) { return l.scroll; });
    press_show_at = pressed && in_scroller ? time + kPressDelay : 0;
  }

  void finger_motion(const SDL_TouchFingerEvent &finger, double time) {
    const auto it = std::find_if(fingers.begin(), fingers.end(), [&](const Finger &f) { return f.id == finger.fingerID; });
    if (it == fingers.end()) return;
    it->x = finger.x * logical_w;
    it->y = finger.y * logical_h;
    if (track == Track::Pinch) {
      if (fingers.size() < 2) return;
      const float x = (fingers[0].x + fingers[1].x) / 2, y = (fingers[0].y + fingers[1].y) / 2, spread = finger_spread();
      drag_dx += x - pointer_x;
      drag_dy += y - pointer_y;
      pointer_x = x;
      pointer_y = y;
      if (pinch_spread > 0 && spread > 0) pinch_scale *= spread / pinch_spread;
      pinch_spread = spread;
      dirty = true;
      return;
    }
    if (it != fingers.begin()) return;
    const float dx = it->x - pointer_x, dy = it->y - pointer_y;
    pointer_x = it->x;
    pointer_y = it->y;
    update_hover();
    track_move(time, dx, dy);
  }

  void finger_up(const SDL_TouchFingerEvent &finger, double time, bool canceled) {
    const auto it = std::find_if(fingers.begin(), fingers.end(), [&](const Finger &f) { return f.id == finger.fingerID; });
    if (it == fingers.end()) return;
    const bool primary = it == fingers.begin();
    fingers.erase(it);
    if (track == Track::Pinch) {
      pinch_once = gesture_owner;
      track = Track::Done;
      gesture_owner = 0;
      dirty = true;
    }
    if (!primary) return;
    pointer_down = false;
    if (dragging) drag_end = pressed;
    else if (track == Track::Pending && !canceled) touch_tap(time);
    pressed = 0;
    dragging = false;
    finish_track(time, canceled);
    update_hover();
    dirty = true;
  }

  void handle(const SDL_Event &e) {
    const auto to_logical = [&](float x, float y, float &out_x, float &out_y) {
      const float density = window ? SDL_GetWindowPixelDensity(window) : 1;
      out_x = x * density / dpr;
      out_y = y * density / dpr;
    };
    switch (e.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        quit = true;
        break;
      case SDL_EVENT_WINDOW_HIT_TEST: {
        // Where the system reports presses on a drag area (X11), two of them
        // maximize like a title bar's double click.
        if (!hit_drag) break;
        const double t = now();
        if (t - drag_click_at < 0.5) {
          drag_click_at = 0;
          window_action(1);
        } else {
          drag_click_at = t;
        }
        break;
      }
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      case SDL_EVENT_WINDOW_EXPOSED:
      case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
      case SDL_EVENT_WINDOW_SHOWN:
      case SDL_EVENT_WINDOW_RESTORED:
      case SDL_EVENT_DID_ENTER_FOREGROUND:
        dirty = true;
        force_full = true;
#if defined(__ANDROID__)
        if (e.type == SDL_EVENT_WINDOW_RESTORED || e.type == SDL_EVENT_WINDOW_SHOWN) {
          settle_frames = 4;
          settle_next = now() + 0.1;
        }
#endif
        break;
      case SDL_EVENT_MOUSE_MOTION: {
        float x, y;
        to_logical(e.motion.x, e.motion.y, x, y);
        const float dx = x - pointer_x, dy = y - pointer_y;
        input_touch = false;
        pointer_x = x;
        pointer_y = y;
        if (scrollbar_dragged) {
          drag_scrollbar();
          break;
        }
        update_hover();
        if (selecting) {
          if (InputState *state = focused_input()) {
            state->caret = engine.input_byte_at(*state, x, y);
            input_touched(*state);
          }
        }
        if (text_selecting && pointer_down) {
          uint64_t root = 0;
          TextPoint at;
          if (engine.text_at(pointer_x, pointer_y, engine.selection.root, root, at) && !(at == engine.selection.focus)) {
            engine.selection.focus = at;
            dirty = true;
          }
        }
        track_move(event_time(e), dx, dy);
        break;
      }
      case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        pointer_x = pointer_y = -1;
        update_hover();
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        input_touch = false;
        to_logical(e.button.x, e.button.y, pointer_x, pointer_y);
        update_hover();
        if (e.button.button == SDL_BUTTON_LEFT) {
          fling_id = smooth_id = 0;
          pointer_down = true;
          pointer_tapped = true;
          if (const HitEntry *bar = scrollbar_at(pointer_x, pointer_y)) {
            scrollbar_dragged = bar->id;
            const bool on_thumb = pointer_y >= bar->thumb_y && pointer_y < bar->thumb_y + bar->thumb_h;
            scrollbar_grab = on_thumb ? pointer_y - bar->thumb_y : bar->thumb_h / 2;
            if (!on_thumb) drag_scrollbar();
            dirty = true;
            break;
          }
          begin_track(event_time(e), false);
          pressed = hovered;
          set_focus(hovered, false);
          dirty = true;
          if (InputState *state = input_of(hovered)) pointer_select(*state, e.button.clicks, SDL_GetModState() & SDL_KMOD_SHIFT);
          else start_text_selection(e.button.clicks);
        } else if (e.button.button == SDL_BUTTON_X1 || e.button.button == SDL_BUTTON_X2) {
          on_key(e.button.button == SDL_BUTTON_X1 ? KeyBack : KeyForward, 0);
        } else if (e.button.button == SDL_BUTTON_MIDDLE && sdl_ready) {
          if (InputState *state = input_of(hovered)) {
            set_focus(hovered, false);
            state->caret = state->anchor = engine.input_byte_at(*state, pointer_x, pointer_y);
            char *text = SDL_GetPrimarySelectionText();
            if (text) insert_text(*state, text, EditKind::Other);
            SDL_free(text);
          }
        }
        break;
      }
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        to_logical(e.button.x, e.button.y, pointer_x, pointer_y);
        update_hover();
        if (e.button.button == SDL_BUTTON_RIGHT && hovered) right_clicked = hovered;
        if (e.button.button == SDL_BUTTON_LEFT) {
          pointer_down = false;
          scrollbar_dragged = 0;
          if (dragging) {
            drag_end = pressed;
          } else if (pressed && pressed == hovered) {
            clicked = pressed;
            note_tap(event_time(e), pressed);
          }
          pressed = 0;
          dragging = false;
          finish_track(event_time(e), false);
          if (text_selecting) {
            text_selecting = false;
            if (engine.selection.empty()) engine.selection = {};
            else if (sdl_ready) SDL_SetPrimarySelectionText(engine.selected_text().c_str());
            dirty = true;
          }
          if (selecting && sdl_ready) {
            InputState *state = focused_input();
            if (state && state->has_selection() && !state->password) SDL_SetPrimarySelectionText(std::string(state->selected()).c_str());
          }
          selecting = false;
        }
        dirty = true;
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL:
        wheel(e.wheel.y);
        break;
      case SDL_EVENT_FINGER_DOWN:
        finger_down(e.tfinger, event_time(e));
        break;
      case SDL_EVENT_FINGER_MOTION:
        finger_motion(e.tfinger, event_time(e));
        break;
      case SDL_EVENT_FINGER_UP:
      case SDL_EVENT_FINGER_CANCELED:
        finger_up(e.tfinger, event_time(e), e.type == SDL_EVENT_FINGER_CANCELED);
        break;
      case SDL_EVENT_USER:
        dirty = true;
        break;
      case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_AC_BACK) back_pending = true;
        on_key(key_from_sdl(e.key.key), mods_from_sdl(e.key.mod));
        break;
      case SDL_EVENT_KEY_UP:
        keys.push_back(key_from_sdl(e.key.key));
        keys.push_back(mods_from_sdl(e.key.mod) | kKeyReleased);
        dirty = true;
        break;
      case SDL_EVENT_DROP_BEGIN:
        files_hovering = true;
        dirty = true;
        break;
      case SDL_EVENT_DROP_POSITION:
        to_logical(e.drop.x, e.drop.y, pointer_x, pointer_y);
        dirty = true;
        break;
      case SDL_EVENT_DROP_COMPLETE:
        files_hovering = false;
        dirty = true;
        break;
      case SDL_EVENT_DROP_FILE:
        if (e.drop.data) {
          std::lock_guard lock(picked_lock);
          picked.emplace_back(e.drop.data);
        }
        dirty = true;
        break;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        window_focused = true;
        dirty = true;
        break;
      case SDL_EVENT_TEXT_INPUT:
        if (InputState *state = focused_input()) {
          state->preedit.clear();
          insert_text(*state, e.text.text, EditKind::Type);
        }
        break;
      case SDL_EVENT_TEXT_EDITING:
        set_preedit(e.edit.text ? e.edit.text : "", e.edit.start > 0 ? static_cast<size_t>(e.edit.start) : 0);
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        window_focused = false;
        dirty = true;
        break;
      default:
        break;
    }
  }

  int total_frames() const { return std::max(headless_frames(), 6 + static_cast<int>(drive.size()) * 2); }

  void drive_step() {
    if (drive_at >= drive.size() || frames < 2 || frames % 2 != 0) return;
    const std::string &step = drive[drive_at++];
    if (step == "wait") return;
    float x = 0, y = 0, amount = 0, x2 = 0, y2 = 0;
    if (step.rfind("type:", 0) == 0) {
      SDL_Event e{};
      e.type = SDL_EVENT_TEXT_INPUT;
      e.text.text = step.c_str() + 5;
      handle(e);
      return;
    }
    if (step.rfind("ime:", 0) == 0) {
      set_preedit(step.c_str() + 4, step.size() - 4);
      return;
    }
    if (int used = 0; std::sscanf(step.c_str(), "dragover:%f,%f%n", &x, &y, &used) == 2 || std::sscanf(step.c_str(), "drop:%f,%f,%n", &x, &y, &used) == 2) {
      const auto drop = [&](SDL_EventType type, const char *data = nullptr) {
        SDL_Event e{};
        e.type = type;
        e.drop.x = x * dpr;
        e.drop.y = y * dpr;
        e.drop.data = data;
        handle(e);
      };
      drop(SDL_EVENT_DROP_BEGIN);
      drop(SDL_EVENT_DROP_POSITION);
      if (step.rfind("drop:", 0) == 0) {
        drop(SDL_EVENT_DROP_FILE, step.c_str() + used);
        drop(SDL_EVENT_DROP_COMPLETE);
      }
      return;
    }
    if (step.rfind("key:", 0) == 0) {
      std::string_view rest = std::string_view(step).substr(4);
      uint32_t mods = 0;
      for (size_t plus = rest.find('+'); plus != std::string_view::npos && rest.size() > 1; plus = rest.find('+')) {
        const std::string_view modifier = rest.substr(0, plus);
        if (modifier == "shift") mods |= ModShift;
        else if (modifier == "ctrl") mods |= ModCtrl;
        else if (modifier == "alt") mods |= ModAlt;
        else if (modifier == "meta") mods |= ModMeta;
        rest.remove_prefix(plus + 1);
      }
      on_key(key_from_name(rest), mods);
      return;
    }
    const auto motion = [&] {
      SDL_Event e{};
      e.type = SDL_EVENT_MOUSE_MOTION;
      e.motion.x = x * dpr;
      e.motion.y = y * dpr;
      handle(e);
    };
    const auto button = [&](Uint32 type, Uint8 which, Uint8 clicks = 1) {
      SDL_Event e{};
      e.type = type;
      e.button.button = which;
      e.button.clicks = clicks;
      e.button.x = x * dpr;
      e.button.y = y * dpr;
      handle(e);
    };
    const auto finger = [&](Uint32 type, float fx, float fy, SDL_FingerID id = 1) {
      SDL_Event e{};
      e.type = type;
      e.tfinger.fingerID = id;
      e.tfinger.x = fx / logical_w;
      e.tfinger.y = fy / logical_h;
      handle(e);
    };
    float ms = 0;
    if (std::sscanf(step.c_str(), "touch:%f,%f", &x, &y) == 2) {
      finger(SDL_EVENT_FINGER_DOWN, x, y);
    } else if (std::sscanf(step.c_str(), "touchmove:%f,%f", &x, &y) == 2) {
      finger(SDL_EVENT_FINGER_MOTION, x, y);
    } else if (std::sscanf(step.c_str(), "touchup:%f,%f", &x, &y) == 2) {
      finger(SDL_EVENT_FINGER_UP, x, y);
    } else if (std::sscanf(step.c_str(), "tap:%f,%f", &x, &y) == 2) {
      finger(SDL_EVENT_FINGER_DOWN, x, y);
      finger(SDL_EVENT_FINGER_UP, x, y);
    } else if (std::sscanf(step.c_str(), "longpress:%f,%f", &x, &y) == 2) {
      finger(SDL_EVENT_FINGER_DOWN, x, y);
      drive_clock += kLongPress + 0.01;
      run_timers();
      finger(SDL_EVENT_FINGER_UP, x, y);
    } else if (std::sscanf(step.c_str(), "swipe:%f,%f,%f,%f,%f", &x, &y, &x2, &y2, &ms) == 5) {
      constexpr int kSteps = 12;
      finger(SDL_EVENT_FINGER_DOWN, x, y);
      for (int i = 1; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / kSteps;
        drive_clock += ms / 1000.0 / kSteps;
        finger(SDL_EVENT_FINGER_MOTION, x + (x2 - x) * t, y + (y2 - y) * t);
      }
      finger(SDL_EVENT_FINGER_UP, x2, y2);
    } else if (std::sscanf(step.c_str(), "pinch:%f,%f,%f,%f", &x, &y, &x2, &y2) == 4) {
      constexpr int kSteps = 8;
      finger(SDL_EVENT_FINGER_DOWN, x - x2 / 2, y, 1);
      finger(SDL_EVENT_FINGER_DOWN, x + x2 / 2, y, 2);
      for (int i = 1; i <= kSteps; ++i) {
        const float half = (x2 + (y2 - x2) * static_cast<float>(i) / kSteps) / 2;
        finger(SDL_EVENT_FINGER_MOTION, x - half, y, 1);
        finger(SDL_EVENT_FINGER_MOTION, x + half, y, 2);
      }
      finger(SDL_EVENT_FINGER_UP, x - y2 / 2, y, 2);
      finger(SDL_EVENT_FINGER_UP, x + y2 / 2, y, 1);
    } else if (std::sscanf(step.c_str(), "drag:%f,%f,%f,%f", &x, &y, &x2, &y2) == 4) {
      motion();
      button(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
      x = x2;
      y = y2;
      motion();
      button(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
    } else if (std::sscanf(step.c_str(), "dblclick:%f,%f", &x, &y) == 2) {
      motion();
      button(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, 2);
      button(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, 2);
    } else if (std::sscanf(step.c_str(), "click:%f,%f", &x, &y) == 2) {
      motion();
      button(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
      button(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
    } else if (std::sscanf(step.c_str(), "rclick:%f,%f", &x, &y) == 2) {
      motion();
      button(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT);
      button(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_RIGHT);
    } else if (std::sscanf(step.c_str(), "down:%f,%f", &x, &y) == 2) {
      motion();
      button(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
    } else if (std::sscanf(step.c_str(), "up:%f,%f", &x, &y) == 2) {
      motion();
      button(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
    } else if (std::sscanf(step.c_str(), "move:%f,%f", &x, &y) == 2) {
      motion();
    } else if (std::sscanf(step.c_str(), "wheel:%f,%f,%f", &x, &y, &amount) == 3) {
      motion();
      SDL_Event e{};
      e.type = SDL_EVENT_MOUSE_WHEEL;
      e.wheel.y = amount;
      handle(e);
    } else {
      std::fprintf(stderr, "qui: ignoring QUI_DRIVE step '%s'\n", step.c_str());
    }
  }

  bool animating() const { return !cpu && !engine.previous.shaders.empty(); }

  double tick_interval() const {
    double interval = 0;
    for (const ShaderDraw &draw : engine.previous.shaders)
      if (draw.fps > 0) interval = interval == 0 ? 1.0 / draw.fps : std::min(interval, 1.0 / draw.fps);
    return interval;
  }

  void present(IRect damage) {
    if (!window) return;
    if (gl) {
      SDL_GL_SwapWindow(window);
      return;
    }
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (!surface) return;
    damage = intersect(damage, {0, 0, std::min(surface->w, pixel_w), std::min(surface->h, pixel_h)});
    if (damage.empty()) return;
    SDL_ConvertPixels(damage.w, damage.h, SDL_PIXELFORMAT_RGBA32,
                      cpu->pixels() + static_cast<size_t>(damage.y) * pixel_w + damage.x, pixel_w * 4,
                      surface->format,
                      static_cast<uint8_t *>(surface->pixels) + static_cast<size_t>(damage.y) * surface->pitch + static_cast<size_t>(damage.x) * 4, surface->pitch);
    const SDL_Rect rect{damage.x, damage.y, damage.w, damage.h};
    SDL_UpdateWindowSurfaceRects(window, &rect, 1);
  }

  // Before an idle wait, hands freed heap back to the system, at most every
  // five seconds.
  void release_idle_memory() {
#if defined(__GLIBC__)
    if (now() - trimmed_at < 5) return;
    trimmed_at = now();
    malloc_trim(0);
#endif
  }

  bool wait() {
    for (;;) {
      SDL_Event event;
      while (SDL_PollEvent(&event)) handle(event);
      if (quit) return false;
      run_timers();
      if (dirty) {
        dirty = false;
        return true;
      }
      const double interval = animating() ? tick_interval() : 0;
      const bool blinking = focused_input() != nullptr;
      double deadline = long_press_at;
      if (interval > 0) deadline = std::min(deadline, next_tick);
      if (blinking) deadline = std::min(deadline, blink_at);
      if (press_show_at > 0) deadline = std::min(deadline, press_show_at);
      if (fling_id) deadline = std::min(deadline, fling_next);
      if (smooth_id) deadline = std::min(deadline, smooth_next);
      if (ease_next > 0) deadline = std::min(deadline, ease_next);
      if (settle_frames > 0) deadline = std::min(deadline, settle_next);
      bool got = false;
      if (deadline - now() > 0.25) release_idle_memory();
      if (std::isfinite(deadline)) {
        got = SDL_WaitEventTimeout(&event, static_cast<Sint32>(std::ceil(std::max(0.0, (deadline - now()) * 1000.0))));
      } else {
        got = SDL_WaitEvent(&event);
      }
      if (got) handle(event);
      if (settle_frames > 0 && now() >= settle_next) {
        --settle_frames;
        settle_next = now() + 0.12;
        dirty = force_full = true;
      }
      if (blinking && now() >= blink_at) {
        caret_on = !caret_on;
        blink_at = now() + kCaretBlink;
        dirty = true;
      }
      if (interval > 0 && now() >= next_tick) {
        backend->draw(engine.previous, {0, 0, pixel_w, pixel_h}, now());
        present({0, 0, pixel_w, pixel_h});
        next_tick = now() + interval;
      }
    }
  }

  void put_slot(std::vector<uint8_t> &table, uint32_t capacity, uint64_t id, uint32_t flags, uint32_t first, uint32_t count, const float rect[4], bool has_rect) {
    for (uint32_t i = static_cast<uint32_t>(id) & (capacity - 1);; i = (i + 1) & (capacity - 1)) {
      uint8_t *slot = table.data() + kResultsHeader + i * kResultsSlot;
      uint64_t key = 0;
      std::memcpy(&key, slot, 8);
      if (key == 0 || key == id) {
        std::memcpy(slot, &id, 8);
        uint32_t existing = 0;
        std::memcpy(&existing, slot + 8, 4);
        flags |= existing;
        std::memcpy(slot + 8, &flags, 4);
        if (count) {
          std::memcpy(slot + 12, &first, 4);
          std::memcpy(slot + 16, &count, 4);
        }
        if (has_rect) std::memcpy(slot + 24, rect, 16);
        const bool panned = id == active_pan() || id == pan_end;
        if ((id == pressed && dragging) || id == drag_end || panned || id == active_pinch()) {
          std::memcpy(slot + 40, &drag_dx, 4);
          std::memcpy(slot + 44, &drag_dy, 4);
        }
        if (panned) {
          std::memcpy(slot + 48, &vel_x, 4);
          std::memcpy(slot + 52, &vel_y, 4);
        }
        if (id == active_pinch()) std::memcpy(slot + 20, &pinch_scale, 4);
        return;
      }
    }
  }

  void fill_results() {
    constexpr uint32_t kMagic = 0x31495551u;
    size_t needed = (engine.hits.size() + engine.lists.size() + engine.previous.shaders.size()) * 2;
    uint32_t capacity = 16;
    while (capacity < needed) capacity *= 2;

    text_blob.clear();
    bool edits = false;
    for (auto &[id, state] : engine.inputs) {
      if (state.changed) edits = true;
      if (state.submitted) edits = true;
    }

    const size_t keys_at = kResultsHeader + static_cast<size_t>(capacity) * kResultsSlot;
    const size_t text_at = keys_at + keys.size() * 4;
    for (const auto &[id, state] : engine.inputs)
      if (state.changed) text_blob += state.text;
    for (const ShaderDraw &draw : engine.previous.shaders) text_blob += backend->shader_error(draw.hash);
    results.assign(text_at + text_blob.size(), 0);
    uint8_t *p = results.data();
    const float header[] = {logical_w - inset_left - inset_right, logical_h - inset_top - inset_bottom, dpr, dt, pointer_x, pointer_y};
    const uint32_t offsets[] = {static_cast<uint32_t>(keys_at), static_cast<uint32_t>(keys.size() / 2), static_cast<uint32_t>(text_at), pointer_down || pointer_tapped ? 1u : 0u};
    pointer_tapped = false;
    std::memcpy(p, &kMagic, 4);
    std::memcpy(p + 4, &capacity, 4);
    std::memcpy(p + 8, header, sizeof(header));
    std::memcpy(p + 32, offsets, sizeof(offsets));
    std::memcpy(p + 48, &engine_ms, 4);
    const uint32_t focused_flag = window_focused ? 1u : 0u;
    std::memcpy(p + 52, &focused_flag, 4);
    const uint32_t touch_flag = input_touch ? 1u : 0u;
    std::memcpy(p + 56, &touch_flag, 4);
    if (!keys.empty()) std::memcpy(p + keys_at, keys.data(), keys.size() * 4);
    if (!text_blob.empty()) std::memcpy(p + text_at, text_blob.data(), text_blob.size());

    for (const HitEntry &hit : engine.hits) {
      if (hit.scrollbar) continue;
      uint32_t flags = 0;
      if (hit.id == shown_hovered()) flags |= wire::FlagHovered;
      if (hit.id == shown_pressed()) flags |= wire::FlagPressed;
      if (hit.id == clicked) flags |= wire::FlagClicked;
      if (hit.id == right_clicked) flags |= wire::FlagRightClicked;
      if (hit.id == focused) flags |= wire::FlagFocused;
      if (hit.id == drag_start) flags |= wire::FlagDragStart;
      if (hit.id == pressed && dragging) flags |= wire::FlagDragging;
      if (hit.id == drag_end) flags |= wire::FlagDragEnd;
      if (hit.id == long_pressed) flags |= wire::FlagLongPressed;
      if (hit.id == double_tapped) flags |= wire::FlagDoubleTapped;
      if (hit.id == pan_start) flags |= wire::FlagPanStart;
      if (hit.id == active_pan()) flags |= wire::FlagPanning;
      if (hit.id == pan_end) flags |= wire::FlagPanEnd;
      if (hit.id == active_pinch()) flags |= wire::FlagPinching;
      const float rect[] = {hit.x, hit.y, hit.w, hit.h};
      put_slot(results, capacity, hit.id, flags, 0, 0, rect, true);
    }
    for (auto &[id, state] : engine.lists) {
      if (state.frame != engine.frame_number) continue;
      state.compute_range();
      put_slot(results, capacity, id, 0, state.first, std::max(1u, state.visible), nullptr, false);
    }
    uint32_t blob_at = 0;
    for (auto &[id, state] : engine.inputs) {
      if (!state.changed && !state.submitted) continue;
      uint32_t flags = 0;
      if (state.changed) flags |= wire::FlagChanged;
      if (state.submitted) flags |= wire::FlagSubmitted;
      const uint32_t length = state.changed ? static_cast<uint32_t>(state.text.size()) : 0;
      put_slot(results, capacity, id, flags, blob_at, length, nullptr, false);
      if (state.changed) {
        blob_at += length;
        state.seen = state.text;
      }
      state.changed = state.submitted = false;
    }
    for (const ShaderDraw &draw : engine.previous.shaders) {
      const std::string_view error = backend->shader_error(draw.hash);
      if (error.empty()) continue;
      put_slot(results, capacity, draw.id, wire::FlagShaderError, blob_at, static_cast<uint32_t>(error.size()), nullptr, false);
      blob_at += static_cast<uint32_t>(error.size());
    }
    if (clicked || right_clicked || drag_start || drag_end || long_pressed || double_tapped || pan_start || pan_end || pinch_once || edits || !keys.empty()) dirty = true;
    clicked = right_clicked = drag_start = drag_end = long_pressed = double_tapped = pan_start = pan_end = pinch_once = 0;
    drag_dx = drag_dy = 0;
    pinch_scale = 1;
    keys.clear();
  }

  void finish() {
    if (!shot_path.empty()) {
      backend->read_pixels(capture);
      if (write_png(shot_path, capture.data(), pixel_w, pixel_h))
        std::fprintf(stderr, "qui: wrote %s (%dx%d, %zu nodes, %zu draw instances)\n", shot_path.c_str(), pixel_w, pixel_h, engine.nodes.size() - 1, engine.previous.instances.size());
    }
    if (!tree_path.empty()) std::ofstream(tree_path) << engine.tree_json();
    if (!wire_path.empty()) std::ofstream(wire_path, std::ios::binary).write(reinterpret_cast<const char *>(last_wire.data()), static_cast<std::streamsize>(last_wire.size()));
  }

  void end_frame(const uint8_t *wire, size_t len) {
    const double began = now();
    if (!wire_path.empty()) last_wire.assign(wire, wire + len);
    if (window && !headless) read_size();
    engine.decode(wire, len);
    const double t_decode = now();
    engine.insets[0] = inset_top;
    engine.insets[1] = inset_right;
    engine.insets[2] = inset_bottom;
    engine.insets[3] = inset_left;
    engine.time = clock();
    engine.layout(logical_w, logical_h, dpr);
    const double t_layout = now();
    if (engine.menu_open != menu_open) {
      menu_open = engine.menu_open;
      if (menu_open) menu_return = focused;
      else if (const uint64_t back = std::exchange(menu_return, 0); !engine.focus_request) engine.focus_request = back;
    }
    if (engine.focus_request) {
      set_focus(engine.focus_request, false);
      engine.focus_request = 0;
    }
    engine.ui = {shown_hovered(), shown_pressed(), focused, focus_ring, headless || caret_on, scrollbar_dragged ? scrollbar_dragged : scrollbar_hovered};
    engine.paint();
    const double t_paint = now();
    const double raster_ms = engine.fonts.take_raster_ms();
    ease_next = engine.easing ? clock() + (cpu ? kFlingStep : 0.004) : 0;
    if (engine.selection.empty()) engine.selection_cache.clear();
    else engine.selection_cache = engine.selected_text();
    if (focused && std::none_of(engine.hits.begin(), engine.hits.end(), [&](const HitEntry &e) { return e.id == focused; })) set_focus(0, false);
    // Stopping waits a frame, so a tap that the app answers by focusing an input
    // keeps the on-screen keyboard. Android's check for a shown keyboard lags a
    // stop, so a stop and start within one frame would leave it hidden.
    if (std::exchange(stop_text_pending, false) && !focused_input() && sdl_ready && window) SDL_StopTextInput(window);
    update_ime_area();
    size_t upload_bytes = 0;
    const size_t upload_count = engine.fonts.pending_uploads().size();
    for (const AtlasUpload &upload : engine.fonts.pending_uploads()) {
      upload_bytes += upload.pixels.size();
      backend->upload(upload);
    }
    engine.fonts.clear_pending_uploads();
    const double t_upload = now();

    backend->resize(pixel_w, pixel_h);
    IRect damage = engine.swap_and_damage();
    const IRect full{0, 0, pixel_w, pixel_h};
    if (force_full) {
      damage = full;
      force_full = false;
    }
    if (!damage.empty()) backend->draw(engine.previous, damage, headless ? 1.0 : now());
    if (backend->take_compiled()) dirty = true;
    const double t_draw = now();
    engine_ms = static_cast<float>((t_draw - began) * 1000);
    if (!damage.empty() && !headless) present(gl ? full : damage);
    static const double slow_s = [] {
      const char *v = std::getenv("QUI_SLOW_MS");
      return v ? std::atof(v) / 1000.0 : 0.0083;
    }();
    {
      struct Window {
        double since = 0, decode = 0, layout = 0, paint = 0, upload = 0, draw = 0, present = 0;
        size_t frames = 0, uploads = 0, upload_kib = 0, instances = 0;
        uint64_t async0 = 0, sync0 = 0;
      };
      static Window win;
      const double t_now = now();
      if (win.since == 0) win.since = t_now;
      ++win.frames;
      win.decode += t_decode - began;
      win.layout += t_layout - t_decode;
      win.paint += t_paint - t_layout;
      win.upload += t_upload - t_paint;
      win.draw += t_draw - t_upload;
      win.present += t_now - t_draw;
      win.uploads += upload_count;
      win.upload_kib += upload_bytes / 1024;
      win.instances += engine.previous.instances.size();
      if (t_now - win.since >= 2.0) {
        if (!headless && win.frames >= 10) {
          const double n = static_cast<double>(win.frames);
          std::fprintf(stderr, "qui: %zu frames in %.1f s (%.0f fps), avg ms: decode %.1f layout %.1f paint %.1f upload %.1f draw %.1f present %.1f; per frame %.1f uploads, %.0f KiB, %.0f instances; image scaling +%llu async +%llu inline\n", win.frames, t_now - win.since, n / (t_now - win.since), win.decode * 1000 / n, win.layout * 1000 / n, win.paint * 1000 / n, win.upload * 1000 / n, win.draw * 1000 / n, win.present * 1000 / n, static_cast<double>(win.uploads) / n, static_cast<double>(win.upload_kib) / n, static_cast<double>(win.instances) / n, static_cast<unsigned long long>(engine.stat_async_resamples - win.async0), static_cast<unsigned long long>(engine.stat_sync_resamples - win.sync0));
        }
        win = Window{};
        win.since = t_now;
        win.async0 = engine.stat_async_resamples;
        win.sync0 = engine.stat_sync_resamples;
      }
    }
    if (const double t_end = now(); (!headless || slow_s < 0.0083) && t_end - began > slow_s)
      std::fprintf(stderr, "qui: slow frame %.0f ms: decode %.0f layout %.0f paint %.0f upload %.0f (%zu, %zu KiB) draw %.0f present %.0f; glyph raster %.0f ms, image scale %.0f ms; wire %zu KiB, %zu instances\n", (t_end - began) * 1000, (t_decode - began) * 1000, (t_layout - t_decode) * 1000, (t_paint - t_layout) * 1000, (t_upload - t_paint) * 1000, upload_count, upload_bytes / 1024, (t_draw - t_upload) * 1000, (t_end - t_draw) * 1000, raster_ms, engine.sync_resample_ms, len / 1024, engine.previous.instances.size());
#if defined(__ANDROID__)
    if (const uint32_t c = engine.previous.clear; !headless) {
      const float luma = 0.2126f * static_cast<float>((c >> 24) & 0xff) + 0.7152f * static_cast<float>((c >> 16) & 0xff) + 0.0722f * static_cast<float>((c >> 8) & 0xff);
      if (const int light = luma > 128 ? 1 : 0; light != light_bars) {
        light_bars = light;
        SDL_SendAndroidMessage(kLightBarsCommand, light);
      }
    }
#endif
    if (animating()) next_tick = now() + tick_interval();
    if (engine.wants_another_frame) dirty = true;
    if (back_pending) {
#if defined(__ANDROID__)
      if (!back_consumed) move_task_to_back();
#endif
      back_pending = back_consumed = false;
    }
    if (!headless && !scrollbar_dragged) update_hover();
    const double t = now();
    dt = headless ? 1.f / 60.f : static_cast<float>(t - last_frame_time);
    last_frame_time = t;
    ++frames;
  }

  void update_ime_area() {
    InputState *state = focused_input();
    if (!state || !sdl_ready || !window) return;
    float x = 0, y = 0, h = 0;
    engine.input_caret_rect(*state, x, y, h);
    const float density = SDL_GetWindowPixelDensity(window);
    const float scale = density > 0 ? dpr / density : 1;
    const SDL_Rect area{static_cast<int>(x * scale), static_cast<int>(y * scale), 1, static_cast<int>(std::ceil(h * scale))};
    if (area.x == last_ime_area.x && area.y == last_ime_area.y && area.h == last_ime_area.h) return;
    last_ime_area = area;
    SDL_SetTextInputArea(window, &area, 0);
  }

  void close() {
    if (!paste_dir.empty()) {
      std::error_code error;
      std::filesystem::remove_all(paste_dir, error);
    }
    if (cursor_text) SDL_DestroyCursor(cursor_text);
    backend.reset();
    if (gl) SDL_GL_DestroyContext(gl);
    if (window) SDL_DestroyWindow(window);
    if (sdl_ready) SDL_Quit();
  }
};

extern "C" {

void qui_set_app_id(const char *id) { g_app_id = id ? id : ""; }

void qui_set_borderless(int on) { g_borderless = on != 0; }

void qui_window_action(qui_app *app, int action) {
  if (app) app->window_action(action);
}

int qui_window_maximized(qui_app *app) { return app && app->window && (SDL_GetWindowFlags(app->window) & SDL_WINDOW_MAXIMIZED) ? 1 : 0; }

void qui_consume_back(qui_app *app) { app->back_consumed = true; }

void qui_set_clipboard(qui_app *app, const char *text) { app->set_clipboard(text ? text : ""); }

int qui_files_hovering(qui_app *app) { return app && app->files_hovering ? 1 : 0; }

const char *qui_selected_text(qui_app *app) { return app ? app->engine.selection_cache.c_str() : ""; }

void qui_set_smooth_scroll(qui_app *app, int on) {
  if (!app) return;
  app->smooth_wheel = on != 0;
  if (!on) app->smooth_id = 0;
}

namespace {
void picked_files(void *userdata, const char *const *files, int) {
  auto *app = static_cast<qui_app *>(userdata);
  if (files) {
    std::lock_guard lock(app->picked_lock);
    for (; *files; ++files) app->picked.emplace_back(*files);
  }
  qui_wake(app);
}
}  // namespace

void qui_pick_files(qui_app *app, int multiple) {
  if (!app || app->headless || !app->window) return;
  SDL_ShowOpenFileDialog(picked_files, app, app->window, nullptr, 0, nullptr, multiple != 0);
}

const char *qui_picked_file(qui_app *app) {
  if (!app) return nullptr;
  std::lock_guard lock(app->picked_lock);
  if (app->picked.empty()) return nullptr;
  app->picked_out = std::move(app->picked.front());
  app->picked.pop_front();
  return app->picked_out.c_str();
}

void qui_wake(qui_app *app) {
  if (!app || app->headless) return;
  SDL_Event event{};
  event.type = SDL_EVENT_USER;
  SDL_PushEvent(&event);
}

namespace {
Uint32 delayed_wake(void *, SDL_TimerID, Uint32) {
  SDL_Event event{};
  event.type = SDL_EVENT_USER;
  SDL_PushEvent(&event);
  return 0;
}
}  // namespace

void qui_wake_after(qui_app *app, uint32_t ms) {
  if (!app || app->headless) return;
  SDL_AddTimer(ms, delayed_wake, nullptr);
}

qui_app *qui_open(const char *title, int width, int height) {
#if defined(__ANDROID__)
  capture_stdio();
#endif
  auto *app = new qui_app();
  app->open(title, width, height);
  return app;
}

void qui_close(qui_app *app) {
  app->close();
  delete app;
}

const uint8_t *qui_frame_begin(qui_app *app, size_t *len) {
  if (app->headless) {
    if (app->frames >= app->total_frames() && !app->fling_id && !app->smooth_id) {
      app->finish();
      return nullptr;
    }
    app->drive_clock += kFlingStep;
    app->drive_step();
    app->run_timers();
  } else if (!app->wait()) {
    return nullptr;
  }
  app->fill_results();
  *len = app->results.size();
  return app->results.data();
}

void qui_frame_end(qui_app *app, const uint8_t *wire, size_t len) { app->end_frame(wire, len); }

void qui_stage_image(qui_app *app, uint64_t token, uint32_t w, uint32_t h, const uint8_t *rgba, size_t len, void *ctx, void (*release)(void *)) {
  std::shared_ptr<const void> owner(ctx, release);
  if (!app || !rgba || len < static_cast<size_t>(w) * h * 4) return;
  app->engine.stage_image(token, w, h, {rgba, static_cast<size_t>(w) * h * 4}, std::move(owner));
}

}  // extern "C"
