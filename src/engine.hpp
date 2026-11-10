#pragma once

#include "font_manager.hpp"
#include "svg.hpp"
#include "system_fonts.hpp"
#include "text_shaper.hpp"
#include "wire.gen.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <array>
#include <bitset>
#include <vector>

namespace qui {

inline constexpr float kDefaultFontSize = 14;
inline float line_height_for(float size) { return std::ceil(size * 1.4f); }

struct IRect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};

IRect intersect(IRect a, IRect b);
IRect unite(IRect a, IRect b);

struct TextLine {
  const ShapedLine *shaped = nullptr;
  uint32_t first = 0, last = 0;
  float width = 0;
  bool ellipsis = false;
  uint32_t para_byte = 0, start_byte = 0, end_byte = 0;
};

void visual_offsets(const TextLine &line, std::vector<float> &out);
float line_x(const TextLine &line, size_t byte);
size_t byte_at_x(const TextLine &line, float x);

struct TextPoint {
  uint64_t node = 0;
  uint32_t byte = 0;
  bool operator==(const TextPoint &) const = default;
};

// A selection over the text of one `Select` subtree, between two points in
// document order. `anchor_first` remembers the order for when an end leaves
// the frame, as rows of a list do.
struct TextSelection {
  uint64_t root = 0;
  TextPoint anchor, focus;
  bool anchor_first = true;
  bool empty() const { return !root || anchor == focus; }
};

struct UniformRef {
  std::string_view name;
  float v[4] = {};
  uint8_t n = 0;
};

struct Node {
  uint64_t id = 0;
  int32_t parent = -1, first_child = -1, last_child = -1, next_sibling = -1;
  uint8_t kind = 0, dir = 0, align = 0, justify = 0, role = 0, hit = 0, weight = 0;
  uint8_t w_mode = 0, h_mode = 0;
  uint8_t multiline = 0, focus_mode = 0, gestures = 0;
  bool mono = false, clip = false, scroll = false, ellipsis = false, has_pos = false, bottom = false;
  bool password = false, passthrough_nav = false, placeholder_shown = false, has_gradient = false, has_scroll_to = false;
  bool wrap = false;
  float w_val = 0, h_val = 0, min_w = 0, max_w = 0, min_h = 0, max_h = 0;
  float pad[4] = {};
  float gap = 0, pos_x = 0, pos_y = 0;
  int32_t z = 0;
  uint32_t bg = 0, fg = 0, border = 0;
  float border_w = 0, radius = 0, opacity = 1, font_size = 0;
  uint32_t gradient_from = 0, gradient_to = 0, shadow = 0;
  float gradient_angle = 0, shadow_blur = 0, shadow_y = 0, tracking = 0, scroll_to = 0;
  uint32_t jump = 0;
  bool has_jump = false, glide = false, drag = false, fixed = false, no_scrollbar = false;
  std::string_view text, label, src, placeholder;
  uint32_t count = 0;
  float est = 0;
  int64_t item = -1;
  float fps = 0;
  uint32_t uniform_first = 0, uniform_count = 0;
  uint64_t image = 0;
  uint8_t fit = 0;
  uint32_t span_first = 0, span_count = 0, prepend = 0;
  int32_t select_root = -1;
  float ease = 0;
  uint32_t hover_bg = 0;

  float iw = 0, ih = 0, x = 0, y = 0, w = 0, h = 0;
  uint32_t line_first = 0, line_count = 0;
  FontInstanceId font = 0;
  // Set by the width pass for text with a single paragraph, so wrapping does
  // not look the same line up again.
  const ShapedLine *shaped = nullptr;
};

enum InstanceFlag : uint32_t {
  InstSolid = 1u << 0,
  InstGlyph = 1u << 1,
  InstColorGlyph = 1u << 2,
  InstRounded = 1u << 3,
  InstGradient = 1u << 4,
  InstGradientV = 1u << 5,
  InstShadow = 1u << 6,
};

struct Instance {
  float x, y, w, h;
  float u0, v0, u1, v1;
  uint32_t rgba;
  uint32_t flags;
};

inline float encode_color_high(uint32_t color) { return static_cast<float>(color >> 16); }
inline float encode_color_low(uint32_t color) { return static_cast<float>(color & 0xffffu); }

