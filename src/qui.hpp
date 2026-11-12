#pragma once

#include "qui_c.h"
#include "wire.gen.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qui {

using Color = uint32_t;

constexpr Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | a;
}

struct Size {
  uint8_t mode = wire::SizeFit;
  float value = 0;
};
constexpr Size px(float v) { return {wire::SizePx, v}; }
constexpr Size fill(float weight = 1) { return {wire::SizeFill, weight}; }
constexpr Size pct(float percent) { return {wire::SizePct, percent}; }

struct Edges {
  float top = 0, right = 0, bottom = 0, left = 0;
  constexpr Edges() = default;
  constexpr Edges(float all) : top(all), right(all), bottom(all), left(all) {}
  constexpr Edges(float vertical, float horizontal) : top(vertical), right(horizontal), bottom(vertical), left(horizontal) {}
  constexpr Edges(float t, float r, float b, float l) : top(t), right(r), bottom(b), left(l) {}
};

struct Gradient {
  Color from = 0, to = 0;
  float angle = 0;
};

struct BoxShadow {
  Color color = 0;
  float blur = 0, y = 0;
};

// Style and TextStyle, with their encoders and class merges, come from
// tools/gen/spec.txt.
#include "style.gen.inc"

inline constexpr int32_t kOverlayZ = 1 << 20;
inline constexpr float kIconSize = 20;
inline constexpr float kModalMargin = 16;
inline constexpr float kSheetSlide = 48, kSheetTop = 48;
// Below this width a menu opened by touch shows as a sheet.
inline constexpr float kSheetBelow = 600;

inline constexpr TextStyle Body{};
inline constexpr TextStyle H1{.size = 24, .bold = true, .color = Strong, .role = RoleHeading};
inline constexpr TextStyle H2{.size = 18, .bold = true, .color = Strong, .role = RoleHeading};
inline constexpr TextStyle Small{.size = 12, .color = Muted};
inline constexpr TextStyle Code{.size = 13, .mono = true};

enum class Swipe : uint8_t { None, Left, Right, Up, Down };

struct Response {
  bool found = false, hovered = false, pressed = false, clicked = false, right_clicked = false;
  bool focused = false, drag_start = false, dragging = false, drag_end = false;
  bool changed = false, submitted = false;
  bool long_pressed = false, double_tapped = false, pan_start = false, panning = false, pan_end = false, pinching = false;
  float x = 0, y = 0, w = 0, h = 0, dx = 0, dy = 0;
  float vx = 0, vy = 0, scale = 1;
  Swipe swipe = Swipe::None;
  uint32_t first = 0, count = 0;
  std::string_view error{};
};

inline constexpr uint8_t FitCover = wire::FitCover, FitContain = wire::FitContain, FitStretch = wire::FitStretch;
inline constexpr uint8_t SpanImage = wire::SpanImage;
inline constexpr uint8_t SpanBold = wire::SpanBold, SpanMono = wire::SpanMono, SpanItalic = wire::SpanItalic, SpanUnderline = wire::SpanUnderline, SpanStrike = wire::SpanStrike, SpanHoverUnderline = wire::SpanHoverUnderline;

struct Span {
  uint32_t start = 0, end = 0;
  uint8_t flags = 0;
  Color fg = 0, bg = 0;
  std::string_view link{};
  // Key of a loaded image drawn in place of the span, whose bytes must be
  // kImagePlaceholder.
  std::string_view image{};
};

inline constexpr std::string_view kImagePlaceholder = "\xe2\x80\x83\xe2\x80\x82";

// An SVG document drawn as a one-color icon. Declare it once (it can be
// constexpr) and keep it: the engine receives its text the first time `icon`
// uses it, and later frames send only its hash. Paths and the basic shapes are
// drawn with their fills, strokes and transforms; colors are ignored, so
// anything the document paints takes the icon's color.
struct Svg {
  std::string_view text{};
  uint64_t id = 0;

  constexpr Svg() = default;
  constexpr explicit Svg(std::string_view svg) : text(svg), id(hash(svg)) {}

  static constexpr uint64_t hash(std::string_view s) {
    uint64_t h = 0;
    for (const char c : s) {
      h ^= static_cast<unsigned char>(c);
      h *= 1099511628211ull;
    }
    return h ? h : 1;
  }
};

struct RichResult {
  std::string_view hovered{}, clicked{}, right_clicked{};
};

struct Stats {
  size_t nodes = 0, wire_bytes = 0;
  float engine_ms = 0;
};

struct ListOptions {
  Style style{};
  bool bottom = false;
  // Items inserted at the front this frame; the rows on screen keep their place.
  // Native only: the browser ignores it.
  uint32_t prepend = 0;
  // Scrolls this item to the top of the list on this frame only. Native only.
  std::optional<uint32_t> jump_to{};
  // Makes jump_to ease the item to the middle of the list instead.
  bool glide = false;
};

struct Uniform {
  std::string_view name;
  std::initializer_list<float> values;
};

struct ShaderOptions {
  std::string_view src;
  Style style{};
  float fps = 0;
  std::initializer_list<Uniform> uniforms{};
};

struct InputOptions {
  Style style{};
  std::string_view placeholder{};
  float size = 0;
  bool mono = false, multiline = false, enter_submits = false, password = false, autofocus = false, focus = false, passthrough_nav = false;
  // Drops the field's own background and border, focus accent included, for a
  // field drawn inside a box of your own.
  bool bare = false;
};

enum class Side : uint8_t { Left, Right, Top, Bottom };

// State of a panel that slides over other content. Keep one per drawer and set
// open to show or hide it from code.
struct Drawer {
  bool open = false;
  float position = 0, speed = 0;
  bool held = false, started = false;
};

struct MenuItem {
  std::string_view label{};
  // When set, replaces label after the first press; the item is chosen only by
  // the second.
  std::string_view confirm{};
  bool danger = false;
};

// State of a context menu. Keep one per menu and open it with show.
struct Menu {
  bool open = false;
  float x = 0, y = 0;
  // Row positions are index+1, so 0 means none.
  int active = 0, hovered = 0, confirm = 0;
  // Latched on the first frame: 1 for a popup, 2 for a sheet.
  uint8_t mode = 0;
  float header_h = 0;

