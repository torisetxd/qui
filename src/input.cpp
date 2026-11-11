//go:build cgo

#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qui {

using namespace wire;

namespace {

constexpr size_t kUndoLimit = 200;
constexpr size_t kBulletBytes = 3;
const char *const kBullet = "\xe2\x80\xa2";

uint32_t decode_at(std::string_view s, size_t at, size_t &len) {
  const auto c = static_cast<unsigned char>(s[at]);
  len = 1;
  if (c < 0x80) return c;
  const size_t need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
  if (need == 1 || at + need > s.size()) return 0xfffd;
  uint32_t cp = c & (0xffu >> (need + 1));
  for (size_t i = 1; i < need; ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[at + i]) & 0x3f);
  len = need;
  return cp;
}

size_t previous_codepoint(std::string_view s, size_t at) {
  if (at == 0) return 0;
  --at;
  while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80) --at;
  return at;
}

bool extends_grapheme(uint32_t cp) {
  return (cp >= 0x300 && cp <= 0x36f) || (cp >= 0x1ab0 && cp <= 0x1aff) || (cp >= 0x1dc0 && cp <= 0x1dff) ||
         (cp >= 0x20d0 && cp <= 0x20ff) || (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xfe20 && cp <= 0xfe2f) ||
         (cp >= 0x1f3fb && cp <= 0x1f3ff) || (cp >= 0xe0020 && cp <= 0xe007f) || (cp >= 0xe0100 && cp <= 0xe01ef);
}

bool is_regional(uint32_t cp) { return cp >= 0x1f1e6 && cp <= 0x1f1ff; }

bool is_word(uint32_t cp) {
  return cp == '_' || (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp >= 0x80;
}

size_t count_codepoints(std::string_view s) {
  size_t n = 0;
  for (const char c : s) n += (static_cast<unsigned char>(c) & 0xc0) != 0x80;
  return n;
}

uint32_t line_for(const std::vector<TextLine> &lines, const Node &node, size_t byte) {
  uint32_t best = 0;
  for (uint32_t i = 0; i < node.line_count; ++i) {
    if (lines[node.line_first + i].start_byte > byte) break;
    best = i;
  }
  return best;
}

float bidi_line_x(const TextLine &line, size_t relative) {
  const auto &glyphs = line.shaped->glyphs;
  std::vector<float> visual;
  visual_offsets(line, visual);
  const auto edge = [&](uint32_t i, float done) {
    const ShapedGlyph &g = glyphs[line.first + i];
    return visual[i] + g.x_advance * ((g.level & 1) ? 1 - done : done);
  };
  for (uint32_t i = 0; i < visual.size(); ++i) {
    const ShapedGlyph &g = glyphs[line.first + i];
    if (relative <= g.cluster_start_byte) {
      const bool direction_changes = i > 0 && (glyphs[line.first + i - 1].level & 1) != (g.level & 1);
      return direction_changes ? edge(i - 1, 1) : edge(i, 0);
    }
    if (relative < g.cluster_end_byte) return edge(i, static_cast<float>(relative - g.cluster_start_byte) / static_cast<float>(g.cluster_end_byte - g.cluster_start_byte));
  }
  return edge(static_cast<uint32_t>(visual.size()) - 1, 1);
}

size_t bidi_byte_at_x(const TextLine &line, float x) {
  const auto &glyphs = line.shaped->glyphs;
  std::vector<float> visual;
  visual_offsets(line, visual);
  uint32_t best = 0;
  float nearest = std::numeric_limits<float>::max();
  for (uint32_t i = 0; i < visual.size(); ++i) {
    const float right = visual[i] + glyphs[line.first + i].x_advance;
    const float distance = x < visual[i] ? visual[i] - x : x >= right ? x - right : 0;
    if (distance < nearest) {
      nearest = distance;
      best = i;
    }
  }
  const ShapedGlyph &g = glyphs[line.first + best];
  const bool left_half = x < visual[best] + g.x_advance / 2;
  return line.para_byte + (left_half != ((g.level & 1) != 0) ? g.cluster_start_byte : g.cluster_end_byte);
}

}  // namespace

