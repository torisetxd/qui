//go:build cgo

#include <chrono>
#include "font_manager.hpp"
#include "system_fonts.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_MULTIPLE_MASTERS_H
#include <hb-ft.h>
#include <hb-ot.h>
#if defined(__ANDROID__)
#include <hb-raster.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace qui {

namespace {

// HarfBuzz and FreeType must use the same grid-fitting mode. Otherwise a
// fixed-width face can advance by 9.59375px while its bitmap occupies a 10px
// cell, producing alternating gaps and crowded-looking letter pairs.
constexpr FT_Int32 kTextLoadFlags =
    FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL;

// Proportional text keeps unhinted horizontal metrics and is placed on quarter
// pixels, which is what evens out the letter spacing at 1x.
constexpr FT_Int32 kSubpixelLoadFlags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;

}  // namespace

struct FontManager::FontInstance {
  FontInstanceId id = 0;
  std::string path;
  FT_Face face = nullptr;
  hb_font_t *hb = nullptr;
  hb_font_t *paint_font = nullptr;
  float pixel_size = 0.0f;
  float weight = 0.0f;
  bool oblique = false;
  float raster_scale = 1.0f;
  int face_index = 0;
  bool ligatures_enabled = true;
  bool subpixel = false;
  FT_Int32 load_flags = kTextLoadFlags;
  FontMetrics metrics;
  std::unordered_map<uint32_t, CachedGlyph> glyphs;

  ~FontInstance() {
    if (paint_font)
      hb_font_destroy(paint_font);
    if (hb)
      hb_font_destroy(hb);
    if (face)
      FT_Done_Face(face);
  }
};

FontManager::FontManager() {
  init_system_fonts();
  if (FT_Init_FreeType(&library_) != 0) {
    std::fprintf(stderr, "font: FT_Init_FreeType failed\n");
    library_ = nullptr;
  }
  // Page zero is also used by solid UI instances. Its first texel is reserved.
  pages_.emplace_back();
}

FontManager::~FontManager() {
  fonts_.clear();
  if (library_)
    FT_Done_FreeType(library_);
}

FontInstanceId FontManager::load_font(const std::string &path, float pixel_size,
                                      bool ligatures_enabled, bool subpixel) {
  return load_font_face(path, 0, pixel_size, ligatures_enabled, subpixel);
}

