#include <stddef.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_psram.h"

#include "main.h"

_Static_assert(LCD_WIDTH % 8 == 0, "dithering processes 8 pixels at a time");
_Static_assert(LCD_HEIGHT % LCD_STRIP_LINES == 0, "strips must tile the frame");

static TaskHandle_t main_task;
static TaskHandle_t dither_task;
static dither_job_t dither_job;
SemaphoreHandle_t free_strip_buffers;

bool saveFrame = false;

static void dither_worker(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        Dither(dither_job.src, dither_job.dest, LCD_WIDTH,
                                      dither_job.rows, dither_job.first_row);
        xTaskNotifyGive(main_task);
    }
}

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
    Dither(src, dest, LCD_WIDTH, top_rows, first_row);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}


void app_main(void)
{
    if (!esp_psram_is_initialized()) return;

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

    init_buttons();
    esp_err_t err = init_sd_card();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD init failed: %s (continuing without SD)", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(init_display());
    ESP_ERROR_CHECK(init_camera_ov3660());

    int next_strip = 0;


    /* looooooooooooooooooop */
    while (true) {
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for (int y = 0; y < LCD_HEIGHT; y += LCD_STRIP_LINES) {
            uint8_t *strip = strips[next_strip];
            next_strip = (next_strip + 1) % STRIP_BUFFERS;

            xSemaphoreTake(free_strip_buffers, portMAX_DELAY);

            // memcpy(strip, frame->buf + (size_t)y * ROW_BYTES, STRIP_BYTES);
            dither_strip(frame->buf + (size_t)y * ROW_BYTES, strip, y, LCD_STRIP_LINES);

            esp_err_t err = draw_frame(0, y, LCD_WIDTH, y + LCD_STRIP_LINES, strip);
            if (err != ESP_OK) {
                xSemaphoreGive(free_strip_buffers);
            }
        }

        if (saveFrame) {
            saveFrame = false;
            save_frame_to_sd(frame);
        }

        // Every strip is already copied out, the camera can refill this buffer.
        esp_camera_fb_return(frame);
    }
}
