// Simple OLED display and text logic
// Copyright © 2019 Adrian Kennard Andrews & Arnold Ltd
static const char TAG[] = "OLED";

#include <unistd.h>
#include <string.h>
#include <driver/i2c.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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

#define OLEDSIZE (CONFIG_OLED_WIDTH * CONFIG_OLED_HEIGHT * CONFIG_OLED_BPP / 8)
static uint8_t *oled = NULL;
static TaskHandle_t oled_task_id = NULL;
static SemaphoreHandle_t oled_mutex = NULL;
static int8_t oled_port = 0;
static int8_t oled_address = 0;
static int8_t oled_flip = 0;
static volatile uint8_t oled_changed = 1;
static volatile uint8_t oled_update = 0;
static uint8_t oled_contrast = 127;

void
oled_clear (void)
{
   if (!oled)
      return;
   memset (oled, 0, OLEDSIZE);
   oled_changed = 1;
}

void
oled_set_contrast (uint8_t contrast)
{
   if (!oled)
      return;
   oled_lock ();
   oled_contrast = contrast;
   if (oled_update)
      oled_update = 1;          // Force sending new contrast
   oled_changed = 1;
   oled_unlock ();
}

void
oled_set (int x, int y, int v)
{
   if (!oled)
      return;
   if (x < 0 || x >= CONFIG_OLED_WIDTH || y < 0 || y >= CONFIG_OLED_HEIGHT)
      return;
   uint8_t s = ((8 - CONFIG_OLED_BPP) - CONFIG_OLED_BPP * (x % (8 / CONFIG_OLED_BPP)));
   uint8_t m = (((1 << CONFIG_OLED_BPP) - 1) << s);
   uint8_t *p = oled + y * CONFIG_OLED_WIDTH * CONFIG_OLED_BPP / 8 + x * CONFIG_OLED_BPP / 8;
   *p = (*p & ~m) | ((v << s) & m);
}

int
oled_get (int x, int y)
{
   if (!oled)
      return -1;
   if (x < 0 || x >= CONFIG_OLED_WIDTH || y < 0 || y >= CONFIG_OLED_HEIGHT)
      return -1;
   uint8_t s = ((8 - CONFIG_OLED_BPP) - CONFIG_OLED_BPP * (x % (8 / CONFIG_OLED_BPP)));
   uint8_t m = (((1 << CONFIG_OLED_BPP) - 1) << s);
   uint8_t *p = oled + y * CONFIG_OLED_WIDTH * CONFIG_OLED_BPP / 8 + x * CONFIG_OLED_BPP / 8;
   return (*p & m) >> s;
}

static inline int
oled_copy (int x, int y, const uint8_t * src, int dx)
{                               // Copy pixels
   if (!oled)
      return 0;
   x -= x % (8 / CONFIG_OLED_BPP);      // Align to byte
   dx -= dx % (8 / CONFIG_OLED_BPP);    // Align to byte
   if (y >= 0 && y < CONFIG_OLED_HEIGHT && x + dx >= 0 && x < CONFIG_OLED_WIDTH)
   {                            // Fits
      int pix = dx;
      if (x < 0)
      {                         // Truncate left
         pix += x;
         x = 0;
      }
      if (x + pix > CONFIG_OLED_WIDTH)
         pix = CONFIG_OLED_WIDTH - x;   // Truncate right
      uint8_t *dst = oled + y * CONFIG_OLED_WIDTH * CONFIG_OLED_BPP / 8 + x * CONFIG_OLED_BPP / 8;
      if (src)
      {                         // Copy
         if (memcmp (dst, src, pix * CONFIG_OLED_BPP / 8))
         {                      // Changed
            memcpy (dst, src, pix * CONFIG_OLED_BPP / 8);
            oled_changed = 1;
         }
      } else
      {                         // Clear
         memset (dst, 0, pix * CONFIG_OLED_BPP / 8);
         oled_changed = 1;
      }
   }
   if (!src)
      return 0;
   return dx * CONFIG_OLED_BPP / 8;     // Bytes (would be) copied
}

int
oled_text (int8_t size, int x, int y, const char *fmt, ...)
{                               // Size negative for descenders
   if (!oled)
      return 0;
   va_list ap;
   char temp[CONFIG_OLED_WIDTH / 4 + 2],
    *t = temp;
   va_start (ap, fmt);
   vsnprintf (temp, sizeof (temp), fmt, ap);
   va_end (ap);
   int z = 7;
   if (size < 0)
   {
      size = -size;
      z = 9;
   } else if (!size)
      z = 5;
   if (size > sizeof (fonts) / sizeof (*fonts))
      size = sizeof (fonts) / sizeof (*fonts);
   if (!fonts[size])
      return 0;
   int w = (size ? 6 * size : 4);
   int h = (size ? 9 * size : 5);
   y -= size * 2;               // Baseline
   while (*t)
   {
      int c = *t++;
      if (c >= 0x7F)
         continue;
      int ww = w;
      if (c < ' ')
      {                         // Sub space
         if (size)
            ww = size * c;
         c = ' ';
      }
      const uint8_t *base = fonts[size] + (c - ' ') * h * w * CONFIG_OLED_BPP / 8;
      if (size && (c == '.' || c == ':'))
      {
         ww = size * 2;
         base += size * 2 * CONFIG_OLED_BPP / 8;
      }                         // Special case for .
      for (int dy = 0; dy < (size ? : 1) * z; dy++)
      {
         oled_copy (x, y + h - 1 - dy, base, ww);
         base += w * CONFIG_OLED_BPP / 8;
      }
      x += ww;
   }
   return x;
}