float line_x(const TextLine &line, size_t byte) {
  const auto &glyphs = line.shaped->glyphs;
  if (line.first >= line.last) return 0;
  if (line.shaped->has_rtl) return bidi_line_x(line, byte - line.para_byte);
  const float origin = glyphs[line.first].x;
  const size_t relative = byte - line.para_byte;
  for (uint32_t i = line.first; i < line.last; ++i) {
    const ShapedGlyph &g = glyphs[i];
    if (relative <= g.cluster_start_byte) return g.x - origin;
    if (relative < g.cluster_end_byte) {
      const auto done = static_cast<float>(relative - g.cluster_start_byte);
      return g.x - origin + g.x_advance * done / static_cast<float>(g.cluster_end_byte - g.cluster_start_byte);
    }
  }
  return line.width;
}

size_t byte_at_x(const TextLine &line, float x) {
  const auto &glyphs = line.shaped->glyphs;
  if (line.first >= line.last) return line.start_byte;
  if (line.shaped->has_rtl) return bidi_byte_at_x(line, x);
  const float origin = glyphs[line.first].x;
  for (uint32_t i = line.first; i < line.last; ++i) {
    const ShapedGlyph &g = glyphs[i];
    if (x < g.x - origin + g.x_advance / 2) return line.para_byte + g.cluster_start_byte;
  }
  return line.para_byte + glyphs[line.last - 1].cluster_end_byte;
}


size_t next_boundary(std::string_view s, size_t at) {
  if (at >= s.size()) return s.size();
  size_t len = 0;
  uint32_t cp = decode_at(s, at, len);
  at += len;
  if (is_regional(cp) && at < s.size() && is_regional(decode_at(s, at, len))) at += len;
  while (at < s.size()) {
    cp = decode_at(s, at, len);
    if (cp == 0x200d) {
      at += len;
      if (at < s.size()) {
        decode_at(s, at, len);
        at += len;
      }
      continue;
    }
    if (!extends_grapheme(cp)) break;
    at += len;
  }
  return at;
}

size_t previous_boundary(std::string_view s, size_t at) {
  if (at == 0) return 0;
  const size_t newline = s.rfind('\n', at - 1);
  const size_t start = newline == std::string_view::npos ? 0 : newline + 1;
  if (start >= at) return previous_codepoint(s, at);
  size_t position = start, last = start;
  while (position < at) {
    last = position;
    position = next_boundary(s, position);
  }
  return last;
}

size_t next_word(std::string_view s, size_t at) {
  size_t len = 0;
  while (at < s.size() && !is_word(decode_at(s, at, len))) at += len;
  while (at < s.size() && is_word(decode_at(s, at, len))) at += len;
  return at;
}

size_t previous_word(std::string_view s, size_t at) {
  size_t len = 0;
  while (at > 0) {
    const size_t before = previous_codepoint(s, at);
    if (is_word(decode_at(s, before, len))) break;
    at = before;
  }
  while (at > 0) {
    const size_t before = previous_codepoint(s, at);
    if (!is_word(decode_at(s, before, len))) break;
    at = before;
  }
  return at;
}

void word_range(std::string_view s, size_t at, size_t &from, size_t &to) {
  from = to = std::min(at, s.size());
  if (s.empty()) return;
  size_t len = 0;
  const size_t probe = from < s.size() ? from : previous_codepoint(s, from);
  const bool word = is_word(decode_at(s, probe, len));
  from = probe;
  to = probe;
  while (from > 0) {
    const size_t before = previous_codepoint(s, from);
    if (is_word(decode_at(s, before, len)) != word) break;
    from = before;
  }
  while (to < s.size() && is_word(decode_at(s, to, len)) == word) to += len;
}

