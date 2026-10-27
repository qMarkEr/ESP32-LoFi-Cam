#include "main.h"

static uint16_t rb_square_lut[32][32];
static uint16_t g_square_lut[32][64];
static uint16_t packed_palette[PALETTE_SIZE];

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

static inline uint8_t select_palette(uint32_t brightness,
                                     uint32_t first,
                                     uint32_t second,
                                     uint32_t third)
{
    uint32_t min_diff = brightness;
    uint8_t index = 0;
    uint32_t diff = brightness > first ? brightness - first : first - brightness;
    if (diff <= 10) {
        return 1;
    }
    if (diff < min_diff) {
        min_diff = diff;
        index = 1;
    }

    diff = brightness > second ? brightness - second : second - brightness;
    if (diff <= 10) {
        return 2;
    }
    if (diff < min_diff) {
        min_diff = diff;
        index = 2;
    }

    diff = brightness > third ? brightness - third : third - brightness;
    if (diff <= 10) {
        return 3;
    }
    if (diff < min_diff) {
        index = 3;
    }
    return index;
}

void img_process_init(void)
{
    for (int threshold_index = 0; threshold_index < 32; threshold_index++) {
        int threshold = threshold_index - 16;
        for (int channel = 0; channel < 32; channel++) {
            int value = clamp8((channel << 3) + threshold);
            rb_square_lut[threshold_index][channel] = (uint16_t)(value * value);
        }
        for (int channel = 0; channel < 64; channel++) {
            int value = clamp8((channel << 2) + threshold);
            g_square_lut[threshold_index][channel] = (uint16_t)(value * value);
        }
    }

    const Color *palette = palettesss[0];
    for (int i = 0; i < PALETTE_SIZE; i++) {
        packed_palette[i] = pack_rgb565(palette[i].b, palette[i].g, palette[i].r);
    }
}

void gameboy_dither_gray_to_rgb565(const uint8_t *src, uint8_t *dest, int width, int height)
{
    const Color *palette = palettesss[0];
    uint32_t palette_brightness[3] = {
        (uint32_t)palette[1].dst,
        (uint32_t)palette[2].dst,
        (uint32_t)palette[3].dst,
    };
    size_t index = 0;
    for (int y = 0; y < height; y++) {
        const int8_t *threshold_row = bayer8x8[y & 7];
        for (int x = 0; x < width; x++) {
            uint16_t pixel = ((uint16_t)src[index] << 8) | src[index + 1];
            int threshold_index = threshold_row[x & 7] + 16;
            uint32_t brightness =
                rb_square_lut[threshold_index][(pixel >> 11) & 0x1F] +
                g_square_lut[threshold_index][(pixel >> 5) & 0x3F] +
                rb_square_lut[threshold_index][pixel & 0x1F];

            uint8_t palette_index = 0;
            if (brightness == DST_MAX) {
                palette_index = PALETTE_SIZE - 1;
            } else if (brightness != DST_MIN) {
                palette_index = select_palette(
                    brightness,
                    palette_brightness[0],
                    palette_brightness[1],
                    palette_brightness[2]);
            }

            uint16_t output = packed_palette[palette_index];
            dest[index] = (uint8_t)(output >> 8);
            dest[index + 1] = (uint8_t)output;
            index += 2;
        }
    }
}
