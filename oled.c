/*
 * Simple OLED display and text logic Copyright ©2019-21 Adrian Kennard, Andrews & Arnold Ltd This code handles SPI to an SSD1351
 * controller, but can easily be adapted to others
 * 
 * The drawing functions all text and some basic graphics Always use oled_lock() and oled_unlock() around drawing, this ensures they
 * are atomically updated to the physical display, and allows the drawing state to be held without clashes with other tasks.
 * 
 * The drawing state includes:- - Position of cursor - Foreground and background colour - Alignment of that position in next drawn
 * object - Movement after drawing (horizontal or vertical)
 * 
 * Pixels are set to an "intensity" (0-255) to which a current foreground and background colour is applied. In practice this may be
 * fewer bits, e.g. on SDD1351 only top 4 bits of intensity are used, multiplied by the selected colour to make a 16 bit RGB For a
 * mono display the intensity directly relates to the grey scale used.
 * 
 * Functions are described in the include file.
 * 
 */
static const char TAG[] = "OLED";

#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <hal/spi_types.h>
#include <driver/spi_common.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "oled.h"

#ifdef	CONFIG_OLED_FONT0
#include "font0.h"
#endif
#ifdef	CONFIG_OLED_FONT1
#include "font1.h"
#endif
#ifdef	CONFIG_OLED_FONT2
#include "font2.h"
#endif
#ifdef	CONFIG_OLED_FONT3
#include "font3.h"
#endif
#ifdef	CONFIG_OLED_FONT4
#include "font4.h"
#endif
#ifdef	CONFIG_OLED_FONT5
#include "font5.h"
#endif

static uint8_t const *fonts[] = {
#ifdef	CONFIG_OLED_FONT0
   font0,
#else
   NULL,
#endif
#ifdef	CONFIG_OLED_FONT1
   font1,
#else
   NULL,
#endif
#ifdef	CONFIG_OLED_FONT2
   font2,
#else
   NULL,
#endif
#ifdef	CONFIG_OLED_FONT3
   font3,
#else
   NULL,
#endif
#ifdef	CONFIG_OLED_FONT4
   font4,
#else
   NULL,
#endif
#ifdef	CONFIG_OLED_FONT5
   font5,
#else
   NULL,
#endif
};

#define	BLACK	0
#if CONFIG_OLED_BPP == 16
/* RGB */
#define ISHIFT  4               /* 4 bits per colour intensity */
#define R       (1<<11)
#define G       (1<<5)
#define B       (1)

#define RED     (R+R)
#define GREEN   (G+G+G+G)
#define BLUE    (B+B)

#define CYAN    (GREEN+BLUE)
#define MAGENTA (RED+BLUE)
#define YELLOW  (RED+GREEN)

#define WHITE   (RED+GREEN+BLUE)

#elif CONFIG_OLED_BPP <= 8
/* Grey */
#define WHITE   1
#define ISHIFT  (8-CONFIG_OLED_BPP)

#endif

#if CONFIG_OLED_BPP>16
typedef uint32_t oled_cell_t;
#define OLEDSIZE (CONFIG_OLED_WIDTH * CONFIG_OLED_HEIGHT * sizeof(oled_cell_t))
#elif CONFIG_OLED_BPP>8
typedef uint16_t oled_cell_t;
#define OLEDSIZE (CONFIG_OLED_WIDTH * CONFIG_OLED_HEIGHT * sizeof(oled_cell_t))
#else
typedef uint8_t oled_cell_t;
#define OLEDSIZE (CONFIG_OLED_WIDTH * CONFIG_OLED_HEIGHT * CONFIG_OLED_BPP / 8)
#endif
static oled_cell_t *oled = NULL;

/* general global stuff */
static TaskHandle_t oled_task_id = NULL;
static SemaphoreHandle_t oled_mutex = NULL;
static int8_t   oled_port = 0;
static int8_t   oled_flip = 0;
static int8_t   oled_dc = -1;
static int8_t   oled_rst = -1;
static int8_t   oled_locks = 0;
static spi_device_handle_t oled_spi;
static volatile uint8_t oled_changed = 1;
static volatile uint8_t oled_update = 0;
static oled_intensity_t oled_contrast = 255;