inline uint32_t mix_colors(uint32_t a, uint32_t b, float t) {
  const float alpha_a = static_cast<float>(a & 0xffu) * (1 - t), alpha_b = static_cast<float>(b & 0xffu) * t;
  const float alpha = alpha_a + alpha_b;
  if (alpha <= 0) return 0;
  uint32_t out = static_cast<uint32_t>(alpha + 0.5f);
  for (const int shift : {24, 16, 8}) {
    const float channel = (static_cast<float>((a >> shift) & 0xffu) * alpha_a + static_cast<float>((b >> shift) & 0xffu) * alpha_b) / alpha;
    out |= static_cast<uint32_t>(channel + 0.5f) << shift;
  }
  return out;
}
static_assert(sizeof(Instance) == 40);

struct RoundClip {
  int x = 0, y = 0, w = 0, h = 0;
  float radius = 0;
  bool operator==(const RoundClip &) const = default;
};

struct Batch {
  uint32_t first = 0, count = 0;
  IRect clip;
  RoundClip round;
  int page = -1;
  int shader = -1;
};

struct ShaderUniform {
  std::string name;
  float v[4] = {};
  uint8_t n = 0;
  bool operator==(const ShaderUniform &) const = default;
};

struct ShaderDraw {
  uint64_t id = 0;
  uint64_t hash = 0;
  std::string src;
  IRect rect;
  float fps = 0;
  float opacity = 1;
  std::vector<ShaderUniform> uniforms;
};

struct FrameData {
  std::vector<Instance> instances;
  std::vector<Batch> batches;
  std::vector<ShaderDraw> shaders;
  uint32_t clear = 0;
  int width = 0, height = 0;
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual void resize(int width, int height) = 0;
  virtual void upload(const AtlasUpload &upload) = 0;
  virtual void draw(const FrameData &frame, IRect damage, double time) = 0;
  virtual void read_pixels(std::vector<uint32_t> &out) = 0;
  virtual std::string_view shader_error(uint64_t) const { return {}; }
  virtual bool take_compiled() { return false; }
};

class CpuBackend : public Backend {
 public:
  virtual const uint32_t *pixels() const = 0;
};

std::unique_ptr<CpuBackend> make_cpu_backend();
std::unique_ptr<Backend> make_gl_backend(void *(*get_proc)(const char *), bool offscreen, bool es);

struct HitEntry {
  uint64_t id = 0;
  float x = 0, y = 0, w = 0, h = 0;
  float cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
  bool press = false, scroll = false, focusable = false, scrollbar = false, link = false;
  float thumb_y = 0, thumb_h = 0;
  int32_t scroll_parent = -1, parent = -1;
  uint8_t gestures = 0, role = 0;
};

struct ScrollState {
  float offset = 0, content = 0, viewport = 0;
  uint64_t frame = 0;
};

struct ListState {
  uint32_t anchor = 0, count = 0, first = 0, visible = 0;
  float offset = 0, est = 32, gap = 0, viewport = 0;
  bool seen = false, bottom = false, stuck = true, jumped = false;
  // An eased jump toward `glide_to`, which ends centered in the viewport.
  bool gliding = false;
  uint32_t glide_to = 0;
  double glide_at = 0;
  uint64_t frame = 0;
  std::unordered_map<uint32_t, float> heights;

  float height(uint32_t index) const;
  float distance_to(uint32_t index) const;
  float average() const;
  void scroll_by(float delta);
  void jump(float fraction);
  void compute_range();
};

enum class EditKind : uint8_t { None, Type, Erase, Other };

struct InputState {
  struct Snapshot {
    std::string text;
    size_t caret = 0, anchor = 0;
  };

  uint64_t id = 0, frame = 0;
  int node = -1;
  std::string text, seen, display, preedit, placeholder;
  size_t caret = 0, anchor = 0, preedit_cursor = 0, last_end = 0;
  float scroll_x = 0, scroll_y = 0, scroll_limit_y = 0, goal_x = -1;
  bool changed = false, submitted = false, multiline = false, submit_on_enter = false, password = false, reveal = true, passthrough_nav = false;
  EditKind last_edit = EditKind::None;
  std::vector<Snapshot> undo, redo;

