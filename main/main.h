#include "consts.h"

void img_process_init(void);
void gameboy_dither_gray_to_rgb565(const uint8_t *src, uint8_t *dest, int width, int height, int first_row);