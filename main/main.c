#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdmmc_host.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "img_converters.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "main.h"

#define ROW_BYTES (LCD_WIDTH * sizeof(uint16_t))
#define STRIP_BYTES (ROW_BYTES * LCD_STRIP_LINES)
#define STRIP_BUFFERS 2
#define TIMING_LOG_FRAME_COUNT 30

#define SD_MOUNT_POINT "/sdcard"
#define SDMMC_PIN_CMD GPIO_NUM_38
#define SDMMC_PIN_CLK GPIO_NUM_39
#define SDMMC_PIN_D0 GPIO_NUM_40
#define SAVE_BUTTON_GPIO GPIO_NUM_20

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
static sdmmc_card_t *sd_card;
static uint32_t picture_count;
static portMUX_TYPE display_timing_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t display_transfer_started_us[STRIP_BUFFERS];
static uint8_t display_transfer_head;
static uint8_t display_transfer_count;
static uint64_t display_transfer_total_us;
static uint32_t display_transfer_completed_count;

static bool on_color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_io_event_data_t *event_data,
                                   void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    int64_t completed_us = esp_timer_get_time();
    BaseType_t task_woken = pdFALSE;

    portENTER_CRITICAL_ISR(&display_timing_mux);
    if (display_transfer_count > 0) {
        int64_t elapsed_us =
            completed_us - display_transfer_started_us[display_transfer_head];
        display_transfer_total_us += (uint64_t)elapsed_us;
        display_transfer_completed_count++;
        display_transfer_head = (display_transfer_head + 1) % STRIP_BUFFERS;
        display_transfer_count--;
    }
    portEXIT_CRITICAL_ISR(&display_timing_mux);

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

        .xclk_freq_hz   = 26666666,
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,
        .pixel_format   = PIXFORMAT_RGB565,
        .frame_size     = FRAMESIZE_QVGA,
        .fb_count       = 4,
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&config);
}

static void setup_camera() {
    ESP_ERROR_CHECK(init_camera());

    sensor_t *cm = esp_camera_sensor_get();
    cm->set_saturation(cm, 2);
    cm->set_whitebal(cm, 1);
    cm->set_denoise(cm, 0);
    cm->set_awb_gain(cm, 1);
    cm->set_wb_mode(cm, 0);
    cm->set_brightness(cm, 0);
    cm->set_contrast(cm, 2);
    cm->set_special_effect(cm, 0);
    cm->set_lenc(cm, 1);
    cm->set_exposure_ctrl(cm, 0);
    cm->set_aec_value(cm, 300);
    cm->set_gain_ctrl(cm, 0);
    cm->set_agc_gain(cm, 10);
    cm->set_aec2(cm, 1);
    cm->set_ae_level(cm, 0);
    cm->set_gainceiling(cm, GAINCEILING_128X);
}


