#include <stdint.h>
#include <stdlib.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"

#define MHZ 1000000

#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (80 * MHZ)

#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_STRIP_LINES 24
#define PALETTE_SIZE 11
#define PALETTE_COUNT 5
#define DST_MAX (255 * 255 * 3)
#define DST_MIN 0
#define DST_DIFF 1000

#define CAM_PIN_PWDN   (-1)
#define CAM_PIN_RESET  (-1)
#define CAM_PIN_XCLK   (15)
#define CAM_PIN_SDA    (4)
#define CAM_PIN_SCL    (5)

#define CAM_PIN_VSYNC  (6)
#define CAM_PIN_HREF   (7)
#define CAM_PIN_PCLK   (13)

#define LCD_PIN_MOSI   (1) 
#define LCD_PIN_SCLK   (14) 
#define LCD_PIN_CS     (21) 
#define LCD_PIN_DC     (3) 
#define LCD_PIN_RESET  (2) 

#define CONTRAST_STRENGTH 160

#define ROW_BYTES (LCD_WIDTH * sizeof(uint16_t))
#define STRIP_BYTES (ROW_BYTES * LCD_STRIP_LINES)
#define STRIP_BUFFERS 2

static const char *TAG = "camera_lcd";

#define SD_MOUNT_POINT "/sdcard"

#define SDMMC_PIN_CMD GPIO_NUM_38
#define SDMMC_PIN_CLK GPIO_NUM_39
#define SDMMC_PIN_D0 GPIO_NUM_40

#define SAVE_BUTTON_GPIO GPIO_NUM_20
#define PALETTE_BUTTON_GPIO GPIO_NUM_19

typedef struct {
    const uint8_t *src;
    uint8_t *dest;
    int first_row;
    int rows;
} dither_job_t;

typedef void (*button_cb_t)(void *arg);

typedef struct {
    gpio_num_t   pin;
    button_cb_t  on_press;
    void        *arg;            // passed to on_press
    bool         was_released;
} button_t;

typedef union {
  struct {
    uint16_t b :5, g :6, r :5;
  };
  uint16_t raw;
} Color;

extern const uint8_t DB[8];
extern const uint8_t palette_rgb888[][3];
extern const Color palettesss[PALETTE_COUNT][PALETTE_SIZE];
extern const int8_t bayer8x8[8][8];
extern const int8_t BAYER[4][4];