FontInstanceId FontManager::load_font_face(const std::string &path,
                                           int face_index, float pixel_size,
                                           bool ligatures_enabled,
                                           bool subpixel, float weight, bool oblique) {
  if (!library_ || pixel_size <= 0.0f)
    return 0;

  for (const auto &font : fonts_) {
    if (font && font->path == path && font->face_index == face_index &&
        font->pixel_size == pixel_size && font->subpixel == subpixel &&
        font->weight == weight && font->oblique == oblique)
      return font->id;
  }

  auto instance = std::make_unique<FontInstance>();
  if (FT_New_Face(library_, path.c_str(), face_index, &instance->face) != 0) {
    std::fprintf(stderr, "font: failed to load '%s'\n", path.c_str());
    return 0;
  }
  FT_MM_Var *axes = nullptr;
  if (weight > 0.0f && FT_HAS_MULTIPLE_MASTERS(instance->face) &&
      FT_Get_MM_Var(instance->face, &axes) == 0) {
    std::vector<FT_Fixed> coords(axes->num_axis);
    for (FT_UInt axis = 0; axis < axes->num_axis; ++axis) {
      const bool is_weight = axes->axis[axis].tag == FT_MAKE_TAG('w', 'g', 'h', 't');
      coords[axis] = is_weight ? std::clamp(static_cast<FT_Fixed>(weight * 65536.0f), axes->axis[axis].minimum, axes->axis[axis].maximum)
                               : axes->axis[axis].def;
    }
    FT_Set_Var_Design_Coordinates(instance->face, axes->num_axis, coords.data());
    FT_Done_MM_Var(library_, axes);
  }
  if (FT_Set_Pixel_Sizes(instance->face, 0,
                         static_cast<FT_UInt>(pixel_size + 0.5f)) != 0) {
    int best_strike = -1;
    float best_distance = std::numeric_limits<float>::max();
    for (int index = 0; index < instance->face->num_fixed_sizes; ++index) {
      const float strike_size =
          instance->face->available_sizes[index].y_ppem / 64.0f;
      const float distance = std::abs(strike_size - pixel_size);
      if (distance < best_distance) {
        best_distance = distance;
        best_strike = index;
      }
    }
    if (best_strike < 0 || FT_Select_Size(instance->face, best_strike) != 0) {
      std::fprintf(stderr, "font: failed to set %.1fpx size for '%s'\n",
                   pixel_size, path.c_str());
      return 0;
    }
    const float strike_size =
        instance->face->available_sizes[best_strike].y_ppem / 64.0f;
    instance->raster_scale =
        strike_size > 0.0f ? pixel_size / strike_size : 1.0f;
  }

  instance->hb = hb_ft_font_create_referenced(instance->face);
  if (!instance->hb) {
    std::fprintf(stderr, "font: failed to create HarfBuzz font for '%s'\n",
                 path.c_str());
    return 0;
  }
  instance->subpixel = subpixel;
  instance->load_flags = subpixel ? kSubpixelLoadFlags : kTextLoadFlags;
  hb_ft_font_set_load_flags(instance->hb, instance->load_flags);
  // Leave HarfBuzz at the selected bitmap strike. Overriding its scale made
  // hb-ft rescale advances a second time, collapsing a 14px emoji's advance
  // to about 2px; the shaper applies position_scale instead.

#if defined(__ANDROID__)
  if (hb_ot_color_has_paint(hb_font_get_face(instance->hb))) {
    instance->paint_font = hb_font_create(hb_font_get_face(instance->hb));
    const int scale = static_cast<int>(pixel_size * 64.0f);
    hb_font_set_scale(instance->paint_font, scale, scale);
  }
#endif

  instance->id = static_cast<FontInstanceId>(fonts_.size() + 1);
  instance->path = path;
  instance->face_index = face_index;
  instance->pixel_size = pixel_size;
  instance->weight = weight;
  instance->oblique = oblique;
  instance->ligatures_enabled = ligatures_enabled;
  instance->metrics.ascender =
      instance->face->size->metrics.ascender / 64.0f * instance->raster_scale;
  instance->metrics.descender =
      instance->face->size->metrics.descender / 64.0f * instance->raster_scale;
  instance->metrics.line_height =
      instance->face->size->metrics.height / 64.0f * instance->raster_scale;
  // The terminal grid is based on the advance of a normal printable glyph.
  // max_advance also includes unusually wide symbols and makes every terminal
  // column far too wide for otherwise-monospace fonts.
  if (FT_Load_Char(instance->face, 'M', FT_LOAD_DEFAULT) == 0)
    instance->metrics.cell_advance =
        instance->face->glyph->advance.x / 64.0f * instance->raster_scale;
  else
    instance->metrics.cell_advance =
        instance->face->size->metrics.max_advance / 64.0f *
        instance->raster_scale;
  instance->metrics.position_scale = instance->raster_scale;

  fonts_.push_back(std::move(instance));
  return fonts_.back()->id;
}

bool FontManager::valid_font(FontInstanceId id) const {
  return id != 0 && id <= fonts_.size() && fonts_[id - 1] != nullptr;
}

hb_font_t *FontManager::hb_font(FontInstanceId id) const {
  return valid_font(id) ? fonts_[id - 1]->hb : nullptr;
}

FT_Face FontManager::ft_face(FontInstanceId id) const {
  return valid_font(id) ? fonts_[id - 1]->face : nullptr;
}

const FontMetrics *FontManager::metrics(FontInstanceId id) const {
  return valid_font(id) ? &fonts_[id - 1]->metrics : nullptr;
}

const std::string *FontManager::font_path(FontInstanceId id) const {
  return valid_font(id) ? &fonts_[id - 1]->path : nullptr;
}

bool FontManager::subpixel(FontInstanceId id) const {
  return valid_font(id) && fonts_[id - 1]->subpixel;
}

bool FontManager::has_codepoint(FontInstanceId font,
                                uint32_t codepoint) const {
  return valid_font(font) &&
         FT_Get_Char_Index(fonts_[font - 1]->face, codepoint) != 0;
}

