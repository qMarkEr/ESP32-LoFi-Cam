#include "main.h"

#define SATURATION 512
#define LUMA_WEIGHT 2

static uint8_t palette_index = 3;

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
  const Color *palette = palettesss[palette_index];
  for (int row = 0; row < height; row++) {
    int y = first_row + row;
    for (int x = 0; x < width; x++) {
      size_t index = ((size_t)row * width + x) * 2;
      uint16_t pixel = ((uint16_t)src[index] << 8) | (src[index + 1]);

      int b5 = (pixel >> 11) & 0x1F;
      int g6 = (pixel >> 5) & 0x3F;
      int r5 = pixel & 0x1F;
      int r = (b5 << 3) | (b5 >> 2);
      int g = (g6 << 2) | (g6 >> 4);
      int b = (r5 << 3) | (r5 >> 2);

      int luma = (r * 77 + g * 150 + b * 29) >> 8;
      r = clamp(luma + (((r - luma) * SATURATION) >> 8));
      g = clamp(luma + (((g - luma) * SATURATION) >> 8));
      b = clamp(luma + (((b - luma) * SATURATION) >> 8));

      int threshold = bayer8x8[y & 7][x & 7];
      r = clamp(r + threshold);
      g = clamp(g + threshold);
      b = clamp(b + threshold);

      uint8_t palette_entry = 0;
      int best_distance = 0x7FFFFFFF;
      for (uint8_t i = 0; i < PALETTE_SIZE; i++) {
        int dr = r - palette[i].r;
        int dg = g - palette[i].g;
        int db = b - palette[i].b;
        int dl = (dr * 77 + dg * 150 + db * 29) >> 8;
        int distance = dr * dr + dg * dg + db * db +
                       LUMA_WEIGHT * dl * dl;
        if (distance < best_distance) {
          best_distance = distance;
          palette_entry = i;
          if (distance == 0) {
            break;
          }
        }
      }

      uint16_t out = pack_rgb565(
          palette[palette_entry].r,
          palette[palette_entry].g,
          palette[palette_entry].b);
      dest[index] = out >> 8;
      dest[index + 1] = out & 0xFF;
    }
  }
}