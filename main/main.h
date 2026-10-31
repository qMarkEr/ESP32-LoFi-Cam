#include "consts.h"

void s_curve_init(void);
void gameboy_dither_gray_to_rgb565(const uint8_t *src, uint8_t *dest, int width, int height, int first_row);