namespace {

// FreeType exposes COLR v1 glyphs but does not rasterize them.
bool paint_color_glyph(hb_font_t *font, uint32_t glyph, std::vector<uint8_t> &pixels,
                       FT_Bitmap &bitmap, int &left, int &top) {
#if defined(__ANDROID__)
  if (!hb_ot_color_glyph_has_paint(hb_font_get_face(font), glyph))
    return false;
  static hb_raster_paint_t *const paint = hb_raster_paint_create_or_fail();
  hb_glyph_extents_t extents{};
  if (!paint || !hb_font_get_glyph_extents(font, glyph, &extents))
    return false;
  hb_raster_paint_set_scale_factor(paint, 64.0f, 64.0f);
  hb_raster_paint_set_transform(paint, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
  if (!hb_raster_paint_set_glyph_extents(paint, &extents))
    return false;
  hb_raster_paint_glyph(paint, font, glyph);
  hb_raster_image_t *image = hb_raster_paint_render(paint);
  if (!image)
    return false;
  hb_raster_extents_t area{};
  hb_raster_image_get_extents(image, &area);
  pixels.resize(static_cast<size_t>(area.height) * area.stride);
  const uint8_t *source = hb_raster_image_get_buffer(image);
  for (unsigned row = 0; row < area.height; ++row)
    std::memcpy(pixels.data() + (area.height - 1 - row) * area.stride,
                source + row * area.stride, area.stride);
  hb_raster_paint_recycle_image(paint, image);
  bitmap.width = area.width;
  bitmap.rows = area.height;
  bitmap.pitch = static_cast<int>(area.stride);
  bitmap.buffer = pixels.data();
  bitmap.pixel_mode = FT_PIXEL_MODE_BGRA;
  left = area.x_origin;
  top = area.y_origin + static_cast<int>(area.height);
  return true;
#else
  (void)font, (void)glyph, (void)pixels, (void)bitmap, (void)left, (void)top;
  return false;
#endif
}

bool emoji_presentation(uint32_t codepoint) {
  return (codepoint >= 0x1f000 && codepoint <= 0x1faff) ||
         (codepoint >= 0x2600 && codepoint <= 0x27ff) ||
         (codepoint >= 0x2300 && codepoint <= 0x23ff);
}

}  // namespace

FontInstanceId FontManager::font_for_codepoint(FontInstanceId preferred,
                                                uint32_t codepoint, bool emoji) {
  if (!valid_font(preferred) || codepoint > 0x10ffff)
    return preferred;
  if (has_codepoint(preferred, codepoint) &&
      (!emoji || FT_HAS_COLOR(fonts_[preferred - 1]->face)))
    return preferred;

  const uint64_t key = (static_cast<uint64_t>(preferred) << 32) | codepoint |
                       (emoji ? 0x80000000u : 0u);
  const auto cached = fallback_fonts_.find(key);
  if (cached != fallback_fonts_.end())
    return cached->second;

  const float pixel_size = fonts_[preferred - 1]->pixel_size;
  const bool ligatures_enabled = fonts_[preferred - 1]->ligatures_enabled;
  const bool subpixel_text = fonts_[preferred - 1]->subpixel;
  FontInstanceId result = 0;
  for (const SystemFont &font : fallback_fonts(codepoint, emoji || emoji_presentation(codepoint))) {
    if (font.path.empty())
      continue;
    const FontInstanceId candidate =
        load_font_face(font.path, font.index, pixel_size, ligatures_enabled,
                       subpixel_text, font.weight);
    if (candidate && has_codepoint(candidate, codepoint) &&
        (!emoji || FT_HAS_COLOR(fonts_[candidate - 1]->face))) {
      result = candidate;
      break;
    }
  }
  if (!result)
    result = emoji ? font_for_codepoint(preferred, codepoint) : preferred;
  fallback_fonts_[key] = result;
  return result;
}

bool FontManager::allocate_atlas_rect(uint32_t width, uint32_t height,
                                      uint32_t &page, uint32_t &x,
                                      uint32_t &y) {
  if (width + kAtlasPadding * 2 > kMaxAtlasSize ||
      height + kAtlasPadding * 2 > kMaxAtlasSize)
    return false;

  const uint32_t required_width = width + kAtlasPadding * 2;
  const uint32_t required_height = height + kAtlasPadding * 2;
  const auto page_size_for = [](uint32_t rw, uint32_t rh) {
    uint32_t size = kDefaultAtlasSize;
    while ((size < rw || size < rh) && size < kMaxAtlasSize)
      size *= 2;
    return size;
  };
  for (;;) {
    AtlasPage &current = pages_.back();
    if (required_width > current.width || required_height > current.height) {
      const uint32_t size = page_size_for(required_width, required_height);
      pages_.push_back({1, 1, 0, size, size});
      continue;
    }
    if (current.cursor_x + required_width > current.width) {
      current.cursor_x = 1;
      current.cursor_y += current.shelf_height;
      current.shelf_height = 0;
    }
    if (current.cursor_y + required_height <= current.height) {
      page = static_cast<uint32_t>(pages_.size() - 1);
      x = current.cursor_x + kAtlasPadding;
      y = current.cursor_y + kAtlasPadding;
      current.cursor_x += required_width;
      current.shelf_height = std::max(current.shelf_height, required_height);
      return true;
    }
    const uint32_t size = page_size_for(required_width, required_height);
    pages_.push_back({1, 1, 0, size, size});
  }
}

bool FontManager::upload_rgba(uint32_t width, uint32_t height,
                              const std::vector<uint8_t> &pixels,
                              AtlasRegion &region) {
  const size_t expected = static_cast<size_t>(width) * height * 4;
  if (width == 0 || height == 0 || pixels.size() != expected)
    return false;

  // Terminal image clients commonly delete and retransmit frames under the
  // same Kitty image id. The logical image disappears, but atlas allocations
  // are immutable, so allocating on every animation tick eventually exhausts
  // device memory. Cache by image content so a finite animation uploads each
  // distinct frame once and then reuses its atlas region.
  RgbaUploadKey key;
  key.width = width;
  key.height = height;
  key.hash_a = 1469598103934665603ull;
  key.hash_b = 0x9e3779b97f4a7c15ull;
  for (const uint8_t byte : pixels) {
    key.hash_a = (key.hash_a ^ byte) * 1099511628211ull;
    key.hash_b ^= static_cast<uint64_t>(byte) + 0x9e3779b97f4a7c15ull +
                  (key.hash_b << 6) + (key.hash_b >> 2);
  }
  if (const auto cached = rgba_upload_cache_.find(key);
      cached != rgba_upload_cache_.end()) {
    region = cached->second;
    return true;
  }

  uint32_t page = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  if (!allocate_atlas_rect(width, height, page, x, y))
    return false;

  AtlasUpload upload;
  upload.atlas_page = page;
  upload.destination_x = x;
  upload.destination_y = y;
  upload.width = width;
  upload.height = height;
  upload.page_width = pages_[page].width;
  upload.page_height = pages_[page].height;
  upload.pixels = pixels;
  // The color-bitmap shader expects premultiplied atlas samples.
  premultiply(upload.pixels);
  pending_uploads_.push_back(std::move(upload));
  region.page = page;
  const float page_w = static_cast<float>(pages_[page].width);
  const float page_h = static_cast<float>(pages_[page].height);
  region.u0 = static_cast<float>(x) / page_w;
  region.v0 = static_cast<float>(y) / page_h;
  region.u1 = static_cast<float>(x + width) / page_w;
  region.v1 = static_cast<float>(y + height) / page_h;
  rgba_upload_cache_.emplace(key, region);
  return true;
}

bool FontManager::allocate_rgba(uint32_t width, uint32_t height, AtlasRegion &region, uint32_t &alloc_width, uint32_t &alloc_height) {
  if (width == 0 || height == 0) return false;
  auto best = free_rgba_.end();
  for (auto it = free_rgba_.begin(); it != free_rgba_.end(); ++it) {
    if (it->w < width || it->h < height) continue;
    if (static_cast<uint64_t>(it->w) * it->h > static_cast<uint64_t>(width) * height * 2) continue;
    if (best == free_rgba_.end() || it->w * it->h < best->w * best->h) best = it;
  }
  const auto fill = [&](uint32_t page, uint32_t x, uint32_t y) {
    const float page_w = static_cast<float>(pages_[page].width);
    const float page_h = static_cast<float>(pages_[page].height);
    region.page = page;
    region.u0 = static_cast<float>(x) / page_w;
    region.v0 = static_cast<float>(y) / page_h;
    region.u1 = static_cast<float>(x + width) / page_w;
    region.v1 = static_cast<float>(y + height) / page_h;
  };
  if (best != free_rgba_.end()) {
    const uint32_t page = best->region.page;
    const uint32_t x = static_cast<uint32_t>(std::lround(best->region.u0 * static_cast<float>(pages_[page].width)));
    const uint32_t y = static_cast<uint32_t>(std::lround(best->region.v0 * static_cast<float>(pages_[page].height)));
    alloc_width = best->w;
    alloc_height = best->h;
    free_rgba_.erase(best);
    fill(page, x, y);
    return true;
  }
  uint32_t page = 0, x = 0, y = 0;
  if (!allocate_atlas_rect(width, height, page, x, y)) return false;
  alloc_width = width;
  alloc_height = height;
  fill(page, x, y);
  return true;
}

void FontManager::release_rgba(const AtlasRegion &region, uint32_t alloc_width, uint32_t alloc_height) {
  free_rgba_.push_back({region, alloc_width, alloc_height});
}

void premultiply(std::vector<uint8_t> &pixels) {
  for (size_t at = 0; at + 3 < pixels.size(); at += 4) {
    const uint32_t alpha = pixels[at + 3];
    if (alpha == 255) continue;
    pixels[at] = static_cast<uint8_t>((pixels[at] * alpha + 127) / 255);
    pixels[at + 1] = static_cast<uint8_t>((pixels[at + 1] * alpha + 127) / 255);
    pixels[at + 2] = static_cast<uint8_t>((pixels[at + 2] * alpha + 127) / 255);
  }
}

void FontManager::write_rgba(const AtlasRegion &region, uint32_t width, uint32_t height, const std::vector<uint8_t> &pixels, bool premultiplied) {
  if (pixels.size() != static_cast<size_t>(width) * height * 4) return;
  AtlasUpload upload;
  upload.atlas_page = region.page;
  upload.page_width = pages_[region.page].width;
  upload.page_height = pages_[region.page].height;
  upload.destination_x = static_cast<uint32_t>(std::lround(region.u0 * static_cast<float>(upload.page_width)));
  upload.destination_y = static_cast<uint32_t>(std::lround(region.v0 * static_cast<float>(upload.page_height)));
  upload.width = width;
  upload.height = height;
  upload.pixels = pixels;
  if (!premultiplied) premultiply(upload.pixels);
  pending_uploads_.push_back(std::move(upload));
}

const CachedGlyph *FontManager::get_glyph(FontInstanceId font,
                                          uint32_t glyph_id,
                                          uint32_t quarter_pixel) {
  if (!valid_font(font))
    return nullptr;
  FontInstance &instance = *fonts_[font - 1];
  const uint32_t cache_key = glyph_id | (quarter_pixel << 24);
  const auto existing = instance.glyphs.find(cache_key);
  if (existing != instance.glyphs.end()) {
    ++glyph_cache_hits_;
    return &existing->second;
  }
  ++glyph_cache_misses_;
  struct RasterTimer {
    double &total;
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    ~RasterTimer() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(); }
  } raster_timer{raster_ms_};

