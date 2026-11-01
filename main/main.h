#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "consts.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_check.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

void init_buttons(void);
extern bool saveFrame;

esp_err_t init_display(void);
esp_err_t draw_frame(int x_start, int y_start, int x_end, int y_end, const void *color_data);
extern SemaphoreHandle_t free_strip_buffers;

esp_err_t init_camera_ov3660(void);

esp_err_t init_sd_card(void);
esp_err_t save_frame_to_sd(camera_fb_t *frame);

void Dither(const uint8_t *src, uint8_t *dest, int width, int height, int first_row);
void NextPalette(void);
uint8_t GetPaletteIndex(void);