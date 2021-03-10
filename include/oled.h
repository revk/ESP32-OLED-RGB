// Simple OLED display and text logic
// Copyright © 2019 Adrian Kennard Andrews & Arnold Ltd

void oled_start (int8_t port, uint8_t address, int8_t scl, int8_t sda,int8_t flip);
void oled_lock(void);
void oled_unlock(void);
void oled_set_contrast(uint8_t contrast); // Locks and unlocks so do not call while locked
void oled_clear(void);
int oled_text (int8_t size, int x, int y, const char *fmt,...);
int oled_icon (int x, int y, const void *p, int w, int h);
void oled_set(int x,int y,int v);
int oled_get(int x,int y);