/* drawing state */
static oled_pos_t x = 0,
                y = 0;          /* position */
static oled_align_t a = 0;      /* alignment and movement */
static char     f = 0,          /* colour */
                b = 0;
static uint32_t f_mul = 0,
                b_mul = 0;      /* actual f/b colour multiplier */

/* state control */
void
oled_pos(oled_pos_t newx, oled_pos_t newy, oled_align_t newa)
{                               /* Set position */
   x = newx;
   y = newy;
   a = (newa ? : (OLED_L | OLED_T | OLED_H));
}

static          uint32_t
oled_colour_lookup(char c)
{                               /* character to colour mapping, default is white */
   switch (c)
   {
   case 'k':
   case 'K':
      return BLACK;
#if CONFIG_OLED_BPP == 16
   case 'r':
      return (RED >> 1);
   case 'E':
      return RED;
   case 'g':
      return (GREEN >> 1);
   case 'G':
      return GREEN;
   case 'b':
      return (BLUE >> 1);
   case 'B':
      return BLUE;
   case 'c':
      return (CYAN >> 1);
   case 'C':
      return CYAN;
   case 'm':
      return (MAGENTA >> 1);
   case 'M':
      return MAGENTA;
   case 'y':
      return (YELLOW >> 1);
   case 'Y':
      return YELLOW;
   case 'w':
      return (WHITE >> 1);
   case 'o':
   case 'O':
      return RED + (GREEN >> 1);
#endif
   }
   return WHITE;
}

void
oled_colour(char newf)
{                               /* Set foreground */
   f_mul = oled_colour_lookup(f = newf);
}

void
oled_background(char newb)
{                               /* Set background */
   b_mul = oled_colour_lookup(b = newb);
}

/* State get */
oled_pos_t
oled_x(void)
{
   return x;
}

oled_pos_t
oled_y(void)
{
   return y;
}

oled_align_t
oled_a(void)
{
   return a;
}

char
oled_f(void)
{
   return f;
}

char
oled_b(void)
{
   return b;
}

/* support */
static inline void
oled_pixel(oled_pos_t x, oled_pos_t y, oled_intensity_t i)
{                               /* set a pixel */
   if (x < 0 || x >= CONFIG_OLED_WIDTH || y < 0 || y >= CONFIG_OLED_HEIGHT)
      return;                   /* out of display */
#if CONFIG_OLED_BPP <= 8
#error	Not coded greyscale yet
#else
   oled[(y * CONFIG_OLED_WIDTH) + x] = ntohs(f * (i >> ISHIFT) + b * ((~i) >> ISHIFT));
#endif
}

static void
oled_draw(oled_pos_t w, oled_pos_t h, oled_pos_t wm, oled_pos_t hm, oled_pos_t * xp, oled_pos_t * yp)
{                               /* move x/y based on drawing a box w/h, set x/y as top left of said box */
   oled_pos_t      l = x,
                   t = y;
   if ((a & OLED_C) == OLED_C)
      l -= (w - 1) / 2;
   else if (a & OLED_R)
      l -= (w - 1);
   if ((a & OLED_M) == OLED_M)
      t -= (h - 1) / 2;
   else if (a & OLED_B)
      t -= (h - 1);
   if (a & OLED_H)
   {
      if (a & OLED_L)
         x += w + wm;
      if (a & OLED_R)
         x -= w + wm;
   }
   if (a & OLED_V)
   {
      if (a & OLED_T)
         y += h + hm;
      if (a & OLED_B)
         y -= h + hm;
   }
   if (xp)
      *xp = l;
   if (yp)
      *yp = t;
}