std::string clean_input(std::string_view text, bool multiline) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') continue;
      out.push_back(multiline ? '\n' : ' ');
    } else if (c == '\n') {
      out.push_back(multiline ? '\n' : ' ');
    } else if (c == '\t') {
      out.push_back(' ');
    } else if (c >= 0x20 && c != 0x7f) {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::string_view InputState::selected() const {
  return std::string_view(text).substr(selection_start(), selection_end() - selection_start());
}

void InputState::snapshot() {
  if (undo.size() >= kUndoLimit) undo.erase(undo.begin());
  undo.push_back({text, caret, anchor});
}

void InputState::replace_range(size_t from, size_t to, std::string_view with, EditKind kind) {
  const bool continues = kind != EditKind::Other && kind == last_edit && from <= last_end && to >= from &&
                         (kind == EditKind::Erase ? to == last_end : from == last_end) && with.find(' ') == std::string_view::npos;
  if (!continues) snapshot();
  text.replace(from, to - from, with);
  caret = anchor = from + with.size();
  last_edit = kind;
  last_end = caret;
  changed = true;
  goal_x = -1;
  redo.clear();
}

void InputState::restore(std::vector<Snapshot> &from, std::vector<Snapshot> &to) {
  if (from.empty()) return;
  to.push_back({text, caret, anchor});
  text = std::move(from.back().text);
  caret = from.back().caret;
  anchor = from.back().anchor;
  from.pop_back();
  last_edit = EditKind::None;
  goal_x = -1;
  changed = true;
}

void InputState::assign(std::string_view value) {
  text = value;
  seen = value;
  caret = anchor = text.size();
  preedit.clear();
  undo.clear();
  redo.clear();
  last_edit = EditKind::None;
  goal_x = -1;
  scroll_x = scroll_y = 0;
  reveal = true;
}

void InputState::rebuild_display() {
  if (password) {
    display.clear();
    const size_t n = count_codepoints(text);
    display.reserve(n * kBulletBytes);
    for (size_t i = 0; i < n; ++i) display += kBullet;
    return;
  }
  display = text;
  if (!preedit.empty()) display.insert(caret, preedit);
}

size_t InputState::to_display(size_t byte) const {
  if (password) return count_codepoints(std::string_view(text).substr(0, byte)) * kBulletBytes;
  if (!preedit.empty() && byte > caret) return byte + preedit.size();
  return byte;
}

size_t InputState::from_display(size_t byte) const {
  if (password) {
    size_t remaining = byte / kBulletBytes, at = 0, len = 0;
    while (remaining-- > 0 && at < text.size()) {
      decode_at(text, at, len);
      at += len;
    }
    return at;
  }
  if (preedit.empty()) return byte;
  if (byte <= caret) return byte;
  if (byte >= caret + preedit.size()) return byte - preedit.size();
  return caret;
}

size_t InputState::caret_display() const {
  return to_display(caret) + (password || preedit.empty() ? 0 : preedit_cursor);
}

void Engine::apply_input_view(Node &node, const InputState &state) {
  node.placeholder_shown = state.display.empty() && !state.placeholder.empty();
  node.text = node.placeholder_shown ? std::string_view(state.placeholder) : std::string_view(state.display);
}

void Engine::sync_input(Node &node, int index) {
  const auto [it, fresh] = inputs.try_emplace(node.id);
  InputState &state = it->second;
  // State outlives a hidden field until prune(), so autofocus keys off the
  // field reappearing rather than its state being new.
  const bool appeared = fresh || state.frame + 1 != frame_number;
  state.id = node.id;
  state.frame = frame_number;
  state.node = index;
  state.multiline = node.multiline != 0;
  state.submit_on_enter = node.multiline == 2;
  state.password = node.password;
  state.passthrough_nav = node.passthrough_nav;
  if (state.placeholder != node.placeholder) state.placeholder = node.placeholder;
  if (fresh || node.text != state.seen) state.assign(node.text);
  if (node.focus_mode == 2 || (appeared && node.focus_mode == 1)) focus_request = node.id;
  state.rebuild_display();
  apply_input_view(node, state);
}

void Engine::refresh_input(InputState &state) {
  if (state.node >= static_cast<int>(nodes.size()) || nodes[static_cast<size_t>(state.node)].id != state.id) return;
  Node &node = nodes[static_cast<size_t>(state.node)];
  state.rebuild_display();
  apply_input_view(node, state);
  wrap(node, std::max(0.f, node.w - node.pad[1] - node.pad[3]));
  state.reveal = true;
}

float Engine::input_text_top(const Node &node, const InputState &state) const {
  const float size = node.font_size > 0 ? node.font_size : kDefaultFontSize;
  const float line_height = line_height_for(size) * dpr;
  const float inner_h = (node.h - node.pad[0] - node.pad[2]) * dpr;
  const float centered = state.multiline ? 0 : (inner_h - line_height) / 2;
  return (node.y + node.pad[0]) * dpr + centered - state.scroll_y;
}

size_t Engine::input_byte_at(const InputState &state, float x, float y) const {
  const Node &node = nodes[static_cast<size_t>(state.node)];
  if (node.placeholder_shown || node.line_count == 0) return 0;
  const float size = node.font_size > 0 ? node.font_size : kDefaultFontSize;
  const float line_height = line_height_for(size) * dpr;
  const float along = (x - node.x - node.pad[3]) * dpr + state.scroll_x;
  const float down = y * dpr - input_text_top(node, state);
  const auto index = static_cast<uint32_t>(std::clamp(std::floor(down / line_height), 0.f, static_cast<float>(node.line_count - 1)));
  return state.from_display(byte_at_x(lines[node.line_first + index], along));
}

void Engine::input_caret_rect(const InputState &state, float &x, float &y, float &h) const {
  const Node &node = nodes[static_cast<size_t>(state.node)];
  const float size = node.font_size > 0 ? node.font_size : kDefaultFontSize;
  const float line_height = line_height_for(size) * dpr;
  uint32_t index = 0;
  float along = 0;
  if (!node.placeholder_shown && node.line_count) {
    index = line_for(lines, node, state.caret_display());
    along = line_x(lines[node.line_first + index], state.caret_display());
  }
  x = node.x + node.pad[3] + (along - state.scroll_x) / dpr;
  y = (input_text_top(node, state) + line_height * static_cast<float>(index)) / dpr;
  h = line_height / dpr;
}

size_t Engine::input_line_edge(const InputState &state, bool end) const {
  const Node &node = nodes[static_cast<size_t>(state.node)];
  if (node.placeholder_shown || node.line_count == 0) return 0;
  const TextLine &line = lines[node.line_first + line_for(lines, node, state.caret_display())];
  size_t byte = line.start_byte;
  if (end && line.first < line.last) byte = line.para_byte + line.shaped->glyphs[line.last - 1].cluster_end_byte;
  return state.from_display(byte);
}

size_t Engine::input_vertical(InputState &state, int direction) const {
  const Node &node = nodes[static_cast<size_t>(state.node)];
  if (node.placeholder_shown || node.line_count == 0) return state.caret;
  const size_t byte = state.caret_display();
  const uint32_t index = line_for(lines, node, byte);
  if (state.goal_x < 0) state.goal_x = line_x(lines[node.line_first + index], byte);
  if (direction < 0 && index == 0) return 0;
  if (direction > 0 && index + 1 >= node.line_count) return state.text.size();
  const uint32_t target = direction < 0 ? index - 1 : index + 1;
  return state.from_display(byte_at_x(lines[node.line_first + target], state.goal_x));
}

void Engine::paint_input(const Node &node, float alpha, IRect clip) {
  const auto it = inputs.find(node.id);
  if (it == inputs.end() || node.line_count == 0) return;
  InputState &state = it->second;

  const float size = node.font_size > 0 ? node.font_size : kDefaultFontSize;
  const float line_height = line_height_for(size) * dpr;
  const float left = (node.x + node.pad[3]) * dpr;
  const float inner_w = std::max(0.f, (node.w - node.pad[1] - node.pad[3]) * dpr);
  const float inner_h = std::max(0.f, (node.h - node.pad[0] - node.pad[2]) * dpr);
  const IRect inner = intersect(clip, {static_cast<int>(std::floor(left)), static_cast<int>(std::floor((node.y + node.pad[0]) * dpr)),
                                       static_cast<int>(std::ceil(inner_w)), static_cast<int>(std::ceil(inner_h))});
  const float caret_w = std::max(1.f, std::round(1.5f * dpr));
  const bool focused = node.id == ui.focused;

  const size_t caret_byte = state.caret_display();
  const uint32_t caret_line = node.placeholder_shown ? 0 : line_for(lines, node, caret_byte);
  const float caret_x = node.placeholder_shown ? 0 : line_x(lines[node.line_first + caret_line], caret_byte);
  const float content_h = line_height * static_cast<float>(node.line_count);
  const float content_w = state.multiline ? 0 : lines[node.line_first].width;

  if (state.reveal && focused) {
    if (!state.multiline) {
      if (caret_x + caret_w - state.scroll_x > inner_w) state.scroll_x = caret_x + caret_w - inner_w;
      if (caret_x < state.scroll_x) state.scroll_x = caret_x;
    } else {
      const float caret_y = line_height * static_cast<float>(caret_line);
      if (caret_y + line_height - state.scroll_y > inner_h) state.scroll_y = caret_y + line_height - inner_h;
      if (caret_y < state.scroll_y) state.scroll_y = caret_y;
    }
    state.reveal = false;
  }
  state.scroll_x = std::clamp(state.scroll_x, 0.f, std::max(0.f, content_w + caret_w - inner_w));
  state.scroll_limit_y = std::max(0.f, content_h - inner_h);
  state.scroll_y = std::clamp(state.scroll_y, 0.f, state.scroll_limit_y);

  const float top = input_text_top(node, state);
  const auto fade = [&](uint32_t c) {
    const auto a = static_cast<uint32_t>(static_cast<float>(c & 0xffu) * alpha + 0.5f);
    return (c & 0xffffff00u) | std::min(a, 255u);
  };
  const auto span = [&](size_t from, size_t to, auto &&draw) {
    for (uint32_t i = 0; i < node.line_count; ++i) {
      const TextLine &line = lines[node.line_first + i];
      if (to <= line.start_byte || from > line.end_byte) continue;
      if (line.shaped->has_rtl) {
        const float a = line_x(line, std::max<size_t>(from, line.start_byte)), b = line_x(line, std::min<size_t>(to, line.end_byte));
        if (a != b) draw(left + std::min(a, b) - state.scroll_x, top + line_height * static_cast<float>(i), std::abs(b - a));
        continue;
      }
      const float x0 = from <= line.start_byte ? 0 : line_x(line, from);
      const bool crosses = to > line.end_byte;
      const float x1 = crosses ? line.width + 4 * dpr : to >= line.end_byte ? line.width : line_x(line, to);
      if (x1 > x0) draw(left + x0 - state.scroll_x, top + line_height * static_cast<float>(i), x1 - x0);
    }
  };

  if (focused && state.has_selection() && !node.placeholder_shown) {
    const uint32_t color = fade(resolve(Selection));
    span(state.to_display(state.selection_start()), state.to_display(state.selection_end()), [&](float x, float y, float w) {
      add_rect(std::round(x), std::round(y), std::round(w), line_height, color, 0, 0, inner);
    });
  }

  paint_text(node, alpha, inner, -state.scroll_x, -state.scroll_y, node.placeholder_shown ? static_cast<uint32_t>(Muted) : 0);

  if (!state.preedit.empty() && !state.password) {
    const uint32_t color = fade(resolve(node.fg ? node.fg : Ink));
    const float thickness = std::max(1.f, std::round(dpr));
    span(state.caret, state.caret + state.preedit.size(), [&](float x, float y, float w) {
      add_rect(std::round(x), std::round(y + line_height - thickness), std::round(w), thickness, color, 0, 0, inner);
    });
  }

  if (focused) {
    const uint32_t color = ui.caret_on ? fade(resolve(node.fg ? node.fg : Ink)) : 0;
    add_rect(std::round(left + caret_x - state.scroll_x), std::round(top + line_height * static_cast<float>(caret_line)), caret_w, line_height, color, 0, 0, inner);
  }

  if (state.multiline && state.scroll_limit_y > 0) add_scrollbar(node, alpha, clip, state.scroll_y / state.scroll_limit_y, inner_h / content_h);
}

}  // namespace qui