  bool has_selection() const { return caret != anchor; }
  size_t selection_start() const { return std::min(caret, anchor); }
  size_t selection_end() const { return std::max(caret, anchor); }
  std::string_view selected() const;
  void snapshot();
  void replace_range(size_t from, size_t to, std::string_view with, EditKind kind);
  void restore(std::vector<Snapshot> &from, std::vector<Snapshot> &to);
  void assign(std::string_view value);
  void rebuild_display();
  size_t to_display(size_t byte) const;
  size_t from_display(size_t byte) const;
  size_t caret_display() const;
};

size_t next_boundary(std::string_view text, size_t at);
size_t previous_boundary(std::string_view text, size_t at);
size_t next_word(std::string_view text, size_t at);
size_t previous_word(std::string_view text, size_t at);
void word_range(std::string_view text, size_t at, size_t &from, size_t &to);
std::string clean_input(std::string_view text, bool multiline);

struct Interaction {
  uint64_t hovered = 0, pressed = 0, focused = 0;
  bool focus_ring = false, caret_on = true;
  uint64_t scrollbar = 0;
};

class Engine {
 public:
  Engine();
  ~Engine();

  void decode(const uint8_t *wire, size_t len);
  void layout(float width, float height, float dpr);
  void paint();

  void refresh_input(InputState &state);
  size_t input_byte_at(const InputState &state, float x, float y) const;
  void input_caret_rect(const InputState &state, float &x, float &y, float &h) const;
  size_t input_line_edge(const InputState &state, bool end) const;
  size_t input_vertical(InputState &state, int direction) const;

  // Finds the selectable text under a point, or with `root` set, the nearest
  // text of that subtree, so a drag past the text keeps extending.
  bool text_at(float x, float y, uint64_t root, uint64_t &found_root, TextPoint &out) const;
  void select_word(uint64_t root, TextPoint at, bool whole);
  std::string selected_text();
  void resolve_selection();
  TextSelection selection;
  std::string selection_cache;

  std::vector<Node> nodes;
  std::vector<TextLine> lines;
  std::vector<UniformRef> uniforms;
  std::vector<HitEntry> hits;
  struct DragArea {
    uint64_t id = 0;
    float x = 0, y = 0, w = 0, h = 0;
  };
  std::vector<DragArea> drags;
  std::unordered_map<uint64_t, ScrollState> scrolls;
  std::unordered_map<uint64_t, ListState> lists;
  std::unordered_map<uint64_t, InputState> inputs;
  Interaction ui;
  uint64_t focus_request = 0;
  bool menu_open = false;
  float insets[4] = {};
  FontManager fonts;
  TextShaper shaper{fonts};
  FrameData frame, previous;
  uint8_t theme = 0;
  struct SpanRef {
    uint32_t start = 0, end = 0;
    uint8_t flags = 0;
    uint32_t fg = 0, bg = 0;
    uint64_t id = 0, image = 0;
  };
  std::vector<SpanRef> spans;
  std::vector<TextStyleSpan> span_scratch;
  // Straight RGBA the engine reads but may not own: `owner` keeps the bytes
  // alive, so a decoder's buffer can be shared with the resampler thread
  // instead of copied.
  struct ImageSource {
    uint32_t w = 0, h = 0;
    uint64_t version = 0;
    std::shared_ptr<const void> owner;
    std::span<const uint8_t> pixels;
  };
  struct StagedImage {
    uint32_t w = 0, h = 0;
    std::shared_ptr<const void> owner;
    std::span<const uint8_t> pixels;
  };
  std::unordered_map<uint64_t, StagedImage> staged_images;
  // Holds shared pixels under `token` until an ImageShared op names it.
  void stage_image(uint64_t token, uint32_t w, uint32_t h, std::span<const uint8_t> pixels, std::shared_ptr<const void> owner) {
    staged_images[token] = {w, h, std::move(owner), pixels};
  }
  struct ImageRegion {
    AtlasRegion region;
    int w = 0, h = 0;
    uint32_t alloc_w = 0, alloc_h = 0;
    bool ok = false, in_flight = false;
    uint64_t source = 0, version = 0, last_used = 0, written = 0;
  };
  std::unordered_map<uint64_t, ImageSource> images;
  std::unordered_map<uint64_t, ImageRegion> image_regions;
  // Large images are scaled on a worker thread instead of during paint; the
  // last pixels keep showing until the new ones land, then on_async asks for
  // a frame.
  bool async_images = false;
  std::function<void()> on_async;
  struct Resampler;
  std::unique_ptr<Resampler> resampler_;
  IRect image_damage_{};
  void drain_resamples();
  void store_region(ImageRegion &region, uint64_t key, uint64_t version, int dw, int dh, std::vector<uint8_t> &&pixels, bool premultiplied = false);
  void damage_image(const ImageRegion &region, const Instance &instance);
  const ImageRegion &image_region(const Node &node, int box_w, int box_h);
  const ImageRegion &image_region_for(uint64_t key, uint8_t fit, int box_w, int box_h);
  void evict_image_regions();
  void paint_image(const Node &node, float x0, float y0, float w, float h, float alpha, float radius, IRect clip);
  std::unordered_map<uint64_t, SvgIcon> svgs;
  std::unordered_map<uint64_t, ImageRegion> icon_regions;
  const ImageRegion &icon_region(uint64_t key, int box_w, int box_h);
  void paint_icon(const Node &node, float x0, float y0, float w, float h, uint32_t color, IRect clip);
  std::array<uint32_t, 256> palette{};
  std::bitset<256> palette_set;
  float dpr = 1;
  uint64_t frame_number = 0;
  bool wants_another_frame = false;
  // Milliseconds this frame spent scaling images on the paint thread.
  double sync_resample_ms = 0;
  // Counters for frame diagnostics: scaling jobs queued and images scaled inline.
  uint64_t stat_async_resamples = 0, stat_sync_resamples = 0;
  // The clock for eases, in seconds; `easing` is set by paint while one runs.
  double time = 0;
  bool easing = false;