static void
oled_block16(oled_pos_t x, oled_pos_t y, oled_pos_t w, oled_pos_t h, const uint8_t * data, int l)
{                               /* Draw a block from 16 bit greyscale data, l is data width for each row */
   if (!l)
      l = (w + 1) / 2;          /* default is pixels width */
   for (oled_pos_t row = 0; row < h; row++)
   {
      for (oled_pos_t col = 0; col < w; col++)
      {
         uint8_t         v = data[col / 2];
         oled_pixel(x + col, y + row, (v & 0xF0) | (v >> 4));
         col++;
         if (col < w)
            oled_pixel(x + col, y + row, (v & 0xF) | (v << 4));
      }
      data += l;
   }
}

/* drawing */
void
oled_clear(oled_intensity_t i)
{
   if (!oled)
      return;
   for (oled_pos_t y = 0; y < CONFIG_OLED_HEIGHT; y++)
      for (oled_pos_t x = 0; x < CONFIG_OLED_WIDTH; x++)
         oled_pixel(x, y, i);
}

void
oled_set_contrast(oled_intensity_t contrast)
{
   if (!oled)
      return;
   oled_contrast = contrast;
   oled_update = 1;
   oled_changed = 1;
}

void
oled_box(oled_pos_t w, oled_pos_t h, oled_intensity_t i)
{                               /* draw a box, not filled */
   oled_pos_t      x,
                   y;
   oled_draw(w, h, 0, 0, &x, &y);
   for (oled_pos_t n = 0; n < w; n++)
   {
      oled_pixel(x + n, y, i);
      oled_pixel(x + n, y + h - 1, i);
   }
   for (oled_pos_t n = 1; n < w - 1; n++)
   {
      oled_pixel(x, y + n, i);
      oled_pixel(x + w - 1, y + n, i);
   }
}

void
oled_fill(oled_pos_t w, oled_pos_t h, oled_intensity_t i)
{                               /* draw a filled rectangle */
   oled_pos_t      x,
                   y;
   oled_draw(w, h, 0, 0, &x, &y);
   for (oled_pos_t row = 0; row < h; row++)
      for (oled_pos_t col = 0; col < w; col++)
         oled_pixel(x + col, y + row, i);
}

void
oled_icon16(oled_pos_t w, oled_pos_t h, const void *data)
{                               /* Icon, 16 bit packed */
   if (!data)
      oled_fill(w, h, 0);       /* No icon */
   else
   {
      oled_pos_t      x,
                      y;
      oled_draw(w, h, 0, 0, &x, &y);
      oled_block16(x, y, w, h, data, 0);
   }
}

void
oled_text(int8_t size, const char *fmt,...)
{                               /* Size negative for descenders */
   if (!oled)
      return;
   oled_changed = 1;            /* TODO */
   va_list         ap;
   char            temp[CONFIG_OLED_WIDTH / 4 + 2];
   va_start(ap, fmt);
   vsnprintf(temp, sizeof(temp), fmt, ap);
   va_end(ap);
   int             z = 7;       /* effective height */
   if (size < 0)
   {                            /* indicates descenders allowed */
      size = -size;
      z = 9;
   } else if (!size)
      z = 5;
   if (size > sizeof(fonts) / sizeof(*fonts))
      size = sizeof(fonts) / sizeof(*fonts);
   if (!fonts[size])
      return;
   int             fontw = (size ? 6 * size : 4);       /* pixel width of characters in font file */
   int             fonth = (size ? 9 * size : 5);       /* pixel height of characters in font file */

   int             w = 0;       /* width of overall text */
   int             h = z * (size ? : 1);        /* height of overall text */
   int             cwidth(char c)
   {                            /* character width as printed - some characters are done narrow, and <' ' is fixed size move */
      if (c & 0x80)
         return 0;
      if (size)
      {
         if (c < ' ')
            return c * size;
         if (c == ':' || c == '.')
            return size * 2;
      }
                      return fontw;
   }
   const uint8_t  *fontdata(char c)
   {
      const uint8_t  *d = fonts[size] + (c - ' ') * fonth * fontw / 2;
      if              (c == ':' || c == '.')
                         d += size;
                    //2 pixels in
                      return d;
   }
   for             (char *p = temp; *p; p++)
      w += cwidth(*p);
   oled_pos_t      x,
                   y;
   if (w)
      w -= (size ? : 1);
   //Margin right hand pixel needs removing from width
      oled_draw(w, h, size ? : 1, size ? : 1, &x, &y);  /* starting point */
   for (char *p = temp; *p; p++)
   {
      int             c = *p;
      int             charw = cwidth(c);
      if (charw)
      {
         if (c < ' ')
            c = ' ';
         oled_block16(x, y, charw, h, fontdata(c), fontw / 2);
         x += charw;
      }
   }
}

