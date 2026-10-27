#include <assert.h>
#include <math.h>
#include "main.h"

// Per Bayer cell [y & 7][x & 7]: three tables that add the threshold, clamp,
// requantize to RGB565 and return the value already shifted into its field.
#define LUT_G 32
#define LUT_B 96
#define LUT_BLOCK 128

// Total threshold spread in 0..255 channel units. Tune it: about the typical
// distance between neighbouring palette colours (start with 48..64).
#define DITHER_AMPLITUDE 64
// bayer8x8 values are assumed to be 0..BAYER_LEVELS-1 (64 levels for 8x8).
#define BAYER_LEVELS 64

static uint16_t channel_lut[8][8][LUT_BLOCK];
static uint8_t nearest_lut[65536];            // RGB565 -> palette index
static uint32_t pair_palette[PALETTE_SIZE][PALETTE_SIZE];

// --- tuning ---
#define INPUT_BLACK 0       // camera value that maps to 0
#define INPUT_WHITE 200     // camera value that maps to 255 (lower = brighter)
#define GAMMA 0.7f          // <1 brightens midtones
#define DITHER_BIAS 0       // add e.g. +16 for extra brightening
#define LUMA_WEIGHT 3       // 0 = pure RGB, higher = closer to brightness-based

static inline int adjust(int v)
{
    float x = (float)(v - INPUT_BLACK) / (INPUT_WHITE - INPUT_BLACK);
    x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    return (int)(powf(x, GAMMA) * 255.f + 0.5f);
}

static inline uint8_t clamp8(int value)
{
    return value < 0 ? 0 : (value > 255 ? 255 : (uint8_t)value);
}

static inline int expand5(int v) { return (v << 3) | (v >> 2); }
static inline int expand6(int v) { return (v << 2) | (v >> 4); }

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t red = ((r * 31 + 127) >> 8);
    uint16_t green = ((g * 63 + 127) >> 8);
    uint16_t blue = ((b * 31 + 127) >> 8);

    return (red << 11) | (green << 5) | blue;
}

void img_process_init(void)
{
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            // Centered threshold: -AMPLITUDE/2 .. +AMPLITUDE/2
			int t = ((int)bayer8x8[row][col] * DITHER_AMPLITUDE) / BAYER_LEVELS
					- DITHER_AMPLITUDE / 2 + DITHER_BIAS;
            uint16_t *lut = channel_lut[row][col];
			for (int c = 0; c < 32; c++) {
				int v = clamp8(adjust(expand5(c)) + t);
				lut[c] = (uint16_t)(((v * 31 + 127) >> 8) << 11);       // R
                lut[LUT_B + c] = (uint16_t)((v * 31 + 127) >> 8);       // B
			}
			for (int c = 0; c < 64; c++) {
				int v = clamp8(adjust(expand6(c)) + t);
				lut[LUT_G + c] = (uint16_t)(((v * 63 + 127) >> 8) << 5); // G
			}
        }
    }

    const Color *palette = palettesss[5];

    // Colours as they appear on the panel. The original code feeds palette.b
    // into the red field and palette.r into the blue field, so compare in that
    // same space. If the result looks red/blue-swapped, swap pr and pb.
    int pr[PALETTE_SIZE], pg[PALETTE_SIZE], pb[PALETTE_SIZE];
    uint16_t colors[PALETTE_SIZE];
    for (int i = 0; i < PALETTE_SIZE; i++) {
        pr[i] = palette[i].r;
        pg[i] = palette[i].g;
        pb[i] = palette[i].b;
        uint16_t color = pack_rgb565(pr[i], pg[i], pb[i]);
        colors[i] = (uint16_t)((color >> 8) | (color << 8));
    }

    // Nearest palette colour for every RGB565 value.
    for (int idx = 0; idx < 65536; idx++) {
        int r = expand5(idx >> 11);
        int g = expand6((idx >> 5) & 0x3F);
        int b = expand5(idx & 0x1F);
        int best = 0;
        int best_dist = 0x7FFFFFFF;
        for (int i = 0; i < PALETTE_SIZE; i++) {
            int dr = r - pr[i], dg = g - pg[i], db = b - pb[i];
			int dl = (dr * 77 + dg * 150 + db * 29) >> 8;   // luma difference
			int dist = dr * dr + dg * dg + db * db + LUMA_WEIGHT * dl * dl;
            if (dist < best_dist) {
                best_dist = dist;
                best = i;
            }
        }
        nearest_lut[idx] = (uint8_t)best;
    }

    for (int first = 0; first < PALETTE_SIZE; first++) {
        for (int second = 0; second < PALETTE_SIZE; second++) {
            pair_palette[first][second] = colors[first] | ((uint32_t)colors[second] << 16);
        }
    }
}

// Camera RGB565 is big-endian, so a little-endian load gives
// pixels[7:0] = RRRRRGGG and pixels[15:8] = GGGBBBBB.
static inline uint32_t palette_index(uint32_t pixels, const uint16_t *lut)
{
    uint32_t r = (pixels >> 3) & 0x1F;
    uint32_t g = ((pixels & 0x07) << 3) | ((pixels >> 13) & 0x07);
    uint32_t b = (pixels >> 8) & 0x1F;
    return nearest_lut[lut[r] | lut[LUT_G + g] | lut[LUT_B + b]];
}

void gameboy_dither_gray_to_rgb565(const uint8_t *src, uint8_t *dest, int width, int height, int first_row)
{
    const uint32_t *in = (const uint32_t *)src;
    uint32_t *out = (uint32_t *)dest;
    for (int y = 0; y < height; y++) {
        const uint16_t *row = channel_lut[(first_row + y) & 7][0];
        for (int x = 0; x < width; x += 8, in += 4, out += 4) {
#pragma GCC unroll 4
            for (int k = 0; k < 4; k++) {
                uint32_t pixels = in[k];
                uint32_t first = palette_index(pixels, row + (2 * k) * LUT_BLOCK);
                uint32_t second = palette_index(pixels >> 16, row + (2 * k + 1) * LUT_BLOCK);
                out[k] = pair_palette[first][second];
            }
        }
    }
}