#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct hb_font_t;
struct FT_LibraryRec_;
struct FT_FaceRec_;
using FT_Library = FT_LibraryRec_ *;
using FT_Face = FT_FaceRec_ *;

namespace qui {

// Straight RGBA to premultiplied, in place.
void premultiply(std::vector<uint8_t> &pixels);

// Large pixel buffers cost tens of milliseconds to fault in on a phone, so
// scaled images and their uploads pass the same few buffers around instead of
// allocating one per frame. Safe to use from any thread.
std::vector<uint8_t> take_pixel_buffer(size_t bytes);
void give_pixel_buffer(std::vector<uint8_t> &&buffer);


using FontInstanceId = uint32_t;

struct FontMetrics {
  float ascender = 0.0f;
  float descender = 0.0f;
  float line_height = 0.0f;
  float cell_advance = 0.0f;
  // HarfBuzz positions for fixed-strike bitmap faces (color emoji) are in
  // strike pixels; multiply by this to get pixels at the requested size.
  float position_scale = 1.0f;
};

struct CachedGlyph {
  FontInstanceId font = 0;
  uint32_t glyph_id = 0;
  uint32_t atlas_page = 0;
  uint32_t atlas_x = 0;
  uint32_t atlas_y = 0;
  uint32_t bitmap_width = 0;
  uint32_t bitmap_height = 0;
  int32_t bitmap_left = 0;
  int32_t bitmap_top = 0;
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 0.0f;
  float v1 = 0.0f;
  float advance_x = 0.0f;
  bool has_bitmap = false;
  bool color_bitmap = false;
};

struct AtlasUpload {
  uint32_t atlas_page = 0;
  uint32_t destination_x = 0;
  uint32_t destination_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t page_width = 0;
  uint32_t page_height = 0;
  uint64_t staging_offset = 0;
  std::vector<uint8_t> pixels;
};

struct AtlasRegion {
  uint32_t page = 0;
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 0.0f;
  float v1 = 0.0f;
};

class FontManager {
 public:
  FontManager();
  ~FontManager();

  FontManager(const FontManager &) = delete;
  FontManager &operator=(const FontManager &) = delete;

  FontInstanceId load_font(const std::string &path, float pixel_size,
                           bool ligatures_enabled = true, bool subpixel = false);
  FontInstanceId load_font_face(const std::string &path, int face_index,
                                float pixel_size, bool ligatures_enabled = true,
                                bool subpixel = false, float weight = 0.0f,
                                bool oblique = false);
  bool valid_font(FontInstanceId id) const;
  hb_font_t *hb_font(FontInstanceId id) const;
  FT_Face ft_face(FontInstanceId id) const;
  const FontMetrics *metrics(FontInstanceId id) const;
  const std::string *font_path(FontInstanceId id) const;
  bool has_codepoint(FontInstanceId font, uint32_t codepoint) const;
  // emoji asks for a color font even when preferred covers the codepoint, for
  // a base character that a variation selector or keycap turns into an emoji.
  FontInstanceId font_for_codepoint(FontInstanceId preferred,
                                    uint32_t codepoint, bool emoji = false);

  bool subpixel(FontInstanceId id) const;
  const CachedGlyph *get_glyph(FontInstanceId font, uint32_t glyph_id,
                               uint32_t quarter_pixel = 0);
  bool upload_rgba(uint32_t width, uint32_t height,
                   const std::vector<uint8_t> &pixels, AtlasRegion &region);
  // Reserves atlas space that can be rewritten in place and returned, for images
  // that change or scroll away. Reuses a released region when one fits.
  bool allocate_rgba(uint32_t width, uint32_t height, AtlasRegion &region,
                     uint32_t &alloc_width, uint32_t &alloc_height);
  void write_rgba(const AtlasRegion &region, uint32_t width, uint32_t height,
                  const std::vector<uint8_t> &pixels, bool premultiplied = false);
  void write_rgba(const AtlasRegion &region, uint32_t width, uint32_t height,
                  std::vector<uint8_t> &&pixels, bool premultiplied);
  void release_rgba(const AtlasRegion &region, uint32_t alloc_width,
                    uint32_t alloc_height);

  const std::vector<AtlasUpload> &pending_uploads() const {
    return pending_uploads_;
  }
  void clear_pending_uploads();

  FT_Library ft_library() const { return library_; }

  uint32_t atlas_page_count() const {
    return static_cast<uint32_t>(pages_.size());
  }
  uint32_t page_width(uint32_t page) const { return pages_[page].width; }
  uint32_t page_height(uint32_t page) const { return pages_[page].height; }
  uint32_t atlas_width() const { return kDefaultAtlasSize; }
  uint32_t atlas_height() const { return kDefaultAtlasSize; }
  // The longest side allocate_rgba can place: a page less its padding and the
  // one-pixel border its shelves start after.
  static constexpr uint32_t max_rgba_side() { return kMaxAtlasSize - 2 * kAtlasPadding - 1; }

  uint64_t glyph_rasterizations() const { return glyph_rasterizations_; }
  uint64_t glyph_cache_hits() const { return glyph_cache_hits_; }
  uint64_t glyph_cache_misses() const { return glyph_cache_misses_; }
  // Time spent rasterizing glyphs since the last call, for frame diagnostics.
  double take_raster_ms() { return std::exchange(raster_ms_, 0.0); }

 private:
  static constexpr uint32_t kDefaultAtlasSize = 512;
  static constexpr uint32_t kMaxAtlasSize = 2048;
  static constexpr uint32_t kAtlasPadding = 2;

  struct FontInstance;
  struct AtlasPage {
    uint32_t cursor_x = 1;
    uint32_t cursor_y = 1;
    uint32_t shelf_height = 0;
    uint32_t width = kDefaultAtlasSize;
    uint32_t height = kDefaultAtlasSize;
  };
  struct RgbaUploadKey {
    uint64_t hash_a = 0;
    uint64_t hash_b = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    bool operator==(const RgbaUploadKey &) const = default;
  };
  struct RgbaUploadKeyHash {
    size_t operator()(const RgbaUploadKey &key) const {
      const uint64_t rotated = (key.hash_b << 27) | (key.hash_b >> 37);
      return static_cast<size_t>(
          key.hash_a ^ rotated ^ (static_cast<uint64_t>(key.width) << 32) ^
          key.height);
    }
  };

  FT_Library library_ = nullptr;
  std::vector<std::unique_ptr<FontInstance>> fonts_;
  std::vector<AtlasPage> pages_;
  struct FreeRegion {
    AtlasRegion region;
    uint32_t w = 0, h = 0;
  };
  std::vector<FreeRegion> free_rgba_;
  std::vector<AtlasUpload> pending_uploads_;
  std::unordered_map<RgbaUploadKey, AtlasRegion, RgbaUploadKeyHash>
      rgba_upload_cache_;
  std::unordered_map<uint64_t, FontInstanceId> fallback_fonts_;
  uint64_t glyph_rasterizations_ = 0;
  uint64_t glyph_cache_hits_ = 0;
  uint64_t glyph_cache_misses_ = 0;
  double raster_ms_ = 0.0;

  bool allocate_atlas_rect(uint32_t width, uint32_t height,
                           uint32_t &page, uint32_t &x, uint32_t &y);
};

}  // namespace qui