  IRect swap_and_damage();
  std::string tree_json() const;

 private:
  SystemFont sans_regular_, sans_bold_, mono_regular_, mono_bold_;
  std::unordered_map<uint64_t, FontInstanceId> font_cache_;
  int32_t scroll_parent_ = -1, hit_parent_ = -1;
  std::vector<float> visual_;
  RoundClip round_clip_;
  struct SelectText {
    int32_t node = -1;
    IRect clip;
  };
  std::vector<SelectText> select_texts_;
  int32_t sel_root_ = -1, sel_lo_ = -1, sel_hi_ = -1;
  uint32_t sel_lo_byte_ = 0, sel_hi_byte_ = 0;
  uint32_t text_byte_at(const Node &node, float x, float y) const;
  float text_top(const Node &node) const;
  float line_left(const Node &node, const TextLine &line) const;

  FontInstanceId font_for(const Node &node);
  FontInstanceId font_for(const Node &node, bool bold, bool mono, bool italic = false);
  const ShapedLine &shape_paragraph(const Node &node, std::string_view para, uint32_t para_byte);
  uint32_t resolve(uint32_t color) const;
  void intrinsic_width(Node &node);
  void distribute_width(Node &node);
  void intrinsic_height(Node &node);
  void distribute_height(Node &node);
  void place_children(Node &node);
  void place_list(Node &node);
  void wrap(Node &node, float inner_width);
  float flow_lines(Node &node, bool place);
  std::vector<Node *> flow_line_;
  void sync_input(Node &node, int index);
  void apply_input_view(Node &node, const InputState &state);
  float input_text_top(const Node &node, const InputState &state) const;
  struct EaseValues {
    uint32_t bg = 0, border = 0, fg = 0, tint = 0;
    float radius = 0, opacity = 1;
    bool operator==(const EaseValues &) const = default;
  };
  struct EaseState {
    EaseValues from, to;
    double start = 0, duration = 0;
    uint64_t frame = 0;
  };
  std::unordered_map<uint64_t, EaseState> eases_;
  // The hovered hit and the hits around it, from the last paint.
  std::vector<uint64_t> hover_chain_;
  EaseValues ease(const Node &node, const EaseValues &target);
  void paint_node(int index, float opacity, IRect clip);
  void paint_children(const Node &node, float opacity, IRect clip);
  void paint_text(const Node &node, float opacity, IRect clip, float dx = 0, float dy = 0, uint32_t color_override = 0);
  void paint_input(const Node &node, float alpha, IRect clip);
  void add_rect(float x, float y, float w, float h, uint32_t color, float radius, float stroke, IRect clip);
  void add_gradient(float x, float y, float w, float h, uint32_t from, uint32_t to, float angle, float radius, IRect clip);
  void add_scrollbar(const Node &node, float alpha, IRect clip, float position, float fraction);
  void add_instance(const Instance &instance, int page, IRect clip);
  void prune();
};

}  // namespace qui