  void show(float at_x, float at_y) { *this = {.open = true, .x = at_x, .y = at_y}; }
};

struct DrawerOptions {
  Side side = Side::Left;
  float size = 0;
  Style style{};
  std::string_view key{};
};

inline void set_app_id(const char *id) { qui_set_app_id(id); }
// Call before creating the Ui: the window has no system title bar or border.
// Mark the title bar with Style::drag and draw your own window buttons; the
// edges still resize it.
inline void set_borderless(bool on) { qui_set_borderless(on ? 1 : 0); }
// Call before creating the Ui: the window gets an alpha channel and each frame
// starts fully transparent, so a root box with a bg and radius gives the
// window rounded corners. Square them off while window_maximized().
inline void set_transparent(bool on) { qui_set_transparent(on ? 1 : 0); }

class Ui {
 public:
  Ui(const char *title, int width, int height) : app_(qui_open(title, width, height)) {
    buf_.reserve(1 << 16);
    stack_.reserve(64);
  }
  ~Ui() { qui_close(app_); }
  Ui(const Ui &) = delete;
  Ui &operator=(const Ui &) = delete;

  bool frame() {
    if (started_) {
      stats_.nodes = nodes_;
      stats_.wire_bytes = buf_.size();
      flush_images();
      qui_frame_end(app_, buf_.data(), buf_.size());
    }
    size_t len = 0;
    res_ = qui_frame_begin(app_, &len);
    if (!res_) return false;
    started_ = true;
    send_keys_ = (header_u32(60) & 1) != 0;
    nodes_ = 0;
    ++frame_count_;
    std::erase_if(tweens_, [&](const auto &kv) { return kv.second.seen + 1 < frame_count_; });
    buf_.clear();
    stack_.clear();
    stack_.push_back({kSeed, 0});
    u8(wire::OpFrame);
    u8(wire::kVersion);
    u8(theme_);
    emit_palette();
    image_at_ = buf_.size();
    return true;
  }

  void wake() { qui_wake(app_); }
  void wake_after(uint32_t ms) { qui_wake_after(app_, ms); }
  // Opens the system file picker without blocking; chosen paths arrive later
  // through picked_files, and the app is woken when they do. Native only.
  void pick_files(bool multiple) { qui_pick_files(app_, multiple ? 1 : 0); }
  std::vector<std::string> picked_files() {
    std::vector<std::string> out;
    while (const char *path = qui_picked_file(app_)) out.emplace_back(path);
    return out;
  }
  void set_clipboard(const std::string &text) { qui_set_clipboard(app_, text.c_str()); }
  // Whether files are dragged over the window; dropped ones arrive through
  // picked_files. Native only.
  bool files_hovering() const { return qui_files_hovering(app_) != 0; }
  // The text selected in a `select` box as of the last frame.
  std::string selected_text() const { return qui_selected_text(app_); }
  // Eases mouse wheel steps. Browsers smooth the wheel themselves.
  void set_smooth_scroll(bool on) { qui_set_smooth_scroll(app_, on ? 1 : 0); }
  void minimize_window() { qui_window_action(app_, 0); }
  void toggle_maximized() { qui_window_action(app_, 1); }
  // Closes the window as its close button would, ending the frame loop.
  void close_window() { qui_window_action(app_, 2); }
  bool window_maximized() const { return qui_window_maximized(app_) != 0; }
  // Makes drawers, ease and tween snap instead of animating.
  void set_reduce_motion(bool on) { reduce_motion_ = on; }

  // Last frame's Response for the node with this key in the current scope, so
  // a style can depend on hover before the node is built.
  Response peek(std::string_view key) const { return lookup(mix(stack_.back().id, key.data(), key.size())); }

  // Eases toward target over ms milliseconds and returns this frame's value,
  // for what Style::ease does not animate, such as a size or a position. Call
  // it once per frame per key. The first call, and a call after a frame
  // without one, returns target.
  float tween(std::string_view key, float target, float ms) { return tween_from(key, target, target, ms); }

  // Like tween, but starts at from the first time the key is seen, so a box can
  // animate in when it appears.
  float tween_from(std::string_view key, float from, float target, float ms) {
    const uint64_t id = mix(stack_.back().id, key.data(), key.size());
    auto [it, fresh] = tweens_.try_emplace(id);
    Tween &t = it->second;
    if (fresh || t.seen + 1 < frame_count_ || reduce_motion_ || ms <= 0) {
      t = {target, target, 0, 0, 0};
      if (from != target && !reduce_motion_ && ms > 0) t = {from, target, 0, ms / 1000, 0};
    } else if (t.to != target) {
      t = {t.value(), target, 0, ms / 1000, 0};
    } else {
      t.elapsed += std::min(dt(), 1.f / 30);
    }
    t.seen = frame_count_;
    if (t.elapsed < t.duration) wake();
    return t.value();
  }
  void set_theme(uint8_t theme) { theme_ = theme; }

  constexpr static Color token(uint8_t id) { return Color(id) << 24; }

  void set_palette(uint8_t base, std::initializer_list<std::pair<Color, Color>> colors) {
    theme_ = base;
    palette_.clear();
    for (const auto &[token, value] : colors) set_token(token, value);
  }

  void set_token(Color token, Color value) {
    const auto id = static_cast<uint8_t>(token >> 24);
    for (auto &entry : palette_) {
      if (entry.first == id) {
        entry.second = value;
        return;
      }
    }
    palette_.emplace_back(id, value);
    std::sort(palette_.begin(), palette_.end());
  }

  void define_class(std::string name, Style style) {
    style.key = style.label = {};
    style_classes_[std::move(name)] = std::move(style);
  }

  void define_text_class(std::string name, TextStyle style) { text_classes_[std::move(name)] = std::move(style); }
  float width() const { return header(8); }
  float height() const { return header(12); }
  float dpr() const { return header(16); }
  float dt() const { return header(20); }
  float pointer_x() const { return header(24); }
  float pointer_y() const { return header(28); }
  bool pointer_down() const { return header_u32(44) != 0; }
  bool pointer_touch() const { return header_u32(56) != 0; }
  bool window_focused() const { return header_u32(52) != 0; }
  Stats stats() const {
    Stats out = stats_;
    out.engine_ms = header(48);
    return out;
  }