static          esp_err_t
oled_cmd(uint8_t cmd)
{                               /* Send command */
   gpio_set_level(oled_dc, 0);
   spi_transaction_t t = {
      .length = 8,
      .tx_data = {cmd},
      .flags = SPI_TRANS_USE_TXDATA,
   };
   esp_err_t       e = spi_device_polling_transmit(oled_spi, &t);
   return e;
}

static          esp_err_t
oled_data(int len, void *data)
{                               /* Send data */
   gpio_set_level(oled_dc, 1);
   spi_transaction_t c = {
      .length = 8 * len,
      .tx_buffer = data,
   };
   return spi_device_transmit(oled_spi, &c);
}

static          esp_err_t
oled_cmd1(uint8_t cmd, uint8_t a)
{                               /* Send a command with an arg */
   esp_err_t       e = oled_cmd(cmd);
   if (e)
      return e;
   gpio_set_level(oled_dc, 1);
   spi_transaction_t d = {
      .length = 8,
      .tx_data = {a},
      .flags = SPI_TRANS_USE_TXDATA,
   };
   return spi_device_polling_transmit(oled_spi, &d);
}

static          esp_err_t
oled_cmd2(uint8_t cmd, uint8_t a, uint8_t b)
{                               /* Send a command with args */
   esp_err_t       e = oled_cmd(cmd);
   if (e)
      return e;
   gpio_set_level(oled_dc, 1);
   spi_transaction_t d = {
      .length = 16,
      .tx_data = {a, b},
      .flags = SPI_TRANS_USE_TXDATA,
   };
   return spi_device_polling_transmit(oled_spi, &d);
}

static          esp_err_t
oled_cmd3(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c)
{                               /* Send a command with args */
   esp_err_t       e = oled_cmd(cmd);
   if (e)
      return e;
   gpio_set_level(oled_dc, 1);
   spi_transaction_t d = {
      .length = 24,
      .tx_data = {a, b, c},
      .flags = SPI_TRANS_USE_TXDATA,
   };
   return spi_device_polling_transmit(oled_spi, &d);
}

static void
oled_task(void *p)
{
   int             try = 10;
   esp_err_t       e = 0;
   usleep(300000);              /* 300ms to start up */
   while (try--)
   {
      oled_lock();
      if (oled_rst >= 0)
      {
         /* Reset */
         gpio_set_level(oled_rst, 0);
         usleep(1000);
         gpio_set_level(oled_rst, 1);
         usleep(1000);
      }
      e = oled_cmd(0xAF);       /* start */
      usleep(10000);
      /* Many of these are setting as defaults, just to be sure */
      e += oled_cmd(0xA5);      /* white */
      e += oled_cmd1(0xA0, oled_flip ? 0x34 : 0x26);    /* flip and colour mode */
      e += oled_cmd1(0xFD, 0x12);       /* unlock */
      e += oled_cmd1(0xFD, 0xB1);       /* unlock */
      e += oled_cmd1(0xB3, 0xF1);       /* Frequency */
      e += oled_cmd1(0xCA, 0x7F);       /* MUX */
      e += oled_cmd1(0xA1, 0x00);       /* Start 0 */
      e += oled_cmd1(0xA2, 0x00);       /* Offset 0 */
      e += oled_cmd1(0xAB, 0x01);       /* Regulator */
      e += oled_cmd3(0xB4, 0xA0, 0xB5, 0x55);   /* VSL */
      e += oled_cmd3(0xC1, 0xC8, 0x80, 0xC0);   /* Contrast */
      e += oled_cmd1(0xC7, 0x0F);       /* current */
      e += oled_cmd1(0xB1, 0x32);       /* clocks */
      e += oled_cmd3(0xB2, 0xA4, 0x00, 0x00);   /* enhance */
      e += oled_cmd1(0xBB, 0x17);       /* pre-charge voltage */
      e += oled_cmd1(0xB6, 0x01);       /* pre-charge period */
      e += oled_cmd1(0xBE, 0x05);       /* COM deselect voltage */
      e += oled_cmd1(0xFD, 0xB0);       /* lock */
      oled_cmd2(0x15, 0, 127);
      oled_cmd2(0x75, 0, 127);
      oled_cmd(0x5C);
      oled_data(OLEDSIZE, (void *)oled);
      oled_cmd(0xA6);
      oled_unlock();
      if (!e)
         break;
      sleep(1);
   }
   if (e)
   {
      ESP_LOGE(TAG, "Configuration failed %s", esp_err_to_name(e));
      free(oled);
      oled = NULL;
      oled_port = -1;
      vTaskDelete(NULL);
      return;
   }
   oled_update = 1;
   while (1)
   {                            /* Update */
      if (!oled_changed)
      {
         usleep(100000);
         continue;
      }
      oled_lock();
      oled_changed = 0;
      oled_cmd2(0x15, 0, 127);
      oled_cmd2(0x75, 0, 127);
      oled_cmd(0x5C);
      oled_data(OLEDSIZE, (void *)oled);
      if (oled_update)
      {
         oled_update = 0;
         oled_cmd1(0xC7, oled_contrast >> 4);
      }
      oled_unlock();
   }
}

