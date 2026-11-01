#include <sys/stat.h>

#include "main.h"

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "img_converters.h"
#include "sdmmc_cmd.h"

#include "esp_lcd_st7789.h"

#define BUTTON_INIT(p, cb, a) { .pin = (p), .on_press = (cb), .arg = (a), .was_released = true }

static sdmmc_card_t *sd_card;
static uint32_t picture_count;

static esp_lcd_panel_handle_t panel;

static QueueHandle_t button_queue;

static volatile bool save_requested = false;
static void on_save(void *arg) { (void)arg; saveFrame = true; }
static void on_palette_change(void *arg) { (void)arg; NextPalette(); }

static button_t save_btn = BUTTON_INIT(SAVE_BUTTON_GPIO, on_save, NULL);
static button_t palette_btn = BUTTON_INIT(PALETTE_BUTTON_GPIO, on_palette_change, NULL);

static void IRAM_ATTR button_isr(void *arg)       // arg is the button_t *
{
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(button_queue, &arg, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void button_task(void *unused)
{
    button_t *btn;
    for (;;) {
        if (xQueueReceive(button_queue, &btn, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(30));           // debounce
            if (gpio_get_level(btn->pin) == 0) {     // still pressed
                btn->on_press(btn->arg);
            }
            xQueueReset(button_queue);               // drop bounce edges from the same press
        }
    }
}

esp_err_t save_frame_to_sd(camera_fb_t *frame) {
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

esp_err_t draw_frame(int x_start, int y_start, int x_end, int y_end, const void *color_data) {
    return esp_lcd_panel_draw_bitmap(
            panel, 
            x_start, y_start, 
            x_end, y_end, 
            color_data
        );
}

static bool on_color_transfer_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx 
) {
    (void)panel_io;
    (void)event_data;
    BaseType_t task_woken = pdFALSE;

    xSemaphoreGiveFromISR(user_ctx, &task_woken);
    return task_woken == pdTRUE;
}


void init_buttons(void)
{
    button_t *btns[] = {
        &save_btn,
        &palette_btn
    };
    button_queue = xQueueCreate(8, sizeof(button_t *));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    for (int i = 0; i < 2; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << btns[i]->pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = GPIO_INTR_NEGEDGE,          // press = falling edge
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        ESP_ERROR_CHECK(gpio_isr_handler_add(btns[i]->pin, button_isr, btns[i]));
    }
    xTaskCreate(button_task, "buttons", 3072, NULL, 3, NULL);
}

esp_err_t init_camera_ov3660(void) {

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
        .fb_count       = 2,
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_LATEST,
    };
    ESP_RETURN_ON_ERROR(esp_camera_init(&config), TAG, "Camera init failed");

    sensor_t *cm = esp_camera_sensor_get();
    cm->set_saturation(cm, 2);
    cm->set_whitebal(cm, 1);
    cm->set_awb_gain(cm, 1);
    cm->set_wb_mode(cm, 2);
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

    return ESP_OK;
}

esp_err_t init_sd_card(void)
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
        ESP_LOGW(TAG, "Image saving is unavailable until restart");
        return err;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t init_display(void)
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