  template <class F>
  Response column(const Style &style, F &&body) { return container(wire::DirColumn, style, body); }
  template <class F>
  Response row(const Style &style, F &&body) { return container(wire::DirRow, style, body); }
  template <class F>
  Response stack(const Style &style, F &&body) { return container(wire::DirStack, style, body); }

  void text(std::string_view value, TextStyle style = Body) { text_impl(value, {}, style); }

  // Text with styled spans (bold, monospace, colors, backgrounds, underline,
  // strike-through) and clickable links, wrapped as one paragraph. Offsets are
  // UTF-8 byte offsets. Spans must be sorted and must not overlap. A span with a
  // link is reported by that string in the result, from the previous frame.
  RichResult rich(std::string_view value, std::span<const Span> spans, TextStyle style = Body) {
    return text_impl(value, spans, style);
  }

  RichResult text_impl(std::string_view value, std::span<const Span> spans, TextStyle style) {
    RichResult out;
    if (!style.klass.empty()) style = resolve_text(style);
    const uint64_t id = open(wire::KindText, {});
    str(wire::OpText, value);
    for (const Span &s : spans) {
      if (s.end <= s.start || s.end > value.size()) continue;
      uint64_t span_id = 0;
      if (!s.link.empty()) {
        const std::string tagged = "link:" + std::string(s.link);
        span_id = mix(id, tagged.data(), tagged.size());
        const Response r = lookup(span_id);
        if (r.hovered) out.hovered = s.link;
        if (r.clicked) out.clicked = s.link;
        if (r.right_clicked) out.right_clicked = s.link;
      }
      u8(wire::OpSpan);
      u32(s.start);
      u32(s.end);
      u8(s.image.empty() ? s.flags : s.flags | wire::SpanImage);
      u32(s.fg);
      u32(s.bg);
      u64(span_id);
      u64(s.image.empty() ? 0 : mix(0, s.image.data(), s.image.size()));
    }
    gen::encode(buf_, style, reduce_motion_);
    u8(wire::OpEnd);
    return out;
  }

  // Hands the engine straight (not premultiplied) RGBA pixels under `key`. Do it
  // once; `image` then refers to it by key. Loading a key again replaces it.
  void load_image(std::string_view key, int width, int height, const uint8_t *rgba) {
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return;
    const uint64_t id = mix(0, key.data(), key.size());
    loaded_images_.insert(id);
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    pending_images_.push_back(wire::OpImageData);
    pending_images_.insert(pending_images_.end(), reinterpret_cast<const uint8_t *>(&id), reinterpret_cast<const uint8_t *>(&id) + 8);
    const uint16_t w = static_cast<uint16_t>(width), h = static_cast<uint16_t>(height);
    pending_images_.insert(pending_images_.end(), reinterpret_cast<const uint8_t *>(&w), reinterpret_cast<const uint8_t *>(&w) + 2);
    pending_images_.insert(pending_images_.end(), reinterpret_cast<const uint8_t *>(&h), reinterpret_cast<const uint8_t *>(&h) + 2);
    pending_images_.insert(pending_images_.end(), rgba, rgba + bytes);
  }

