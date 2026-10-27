#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_camera.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "main.h"

static const char *TAG = "camera_lcd";
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t color_transfer_done;

static bool on_color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_io_event_data_t *event_data,
                                   void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(user_ctx, &task_woken);
    return task_woken == pdTRUE;
}

static esp_err_t init_display(void)
{
    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_color_transfer_done,
        .user_ctx = color_transfer_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &panel_io),
        TAG, "LCD panel IO init failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel),
                        TAG, "ST7789 driver init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "LCD reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true),
                        TAG, "LCD color inversion setup failed");

    // A 240x320 ST7789 becomes a 320x240 logical display in landscape.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, true),
                        TAG, "LCD orientation setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true),
                        TAG, "LCD display enable failed");
    return ESP_OK;
}

static esp_err_t init_camera(void)
{
    const camera_config_t config = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,
        .pin_xclk       = CAM_PIN_XCLK,
        .pin_sccb_sda   = CAM_PIN_SDA,
        .pin_sccb_scl   = CAM_PIN_SCL,
        .pin_vsync      = CAM_PIN_VSYNC,
        .pin_href       = CAM_PIN_HREF,
        .pin_pclk       = CAM_PIN_PCLK,

        .pin_d0         = DB[0],
        .pin_d1         = DB[1],
        .pin_d2         = DB[2],
        .pin_d3         = DB[3],
        .pin_d4         = DB[4],
        .pin_d5         = DB[5],
        .pin_d6         = DB[6],
        .pin_d7         = DB[7],

        .xclk_freq_hz   = 24 * MHZ,
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,
        .pixel_format   = PIXFORMAT_RGB565,
        .frame_size     = FRAMESIZE_QVGA,
        .fb_count       = 2,
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&config);
}

void app_main(void)
{
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is required for camera and display frame buffers");
        return;
    }

    uint8_t *display_fb = heap_caps_malloc(
        LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(display_fb ? ESP_OK : ESP_ERR_NO_MEM);
    img_process_init();

    color_transfer_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(color_transfer_done ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(init_display());
    ESP_ERROR_CHECK(init_camera());

    sensor_t *cm = esp_camera_sensor_get();
    ESP_ERROR_CHECK(cm ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(cm->set_denoise ? cm->set_denoise(cm, 0) : ESP_ERR_NOT_SUPPORTED);
    ESP_ERROR_CHECK(cm->set_awb_gain ? cm->set_awb_gain(cm, 0) : ESP_ERR_NOT_SUPPORTED);

    int64_t dither_window_start_us = esp_timer_get_time();
    uint64_t dither_time_us = 0;
    uint32_t dither_samples = 0;

    while (true) {
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame == NULL) {
            ESP_LOGE(TAG, "Camera did not return a frame");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (frame->width != LCD_WIDTH || frame->height != LCD_HEIGHT) {
            ESP_LOGE(TAG, "Frame size %ux%u does not match LCD %ux%u",
                     (unsigned)frame->width, (unsigned)frame->height,
                     LCD_WIDTH, LCD_HEIGHT);
            esp_camera_fb_return(frame);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t expected_frame_len = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
        if (frame->len != expected_frame_len) {
            ESP_LOGE(TAG, "Frame length %u does not match expected %u",
                     (unsigned)frame->len, (unsigned)expected_frame_len);
            esp_camera_fb_return(frame);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int64_t dither_start_us = esp_timer_get_time();
        gameboy_dither_gray_to_rgb565(frame->buf, display_fb, frame->width, frame->height);
        dither_time_us += (uint64_t)(esp_timer_get_time() - dither_start_us);
        dither_samples++;

        esp_err_t sync_err = esp_cache_msync(
            display_fb, frame->len,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (sync_err != ESP_OK && sync_err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(TAG, "Display buffer cache sync failed: %s", esp_err_to_name(sync_err));
            esp_camera_fb_return(frame);
            continue;
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(
            panel, 0, 0, frame->width, frame->height, display_fb);
        if (err == ESP_OK) {
            xSemaphoreTake(color_transfer_done, portMAX_DELAY);
            int64_t now_us = esp_timer_get_time();
            if (now_us - dither_window_start_us >= 1000000) {
                ESP_LOGI(TAG, "Avg dithering: %.2f ms/frame (%u frames)",
                         dither_samples
                             ? (double)dither_time_us / (double)dither_samples / 1000.0
                             : 0.0,
                         (unsigned)dither_samples);
                dither_window_start_us = now_us;
                dither_time_us = 0;
                dither_samples = 0;
            }
        } else {
            ESP_LOGE(TAG, "LCD transfer failed: %s", esp_err_to_name(err));
        }
        esp_camera_fb_return(frame);
    }
}
