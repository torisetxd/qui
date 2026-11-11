#pragma once

#include "engine.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace qui {

// A QUI_DRIVE selector: parts separated by spaces, each a descendant of the
// one before. A part is `@key`, `"exact text"` or `~substring` (any case),
// matched against the key, text, label or placeholder, and `[n]` after a part
// keeps only its nth match, counting from 0.
struct Selector {
  struct Part {
    enum Kind : uint8_t { Key, Text, Contains } kind = Key;
    std::string value;
    int index = -1;
  };
  std::vector<Part> parts;
  std::string source;
};

// Reads a selector from the front of `in`, up to a ',' outside quotes or the
// end, and leaves the rest in `in`.
bool parse_selector(std::string_view &in, Selector &out, std::string &error);

// The matching nodes, in tree order.
std::vector<int32_t> select(const Engine &engine, const Selector &selector);

// Visible nodes whose key or text resembles the last part of the selector, or
// failing that the keys on screen, to suggest what the selector missed.
std::string selector_hint(const Engine &engine, const Selector &selector);

}  // namespace qui
