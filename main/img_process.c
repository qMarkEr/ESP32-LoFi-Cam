#include "main.h"

static uint8_t palette_index = 5;

static inline uint8_t clamp(int v) {
  return v < 0 ? 0 : ((v > 255) ? 255 : (uint8_t)v);
}

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t R = ((r * 31 + 127) >> 8);
  uint16_t G = ((g * 63 + 127) >> 8);
  uint16_t B = ((b * 31 + 127) >> 8);

  return (R << 11) | (G << 5) | B;
}

void gameboy_dither_gray_to_rgb565(const uint8_t *src, uint8_t *dest, int width, int height, int first_row) {
  const Color *plt = palettesss[palette_index];
  for (int row = 0; row < height; row++) {
    int y = first_row + row;
    for (int x = 0; x < width; x++) {
      size_t index = ((size_t)row * width + x) * 2;
      uint16_t pixel = ((uint16_t)src[index] << 8) | (src[index + 1]);

      uint8_t b = ((pixel >> 11) & 0x1F) << 3;
      uint8_t g = ((pixel >> 5) & 0x3F) << 2;
      uint8_t r = (pixel & 0x1F) << 3;
      int16_t threshold = bayer8x8[y & 7][x & 7];


      r = clamp((int16_t)r + threshold);
      g = clamp((int16_t)g + threshold);
      b = clamp((int16_t)b + threshold);

      int dst = r * r + b * b + g * g;
      int min_diff = dst;
      uint8_t idx = 0;
      if (dst == DST_MAX) {
        idx = PALETTE_SIZE - 1;
      } else if (dst != DST_MIN) {
        for (uint8_t i = 1; i < PALETTE_SIZE - 1; ++i) {
          int diff = abs(dst - plt[i].dst);
          if (diff <= 10) {
            idx = i;
            break;
          }
          if (diff < min_diff) {
            min_diff = diff;
            idx = i;
          }
        }
      }
      r = plt[idx].b;
      g = plt[idx].g;
      b = plt[idx].r;
      uint16_t out = pack_rgb565(b, g, r);
      // dest[index] = out;
      dest[index] = out >> 8;
      dest[index + 1] = out & 0xFF;
    }
  }
}