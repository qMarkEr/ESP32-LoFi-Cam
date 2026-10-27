#include <assert.h>

#include "main.h"

// One block per Bayer cell [y & 7][x & 7] holding clamp8(channel + threshold)^2.
// R and B share the 5-bit table at [0..31], G uses [32..95].
#define LUT_G 32
#define LUT_BLOCK 96
#define BUCKET_SHIFT 14

static uint16_t square_lut[8][8][LUT_BLOCK];
static uint32_t index_lut[(DST_MAX >> BUCKET_SHIFT) + 1];
static uint32_t pair_palette[PALETTE_SIZE][PALETTE_SIZE];

static inline uint8_t clamp8(int value)
{
    return value < 0 ? 0 : (value > 255 ? 255 : (uint8_t)value);
}

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
            int threshold = bayer8x8[row][col];
            uint16_t *lut = square_lut[row][col];
            for (int channel = 0; channel < 32; channel++) {
                int value = clamp8((channel << 3) + threshold);
                lut[channel] = (uint16_t)(value * value);
            }
            for (int channel = 0; channel < 64; channel++) {
                int value = clamp8((channel << 2) + threshold);
                lut[LUT_G + channel] = (uint16_t)(value * value);
            }
        }
    }

    // Nearest palette color by brightness (palette sorted by dst, ties go to the
    // darker color, color 0 is anchored at brightness 0). That is the number of
    // midpoints the brightness has reached, computed as
    // (index_lut[b >> BUCKET_SHIFT] + b) >> 29: bits 29..31 hold the index at the
    // bucket start, the low bits hold 2^29 - midpoint so reaching it carries into bit 29.
    const Color *palette = palettesss[0];
    uint32_t bounds[PALETTE_SIZE - 1];
    for (int i = 0; i < PALETTE_SIZE - 1; i++) {
        uint32_t lower = i == 0 ? 0 : (uint32_t)palette[i].dst;
        bounds[i] = (lower + (uint32_t)palette[i + 1].dst) / 2 + 1;
    }
    for (uint32_t bucket = 0; bucket < sizeof(index_lut) / sizeof(index_lut[0]); bucket++) {
        uint32_t start = bucket << BUCKET_SHIFT;
        uint32_t base = 0;
        uint32_t carry = 0;
        for (int i = 0; i < PALETTE_SIZE - 1; i++) {
            if (bounds[i] <= start) {
                base++;
            } else if (bounds[i] < start + (1u << BUCKET_SHIFT)) {
                assert(carry == 0 && "two palette midpoints in one bucket");
                carry = (1u << 29) - bounds[i];
            }
        }
        index_lut[bucket] = (base << 29) + carry;
    }

    // Two output pixels per word, high byte first as the panel expects.
    uint16_t colors[PALETTE_SIZE];
    for (int i = 0; i < PALETTE_SIZE; i++) {
        uint16_t color = pack_rgb565(palette[i].b, palette[i].g, palette[i].r);
        colors[i] = (uint16_t)((color >> 8) | (color << 8));
    }
    for (int first = 0; first < PALETTE_SIZE; first++) {
        for (int second = 0; second < PALETTE_SIZE; second++) {
            pair_palette[first][second] = colors[first] | ((uint32_t)colors[second] << 16);
        }
    }
}

// Camera RGB565 is big-endian, so a little-endian load gives
// pixels[7:0] = RRRRRGGG and pixels[15:8] = GGGBBBBB.
static inline uint32_t pixel_brightness(uint32_t pixels, const uint16_t *lut)
{
    return lut[(pixels >> 3) & 0x1F] +
           lut[LUT_G + (((pixels & 0x07) << 3) | ((pixels >> 13) & 0x07))] +
           lut[(pixels >> 8) & 0x1F];
}

static inline uint32_t palette_index(uint32_t brightness)
{
    return (index_lut[brightness >> BUCKET_SHIFT] + brightness) >> 29;
}

// width must be a multiple of 8, src and dest 4-byte aligned. first_row is the
// frame row of src[0], so strips keep their Bayer phase.
void gameboy_dither_gray_to_rgb565(const uint8_t *src, uint8_t *dest, int width, int height, int first_row)
{
    const uint32_t *in = (const uint32_t *)src;
    uint32_t *out = (uint32_t *)dest;
    for (int y = 0; y < height; y++) {
        const uint16_t *row = square_lut[(first_row + y) & 7][0];
        for (int x = 0; x < width; x += 8, in += 4, out += 4) {
#pragma GCC unroll 4
            for (int k = 0; k < 4; k++) {
                uint32_t pixels = in[k];
                uint32_t first = palette_index(pixel_brightness(pixels, row + (2 * k) * LUT_BLOCK));
                uint32_t second = palette_index(pixel_brightness(pixels >> 16, row + (2 * k + 1) * LUT_BLOCK));
                out[k] = pair_palette[first][second];
            }
        }
    }
}
