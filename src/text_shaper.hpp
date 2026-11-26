#pragma once

#include "font_manager.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qui {

struct TextStyleSpan {
  uint32_t start_byte = 0;
  uint32_t end_byte = 0;
  uint32_t packed_rgba = 0xffffffffu;
  FontInstanceId font = 0;
  bool ligatures_enabled = true;
};

struct ShapedGlyph {
  FontInstanceId font = 0;
  uint32_t glyph_id = 0;
  uint32_t cluster_start_byte = 0;
  uint32_t cluster_end_byte = 0;
  float x = 0.0f;
  float y = 0.0f;
  float x_advance = 0.0f;
  float y_advance = 0.0f;
  float x_offset = 0.0f;
  float y_offset = 0.0f;
  uint32_t packed_rgba = 0xffffffffu;
  uint8_t level = 0;
};

struct ShapedLine {
  std::vector<ShapedGlyph> glyphs;
  float total_advance = 0.0f;
  float ascent = 0.0f;
  float descent = 0.0f;
  float line_height = 0.0f;
  bool has_rtl = false;
};

struct ShapingOptions {
  FontInstanceId default_font = 0;
  uint32_t default_color = 0xffffffffu;
  bool ligatures_enabled = true;
  float tracking = 0.0f;
};

class TextShaper {
 public:
  explicit TextShaper(FontManager &fonts) : fonts_(fonts) {}

  const ShapedLine &shape_line(std::string_view utf8,
                               std::span<const TextStyleSpan> styles,
                               ShapingOptions options);
  const ShapedLine &shape_line_single(std::string_view utf8,
                                      TextStyleSpan style,
                                      ShapingOptions options);


  void trim() {
    if (cache_.size() >= kMaximumCacheEntries || cache_bytes_ > kMaximumCacheBytes)
      clear_cache();
  }
  void clear_cache() {
    cache_.clear();
    cache_bytes_ = 0;
  }
  uint64_t shape_calls() const { return shape_calls_; }
  uint64_t cache_hits() const { return cache_hits_; }
  uint64_t cache_misses() const { return cache_misses_; }

 private:
  static constexpr size_t kMaximumCacheBytes = 8u * 1024u * 1024u;
  static constexpr size_t kMaximumCacheEntries = 4096;

  struct Run {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t packed_rgba = 0xffffffffu;
    FontInstanceId font = 0;
    bool ligatures_enabled = true;
  };

  FontManager &fonts_;
  std::unordered_map<std::string, ShapedLine> cache_;
  ShapedLine uncached_line_;
  // Scratch storage reused across calls so a cache hit (the common case for
  // static UI text redrawn every frame) allocates no heap memory.
  std::vector<Run> scratch_runs_;
  std::vector<uint8_t> scratch_levels_;
  std::string scratch_key_;
  float scratch_tracking_ = 0.0f;
  size_t cache_bytes_ = 0;
  uint64_t shape_calls_ = 0;
  uint64_t cache_hits_ = 0;
  uint64_t cache_misses_ = 0;

  void make_runs(std::vector<Run> &runs, uint32_t text_size,
                 std::span<const TextStyleSpan> styles, ShapingOptions options);
  static void make_cache_key(std::string &key, std::string_view text,
                             const std::vector<Run> &runs, float tracking);
  const ShapedLine &shape_from_runs(std::string_view utf8);
};

}  // namespace qui
