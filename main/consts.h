#include <stdint.h>
#include <stdlib.h>
#include "driver/spi_master.h"

#define MHZ 1000000

#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (80 * MHZ)

#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define PALETTE_SIZE 5
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

typedef struct {
  uint8_t r, g, b;
  int dst;
} Color;

extern const uint8_t DB[8];
extern const int8_t bayer8x8[8][8];
extern const Color palettesss[8][PALETTE_SIZE];