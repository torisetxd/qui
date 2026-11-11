//go:build cgo

#include "drive.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace qui {

namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool contains(std::string_view haystack, std::string_view lowered) {
  return !haystack.empty() && lower(haystack).find(lowered) != std::string::npos;
}

std::string quote(std::string_view text) {
  constexpr size_t kMax = 80;
  bool cut = false;
  if (text.size() > kMax) {
    size_t end = kMax;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    text = text.substr(0, end);
    cut = true;
  }
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      out += c;
    }
  }
  if (cut) out += "...";
  return out + "\"";
}

std::string key_ref(std::string_view key) {
  const bool plain = std::none_of(key.begin(), key.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == '[' || c == '"' || c == ';'; });
  return "@" + (plain ? std::string(key) : quote(key));
}

const char *kind_name(const Node &node) {
  static const char *const kinds[] = {"box", "text", "button", "list", "shader", "input", "image", "icon"};
  static const char *const roles[] = {"box", "heading", "link", "image", "main", "nav", "switch", "slider", "menu", "menuitem", "button"};
  if (node.kind == wire::KindBox && node.role > RoleNone && node.role <= RoleButton) return roles[node.role];
  return kinds[node.kind < 8 ? node.kind : 0];
}

bool part_matches(const Node &node, const Selector::Part &part) {
  switch (part.kind) {
    case Selector::Part::Key: return node.key_name == part.value;
    case Selector::Part::Text: return node.text == part.value || node.label == part.value || node.placeholder == part.value;
    case Selector::Part::Contains: return contains(node.text, part.value) || contains(node.label, part.value) || contains(node.placeholder, part.value);
  }
  return false;
}

void skip_spaces(std::string_view &in) {
  while (!in.empty() && std::isspace(static_cast<unsigned char>(in.front()))) in.remove_prefix(1);
}

bool read_word(std::string_view &in, std::string &out, std::string &error) {
  if (!in.empty() && in.front() == '"') {
    in.remove_prefix(1);
    for (;;) {
      if (in.empty()) {
        error = "unterminated quote";
        return false;
      }
      char c = in.front();
      in.remove_prefix(1);
      if (c == '"') return true;
      if (c == '\\' && !in.empty()) {
        c = in.front();
        in.remove_prefix(1);
      }
      out += c;
    }
  }
  size_t n = 0;
  while (n < in.size() && !std::isspace(static_cast<unsigned char>(in[n])) && in[n] != ',' && in[n] != '[') ++n;
  out.assign(in.substr(0, n));
  in.remove_prefix(n);
  if (out.empty()) error = "a selector part needs a name after @ or ~";
  return !out.empty();
}

}  // namespace

bool parse_selector(std::string_view &in, Selector &out, std::string &error) {
  const std::string_view start = in;
  skip_spaces(in);
  while (!in.empty() && in.front() != ',') {
    Selector::Part part;
    const char lead = in.front();
    if (lead == '@') {
      in.remove_prefix(1);
      part.kind = Selector::Part::Key;
    } else if (lead == '~') {
      in.remove_prefix(1);
      part.kind = Selector::Part::Contains;
    } else if (lead == '"') {
      part.kind = Selector::Part::Text;
    } else {
      error = "a selector part starts with @key, \"text\" or ~text";
      return false;
    }
    if (!read_word(in, part.value, error)) return false;
    if (part.kind == Selector::Part::Contains) part.value = lower(part.value);
    if (!in.empty() && in.front() == '[') {
      const size_t close = in.find(']');
      int index = -1;
      if (close == std::string_view::npos || std::sscanf(std::string(in.substr(1, close - 1)).c_str(), "%d", &index) != 1 || index < 0) {
        error = "[n] needs a number from 0";
        return false;
      }
      part.index = index;
      in.remove_prefix(close + 1);
    }
    out.parts.push_back(std::move(part));
    skip_spaces(in);
  }
  if (out.parts.empty()) {
    error = "expected a selector";
    return false;
  }
  std::string_view used = start.substr(0, start.size() - in.size());
  while (!used.empty() && std::isspace(static_cast<unsigned char>(used.front()))) used.remove_prefix(1);
  while (!used.empty() && std::isspace(static_cast<unsigned char>(used.back()))) used.remove_suffix(1);
  out.source = used;
  return true;
}