const char     *
oled_start(int8_t port, int8_t cs, int8_t clk, int8_t din, int8_t dc, int8_t rst, int8_t flip)
{                               /* Start OLED task and display */
   if (din < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(din))
      return "DIN?";
   if (clk < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(clk))
      return "CLK?";
   if (dc < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(dc))
      return "DC?";
   if (cs < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(cs))
      return "CS?";
   if (port != SPI2_HOST && port != SPI3_HOST)
      return "Bad port";
   if (rst >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(rst))
      return "RST?";
   oled_mutex = xSemaphoreCreateMutex();        /* Shared text access */
   oled = malloc(OLEDSIZE);
   if (!oled)
      return "Mem?";
   memset(oled, 0, OLEDSIZE);
   oled_flip = flip;
   oled_port = port;
   oled_dc = dc;
   oled_rst = rst;
   spi_bus_config_t config = {
      .mosi_io_num = din,
      .miso_io_num = -1,
      .sclk_io_num = clk,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 8 * (OLEDSIZE + 8),
      .flags = SPICOMMON_BUSFLAG_MASTER,
   };
   if (port == HSPI_HOST && din == 22 && clk == 18 && cs == 5)
      config.flags |= SPICOMMON_BUSFLAG_IOMUX_PINS;
   if (spi_bus_initialize(port, &config, 2))
      return "Init?";
   spi_device_interface_config_t devcfg =
   {
      .clock_speed_hz = SPI_MASTER_FREQ_20M | SPI_DEVICE_3WIRE,
      .mode = 0,
      .spics_io_num = cs,
      .queue_size = 1,
   };
   if (spi_bus_add_device(port, &devcfg, &oled_spi))
      return "Add?";
   gpio_set_direction(dc, GPIO_MODE_OUTPUT);
   if (rst >= 0)
      gpio_set_direction(rst, GPIO_MODE_OUTPUT);
   xTaskCreate(oled_task, "OLED", 8 * 1024, NULL, 2, &oled_task_id);
   return NULL;
}

void
oled_lock(void)
{                               /* Lock display task */
   if (oled_mutex)
      xSemaphoreTake(oled_mutex, portMAX_DELAY);
   oled_locks++;
   /* preset state */
   oled_background('k');
   oled_colour('w');
   oled_pos(0, 0, OLED_L | OLED_T | OLED_H);
}

void
oled_unlock(void)
{                               /* Unlock display task */
   oled_locks--;
   if (oled_mutex)
      xSemaphoreGive(oled_mutex);
}