  CachedGlyph glyph;
  glyph.font = font;
  glyph.glyph_id = glyph_id;
  FT_Face face = instance.face;
  const FT_Int32 load_flags = instance.load_flags | FT_LOAD_COLOR;
  if (instance.oblique) {
    FT_Matrix slant{0x10000, 0x3600, 0, 0x10000};
    FT_Set_Transform(face, &slant, nullptr);
  }
  if (FT_Load_Glyph(face, glyph_id, load_flags) != 0) {
    std::fprintf(stderr, "font: FT_Load_Glyph failed for glyph %u\n",
                 static_cast<unsigned>(glyph_id));
    return nullptr;
  }
  glyph.advance_x =
      face->glyph->advance.x / 64.0f * instance.raster_scale;

  if (quarter_pixel != 0 && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
    FT_Outline_Translate(&face->glyph->outline,
                         static_cast<FT_Pos>(quarter_pixel) * 16, 0);

  std::vector<uint8_t> painted;
  FT_Bitmap painted_bitmap{};
  int painted_left = 0, painted_top = 0;
  const bool use_paint = instance.paint_font &&
      paint_color_glyph(instance.paint_font, glyph_id, painted, painted_bitmap,
                        painted_left, painted_top);
  if (!use_paint && FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
    std::fprintf(stderr, "font: FT_Render_Glyph failed for glyph %u\n",
                 static_cast<unsigned>(glyph_id));
    return nullptr;
  }
  const FT_Bitmap &bitmap = use_paint ? painted_bitmap : face->glyph->bitmap;
  if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
      bitmap.pixel_mode != FT_PIXEL_MODE_BGRA &&
      bitmap.pixel_mode != FT_PIXEL_MODE_MONO) {
    std::fprintf(stderr, "font: unsupported bitmap mode %u for glyph %u\n",
                 static_cast<unsigned>(bitmap.pixel_mode),
                 static_cast<unsigned>(glyph_id));
    return nullptr;
  }