int
oled_icon (int x, int y, const void *p, int w, int h)
{                               // Plot an icon
   if (!oled)
      return 0;
   for (int dy = 0; dy < h; dy++)
      p += oled_copy (x, y + h - dy - 1, p, w);
   return x + w;
}

static void
oled_task (void *p)
{
   int try = 10;
   esp_err_t e = 0;
   while (try--)
   {
      oled_lock ();
      i2c_cmd_handle_t t = i2c_cmd_link_create ();
      i2c_master_start (t);
      i2c_master_write_byte (t, (oled_address << 1) | I2C_MASTER_WRITE, true);
      i2c_master_write_byte (t, 0x00, true);    // Cmds
      i2c_master_write_byte (t, 0xA5, true);    // White
      i2c_master_write_byte (t, 0xAF, true);    // On
      i2c_master_write_byte (t, 0xA0, true);    // Remap
      i2c_master_write_byte (t, oled_flip ? 0x52 : 0x41, true); // Match display
      i2c_master_stop (t);
      e = i2c_master_cmd_begin (oled_port, t, 10 / portTICK_PERIOD_MS);
      i2c_cmd_link_delete (t);
      oled_unlock ();
      if (!e)
         break;
      sleep (1);
   }
   if (e)
   {
      ESP_LOGE (TAG, "Configuration failed %s", esp_err_to_name (e));
      free (oled);
      oled = NULL;
      oled_port = -1;
      vTaskDelete (NULL);
      return;
   }

   while (1)
   {                            // Update
      if (!oled_changed)
      {
         usleep (100000);
         continue;
      }
      oled_lock ();
      oled_changed = 0;
      i2c_cmd_handle_t t;
      e = 0;
      if (oled_update < 2)
      {                         // Set up
         t = i2c_cmd_link_create ();
         i2c_master_start (t);
         i2c_master_write_byte (t, (oled_address << 1) | I2C_MASTER_WRITE, true);
         i2c_master_write_byte (t, 0x00, true); // Cmds
         if (oled_update)
            i2c_master_write_byte (t, 0xA4, true);      // Normal mode
         i2c_master_write_byte (t, 0x81, true); // Contrast
         i2c_master_write_byte (t, oled_contrast, true);        // Contrast
         i2c_master_write_byte (t, 0x15, true); // Col
         i2c_master_write_byte (t, 0x00, true); // 0
         i2c_master_write_byte (t, 0x7F, true); // 127
         i2c_master_write_byte (t, 0x75, true); // Row
         i2c_master_write_byte (t, 0x00, true); // 0
         i2c_master_write_byte (t, 0x7F, true); // 127
         i2c_master_stop (t);
         e = i2c_master_cmd_begin (oled_port, t, 100 / portTICK_PERIOD_MS);
         i2c_cmd_link_delete (t);
      }
      if (!e)
      {                         // data
         t = i2c_cmd_link_create ();
         i2c_master_start (t);
         i2c_master_write_byte (t, (oled_address << 1) | I2C_MASTER_WRITE, true);
         i2c_master_write_byte (t, 0x40, true); // Data
         i2c_master_write (t, oled, OLEDSIZE, true);    // Buffer
         i2c_master_stop (t);
         e = i2c_master_cmd_begin (oled_port, t, 100 / portTICK_PERIOD_MS);
         i2c_cmd_link_delete (t);
      }
      if (e)
         ESP_LOGE (TAG, "Data failed %s", esp_err_to_name (e));
      if (!oled_update || e)
      {
         oled_update = 1;       // Resend data
         oled_changed = 1;
      } else
         oled_update = 2;       // All OK
      oled_unlock ();
   }
}

void
oled_start (int8_t port, uint8_t address, int8_t scl, int8_t sda, int8_t flip)
{                               // Start OLED task and display
   if (scl < 0 || sda < 0 || port < 0)
      return;
   oled_mutex = xSemaphoreCreateMutex ();       // Shared text access
   oled = malloc (OLEDSIZE);
   if (!oled)
      return;
   memset (oled, 0, OLEDSIZE);
   oled_flip = flip;
   oled_port = port;
   oled_address = address;
   if (i2c_driver_install (oled_port, I2C_MODE_MASTER, 0, 0, 0))
   {
      ESP_LOGE (TAG, "I2C config fail");
      oled_port = -1;
      free (oled);
      oled = NULL;
   } else
   {
      i2c_config_t config = {
         .mode = I2C_MODE_MASTER,
         .sda_io_num = sda,
         .scl_io_num = scl,
         .sda_pullup_en = true,
         .scl_pullup_en = true,
         .master.clk_speed = 100000,
      };
      if (i2c_param_config (oled_port, &config))
      {
         i2c_driver_delete (oled_port);
         ESP_LOGE (TAG, "I2C config fail");
         oled_port = -1;
      } else
         i2c_set_timeout (oled_port, 160000);   // 2ms? allow for clock stretching
   }
   xTaskCreate (oled_task, "OLED", 8 * 1024, NULL, 2, &oled_task_id);
}

void
oled_lock (void)
{                               // Lock display task
   if (oled_mutex)
      xSemaphoreTake (oled_mutex, portMAX_DELAY);
}

void
oled_unlock (void)
{                               // Unlock display task
   if (oled_mutex)
      xSemaphoreGive (oled_mutex);
}