namespace qui {

float Engine::text_top(const Node &node) const {
  const float size = node.font_size > 0 ? node.font_size : kDefaultFontSize;
  const float line_height = line_height_for(size) * dpr;
  const float free_h = (node.h - node.pad[0] - node.pad[2]) * dpr - line_height * static_cast<float>(node.line_count);
  return (node.y + node.pad[0]) * dpr + (node.align == Center ? free_h / 2 : node.align == End ? free_h : 0);
}

float Engine::line_left(const Node &node, const TextLine &line) const {
  const float free_w = (node.w - node.pad[1] - node.pad[3]) * dpr - line.width;
  return (node.x + node.pad[3]) * dpr + (node.justify == Center ? free_w / 2 : node.justify == End ? free_w : 0);
}

uint32_t Engine::text_byte_at(const Node &node, float x, float y) const {
  if (!node.line_count) return 0;
  const float size = node.font_size > 0 ? node.font_size : kDefaultFontSize;
  const float line_height = line_height_for(size) * dpr;
  const float down = y * dpr - text_top(node);
  if (down < 0) return 0;
  if (down >= line_height * static_cast<float>(node.line_count)) return static_cast<uint32_t>(node.text.size());
  const auto index = static_cast<uint32_t>(std::clamp(std::floor(down / line_height), 0.f, static_cast<float>(node.line_count - 1)));
  const TextLine &line = lines[node.line_first + index];
  return static_cast<uint32_t>(std::min(byte_at_x(line, x * dpr - line_left(node, line)), node.text.size()));
}

bool Engine::text_at(float x, float y, uint64_t root, uint64_t &found_root, TextPoint &out) const {
  const SelectText *best = nullptr;
  float best_distance = std::numeric_limits<float>::max();
  for (const SelectText &text : select_texts_) {
    const Node &node = nodes[static_cast<size_t>(text.node)];
    if (root && nodes[static_cast<size_t>(node.select_root)].id != root) continue;
    const float left = std::max(node.x, static_cast<float>(text.clip.x) / dpr), top = std::max(node.y, static_cast<float>(text.clip.y) / dpr);
    const float right = std::min(node.x + node.w, static_cast<float>(text.clip.x + text.clip.w) / dpr);
    const float bottom = std::min(node.y + node.h, static_cast<float>(text.clip.y + text.clip.h) / dpr);
    if (right <= left || bottom <= top) continue;
    const float dx = x < left ? left - x : x > right ? x - right : 0;
    const float dy = y < top ? top - y : y > bottom ? y - bottom : 0;
    if (!root && (dx > 0 || dy > 0)) continue;
    const float distance = dy * 4096 + dx;
    if (distance < best_distance) {
      best_distance = distance;
      best = &text;
    }
  }
  if (!best) return false;
  const Node &node = nodes[static_cast<size_t>(best->node)];
  found_root = nodes[static_cast<size_t>(node.select_root)].id;
  out = {node.id, text_byte_at(node, x, y)};
  return true;
}

void Engine::select_word(uint64_t root, TextPoint at, bool whole) {
  const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const Node &n) { return n.id == at.node && n.kind == KindText; });
  if (node == nodes.end()) return;
  size_t from = 0, to = node->text.size();
  if (!whole) word_range(node->text, at.byte, from, to);
  selection = {root, {at.node, static_cast<uint32_t>(from)}, {at.node, static_cast<uint32_t>(to)}, true};
}

