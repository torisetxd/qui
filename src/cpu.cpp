//go:build cgo

#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qui {

namespace {

inline uint32_t mul(uint32_t channel, uint32_t coverage) {
  const uint32_t value = channel * coverage + 128;
  return (value + (value >> 8)) >> 8;
}

// The four channels of a pixel sit in 16-bit lanes of one 64-bit word, so a
// single multiply scales all of them with the same rounding as mul().
constexpr uint64_t kLanes = 0x00ff00ff00ff00ffull;

inline uint64_t spread(uint32_t pixel) { return (pixel | (static_cast<uint64_t>(pixel) << 24)) & kLanes; }

inline uint32_t pack(uint64_t lanes) { return static_cast<uint32_t>(lanes | (lanes >> 24)); }

inline uint64_t mul_lanes(uint64_t lanes, uint32_t factor) {
  const uint64_t value = lanes * factor + 0x0080008000800080ull;
  return ((value + ((value >> 8) & kLanes)) >> 8) & kLanes;
}

inline uint32_t over(uint32_t src, uint32_t dst) { return src + pack(mul_lanes(spread(dst), 255 - (src >> 24))); }

inline uint32_t premultiply(uint32_t rgba, uint32_t coverage) {
  const uint32_t a = mul(rgba >> 24, coverage);
  return pack(mul_lanes(spread(rgba & 0x00ffffffu), a)) | (a << 24);
}

typedef uint32_t Quad __attribute__((vector_size(16)));

inline void blend_span(uint32_t *dst, int count, uint32_t src) {
  if ((src >> 24) == 255) {
    std::fill_n(dst, count, src);
    return;
  }
  const uint32_t inverse = 255 - (src >> 24);
  int i = 0;
  // Four pixels at a time, with the same rounding as over().
  for (; i + 4 <= count; i += 4) {
    Quad d;
    std::memcpy(&d, dst + i, sizeof(d));
    Quad rb = (d & 0x00ff00ffu) * inverse + 0x00800080u;
    Quad ga = ((d >> 8) & 0x00ff00ffu) * inverse + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    ga = (ga + ((ga >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
    d = src + (rb | ga);
    std::memcpy(dst + i, &d, sizeof(d));
  }
  for (; i < count; ++i) dst[i] = over(src, dst[i]);
}

inline float smooth_coverage(float dist) {
  const float t = std::clamp(0.5f - dist, 0.f, 1.f);
  return t * t * (3 - 2 * t);
}

inline float rounded_distance(float px, float py, float half_w, float half_h, float r) {
  const float qx = std::abs(px - half_w) - half_w + r;
  const float qy = std::abs(py - half_h) - half_h + r;
  const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}

struct Premul {
  float r, g, b, a;
};

inline Premul unpack(uint32_t rgba) {
  const float a = static_cast<float>(rgba >> 24) / 255;
  return {static_cast<float>(rgba & 0xff) / 255 * a, static_cast<float>((rgba >> 8) & 0xff) / 255 * a,
          static_cast<float>((rgba >> 16) & 0xff) / 255 * a, a};
}

inline Premul unpack_end(const Instance &inst) {
  const auto high = static_cast<uint32_t>(inst.u1), low = static_cast<uint32_t>(inst.v1);
  return unpack((high >> 8) | ((high & 0xff) << 8) | ((low >> 8) << 16) | ((low & 0xff) << 24));
}

inline uint32_t mixed_pixel(const Premul &from, const Premul &to, float t, float cover) {
  const auto channel = [&](float a, float b) { return static_cast<uint32_t>((a + (b - a) * t) * cover * 255 + 0.5f); };
  return channel(from.r, to.r) | (channel(from.g, to.g) << 8) | (channel(from.b, to.b) << 16) | (channel(from.a, to.a) << 24);
}

class Cpu final : public CpuBackend {
 public:
  void resize(int width, int height) override {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    pixels_.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff000000u);
  }

  void upload(const AtlasUpload &upload) override {
    if (upload.atlas_page >= pages_.size()) pages_.resize(upload.atlas_page + 1);
    Page &page = pages_[upload.atlas_page];
    if (page.width != upload.page_width) {
      page.width = upload.page_width;
      page.height = upload.page_height;
      page.pixels.assign(static_cast<size_t>(page.width) * page.height * 4, 0);
    }
    for (uint32_t y = 0; y < upload.height; ++y) {
      std::memcpy(page.pixels.data() + (static_cast<size_t>(upload.destination_y + y) * page.width + upload.destination_x) * 4,
                  upload.pixels.data() + static_cast<size_t>(y) * upload.width * 4, upload.width * 4u);
    }
  }

  void draw(const FrameData &frame, IRect damage, double) override {
    if (damage.empty()) return;
    const uint32_t clear = __builtin_bswap32(frame.clear);
    for (int y = damage.y; y < damage.y + damage.h; ++y)
      std::fill_n(pixels_.data() + static_cast<size_t>(y) * width_ + damage.x, damage.w, clear);

    for (const Batch &batch : frame.batches) {
      if (batch.shader >= 0) continue;
      const IRect clip = intersect(batch.clip, damage);
      if (clip.empty()) continue;
      saved_.clear();
      each_corner_pixel(batch.round, clip, [&](int, int, uint32_t &pixel) { saved_.push_back(pixel); });
      for (uint32_t i = batch.first; i < batch.first + batch.count; ++i) {
        const Instance &inst = frame.instances[i];
        if (inst.flags & InstShadow) {
          shadow(inst, clip);
        } else if (inst.flags & InstSolid) {
          if (inst.flags & InstGradient)
            gradient(inst, clip);
          else if (inst.flags & InstRounded)
            rounded(inst, clip);
          else
            solid(inst, clip);
        } else {
          glyph(inst, clip, batch.page);
        }
      }
      size_t next = 0;
      const float half_w = static_cast<float>(batch.round.w) / 2, half_h = static_cast<float>(batch.round.h) / 2;
      const float radius = std::min({batch.round.radius, half_w, half_h});
      each_corner_pixel(batch.round, clip, [&](int x, int y, uint32_t &pixel) {
        const float dist = rounded_distance(static_cast<float>(x - batch.round.x) + 0.5f, static_cast<float>(y - batch.round.y) + 0.5f, half_w, half_h, radius);
        const auto cover = static_cast<uint32_t>(smooth_coverage(dist) * 255 + 0.5f);
        const uint32_t before = saved_[next++];
        if (cover == 255) return;
        uint32_t mixed = 0;
        for (const uint32_t shift : {0u, 8u, 16u, 24u})
          mixed |= (mul((pixel >> shift) & 0xff, cover) + mul((before >> shift) & 0xff, 255 - cover)) << shift;
        pixel = mixed;
      });
    }
  }

  void read_pixels(std::vector<uint32_t> &out) override { out = pixels_; }

  const uint32_t *pixels() const override { return pixels_.data(); }

 private:
  struct Page {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
  };

  template <typename Visit>
  void each_corner_pixel(const RoundClip &round, IRect clip, Visit &&visit) {
    if (round.radius <= 0) return;
    const int size = std::min(static_cast<int>(std::ceil(round.radius)) + 1, (std::min(round.w, round.h) + 1) / 2);
    const IRect corners[4] = {{round.x, round.y, size, size},
                              {round.x + round.w - size, round.y, size, size},
                              {round.x, round.y + round.h - size, size, size},
                              {round.x + round.w - size, round.y + round.h - size, size, size}};
    for (const IRect &corner : corners) {
      const IRect r = intersect(corner, clip);
      if (r.empty()) continue;
      for (int y = r.y; y < r.y + r.h; ++y) {
        uint32_t *row = pixels_.data() + static_cast<size_t>(y) * width_;
        for (int x = r.x; x < r.x + r.w; ++x) visit(x, y, row[x]);
      }
    }
  }

  void solid(const Instance &inst, IRect clip) {
    const IRect r = intersect({static_cast<int>(inst.x), static_cast<int>(inst.y), static_cast<int>(inst.w), static_cast<int>(inst.h)}, clip);
    if (r.empty()) return;
    const uint32_t src = premultiply(inst.rgba, 255);
    for (int y = r.y; y < r.y + r.h; ++y) blend_span(pixels_.data() + static_cast<size_t>(y) * width_ + r.x, r.w, src);
  }

  void gradient(const Instance &inst, IRect clip) {
    const int x0 = static_cast<int>(inst.x), y0 = static_cast<int>(inst.y);
    const IRect r = intersect({x0, y0, static_cast<int>(inst.w), static_cast<int>(inst.h)}, clip);
    if (r.empty()) return;
    const Premul from = unpack(inst.rgba), to = unpack_end(inst);
    const float s = std::sin(inst.v0), c = std::cos(inst.v0);
    const float length = std::max(1.f, std::abs(inst.w * s) + std::abs(inst.h * c));
    const float half_w = inst.w / 2, half_h = inst.h / 2;
    const float radius = std::min({inst.u0, half_w, half_h});
    const bool is_rounded = (inst.flags & InstRounded) != 0;
    for (int y = r.y; y < r.y + r.h; ++y) {
      uint32_t *row = pixels_.data() + static_cast<size_t>(y) * width_;
      const float py = static_cast<float>(y - y0) + 0.5f;
      for (int x = r.x; x < r.x + r.w; ++x) {
        const float px = static_cast<float>(x - x0) + 0.5f;
        const float cover = is_rounded ? smooth_coverage(rounded_distance(px, py, half_w, half_h, radius)) : 1.f;
        if (cover <= 0) continue;
        const float t = std::clamp(((px - half_w) * s - (py - half_h) * c) / length + 0.5f, 0.f, 1.f);
        row[x] = over(mixed_pixel(from, to, t, cover), row[x]);
      }
    }
  }

  void shadow(const Instance &inst, IRect clip) {
    const int x0 = static_cast<int>(inst.x), y0 = static_cast<int>(inst.y);
    const IRect r = intersect({x0, y0, static_cast<int>(inst.w), static_cast<int>(inst.h)}, clip);
    if (r.empty()) return;
    const float blur = inst.v0, offset = inst.v1;
    const float half_w = inst.w / 2 - blur, half_h = inst.h / 2 - blur;
    const float radius = std::min({inst.u0, half_w, half_h});
    // Pixels well inside the box are hidden by it, so each row skips them.
    const float margin = std::max(radius, 1.f) + 1;
    const int skip_from = x0 + static_cast<int>(std::ceil(blur + margin));
    const int skip_to = x0 + static_cast<int>(std::floor(blur + half_w * 2 - margin));
    for (int y = r.y; y < r.y + r.h; ++y) {
      uint32_t *row = pixels_.data() + static_cast<size_t>(y) * width_;
      const float py = static_cast<float>(y - y0) + 0.5f;
      const float box_y = py - blur + offset;
      const bool skips = skip_to > skip_from && box_y >= margin && box_y <= half_h * 2 - margin;
      for (int x = r.x; x < r.x + r.w; ++x) {
        if (skips && x >= skip_from && x < skip_to) {
          x = skip_to - 1;
          continue;
        }
        const float px = static_cast<float>(x - x0) + 0.5f;
        const float inside = rounded_distance(px - blur, py - blur + offset, half_w, half_h, radius);
        if (inside < -0.5f) continue;
        const float edge = std::clamp((rounded_distance(px - blur, py - blur, half_w, half_h, radius) + blur) / (2 * blur), 0.f, 1.f);
        const float cover = (1 - edge * edge * (3 - 2 * edge)) * smooth_coverage(-inside);
        if (cover <= 0) continue;
        row[x] = over(premultiply(inst.rgba, static_cast<uint32_t>(cover * 255 + 0.5f)), row[x]);
      }
    }
  }

  void rounded(const Instance &inst, IRect clip) {
    const int x0 = static_cast<int>(inst.x), y0 = static_cast<int>(inst.y);
    const int w = static_cast<int>(inst.w), h = static_cast<int>(inst.h);
    const IRect r = intersect({x0, y0, w, h}, clip);
    if (r.empty()) return;
    const float half_w = inst.w / 2, half_h = inst.h / 2;
    const float radius = std::min({inst.u0, half_w, half_h});
    const float stroke = inst.v0;
    const int corner = static_cast<int>(std::ceil(radius)) + 1;
    const int band = stroke > 0 ? static_cast<int>(std::ceil(stroke)) + 1 : 0;
    const int inset = std::max(corner, band);
    const uint32_t solid_src = premultiply(inst.rgba, 255);

    const auto shade = [&](int px, int py) {
      const float dist = rounded_distance(static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f, half_w, half_h, radius);
      const float cover = stroke > 0 ? smooth_coverage(std::abs(dist + stroke / 2) - stroke / 2) : smooth_coverage(dist);
      return static_cast<uint32_t>(cover * 255 + 0.5f);
    };

    // Past the corners coverage only varies along one axis: the middle of an
    // edge row shares one value, and so does each side column of the rows
    // between them.
    const bool split = w > inset * 2;
    const int left = split ? inset : w, right = split ? w - inset : w;
    const int from = r.x - x0, to = r.x + r.w - x0;
    if (h > inset * 2) {
      columns_.resize(static_cast<size_t>(left + w - right));
      for (int px = 0; px < left; ++px) columns_[static_cast<size_t>(px)] = static_cast<uint8_t>(shade(px, inset));
      for (int px = right; px < w; ++px) columns_[static_cast<size_t>(left + px - right)] = static_cast<uint8_t>(shade(px, inset));
    }

    for (int y = r.y; y < r.y + r.h; ++y) {
      const int py = y - y0;
      const bool edge_row = py < inset || py >= h - inset;
      uint32_t *row = pixels_.data() + static_cast<size_t>(y) * width_;
      const auto sides = [&](int begin, int end, int first) {
        for (int px = std::max(begin, from); px < std::min(end, to); ++px) {
          const uint32_t cover = edge_row ? shade(px, py) : columns_[static_cast<size_t>(first + px - begin)];
          if (cover) row[x0 + px] = over(cover == 255 ? solid_src : premultiply(inst.rgba, cover), row[x0 + px]);
        }
      };
      sides(0, left, 0);
      sides(right, w, left);
      const int begin = std::max(left, from), end = std::min(right, to);
      if (begin >= end) continue;
      const uint32_t cover = edge_row ? shade(left, py) : stroke > 0 ? 0u : 255u;
      if (cover) blend_span(row + x0 + begin, end - begin, cover == 255 ? solid_src : premultiply(inst.rgba, cover));
    }
  }

  void glyph(const Instance &inst, IRect clip, int page_index) {
    if (page_index < 0 || static_cast<size_t>(page_index) >= pages_.size()) return;
    const Page &page = pages_[static_cast<size_t>(page_index)];
    if (page.pixels.empty()) return;
    const int x0 = static_cast<int>(inst.x), y0 = static_cast<int>(inst.y);
    const IRect r = intersect({x0, y0, static_cast<int>(inst.w), static_cast<int>(inst.h)}, clip);
    if (r.empty()) return;
    const int ax = static_cast<int>(inst.u0), ay = static_cast<int>(inst.v0);
    const bool color = (inst.flags & InstColorGlyph) != 0;
    const uint32_t tint_alpha = inst.rgba >> 24;
    const bool horizontal = (inst.flags & InstGradient) != 0, vertical = (inst.flags & InstGradientV) != 0;
    const Premul from = unpack(inst.rgba), to = horizontal || vertical ? unpack_end(inst) : from;
    const uint32_t solid_src = premultiply(inst.rgba, 255);

    if (color && inst.u1 > 0 && inst.v1 > 0) {
      // A picture stored smaller than it is shown: sample the nearest texel.
      const int sw = static_cast<int>(inst.u1), sh = static_cast<int>(inst.v1);
      for (int y = r.y; y < r.y + r.h; ++y) {
        const int sy = std::min(sh - 1, static_cast<int>((static_cast<float>(y - y0) + 0.5f) * static_cast<float>(sh) / inst.h));
        const uint8_t *row = page.pixels.data() + static_cast<size_t>(ay + sy) * page.width * 4;
        uint32_t *dst = pixels_.data() + static_cast<size_t>(y) * width_ + r.x;
        for (int x = 0; x < r.w; ++x) {
          const int sx = std::min(sw - 1, static_cast<int>((static_cast<float>(r.x + x - x0) + 0.5f) * static_cast<float>(sw) / inst.w));
          const uint8_t *src = row + static_cast<size_t>(ax + sx) * 4;
          if (src[3] == 0) continue;
          dst[x] = over(mul(src[0], tint_alpha) | (mul(src[1], tint_alpha) << 8) | (mul(src[2], tint_alpha) << 16) | (mul(src[3], tint_alpha) << 24), dst[x]);
        }
      }
      return;
    }

    for (int y = r.y; y < r.y + r.h; ++y) {
      const uint8_t *src = page.pixels.data() + (static_cast<size_t>(ay + y - y0) * page.width + static_cast<size_t>(ax + r.x - x0)) * 4;
      uint32_t *dst = pixels_.data() + static_cast<size_t>(y) * width_ + r.x;
      for (int x = 0; x < r.w; ++x, src += 4) {
        const uint32_t texel_alpha = src[3];
        if (texel_alpha == 0) continue;
        uint32_t pixel;
        if (color) {
          pixel = mul(src[0], tint_alpha) | (mul(src[1], tint_alpha) << 8) | (mul(src[2], tint_alpha) << 16) | (mul(texel_alpha, tint_alpha) << 24);
        } else if (horizontal || vertical) {
          const float t = vertical ? (static_cast<float>(y - y0) + 0.5f) / inst.h : (static_cast<float>(r.x + x - x0) + 0.5f) / inst.w;
          pixel = mixed_pixel(from, to, t, static_cast<float>(texel_alpha) / 255);
        } else {
          pixel = texel_alpha == 255 ? solid_src : premultiply(inst.rgba, texel_alpha);
        }
        dst[x] = over(pixel, dst[x]);
      }
    }
  }

  int width_ = 0, height_ = 0;
  std::vector<uint32_t> pixels_, saved_;
  std::vector<uint8_t> columns_;
  std::vector<Page> pages_;
};

}  // namespace

std::unique_ptr<CpuBackend> make_cpu_backend() { return std::make_unique<Cpu>(); }

}  // namespace qui