  glyph.bitmap_width = static_cast<uint32_t>(
      std::lround(bitmap.width * instance.raster_scale));
  glyph.bitmap_height = static_cast<uint32_t>(
      std::lround(bitmap.rows * instance.raster_scale));
  glyph.bitmap_left = static_cast<int32_t>(
      std::lround((use_paint ? painted_left : face->glyph->bitmap_left) * instance.raster_scale));
  glyph.bitmap_top = static_cast<int32_t>(
      std::lround((use_paint ? painted_top : face->glyph->bitmap_top) * instance.raster_scale));
  glyph.has_bitmap = bitmap.width != 0 && bitmap.rows != 0;
  glyph.color_bitmap = bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;

  if (glyph.has_bitmap) {
    uint32_t page = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    if (!allocate_atlas_rect(glyph.bitmap_width, glyph.bitmap_height, page, x,
                             y)) {
      std::fprintf(stderr, "font: glyph %u is too large for a %ux%u atlas\n",
                   static_cast<unsigned>(glyph_id), kMaxAtlasSize, kMaxAtlasSize);
      return nullptr;
    }
    glyph.atlas_page = page;
    glyph.atlas_x = x;
    glyph.atlas_y = y;
    const float page_w = static_cast<float>(pages_[page].width);
    const float page_h = static_cast<float>(pages_[page].height);
    glyph.u0 = static_cast<float>(x) / page_w;
    glyph.v0 = static_cast<float>(y) / page_h;
    glyph.u1 = static_cast<float>(x + glyph.bitmap_width) / page_w;
    glyph.v1 = static_cast<float>(y + glyph.bitmap_height) / page_h;

    AtlasUpload upload;
    upload.atlas_page = page;
    upload.destination_x = x;
    upload.destination_y = y;
    upload.width = glyph.bitmap_width;
    upload.height = glyph.bitmap_height;
    upload.page_width = pages_[page].width;
    upload.page_height = pages_[page].height;
    upload.pixels.resize(static_cast<size_t>(upload.width) * upload.height * 4);

    const int pitch = bitmap.pitch;
    const uint8_t *first_row = bitmap.buffer;
    if (pitch < 0)
      first_row += static_cast<size_t>(bitmap.rows - 1) *
                   static_cast<size_t>(-pitch);
    auto source_pixel = [&](unsigned source_x, unsigned source_y,
                            unsigned channel) -> uint8_t {
      const uint8_t *source =
          first_row + static_cast<ptrdiff_t>(source_y) * pitch;
      if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        constexpr unsigned rgba_to_bgra[] = {2, 1, 0, 3};
        return source[source_x * 4 + rgba_to_bgra[channel]];
      }
      if (channel < 3)
        return 255;
      if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
        return (source[source_x >> 3] & (0x80u >> (source_x & 7u))) ? 255
                                                                    : 0;
      return source[source_x];
    };
    for (uint32_t row = 0; row < upload.height; ++row) {
      uint8_t *destination =
          upload.pixels.data() + static_cast<size_t>(row) * upload.width * 4;
      for (uint32_t column = 0; column < upload.width; ++column) {
        const unsigned source_x0 = std::min<unsigned>(
            bitmap.width - 1,
            static_cast<unsigned>(column / instance.raster_scale));
        const unsigned source_y0 = std::min<unsigned>(
            bitmap.rows - 1,
            static_cast<unsigned>(row / instance.raster_scale));
        const unsigned source_x1 = std::min<unsigned>(
            bitmap.width,
            std::max(source_x0 + 1,
                     static_cast<unsigned>(
                         std::ceil((column + 1) / instance.raster_scale))));
        const unsigned source_y1 = std::min<unsigned>(
            bitmap.rows,
            std::max(source_y0 + 1,
                     static_cast<unsigned>(
                         std::ceil((row + 1) / instance.raster_scale))));
        const uint64_t sample_count =
            static_cast<uint64_t>(source_x1 - source_x0) *
            (source_y1 - source_y0);
        for (unsigned channel = 0; channel < 4; ++channel) {
          uint64_t sum = 0;
          for (unsigned source_y = source_y0; source_y < source_y1;
               ++source_y) {
            for (unsigned source_x = source_x0; source_x < source_x1;
                 ++source_x)
              sum += source_pixel(source_x, source_y, channel);
          }
          destination[column * 4 + channel] =
              static_cast<uint8_t>((sum + sample_count / 2) / sample_count);
        }
      }
    }
    pending_uploads_.push_back(std::move(upload));
    ++glyph_rasterizations_;
  }

  auto [it, inserted] = instance.glyphs.emplace(cache_key, glyph);
  if (!inserted)
    return &it->second;
  return &it->second;
}

}  // namespace qui
