#include "main.h"

#define SATURATION 512
#define LUMA_WEIGHT 2

static uint8_t palette_index = 3;

void NextPalette(void) {
	palette_index = (palette_index + 1) % PALETTE_COUNT;
}
uint8_t GetPaletteIndex(void) {
	return palette_index;
}

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t R = ((r * 31 + 127) >> 8);
  uint16_t G = ((g * 63 + 127) >> 8);
  uint16_t B = ((b * 31 + 127) >> 8);

  return (R << 11) | (G << 5) | B;
}

static inline uint8_t clamp(int v) {
  return v < 0 ? 0 : ((v > 255) ? 255 : (uint8_t)v);
}

static inline uint8_t to5(int v) { return (uint8_t)((v * 31 + 127) / 255); }
static inline uint8_t to6(int v) { return (uint8_t)((v * 63 + 127) / 255); }

void Dither(const uint8_t *src, uint8_t *dest, int width, int height, int first_row) {
	const Color *palette = palettesss[palette_index];
	for (int row = 0; row < height; row++) {
		int y = first_row + row;
		for (int x = 0; x < width; x++) {
			size_t index = ((size_t)row * width + x) * 2;

			Color pixel = { .raw = (src[index] << 8) | src[index + 1] };

			int r = (pixel.r << 3) | (pixel.r >> 2);
			int g = (pixel.g << 2) | (pixel.g >> 4);
			g = (g * 235) >> 8;

			int b = (pixel.b << 3) | (pixel.b >> 2);

			int luma = (r * 77 + g * 150 + b * 29) >> 8;
			int threshold = BAYER[y & 3][x & 3];
			r = clamp(luma + (((r - luma) * SATURATION) >> 8) + threshold);
			g = clamp(luma + (((g - luma) * SATURATION) >> 8) + threshold);
			b = clamp(luma + (((b - luma) * SATURATION) >> 8) + threshold);

			Color q;
			q.r = to5(r);
			q.g = to6(g);
			q.b = to5(b);

			uint8_t palette_entry = 0;
			int best_distance = 0x7FFFFFFF;
			for (uint8_t i = 0; i < PALETTE_SIZE; i++) {
				int dr = ((int)q.r - palette[i].r) * 8;
				int dg = ((int)q.g - palette[i].g) * 4;
				int db = ((int)q.b - palette[i].b) * 8;
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
			dest[index] = palette[palette_entry].raw >> 8;
			dest[index + 1] = palette[palette_entry].raw & 0xFF;
		}
	}
}