std::vector<int32_t> select(const Engine &engine, const Selector &selector) {
  const auto &nodes = engine.nodes;
  std::vector<char> above;
  std::vector<int32_t> found;
  for (size_t p = 0; p < selector.parts.size(); ++p) {
    const Selector::Part &part = selector.parts[p];
    found.clear();
    for (size_t i = 1; i < nodes.size(); ++i) {
      if (!part_matches(nodes[i], part)) continue;
      bool inside = p == 0;
      for (int32_t a = nodes[i].parent; a > 0 && !inside; a = nodes[static_cast<size_t>(a)].parent) inside = above[static_cast<size_t>(a)] != 0;
      if (inside) found.push_back(static_cast<int32_t>(i));
    }
    if (part.index >= 0) {
      if (static_cast<size_t>(part.index) < found.size()) found = {found[static_cast<size_t>(part.index)]};
      else found.clear();
    }
    above.assign(nodes.size(), 0);
    for (const int32_t i : found) above[static_cast<size_t>(i)] = 1;
  }
  return found;
}

std::string selector_hint(const Engine &engine, const Selector &selector) {
  if (selector.parts.empty()) return {};
  const std::string want = lower(selector.parts.back().value);
  std::string similar, keys;
  std::unordered_set<std::string_view> seen;
  int similar_count = 0;
  for (size_t i = 1; i < engine.nodes.size(); ++i) {
    const Node &node = engine.nodes[i];
    if (node.shown.empty()) continue;
    if (similar_count < 8 && (contains(node.key_name, want) || contains(node.text, want) || contains(node.label, want))) {
      similar += "\n  " + engine.describe(static_cast<int32_t>(i));
      ++similar_count;
    }
    if (seen.size() < 40 && !node.key_name.empty() && seen.insert(node.key_name).second) keys += " " + key_ref(node.key_name);
  }
  if (!similar.empty()) return "similar on screen:" + similar;
  if (!keys.empty()) return "keys on screen:" + keys;
  return "no keyed nodes on screen";
}

std::string Engine::describe(int32_t index) const {
  const Node &node = nodes[static_cast<size_t>(index)];
  std::string out = kind_name(node);
  if (!node.key_name.empty()) out += " " + key_ref(node.key_name);
  if (node.placeholder_shown) out += " placeholder=" + quote(node.text);
  else if (!node.text.empty()) out += " " + quote(node.text);
  if (!node.label.empty() && node.label != node.text) out += " label=" + quote(node.label);
  if (node.hit) out += " hit";
  if (node.id == ui.focused && node.id) out += " focused";
  if (node.shown.empty()) return out + " hidden";
  char rect[96];
  const float s = dpr > 0 ? dpr : 1;
  std::snprintf(rect, sizeof(rect), " %.0f,%.0f %.0fx%.0f", std::floor(static_cast<float>(node.shown.x) / s), std::floor(static_cast<float>(node.shown.y) / s),
                std::round(static_cast<float>(node.shown.w) / s), std::round(static_cast<float>(node.shown.h) / s));
  return out + rect;
}

std::string Engine::tree_outline() const {
  std::vector<int> depth(nodes.size(), 0);
  std::string out;
  size_t hidden = 0;
  for (size_t i = 1; i < nodes.size(); ++i) {
    const Node &node = nodes[i];
    const int base = node.parent > 0 ? depth[static_cast<size_t>(node.parent)] : 0;
    depth[i] = base;
    const bool named = !node.key_name.empty() || !node.text.empty() || !node.label.empty() || node.hit || node.role || node.scroll || node.kind != wire::KindBox || (node.id && node.id == ui.focused);
    if (!named) continue;
    if (node.shown.empty()) {
      ++hidden;
      continue;
    }
    depth[i] = base + 1;
    out.append(static_cast<size_t>(base) * 2, ' ');
    out += describe(static_cast<int32_t>(i));
    out += '\n';
  }
  if (hidden) out += "(" + std::to_string(hidden) + " more clipped or off screen; QUI_TREE=x.json lists every node)\n";
  return out;
}

}  // namespace qui