void Engine::resolve_selection() {
  sel_root_ = sel_lo_ = sel_hi_ = -1;
  if (!selection.root) return;
  int32_t root = -1, anchor = -1, focus = -1;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const Node &node = nodes[i];
    if (node.id == selection.root && node.select_root == static_cast<int32_t>(i)) root = static_cast<int32_t>(i);
    if (node.kind != KindText) continue;
    if (node.id == selection.anchor.node) anchor = static_cast<int32_t>(i);
    if (node.id == selection.focus.node) focus = static_cast<int32_t>(i);
  }
  if (root < 0) {
    selection = {};
    return;
  }
  if (anchor < 0 && focus < 0) return;
  const auto last = static_cast<int32_t>(nodes.size());
  if (anchor >= 0 && focus >= 0) selection.anchor_first = anchor < focus || (anchor == focus && selection.anchor.byte <= selection.focus.byte);
  if (anchor < 0) anchor = selection.anchor_first ? root : last;
  if (focus < 0) focus = selection.anchor_first ? last : root;
  const uint32_t anchor_byte = selection.anchor.byte, focus_byte = selection.focus.byte;
  const bool forward = selection.anchor_first;
  sel_root_ = root;
  sel_lo_ = forward ? anchor : focus;
  sel_hi_ = forward ? focus : anchor;
  sel_lo_byte_ = forward ? anchor_byte : focus_byte;
  sel_hi_byte_ = forward ? focus_byte : anchor_byte;
  if (sel_lo_ == root) sel_lo_byte_ = 0;
  if (sel_hi_ == last) sel_hi_byte_ = std::numeric_limits<uint32_t>::max();
}

std::string Engine::selected_text() {
  resolve_selection();
  std::string out;
  if (sel_root_ < 0 || selection.empty()) return out;
  const Node *previous = nullptr;
  for (int32_t i = std::max(sel_lo_, 0); i <= sel_hi_ && i < static_cast<int32_t>(nodes.size()); ++i) {
    const Node &node = nodes[static_cast<size_t>(i)];
    if (node.kind != KindText || node.select_root != sel_root_) continue;
    const size_t from = i == sel_lo_ ? std::min<size_t>(sel_lo_byte_, node.text.size()) : 0;
    const size_t to = i == sel_hi_ ? std::min<size_t>(sel_hi_byte_, node.text.size()) : node.text.size();
    if (from >= to) continue;
    if (previous) out += node.y >= previous->y + previous->h - 0.5f ? '\n' : ' ';
    out.append(node.text.substr(from, to - from));
    previous = &node;
  }
  return out;
}

}  // namespace qui
