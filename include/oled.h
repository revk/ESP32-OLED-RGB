// Simple OLED display and text logic
// Copyright © 2019 Adrian Kennard Andrews & Arnold Ltd

typedef	uint8_t oled_intensity_t;
typedef uint32_t oled_colour_t;
typedef int16_t oled_pos_t;
typedef	uint8_t oled_align_t;

#define	OLED_T	0x01	/* top align */
#define	OLED_M	0x03	/* middle align */
#define	OLED_B	0x02	/* bottom align */
#define	OLED_V	0x08	/* vertical move */
#define	OLED_L	0x10	/* left align */
#define	OLED_C	0x30	/* centre align */
#define	OLED_R	0x20	/* right align */
#define	OLED_H	0x80	/* horizontal move */

/* Set up SPI, and start the update task */
const char*oled_start (int8_t port, int8_t cs,int8_t clk,int8_t din,int8_t dc,int8_t rst,int8_t flip);

/* locking atomic drawing functions */
void oled_lock(void);	/* sets default state to 0, 0, left, top, horizontal, white on black */
void oled_unlock(void);

/* Overall display contrast setting */
void oled_set_contrast(oled_intensity_t);

/* Drawing functions - do a lock first */
/* State setting */
void oled_pos(oled_pos_t x,oled_pos_t y,oled_align_t);	/* Set position, not y=0 is TOP of display */
void oled_colour(oled_colour_t);	/* Set foreground */
void oled_background(oled_colour_t);	/* Set background */

/* State get */
oled_pos_t oled_x(void);
oled_pos_t oled_y(void);
oled_align_t oled_a(void);
oled_colour_t oled_f(void);
oled_colour_t oled_b(void);

/* Drawing */
void oled_clear(oled_intensity_t);	/* clear whole display to current colour (intensity 0 means background colour) */
void oled_box(oled_pos_t w,oled_pos_t h,oled_intensity_t); /* draw a box, not filled */
void oled_fill(oled_pos_t w,oled_pos_t h,oled_intensity_t); /* draw a filled rectangle */
void oled_text(int8_t size, const char *fmt,...); /* text, use -ve size for descenders versions */
void oled_icon16(oled_pos_t w,oled_pos_t h,const void *data);	/* Icon, 16 bit packed */

/* colours */
#define	BLACK	0

#if CONFIG_OLED_BPP == 16

// 16 bit RGB mode

#define	ISHIFT	4	/* 4 bits per colour intensity */
#define	R	(1<<11)
#define	G	(1<<5)
#define	B	(1)
// Multipliers for 4 bit colour to 16 bit

#define	RED	(R+R)
#define	GREEN	(G+G+G+G)
#define	BLUE	(B+B)

#define	CYAN	(GREEN+BLUE)
#define	MAGENTA	(RED+BLUE)
#define	YELLOW	(RED+GREEN)

#define	WHITE	(RED+GREEN+BLUE)

#elif CONFIG_OLED_BPP <= 8

/* simple grey scale */
#define	WHITE	1
#define	ISHIFT	(8-CONFIG_OLED_BPP)

#endif