static esp_err_t init_sd_card(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 0,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SDMMC_PIN_CLK;
    slot.cmd = SDMMC_PIN_CMD;
    slot.d0 = SDMMC_PIN_D0;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(
        SD_MOUNT_POINT, &host, &slot, &mount_config, &sd_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

static esp_err_t init_save_button(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << SAVE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static esp_err_t save_frame_to_sd(camera_fb_t *frame)
{
    if (sd_card == NULL) {
        ESP_LOGE(TAG, "Cannot save image: SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *jpeg_buffer = NULL;
    size_t jpeg_size = 0;
    if (!frame2jpg(frame, 90, &jpeg_buffer, &jpeg_size)) {
        ESP_LOGE(TAG, "JPEG conversion failed");
        free(jpeg_buffer);
        return ESP_FAIL;
    }

    char path[40];
    struct stat file_info;
    while (true) {
        int path_len = snprintf(path, sizeof(path), SD_MOUNT_POINT "/photo_%04u.jpg",
                                (unsigned)picture_count);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            free(jpeg_buffer);
            return ESP_ERR_INVALID_SIZE;
        }

        if (stat(path, &file_info) == 0) {
            picture_count++;
            continue;
        }
        if (errno != ENOENT) {
            ESP_LOGE(TAG, "Cannot check image path %s: errno %d", path, errno);
            free(jpeg_buffer);
            return ESP_FAIL;
        }
        break;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s for writing: errno %d", path, errno);
        free(jpeg_buffer);
        return ESP_FAIL;
    }

    bool write_ok = fwrite(jpeg_buffer, 1, jpeg_size, file) == jpeg_size;
    if (!write_ok) {
        ESP_LOGE(TAG, "Failed writing image data to %s", path);
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "Failed closing %s: errno %d", path, errno);
        write_ok = false;
    }
    free(jpeg_buffer);

    if (!write_ok) {
        if (remove(path) != 0) {
            ESP_LOGE(TAG, "Failed removing incomplete image %s: errno %d", path, errno);
        }
        return ESP_FAIL;
    }

    picture_count++;
    ESP_LOGI(TAG, "Saved %s (%u bytes)", path, (unsigned)jpeg_size);
    return ESP_OK;
}

void app_main(void)
{
    if (!esp_psram_is_initialized()) {
        return;
    }

    // Strips live in internal RAM: no PSRAM writes, no cache sync, and the next
    // strip is dithered while the previous one is still going out over SPI.
    uint8_t *strips[STRIP_BUFFERS];
    for (int i = 0; i < STRIP_BUFFERS; i++) {
        strips[i] = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_ERROR_CHECK(strips[i] ? ESP_OK : ESP_ERR_NO_MEM);
    }


    free_strip_buffers = xSemaphoreCreateCounting(STRIP_BUFFERS, STRIP_BUFFERS);
    ESP_ERROR_CHECK(free_strip_buffers ? ESP_OK : ESP_ERR_NO_MEM);

    main_task = xTaskGetCurrentTaskHandle();
    BaseType_t worker_created = xTaskCreatePinnedToCore(
        dither_worker, "dither", 3072, NULL, uxTaskPriorityGet(NULL), &dither_task,
        (xPortGetCoreID() + 1) % portNUM_PROCESSORS);
    ESP_ERROR_CHECK(worker_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(init_display());
    setup_camera();
    esp_err_t sd_err = init_sd_card();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "Image saving is unavailable until restart");
    }
    ESP_ERROR_CHECK(init_save_button());
    bool save_button_was_released = gpio_get_level(SAVE_BUTTON_GPIO) != 0;

    int next_strip = 0;
    uint64_t frame_grab_total_us = 0;
    uint64_t dither_total_us = 0;
    uint32_t measured_frame_count = 0;
    uint64_t last_display_transfer_total_us = 0;
    uint32_t last_display_transfer_completed_count = 0;

    s_curve_init();

    while (true) {
        int64_t grab_started_us = esp_timer_get_time();
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (frame->width != LCD_WIDTH || frame->height != LCD_HEIGHT) {
            esp_camera_fb_return(frame);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t expected_frame_len = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
        if (frame->len != expected_frame_len) {
            esp_camera_fb_return(frame);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        frame_grab_total_us += (uint64_t)(esp_timer_get_time() - grab_started_us);
        uint64_t frame_dither_total_us = 0;
        for (int y = 0; y < LCD_HEIGHT; y += LCD_STRIP_LINES) {
            uint8_t *strip = strips[next_strip];
            next_strip = (next_strip + 1) % STRIP_BUFFERS;

            // Wait for the transfer that last used this buffer.
            xSemaphoreTake(free_strip_buffers, portMAX_DELAY);

            // memcpy(strip, frame->buf + (size_t)y * ROW_BYTES, STRIP_BYTES);
            int64_t dither_started_us = esp_timer_get_time();
            dither_strip(frame->buf + (size_t)y * ROW_BYTES, strip, y, LCD_STRIP_LINES);
            frame_dither_total_us += (uint64_t)(esp_timer_get_time() - dither_started_us);

            int64_t transfer_started_us = esp_timer_get_time();
            portENTER_CRITICAL(&display_timing_mux);
            if (display_transfer_count < STRIP_BUFFERS) {
                uint8_t tail =
                    (display_transfer_head + display_transfer_count) % STRIP_BUFFERS;
                display_transfer_started_us[tail] = transfer_started_us;
                display_transfer_count++;
            }
            portEXIT_CRITICAL(&display_timing_mux);

            esp_err_t err = esp_lcd_panel_draw_bitmap(
                panel, 0, y, LCD_WIDTH, y + LCD_STRIP_LINES, strip);
            if (err != ESP_OK) {
                portENTER_CRITICAL(&display_timing_mux);
                if (display_transfer_count > 0) {
                    display_transfer_count--;
                }
                portEXIT_CRITICAL(&display_timing_mux);
                // No completion callback will release this buffer.
                xSemaphoreGive(free_strip_buffers);
            }
        }
        dither_total_us += frame_dither_total_us;
        measured_frame_count++;

        if (measured_frame_count == TIMING_LOG_FRAME_COUNT) {
            uint64_t display_total_us;
            uint32_t display_completed_count;
            portENTER_CRITICAL(&display_timing_mux);
            display_total_us =
                display_transfer_total_us - last_display_transfer_total_us;
            display_completed_count =
                display_transfer_completed_count -
                last_display_transfer_completed_count;
            last_display_transfer_total_us = display_transfer_total_us;
            last_display_transfer_completed_count = display_transfer_completed_count;
            portEXIT_CRITICAL(&display_timing_mux);

            uint64_t display_frame_us = 0;
            if (display_completed_count > 0) {
                display_frame_us = display_total_us * (LCD_HEIGHT / LCD_STRIP_LINES) /
                                   display_completed_count;
            }
            ESP_LOGI(TAG,
                     "avg over %u frames: fb_get=%llu us, dithering=%llu us, "
                     "LCD DMA/frame~%llu us (%u completed strips)",
                     (unsigned)measured_frame_count,
                     (unsigned long long)(frame_grab_total_us / measured_frame_count),
                     (unsigned long long)(dither_total_us / measured_frame_count),
                     (unsigned long long)display_frame_us,
                     (unsigned)display_completed_count);

            frame_grab_total_us = 0;
            dither_total_us = 0;
            measured_frame_count = 0;
        }

        bool save_button_is_released = gpio_get_level(SAVE_BUTTON_GPIO) != 0;
        if (save_button_was_released && !save_button_is_released) {
            vTaskDelay(pdMS_TO_TICKS(30));
            save_button_is_released = gpio_get_level(SAVE_BUTTON_GPIO) != 0;
            if (!save_button_is_released) {
                esp_err_t save_err = save_frame_to_sd(frame);
                if (save_err != ESP_OK) {
                    ESP_LOGE(TAG, "Image save failed: %s", esp_err_to_name(save_err));
                }
            }
        }
        save_button_was_released = save_button_is_released;

        // Every strip is already copied out, the camera can refill this buffer.
        esp_camera_fb_return(frame);
    }
}
