#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_camera.h"
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

#define ROW_BYTES (LCD_WIDTH * sizeof(uint16_t))
#define STRIP_BYTES (ROW_BYTES * LCD_STRIP_LINES)
#define STRIP_BUFFERS 2

_Static_assert(LCD_WIDTH % 8 == 0, "dithering processes 8 pixels at a time");
_Static_assert(LCD_HEIGHT % LCD_STRIP_LINES == 0, "strips must tile the frame");

typedef struct {
    const uint8_t *src;
    uint8_t *dest;
    int first_row;
    int rows;
} dither_job_t;

static const char *TAG = "camera_lcd";
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t free_strip_buffers;
static TaskHandle_t main_task;
static TaskHandle_t dither_task;
static dither_job_t dither_job;

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

static void dither_worker(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        gameboy_dither_gray_to_rgb565(dither_job.src, dither_job.dest, LCD_WIDTH,
                                      dither_job.rows, dither_job.first_row);
        xTaskNotifyGive(main_task);
    }
}

// The top half of the strip is dithered here, the bottom half on the other core.
static void dither_strip(const uint8_t *src, uint8_t *dest, int first_row, int rows)
{
    int top_rows = rows / 2;
    size_t offset = (size_t)top_rows * ROW_BYTES;
    dither_job = (dither_job_t){
        .src = src + offset,
        .dest = dest + offset,
        .first_row = first_row + top_rows,
        .rows = rows - top_rows,
    };
    xTaskNotifyGive(dither_task);
    gameboy_dither_gray_to_rgb565(src, dest, LCD_WIDTH, top_rows, first_row);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static esp_err_t init_display(void)
{
    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = STRIP_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = STRIP_BUFFERS,
        .on_color_trans_done = on_color_transfer_done,
        .user_ctx = free_strip_buffers,
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
        ESP_LOGE(TAG, "PSRAM is required for camera frame buffers");
        return;
    }

    // Strips live in internal RAM: no PSRAM writes, no cache sync, and the next
    // strip is dithered while the previous one is still going out over SPI.
    uint8_t *strips[STRIP_BUFFERS];
    for (int i = 0; i < STRIP_BUFFERS; i++) {
        strips[i] = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_ERROR_CHECK(strips[i] ? ESP_OK : ESP_ERR_NO_MEM);
    }
    img_process_init();

    free_strip_buffers = xSemaphoreCreateCounting(STRIP_BUFFERS, STRIP_BUFFERS);
    ESP_ERROR_CHECK(free_strip_buffers ? ESP_OK : ESP_ERR_NO_MEM);

    main_task = xTaskGetCurrentTaskHandle();
    BaseType_t worker_created = xTaskCreatePinnedToCore(
        dither_worker, "dither", 3072, NULL, uxTaskPriorityGet(NULL), &dither_task,
        (xPortGetCoreID() + 1) % portNUM_PROCESSORS);
    ESP_ERROR_CHECK(worker_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(init_display());
    ESP_ERROR_CHECK(init_camera());

    sensor_t *cm = esp_camera_sensor_get();
    ESP_ERROR_CHECK(cm ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(cm->set_denoise ? cm->set_denoise(cm, 0) : ESP_ERR_NOT_SUPPORTED);
    ESP_ERROR_CHECK(cm->set_awb_gain ? cm->set_awb_gain(cm, 0) : ESP_ERR_NOT_SUPPORTED);

    int64_t dither_window_start_us = esp_timer_get_time();
    uint64_t dither_time_us = 0;
    uint32_t dither_samples = 0;
    int next_strip = 0;

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

        for (int y = 0; y < LCD_HEIGHT; y += LCD_STRIP_LINES) {
            uint8_t *strip = strips[next_strip];
            next_strip = (next_strip + 1) % STRIP_BUFFERS;

            // Wait for the transfer that last used this buffer.
            xSemaphoreTake(free_strip_buffers, portMAX_DELAY);

            int64_t dither_start_us = esp_timer_get_time();
            dither_strip(frame->buf + (size_t)y * ROW_BYTES, strip, y, LCD_STRIP_LINES);
            dither_time_us += (uint64_t)(esp_timer_get_time() - dither_start_us);

            esp_err_t err = esp_lcd_panel_draw_bitmap(
                panel, 0, y, LCD_WIDTH, y + LCD_STRIP_LINES, strip);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LCD transfer failed: %s", esp_err_to_name(err));
                // No completion callback will release this buffer.
                xSemaphoreGive(free_strip_buffers);
            }
        }
        // Every strip is already copied out, the camera can refill this buffer.
        esp_camera_fb_return(frame);
        dither_samples++;

        int64_t now_us = esp_timer_get_time();
        if (now_us - dither_window_start_us >= 1000000) {
            ESP_LOGI(TAG, "Avg dithering: %.2f ms/frame (%u frames)",
                     (double)dither_time_us / (double)dither_samples / 1000.0,
                     (unsigned)dither_samples);
            dither_window_start_us = now_us;
            dither_time_us = 0;
            dither_samples = 0;
        }
    }
}