  // Like load_image, but shares the pixels instead of copying them: `owner`
  // keeps the `width * height * 4` bytes at `rgba` alive until qui is done with
  // them, and qui scales straight from that buffer on its worker thread. Decode
  // off the UI thread, then hand the result over here, and the UI thread never
  // touches the pixels.
  void load_image_shared(std::string_view key, int width, int height, std::shared_ptr<const void> owner, const uint8_t *rgba) {
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535 || !owner || !rgba) return;
    const uint64_t id = mix(0, key.data(), key.size());
    loaded_images_.insert(id);
    const uint64_t token = ++shared_token_;
    auto *held = new std::shared_ptr<const void>(std::move(owner));
    qui_stage_image(app_, token, static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgba, static_cast<size_t>(width) * static_cast<size_t>(height) * 4, held, [](void *ctx) { delete static_cast<std::shared_ptr<const void> *>(ctx); });
    pending_images_.push_back(wire::OpImageShared);
    pending_images_.insert(pending_images_.end(), reinterpret_cast<const uint8_t *>(&id), reinterpret_cast<const uint8_t *>(&id) + 8);
    pending_images_.insert(pending_images_.end(), reinterpret_cast<const uint8_t *>(&token), reinterpret_cast<const uint8_t *>(&token) + 8);
  }

  // Forgets a loaded image and its pixels.
  void free_image(std::string_view key) {
    const uint64_t id = mix(0, key.data(), key.size());
    if (!loaded_images_.erase(id)) return;
    pending_images_.push_back(wire::OpImageFree);
    pending_images_.insert(pending_images_.end(), reinterpret_cast<const uint8_t *>(&id), reinterpret_cast<const uint8_t *>(&id) + 8);
  }

  bool has_image(std::string_view key) const { return loaded_images_.count(mix(0, key.data(), key.size())) != 0; }

  // Draws a loaded picture. With no w and h it takes the image's pixel size; with
  // only a width it keeps the aspect ratio. `radius` clips it, so a square with
  // radius = size / 2 is a circle.
  Response image(std::string_view key, Style style = {}, uint8_t fit = wire::FitCover) {
    const uint64_t id = open(wire::KindImage, style.key);
    u8(wire::OpImg);
    u64(mix(0, key.data(), key.size()));
    if (fit != wire::FitCover) u8_op(wire::OpFit, fit);
    emit_style(style);
    u8(wire::OpEnd);
    return lookup(id);
  }

  // Draws an SVG fitted inside w by h, 20px square unless set, in fg (Ink when
  // unset). With a label it is an image for assistive tech; without one it is
  // decorative.
  Response icon(const Svg &svg, Style style = {}) {
    send_svg(svg);
    if (!style.klass.empty()) style = resolve_style(style);
    if (style.w.mode == wire::SizeFit && style.h.mode == wire::SizeFit) style.w = style.h = px(kIconSize);
    else if (style.h.mode == wire::SizeFit && style.w.mode == wire::SizePx) style.h = style.w;
    if (!style.label.empty() && !style.role) style.role = RoleImage;
    const uint64_t id = open(wire::KindIcon, style.key);
    u8(wire::OpImg);
    u64(svg.id);
    emit_style(style);
    u8(wire::OpEnd);
    return lookup(id);
  }

  // A button showing only an icon: 32px square with the icon centered at 20px
  // unless w and h say otherwise. `label` names it for assistive tech and keys
  // it unless style.key is set. fg colors the icon; left unset it is Muted, and
  // Ink while hovered or focused.
  Response icon_button(const Svg &svg, std::string_view label, Style style = {}) {
    if (!style.klass.empty()) style = resolve_style(style);
    if (style.key.empty()) style.key = label;
    if (style.w.mode == wire::SizeFit) style.w = px(32);
    if (style.h.mode == wire::SizeFit) style.h = style.w.mode == wire::SizePx ? style.w : px(32);
    if (style.radius == 0) style.radius = 6;
    style.align = style.justify = Center;
    style.hit = true;
    style.role = RoleButton;
    style.label = label;
    Color fg = style.fg;
    if (!fg) {
      const Response prev = peek(style.key);
      fg = prev.hovered || prev.focused ? Ink : Muted;
    }
    style.fg = 0;
    float size = kIconSize;
    if (style.w.mode == wire::SizePx && style.w.value < kIconSize + 8) size = std::max(style.w.value - 8, 8.f);
    return row(style, [&] { icon(svg, {.w = px(size), .h = px(size), .fg = fg, .ease = style.ease}); });
  }

  // Overlays float above the window. Call them from the root container, after the
  // content they cover. modal and popup return true when the user clicked outside
  // or pressed Escape, the cue to close them. A press that drifts a few pixels
  // before release still counts as a click outside.
  template <class F>
  bool modal(std::string_view key, Style panel, F &&body) {
    bool dismissed = false;
    const std::string scrim_key = std::string(key) + "/scrim", panel_key = std::string(key) + "/panel";
    stack({.w = fill(), .h = fill(), .align = Center, .justify = Center, .absolute = true, .z = kOverlayZ, .key = key}, [&] {
      const float pop = tween_from(std::string(key) + "/pop", 0.01F, 1, 160);
      const Response scrim = column({.w = fill(), .h = fill(), .absolute = true, .bg = Scrim, .opacity = pop, .hit = true, .key = scrim_key}, [] {});
      if (scrim.clicked || scrim.drag_end) dismissed = true;
      panel.hit = true;
      panel.select = true;
      if (panel.key.empty()) panel.key = panel_key;
      if (const float w = width() - 2 * kModalMargin; w > 0 && (panel.max_w == 0 || panel.max_w > w)) panel.max_w = w;
      if (const float h = height() - 2 * kModalMargin; h > 0 && (panel.max_h == 0 || panel.max_h > h)) panel.max_h = h;
      panel.opacity = pop;
      column(panel, body);
    });
    return dismissed || key_pressed(KeyEscape);
  }

  template <class F>
  bool popup(std::string_view key, float x, float y, Style panel, F &&body) {
    bool dismissed = false;
    const std::string catcher_key = std::string(key) + "/catcher", panel_key = std::string(key) + "/panel";
    if (panel.w.mode == wire::SizePx && x + panel.w.value > width()) x -= x + panel.w.value - width() + 8;
    if (x < 0) x = 0;
    stack({.w = fill(), .h = fill(), .z = kOverlayZ, .key = key, .fixed = true}, [&] {
      const Response catcher = column({.w = fill(), .h = fill(), .absolute = true, .hit = true, .key = catcher_key}, [] {});
      if (catcher.clicked || catcher.right_clicked || catcher.drag_end) dismissed = true;
      panel.absolute = true;
      panel.x = x;
      panel.y = y;
      panel.opacity = tween_from(std::string(key) + "/pop", 0.01F, 1, 110);
      panel.hit = true;
      panel.select = true;
      if (panel.key.empty()) panel.key = panel_key;
      column(panel, body);
    });
    return dismissed || key_pressed(KeyEscape);
  }

  // A panel that slides up from the bottom edge over a scrim, the phone form of
  // modal. Returns true when the scrim is tapped, the panel is swiped down or
  // Escape is pressed. Without a klass, unset panel fields other than pad get a
  // full-width card.
  template <class F>
  bool sheet(std::string_view key, Style panel, F &&body) {
    bool dismissed = false;
    const std::string k(key);
    stack({.w = fill(), .h = fill(), .absolute = true, .z = kOverlayZ, .key = key}, [&] {
      const float pop = tween_from(k + "/pop", 0.01F, 1, 200);
      const Response scrim = column({.w = fill(), .h = fill(), .absolute = true, .bg = Scrim, .opacity = pop, .hit = true, .key = k + "/scrim"}, [] {});
      if (scrim.clicked || scrim.drag_end) dismissed = true;
      column({.w = fill(), .h = fill(), .pad = 8, .align = Center, .justify = End, .absolute = true, .y = (1 - pop) * kSheetSlide}, [&] {
        if (panel.klass.empty()) sheet_defaults(panel);
        if (const float h = height() - kSheetTop; panel.max_h == 0 || panel.max_h > h) panel.max_h = h;
        panel.hit = true;
        panel.select = true;
        panel.opacity = pop;
        panel.gestures |= GesturePanY;
        const std::string panel_key = k + "/panel";
        if (panel.key.empty()) panel.key = panel_key;
        if (column(panel, body).swipe == Swipe::Down) dismissed = true;
      });
    });
    return dismissed || key_pressed(KeyEscape);
  }

  static void sheet_defaults(Style &s) {
    if (s.w.mode == wire::SizeFit && s.w.value == 0) s.w = fill();
    if (s.max_w == 0) s.max_w = 560;
    if (!s.bg) s.bg = Raised;
    if (!s.border) s.border = Line;
    if (s.border_w == 0) s.border_w = 1;
    if (s.radius == 0) s.radius = 16;
  }

  // Shows `text` beside the pointer while `show` is true, usually a response's hovered.
  void tooltip(bool show, std::string_view text, Style style = {}) {
    if (!show || text.empty()) return;
    constexpr float wrap_w = 280, wrap_h = 160;
    Style wrap{.w = px(wrap_w), .h = px(wrap_h), .z = kOverlayZ + 1, .key = "qui/tooltip/wrap", .fixed = true};
    wrap.x = pointer_x() + 12;
    wrap.y = pointer_y() + 16;
    if (wrap.x + wrap_w > width()) {
      wrap.x = pointer_x() - 12 - wrap_w;
      wrap.align = qui::End;
    }
    if (wrap.y + wrap_h > height()) {
      wrap.y = pointer_y() - 16 - wrap_h;
      wrap.justify = qui::End;
    }
    if (!style.bg) style.bg = Raised;
    if (!style.border) style.border = Line;
    if (style.radius == 0) style.radius = 6;
    if (style.pad.top == 0 && style.pad.right == 0 && style.pad.bottom == 0 && style.pad.left == 0) style.pad = {4, 8};
    style.max_w = 280;
    style.key = "qui/tooltip";
    column(wrap, [&] { column(style, [&] { text_impl(text, {}, {.size = 12, .color = Ink}); }); });
  }

  Response button(std::string_view label, Style style = {}) {
    if (style.key.empty()) style.key = label;
    const uint64_t id = open(wire::KindButton, style.key);
    emit_style(style);
    str(wire::OpText, label);
    u8(wire::OpEnd);
    return lookup(id);
  }

  // An on/off switch. Returns true when it was toggled this frame, by a click or
  // by Enter or Space while it has focus. `label` names it for assistive tech.
  bool toggle_switch(std::string_view key, bool on, std::string_view label = {}) {
    const float value = on ? 1.f : 0.f;
    return stack({.w = px(38), .h = px(22), .bg = on ? Accent : Active, .radius = 11, .hit = true, .role = RoleSwitch, .label = label, .key = key}, [&] {
             f32_op(wire::OpValue, value);
             column({.w = px(16), .h = px(16), .absolute = true, .x = on ? 19.f : 3.f, .y = 3, .bg = Strong, .radius = 8}, [] {});
           })
        .clicked;
  }

  // A horizontal slider for a value from 0 to 1; returns the new value. Pressing
  // or dragging sets it; with focus, the arrow keys step it by 5% and Home and
  // End jump to the ends.
  float slider(std::string_view key, float value, Size w, std::string_view label = {}) {
    constexpr float thumb = 14, step = 0.05f;
    const auto clamp01 = [](float v) { return std::min(std::max(v, 0.f), 1.f); };
    value = clamp01(value);
    const Response r = row({.w = w, .h = px(20), .align = Center, .hit = true, .gestures = GesturePanX | GestureTouchOnly, .role = RoleSlider, .label = label, .key = key}, [&] {
      f32_op(wire::OpValue, value);
      column({.w = fill(value), .h = px(6), .bg = Accent, .radius = 3}, [] {});
      column({.w = px(thumb), .h = px(thumb), .bg = Strong, .radius = thumb / 2}, [] {});
      column({.w = fill(1 - value), .h = px(6), .bg = Active, .radius = 3}, [] {});
    });
    if ((r.pressed || r.panning || r.clicked) && r.w > thumb) value = clamp01((pointer_x() - r.x - thumb / 2) / (r.w - thumb));
    if (r.focused) {
      if (key_pressed(KeyLeft) || key_pressed(KeyDown)) value = clamp01(value - step);
      else if (key_pressed(KeyRight) || key_pressed(KeyUp)) value = clamp01(value + step);
      else if (key_pressed(KeyHome)) value = 0;
      else if (key_pressed(KeyEnd)) value = 1;
    }
    return value;
  }

  // Draws an open menu and returns the index of the chosen item, or -1. It closes
  // itself on a choice, a press outside it and Escape. The arrow keys, Home and
  // End move through the items, Enter and Space choose, and focus returns to
  // where it was when the menu closes. Without a klass, unset panel fields get
  // the defaults. A header body, when given, is drawn above the rows.
  int menu(Menu &m, std::span<const MenuItem> items, Style s = {}) { return menu(m, items, s, nullptr); }

  template <class F>
  int menu(Menu &m, std::span<const MenuItem> items, Style s, F &&header) {
    if (!m.open || items.empty()) return -1;
    if (!m.mode) m.mode = pointer_touch() && width() < kSheetBelow ? 2 : 1;
    const bool as_sheet = m.mode == 2;
    constexpr float margin = 8;
    const float row_height = as_sheet ? 48 : 32;
    if (as_sheet) {
      s.w = fill();
      if (s.klass.empty()) sheet_defaults(s);
      if (s.klass.empty() && s.pad.top == 0 && s.pad.right == 0 && s.pad.bottom == 0 && s.pad.left == 0) s.pad = 8;
    } else if (s.klass.empty()) {
      if (s.w.mode == wire::SizeFit && s.w.value == 0) s.w = px(224);
      if (!s.bg) s.bg = Raised;
      if (!s.border) s.border = Line;
      if (s.border_w == 0) s.border_w = 1;
      if (s.radius == 0) s.radius = 8;
      if (s.pad.top == 0 && s.pad.right == 0 && s.pad.bottom == 0 && s.pad.left == 0) s.pad = 6;
      if (!s.shadow.color) s.shadow = {Shadow, 16, 4};
    }
    s.role = RoleMenu;

    const int count = static_cast<int>(items.size());
    const int before = m.active;
    if (key_pressed(KeyDown)) m.active = m.active % count + 1;
    else if (key_pressed(KeyUp)) m.active = m.active > 1 ? m.active - 1 : count;
    else if (key_pressed(KeyHome)) m.active = 1;
    else if (key_pressed(KeyEnd)) m.active = count;

    const float header_h = m.header_h > 0 ? m.header_h + s.gap : 0;
    float panel_h = header_h + static_cast<float>(count) * row_height + static_cast<float>(count - 1) * s.gap + s.pad.top + s.pad.bottom;
    if (const float limit = as_sheet ? height() - kSheetTop : height() - 2 * margin; panel_h > limit) {
      panel_h = s.max_h = limit;
      s.scroll = true;
      if (m.active != before) s.scroll_to = std::max(0.f, header_h + static_cast<float>(m.active - 1) * (row_height + s.gap) - (panel_h - row_height) / 2);
    }
    int chosen = -1, hovered = 0;
    const auto rows = [&] {
      if (m.active == 0) u8_op(wire::OpFocus, 2);
      if constexpr (!std::is_same_v<std::decay_t<F>, std::nullptr_t>) {
        const Response drawn = column({.w = fill(), .hit = true}, header);
        if (drawn.found && drawn.h != m.header_h) {
          m.header_h = drawn.h;
          wake();
        }
      }
      for (int i = 0; i < count; ++i) {
        const MenuItem &item = items[static_cast<size_t>(i)];
        const int position = i + 1;
        const Response r = row({.w = fill(), .h = px(row_height), .pad = {0, as_sheet ? 12.0F : 8.0F}, .align = Center, .bg = position == m.active ? Hover : Color{0}, .radius = as_sheet ? 10.0F : 6.0F, .hit = true, .role = RoleMenuItem}, [&] {
          if (position == m.active) u8_op(wire::OpFocus, 2);
          text(position == m.confirm ? item.confirm : item.label, {.size = as_sheet ? 16.0F : 0.0F, .ellipsis = true, .color = item.danger ? Danger : Ink, .w = fill()});
        });
        if (r.hovered) hovered = position;
        if (r.clicked) {
          if (!item.confirm.empty() && m.confirm != position) {
            m.confirm = position;
            wake();
          } else {
            chosen = i;
          }
        }
      }
    };
    const bool dismissed = as_sheet ? sheet("qui/menu", s, rows) : popup("qui/menu", m.x, std::max(margin, std::min(m.y, height() - panel_h - margin)), s, rows);
    if (hovered != m.hovered) {
      if (hovered) m.active = hovered;
      else if (m.active == m.hovered) m.active = 0;
      m.hovered = hovered;
      wake();
    }
    if (chosen >= 0 || dismissed) {
      m.open = false;
      wake();
    }
    return chosen;
  }

  template <class F>
  Response list(std::string_view key, uint32_t count, float estimate, const ListOptions &options, F &&row_body) {
    const uint64_t id = open(wire::KindList, key);
    emit_style(options.style);
    u8(wire::OpVirtual);
    u32(count);
    f32(estimate);
    u8(options.bottom ? 1 : 0);
    if (options.prepend) {
      u8(wire::OpPrepend);
      u32(options.prepend);
    }
    if (options.jump_to) {
      if (options.glide && !reduce_motion_) u8(wire::OpGlide);
      u8(wire::OpJump);
      u32(*options.jump_to);
    }
    const Response result = lookup(id);
    uint32_t first = result.first + options.prepend, visible = result.count;
    if (!result.found || visible == 0) {
      visible = std::min<uint32_t>(count, 32);
      first = options.bottom && count > visible ? count - visible : 0;
    }
    stack_.push_back({id, 0});
    for (uint32_t i = first; i < std::min(first + visible, count); ++i) {
      uint64_t item = hash_u32(id, i);
      ++nodes_;
      u8(wire::OpOpen);
      u8(wire::KindBox);
      u64(item);
      size_op(wire::OpWidth, fill());
      u8(wire::OpItem);
      u32(i);
      stack_.push_back({item, 0});
      row_body(i);
      stack_.pop_back();
      u8(wire::OpEnd);
    }
    stack_.pop_back();
    u8(wire::OpEnd);
    return result;
  }

  Response input(std::string_view key, std::string &value, const InputOptions &options = {}) {
    const uint64_t id = open(wire::KindInput, key);
    Response result = lookup(id);
    if (result.changed) value.assign(reinterpret_cast<const char *>(res_) + header_u32(40) + result.first, result.count);
    result.first = result.count = 0;
    if (options.bare) u8(wire::OpBare);
    emit_style(options.style);
    str(wire::OpText, value);
    if (!options.placeholder.empty()) str(wire::OpPlaceholder, options.placeholder);
    if (options.size != 0) f32_op(wire::OpFontSize, options.size);
    if (options.mono) u8(wire::OpMono);
    if (options.multiline) u8_op(wire::OpMultiline, options.enter_submits ? 2 : 1);
    if (options.password) u8(wire::OpPassword);
    if (options.passthrough_nav) u8(wire::OpPassthroughNav);
    if (options.focus) u8_op(wire::OpFocus, 2);
    else if (options.autofocus) u8_op(wire::OpFocus, 1);
    u8(wire::OpEnd);
    return result;
  }

  struct KeyEvent {
    uint32_t key = 0, mods = 0;
    bool released = false;
  };

  // Everything typed this frame that no input claimed, releases included.
  std::vector<KeyEvent> key_events() const {
    std::vector<KeyEvent> out;
    if (!res_) return out;
    const uint32_t at = header_u32(32), count = header_u32(36);
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t event[2];
      std::memcpy(event, res_ + at + i * 8, 8);
      out.push_back({event[0], event[1] & 0xff, (event[1] & 0x100) != 0});
    }
    return out;
  }

  bool key_pressed(uint32_t key, uint32_t mods = 0) {
    if (!res_) return false;
    const uint32_t at = header_u32(32), count = header_u32(36);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t event[2];
      std::memcpy(event, res_ + at + i * 8, 8);
      if (event[0] == key && event[1] == mods) {
        if (key == KeyEscape) qui_consume_back(app_);
        return true;
      }
    }
    return false;
  }

  Response shader(const ShaderOptions &options) {
    const uint64_t id = open(wire::KindShader, options.style.key);
    Response result = lookup(id);
    result.first = result.count = 0;
    emit_style(options.style);
    str(wire::OpSrc, options.src);
    if (options.fps != 0) f32_op(wire::OpFps, options.fps);
    for (const Uniform &uniform : options.uniforms) {
      u8(wire::OpUniform);
      u8(static_cast<uint8_t>(uniform.name.size()));
      raw(uniform.name.data(), uniform.name.size());
      u8(static_cast<uint8_t>(uniform.values.size()));
      for (const float v : uniform.values) f32(v);
    }
    u8(wire::OpEnd);
    return result;
  }

  // Fills its parent with content and slides panel over it. A touch swipe on
  // content opens it; a swipe, a tap on the scrim or Escape closes it.
  template <class C, class P>
  Response drawer(Drawer &d, const DrawerOptions &options, C &&content, P &&panel) {
    const float size = options.size > 0 ? options.size : 280;
    const bool horizontal = options.side == Side::Left || options.side == Side::Right;
    Style root{};
    root.w = root.h = fill();
    root.clip = true;
    root.key = options.key;
    root.gestures = (horizontal ? GesturePanX : GesturePanY) | GestureTouchOnly;
    const uint64_t id = open(wire::KindBox, options.key);
    u8_op(wire::OpDir, wire::DirStack);
    emit_style(root);
    const Response result = lookup(id);
    update_drawer(d, options, result, size, horizontal);

    stack_.push_back({id, 0});
    column({.w = fill(), .h = fill(), .key = "content"}, content);
    if (d.position > 0) {
      if (column({.w = fill(), .h = fill(), .absolute = true, .bg = Scrim, .opacity = d.position, .hit = true, .key = "scrim"}, [] {}).clicked) d.open = false;
      const float parent_w = result.found ? result.w : width(), parent_h = result.found ? result.h : height();
      const float travel = size * (d.position - 1);
      Style s = options.style;
      s.key = "panel";
      s.absolute = true;
      s.hit = true;
      s.select = true;
      s.w = horizontal ? px(size) : fill();
      s.h = horizontal ? fill() : px(size);
      s.x = options.side == Side::Left ? travel : options.side == Side::Right ? parent_w - size - travel : 0;
      s.y = options.side == Side::Top ? travel : options.side == Side::Bottom ? parent_h - size - travel : 0;
      column(s, panel);
    }
    stack_.pop_back();
    u8(wire::OpEnd);
    return result;
  }

 private:
  void update_drawer(Drawer &d, const DrawerOptions &options, const Response &r, float size, bool horizontal) {
    constexpr float spring = 20, fling_speed = 1.5f;
    const auto clamp01 = [](float v) { return std::min(std::max(v, 0.f), 1.f); };
    if (!d.started) {
      d.started = true;
      if (d.open) d.position = 1;
    }
    const float direction = options.side == Side::Right || options.side == Side::Bottom ? -1.f : 1.f;
    const float delta = horizontal ? r.dx : r.dy, velocity = horizontal ? r.vx : r.vy;
    if (r.panning || r.pan_end) d.position = clamp01(d.position + direction * delta / size);
    d.held = r.panning;
    if (r.pan_end) {
      const float fling = direction * velocity / size;
      d.open = fling > fling_speed ? true : fling < -fling_speed ? false : d.position > 0.5f;
      d.speed = fling;
    }
    if (d.open && key_pressed(KeyEscape)) d.open = false;
    if (d.held) {
      d.speed = 0;
      return;
    }
    const float target = d.open ? 1.f : 0.f;
    if (reduce_motion_) {
      d.position = target;
      d.speed = 0;
      return;
    }
    if (d.position == target && d.speed == 0) return;
    const float step = std::min(dt(), 1.f / 30);
    d.speed += (spring * spring * (target - d.position) - 2 * spring * d.speed) * step;
    d.position = clamp01(d.position + d.speed * step);
    if (std::abs(target - d.position) < 0.001f && std::abs(d.speed) < 0.02f) {
      d.position = target;
      d.speed = 0;
      return;
    }
    wake();
  }

  struct Scope {
    uint64_t id;
    uint32_t index;
  };

  static constexpr uint64_t kSeed = 14695981039346656037ull;

  static uint64_t mix(uint64_t h, const void *data, size_t len) {
    const auto *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < len; ++i) {
      h ^= p[i];
      h *= 1099511628211ull;
    }
    return h ? h : 1;
  }
  static uint64_t hash_u32(uint64_t seed, uint32_t v) { return mix(seed, &v, 4); }

  float header(size_t offset) const {
    float value = 0;
    if (res_) std::memcpy(&value, res_ + offset, 4);
    return value;
  }

  uint32_t header_u32(size_t offset) const {
    uint32_t value = 0;
    if (res_) std::memcpy(&value, res_ + offset, 4);
    return value;
  }

  void raw(const void *data, size_t len) {
    const size_t at = buf_.size();
    buf_.resize(at + len);
    std::memcpy(buf_.data() + at, data, len);
  }
  void u8(uint8_t v) { buf_.push_back(v); }
  void u32(uint32_t v) { raw(&v, 4); }
  void u64(uint64_t v) { raw(&v, 8); }
  void f32(float v) { raw(&v, 4); }
  void u8_op(uint8_t op, uint8_t v) { u8(op); u8(v); }
  void u32_op(uint8_t op, uint32_t v) { u8(op); u32(v); }
  void f32_op(uint8_t op, float v) { u8(op); f32(v); }
  void str(uint8_t op, std::string_view v) {
    u8(op);
    u32(static_cast<uint32_t>(v.size()));
    raw(v.data(), v.size());
  }
  void size_op(uint8_t op, Size s) {
    if (s.mode == wire::SizeFit) return;
    u8(op);
    u8(s.mode);
    f32(s.value);
  }

  uint64_t open(uint8_t kind, std::string_view key) {
    Scope &scope = stack_.back();
    const uint64_t id = key.empty() ? hash_u32(scope.id, scope.index) : mix(scope.id, key.data(), key.size());
    ++scope.index;
    ++nodes_;
    u8(wire::OpOpen);
    u8(kind);
    u64(id);
    if (send_keys_ && !key.empty()) str(wire::OpKey, key);
    return id;
  }

  void send_svg(const Svg &svg) {
    if (!svg.id || !sent_svgs_.insert(svg.id).second) return;
    pending_images_.push_back(wire::OpSvgData);
    pending_images_.insert(pending_images_.end(), reinterpret_cast<const uint8_t *>(&svg.id), reinterpret_cast<const uint8_t *>(&svg.id) + 8);
    const auto size = static_cast<uint32_t>(svg.text.size());
    pending_images_.insert(pending_images_.end(), reinterpret_cast<const uint8_t *>(&size), reinterpret_cast<const uint8_t *>(&size) + 4);
    pending_images_.insert(pending_images_.end(), svg.text.begin(), svg.text.end());
  }

  void flush_images() {
    if (pending_images_.empty()) return;
    buf_.insert(buf_.begin() + static_cast<std::ptrdiff_t>(image_at_), pending_images_.begin(), pending_images_.end());
    pending_images_.clear();
  }

  void emit_palette() {
    if (palette_.empty()) return;
    u8(wire::OpPalette);
    const uint16_t count = static_cast<uint16_t>(palette_.size());
    u8(static_cast<uint8_t>(count & 255));
    u8(static_cast<uint8_t>(count >> 8));
    for (const auto &[id, value] : palette_) {
      u8(id);
      u32(value);
    }
  }

  template <class Map>
  static std::vector<const typename Map::mapped_type *> class_chain(const Map &map, std::string_view names) {
    std::vector<const typename Map::mapped_type *> out;
    size_t i = 0;
    while (i < names.size()) {
      while (i < names.size() && names[i] == ' ') ++i;
      size_t j = i;
      while (j < names.size() && names[j] != ' ') ++j;
      if (j > i)
        if (const auto it = map.find(std::string(names.substr(i, j - i))); it != map.end()) out.push_back(&it->second);
      i = j;
    }
    return out;
  }

  Style resolve_style(Style s) const {
    const auto chain = class_chain(style_classes_, s.klass);
    s.klass = {};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      Style base = **it;
      if (!base.klass.empty()) base = resolve_style(base);
      gen::merge(s, base);
    }
    return s;
  }

  TextStyle resolve_text(TextStyle t) const {
    const auto chain = class_chain(text_classes_, t.klass);
    t.klass = {};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      TextStyle base = **it;
      if (!base.klass.empty()) base = resolve_text(base);
      gen::merge(t, base);
    }
    return t;
  }

  void emit_style(const Style &given) {
    if (given.klass.empty()) gen::encode(buf_, given, reduce_motion_);
    else gen::encode(buf_, resolve_style(given), reduce_motion_);
  }

  template <class F>
  Response container(uint8_t dir, const Style &style, F &&body) {
    const uint64_t id = open(wire::KindBox, style.key);
    if (dir != wire::DirColumn) u8_op(wire::OpDir, dir);
    emit_style(style);
    stack_.push_back({id, 0});
    body();
    stack_.pop_back();
    u8(wire::OpEnd);
    return lookup(id);
  }

  static Swipe swipe_of(const Response &r) {
    constexpr float speed = 300;
    if (!r.pan_end) return Swipe::None;
    const float x = r.vx < 0 ? -r.vx : r.vx, y = r.vy < 0 ? -r.vy : r.vy;
    if (x >= y && x >= speed) return r.vx < 0 ? Swipe::Left : Swipe::Right;
    if (y > x && y >= speed) return r.vy < 0 ? Swipe::Up : Swipe::Down;
    return Swipe::None;
  }

  Response lookup(uint64_t id) const {
    Response out;
    if (!res_) return out;
    uint32_t capacity = 0;
    std::memcpy(&capacity, res_ + 4, 4);
    if (!capacity) return out;
    for (uint32_t i = static_cast<uint32_t>(id) & (capacity - 1);; i = (i + 1) & (capacity - 1)) {
      const uint8_t *slot = res_ + 64 + static_cast<size_t>(i) * 56;
      uint64_t key = 0;
      std::memcpy(&key, slot, 8);
      if (key == 0) return out;
      if (key != id) continue;
      uint32_t flags = 0;
      std::memcpy(&flags, slot + 8, 4);
      std::memcpy(&out.first, slot + 12, 4);
      std::memcpy(&out.count, slot + 16, 4);
      std::memcpy(&out.x, slot + 24, 4);
      std::memcpy(&out.y, slot + 28, 4);
      std::memcpy(&out.w, slot + 32, 4);
      std::memcpy(&out.h, slot + 36, 4);
      std::memcpy(&out.dx, slot + 40, 4);
      std::memcpy(&out.dy, slot + 44, 4);
      std::memcpy(&out.vx, slot + 48, 4);
      std::memcpy(&out.vy, slot + 52, 4);
      out.long_pressed = flags & wire::FlagLongPressed;
      out.double_tapped = flags & wire::FlagDoubleTapped;
      out.pan_start = flags & wire::FlagPanStart;
      out.panning = flags & wire::FlagPanning;
      out.pan_end = flags & wire::FlagPanEnd;
      out.pinching = flags & wire::FlagPinching;
      if (out.pinching) std::memcpy(&out.scale, slot + 20, 4);
      out.swipe = swipe_of(out);
      out.found = true;
      out.hovered = flags & wire::FlagHovered;
      out.pressed = flags & wire::FlagPressed;
      out.clicked = flags & wire::FlagClicked;
      out.right_clicked = flags & wire::FlagRightClicked;
      out.focused = flags & wire::FlagFocused;
      out.drag_start = flags & wire::FlagDragStart;
      out.dragging = flags & wire::FlagDragging;
      out.drag_end = flags & wire::FlagDragEnd;
      out.changed = flags & wire::FlagChanged;
      out.submitted = flags & wire::FlagSubmitted;
      if (flags & wire::FlagShaderError) out.error = std::string_view(reinterpret_cast<const char *>(res_) + header_u32(40) + out.first, out.count);
      return out;
    }
  }

  struct Tween {
    float from = 0, to = 0, elapsed = 0, duration = 0;
    uint64_t seen = 0;
    float value() const {
      const float k = duration > 0 ? std::min(elapsed / duration, 1.f) : 1.f;
      return from + (to - from) * (1 - (1 - k) * (1 - k) * (1 - k));
    }
  };

  qui_app *app_;
  const uint8_t *res_ = nullptr;
  bool started_ = false;
  bool reduce_motion_ = false;
  bool send_keys_ = false;
  uint64_t frame_count_ = 0;
  std::unordered_map<uint64_t, Tween> tweens_;
  uint8_t theme_ = 0;
  size_t image_at_ = 0;
  std::vector<uint8_t> pending_images_;
  uint64_t shared_token_ = 0;
  std::unordered_set<uint64_t> loaded_images_;
  std::unordered_set<uint64_t> sent_svgs_;
  std::vector<std::pair<uint8_t, Color>> palette_;
  std::unordered_map<std::string, Style> style_classes_;
  std::unordered_map<std::string, TextStyle> text_classes_;
  size_t nodes_ = 0;
  Stats stats_{};
  std::vector<uint8_t> buf_;
  std::vector<Scope> stack_;
};

}  // namespace qui
