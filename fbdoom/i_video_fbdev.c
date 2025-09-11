// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// $Log:$
//
// DESCRIPTION:
//	DOOM graphics stuff for X11, UNIX.
//
//-----------------------------------------------------------------------------

// static const char
// rcsid[] = "$Id: i_x.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include "config.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_event.h"
#include "d_main.h"
#include "i_video.h"
#include "z_zone.h"

#include "tables.h"
#include "doomkeys.h"

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdarg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#ifdef USE_VECTOR
    #include "riscv_vector.h"
#endif

//#define CMAP256

struct fb_var_screeninfo fb = {};
int fb_scaling = 1;
int usemouse = 0;

struct color {
    uint32_t b:8;
    uint32_t g:8;
    uint32_t r:8;
    uint32_t a:8;
};

#ifndef USE_VECTOR
static struct color colors[256];
#endif

// Separate channel arrays for efficient vector access
#ifdef USE_VECTOR
static uint8_t red[256];
static uint8_t green[256];
static uint8_t blue[256];
#endif

// The screen buffer; this is modified to draw things to the screen

byte *I_VideoBuffer = NULL;
byte *I_VideoBuffer_FB = NULL;

/* framebuffer file descriptor */
int fd_fb = 0;

int	X_width;
int X_height;

// If true, game is running as a screensaver

boolean screensaver_mode = false;

// Flag indicating whether the screen is currently visible:
// when the screen isnt visible, don't render the screen

boolean screenvisible;

// Mouse acceleration
//
// This emulates some of the behavior of DOS mouse drivers by increasing
// the speed when the mouse is moved fast.
//
// The mouse input values are input directly to the game, but when
// the values exceed the value of mouse_threshold, they are multiplied
// by mouse_acceleration to increase the speed.

float mouse_acceleration = 2.0;
int mouse_threshold = 10;

// Gamma correction level to use

int usegamma = 0;

typedef struct
{
	byte r;
	byte g;
	byte b;
} col_t;

#ifndef USE_VECTOR
// Palette converted to RGB565

static uint16_t rgb565_palette[256];

void cmap_to_rgb565(uint16_t * out, uint8_t * in, int in_pixels)
{
    int i, j;
    struct color c;
    uint16_t r, g, b;

    for (i = 0; i < in_pixels; i++)
    {
        c = colors[*in]; 
        r = ((uint16_t)(c.r >> 3)) << 11;
        g = ((uint16_t)(c.g >> 2)) << 5;
        b = ((uint16_t)(c.b >> 3)) << 0;
        *out = (r | g | b);

        in++;
        for (j = 0; j < fb_scaling; j++) {
            out++;
        }
    }
}

void cmap_to_fb(uint8_t * out, uint8_t * in, int in_pixels)
{
    int i, j, k;
    struct color c;
    uint32_t pix;
    uint16_t r, g, b;

    for (i = 0; i < in_pixels; i++)
    {
        c = colors[*in];  /* R:8 G:8 B:8 format! */
        r = (uint16_t)(c.r >> (8 - fb.red.length));
        g = (uint16_t)(c.g >> (8 - fb.green.length));
        b = (uint16_t)(c.b >> (8 - fb.blue.length));
        pix = r << fb.red.offset;
        pix |= g << fb.green.offset;
        pix |= b << fb.blue.offset;

        for (k = 0; k < fb_scaling; k++) {
            for (j = 0; j < fb.bits_per_pixel/8; j++) {
                *out = (pix >> (j*8));
                out++;
            }
        }
        in++;
    }
}
#else
void draw_screen_vector(unsigned char *in, unsigned char *out)
{
    int y;
    int x_offset, x_offset_end;

    y = SCREENHEIGHT;
    x_offset     = (((fb.xres - (SCREENWIDTH  * fb_scaling)) * fb.bits_per_pixel/8)) / 2;
    x_offset_end = ((fb.xres - (SCREENWIDTH  * fb_scaling)) * fb.bits_per_pixel/8) - x_offset;

    while (y--)
    {
        out += x_offset;

        int elements_to_process = SCREENWIDTH;

        while (elements_to_process)
        {
            size_t vl = __riscv_vsetvl_e8m8(elements_to_process);

            // Load pixel indices with m8 to match palette LMUL
            vuint8m8_t pixel_index_8 =  __riscv_vle8_v_u8m8(in, vl);

            // Use explicit inline assembly to force register reuse
            vuint8m8_t r_color, g_color, b_color;
            
            // Load red palette and gather in-place
            r_color = __riscv_vle8_v_u8m8(red, 256);
            asm volatile("vrgather.vv %0, %0, %1" : "+&vr"(r_color) : "vr"(pixel_index_8));
            
            // Load green palette and gather in-place
            g_color = __riscv_vle8_v_u8m8(green, 256);
            asm volatile("vrgather.vv %0, %0, %1" : "+&vr"(g_color) : "vr"(pixel_index_8));
            
            // Load blue palette and gather in-place
            b_color = __riscv_vle8_v_u8m8(blue, 256);
            asm volatile("vrgather.vv %0, %0, %1" : "+&vr"(b_color) : "vr"(pixel_index_8));
            
            // Use explicit inline assembly with direct register control
            size_t quarter_vl = vl / 4;
            
            // Use a single inline assembly block to control all operations precisely
            asm volatile(
                // Set vector length for m2 operations
                "vsetvli zero, %3, e8, m2, ta, ma\n\t"
                
                // Segment 0: Extract B0,G0,R0 and store with alpha
                "vmv2r.v v0, %0\n\t"      // B0 -> v0,v1 (segment 0 of b_color)
                "vmv2r.v v2, %1\n\t"      // G0 -> v2,v3 (segment 0 of g_color)  
                "vmv2r.v v4, %2\n\t"      // R0 -> v4,v5 (segment 0 of r_color)
                "vmv.v.i v6, -1\n\t"      // A0 -> v6,v7 (0xFF)
                "vsseg4e8.v v0, (%4)\n\t" // Store BGRA segment 0
                
                // Segment 1: Extract B1,G1,R1 and store with alpha
                "vmv2r.v v0, v26\n\t"     // B1 -> v0,v1 (b_color+2)
                "vmv2r.v v2, v18\n\t"     // G1 -> v2,v3 (g_color+2)
                "vmv2r.v v4, v10\n\t"     // R1 -> v4,v5 (r_color+2)
                "vmv.v.i v6, -1\n\t"      // A1 -> v6,v7 (0xFF)
                "vsseg4e8.v v0, (%5)\n\t" // Store BGRA segment 1
                
                // Segment 2: Extract B2,G2,R2 and store with alpha
                "vmv2r.v v0, v28\n\t"     // B2 -> v0,v1 (b_color+4)
                "vmv2r.v v2, v20\n\t"     // G2 -> v2,v3 (g_color+4)
                "vmv2r.v v4, v12\n\t"     // R2 -> v4,v5 (r_color+4)
                "vmv.v.i v6, -1\n\t"      // A2 -> v6,v7 (0xFF)
                "vsseg4e8.v v0, (%6)\n\t" // Store BGRA segment 2
                
                // Segment 3: Extract B3,G3,R3 and store with alpha
                "vmv2r.v v0, v30\n\t"     // B3 -> v0,v1 (b_color+6)
                "vmv2r.v v2, v22\n\t"     // G3 -> v2,v3 (g_color+6)
                "vmv2r.v v4, v14\n\t"     // R3 -> v4,v5 (r_color+6)
                "vmv.v.i v6, -1\n\t"      // A3 -> v6,v7 (0xFF)
                "vsseg4e8.v v0, (%7)\n\t" // Store BGRA segment 3
                
                :: "vr"(b_color),                                 // %0 (v24)
                   "vr"(g_color),                                 // %1 (v16) 
                   "vr"(r_color),                                 // %2 (v8)
                   "r"(quarter_vl),                               // %3
                   "r"(out),                                      // %4
                   "r"((uint8_t*)out + quarter_vl * 4),          // %5
                   "r"((uint8_t*)out + quarter_vl * 8),          // %6
                   "r"((uint8_t*)out + quarter_vl * 12)          // %7
                : "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"
            );

            in += vl;
            out += 4 * vl; // 4 bytes per pixel for RGBA
            elements_to_process -= vl;

        }
        out += x_offset_end;
    }
}
#endif

void I_InitGraphics (void)
{
    /* Open fbdev file descriptor */
    fd_fb = open("/dev/fb0", O_RDWR);
    if (fd_fb < 0)
    {
        printf("Could not open /dev/fb0");
        exit(-1);
    }

    /* fetch framebuffer info */
    ioctl(fd_fb, FBIOGET_VSCREENINFO, &fb);
    /* change params if needed */
    //ioctl(fd_fb, FBIOPUT_VSCREENINFO, &fb);
    printf("I_InitGraphics: framebuffer: x_res: %d, y_res: %d, x_virtual: %d, y_virtual: %d, bpp: %d, grayscale: %d\n",
            fb.xres, fb.yres, fb.xres_virtual, fb.yres_virtual, fb.bits_per_pixel, fb.grayscale);

    printf("I_InitGraphics: framebuffer: RGBA: %d%d%d%d, red_off: %d, green_off: %d, blue_off: %d, transp_off: %d\n",
            fb.red.length, fb.green.length, fb.blue.length, fb.transp.length, fb.red.offset, fb.green.offset, fb.blue.offset, fb.transp.offset);

    printf("I_InitGraphics: DOOM screen size: w x h: %d x %d\n", SCREENWIDTH, SCREENHEIGHT);

#ifndef USE_VECTOR
    int i = M_CheckParmWithArgs("-scaling", 1);
    if (i > 0) {
        i = atoi(myargv[i + 1]);
        fb_scaling = i;
        printf("I_InitGraphics: Scaling factor: %d\n", fb_scaling);
    } else {
        fb_scaling = fb.xres / SCREENWIDTH;
        if (fb.yres / SCREENHEIGHT < fb_scaling)
            fb_scaling = fb.yres / SCREENHEIGHT;
        printf("I_InitGraphics: Auto-scaling factor: %d\n", fb_scaling);
    }
#endif
    /* Allocate screen to draw to */
	I_VideoBuffer = (byte*)Z_Malloc (SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);  // For DOOM to draw on
	// I_VideoBuffer_FB = (byte*)malloc(fb.xres * fb.yres * (fb.bits_per_pixel/8));     // For a single write() syscall to fbdev

    // SMD: use mmap to avoid extra copies between internal buffer and framebuffer
    I_VideoBuffer_FB = mmap(NULL, fb.xres * fb.yres * (fb.bits_per_pixel/8), PROT_WRITE, MAP_SHARED, fd_fb, 0);

	screenvisible = true;

    extern int I_InitInput(void);
    I_InitInput();
}

void I_ShutdownGraphics (void)
{
	Z_Free (I_VideoBuffer);
	free(I_VideoBuffer_FB);
}

void I_StartFrame (void)
{

}

__attribute__ ((weak)) void I_GetEvent (void)
{
//	event_t event;
//	bool button_state;
//
//	button_state = button_read ();
//
//	if (last_button_state != button_state)
//	{
//		last_button_state = button_state;
//
//		event.type = last_button_state ? ev_keydown : ev_keyup;
//		event.data1 = KEY_FIRE;
//		event.data2 = -1;
//		event.data3 = -1;
//
//		D_PostEvent (&event);
//	}
//
//	touch_main ();
//
//	if ((touch_state.x != last_touch_state.x) || (touch_state.y != last_touch_state.y) || (touch_state.status != last_touch_state.status))
//	{
//		last_touch_state = touch_state;
//
//		event.type = (touch_state.status == TOUCH_PRESSED) ? ev_keydown : ev_keyup;
//		event.data1 = -1;
//		event.data2 = -1;
//		event.data3 = -1;
//
//		if ((touch_state.x > 49)
//		 && (touch_state.x < 72)
//		 && (touch_state.y > 104)
//		 && (touch_state.y < 143))
//		{
//			// select weapon
//			if (touch_state.x < 60)
//			{
//				// lower row (5-7)
//				if (touch_state.y < 119)
//				{
//					event.data1 = '5';
//				}
//				else if (touch_state.y < 131)
//				{
//					event.data1 = '6';
//				}
//				else
//				{
//					event.data1 = '1';
//				}
//			}
//			else
//			{
//				// upper row (2-4)
//				if (touch_state.y < 119)
//				{
//					event.data1 = '2';
//				}
//				else if (touch_state.y < 131)
//				{
//					event.data1 = '3';
//				}
//				else
//				{
//					event.data1 = '4';
//				}
//			}
//		}
//		else if (touch_state.x < 40)
//		{
//			// button bar at bottom screen
//			if (touch_state.y < 40)
//			{
//				// enter
//				event.data1 = KEY_ENTER;
//			}
//			else if (touch_state.y < 80)
//			{
//				// escape
//				event.data1 = KEY_ESCAPE;
//			}
//			else if (touch_state.y < 120)
//			{
//				// use
//				event.data1 = KEY_USE;
//			}
//			else if (touch_state.y < 160)
//			{
//				// map
//				event.data1 = KEY_TAB;
//			}
//			else if (touch_state.y < 200)
//			{
//				// pause
//				event.data1 = KEY_PAUSE;
//			}
//			else if (touch_state.y < 240)
//			{
//				// toggle run
//				if (touch_state.status == TOUCH_PRESSED)
//				{
//					run = !run;
//
//					event.data1 = KEY_RSHIFT;
//
//					if (run)
//					{
//						event.type = ev_keydown;
//					}
//					else
//					{
//						event.type = ev_keyup;
//					}
//				}
//				else
//				{
//					return;
//				}
//			}
//			else if (touch_state.y < 280)
//			{
//				// save
//				event.data1 = KEY_F2;
//			}
//			else if (touch_state.y < 320)
//			{
//				// load
//				event.data1 = KEY_F3;
//			}
//		}
//		else
//		{
//			// movement/menu navigation
//			if (touch_state.x < 100)
//			{
//				if (touch_state.y < 100)
//				{
//					event.data1 = KEY_STRAFE_L;
//				}
//				else if (touch_state.y < 220)
//				{
//					event.data1 = KEY_DOWNARROW;
//				}
//				else
//				{
//					event.data1 = KEY_STRAFE_R;
//				}
//			}
//			else if (touch_state.x < 180)
//			{
//				if (touch_state.y < 160)
//				{
//					event.data1 = KEY_LEFTARROW;
//				}
//				else
//				{
//					event.data1 = KEY_RIGHTARROW;
//				}
//			}
//			else
//			{
//				event.data1 = KEY_UPARROW;
//			}
//		}
//
//		D_PostEvent (&event);
//	}
}

__attribute__ ((weak)) void I_StartTic (void)
{
	I_GetEvent();
}

void I_UpdateNoBlit (void)
{
}

//
// I_FinishUpdate
//

void I_FinishUpdate (void)
{
#ifdef USE_VECTOR
    draw_screen_vector((unsigned char *) I_VideoBuffer, (unsigned char *) I_VideoBuffer_FB);
#else
    int y;
    int x_offset, x_offset_end;
    unsigned char *line_in, *line_out;

    /* Offsets in case FB is bigger than DOOM */
    /* 600 = fb heigt, 200 screenheight */
    /* 600 = fb heigt, 200 screenheight */
    /* 2048 =fb width, 320 screenwidth */
    x_offset     = (((fb.xres - (SCREENWIDTH  * fb_scaling)) * fb.bits_per_pixel/8)) / 2; // XXX: siglent FB hack: /4 instead of /2, since it seems to handle the resolution in a funny way
    //x_offset     = 0;
    x_offset_end = ((fb.xres - (SCREENWIDTH  * fb_scaling)) * fb.bits_per_pixel/8) - x_offset;

    /* DRAW SCREEN */
    line_in  = (unsigned char *) I_VideoBuffer;
    line_out = (unsigned char *) I_VideoBuffer_FB;

    y = SCREENHEIGHT;

    while (y--)
    {
        int i;
        for (i = 0; i < fb_scaling; i++) {
            line_out += x_offset;
#ifdef CMAP256
            for (fb_scaling == 1) {
                memcpy(line_out, line_in, SCREENWIDTH); /* fb_width is bigger than Doom SCREENWIDTH... */
            } else {
                //XXX FIXME fb_scaling support!
            }
#else
            //cmap_to_rgb565((void*)line_out, (void*)line_in, SCREENWIDTH);
            cmap_to_fb((void*)line_out, (void*)line_in, SCREENWIDTH);
#endif
            line_out += (SCREENWIDTH * fb_scaling * (fb.bits_per_pixel/8)) + x_offset_end;
        }
        line_in += SCREENWIDTH;
    }
#endif
}

//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
#define GFX_RGB565(r, g, b)			((((r & 0xF8) >> 3) << 11) | (((g & 0xFC) >> 2) << 5) | ((b & 0xF8) >> 3))
#define GFX_RGB565_R(color)			((0xF800 & color) >> 11)
#define GFX_RGB565_G(color)			((0x07E0 & color) >> 5)
#define GFX_RGB565_B(color)			(0x001F & color)

void I_SetPalette (byte* palette)
{
    //col_t* c;

    //for (i = 0; i < 256; i++)
    //{
    //  c = (col_t*)palette;

    //  rgb565_palette[i] = GFX_RGB565(gammatable[usegamma][c->r],
    //                                 gammatable[usegamma][c->g],
    //                                 gammatable[usegamma][c->b]);

    //  palette += 3;
    //}

    int i;
    /* performance boost:
     * map to the right pixel format over here! */

#ifdef USE_VECTOR
    for (i=0; i<256; ++i ) {
        red[i] = gammatable[usegamma][*palette++];
        green[i] = gammatable[usegamma][*palette++];
        blue[i] = gammatable[usegamma][*palette++];
    }
#else
    for (i=0; i<256; ++i ) {
        colors[i].a = 0;
        colors[i].r = gammatable[usegamma][*palette++];
        colors[i].g = gammatable[usegamma][*palette++];
        colors[i].b = gammatable[usegamma][*palette++];
    }
#endif

    /* Set new color map in kernel framebuffer driver */
    //XXX FIXME ioctl(fd_fb, IOCTL_FB_PUTCMAP, colors);
}

// Given an RGB value, find the closest matching palette index.

int I_GetPaletteIndex (int r, int g, int b)
{
#ifndef USE_VECTOR
    int i, best, best_diff, diff;
    col_t color;

    printf("I_GetPaletteIndex\n");

    best = 0;
    best_diff = INT_MAX;
    for (i = 0; i < 256; ++i)
    {
    	color.r = GFX_RGB565_R(rgb565_palette[i]);
    	color.g = GFX_RGB565_G(rgb565_palette[i]);
    	color.b = GFX_RGB565_B(rgb565_palette[i]);

        diff = (r - color.r) * (r - color.r)
             + (g - color.g) * (g - color.g)
             + (b - color.b) * (b - color.b);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }

        if (diff == 0)
        {
            break;
        }
    }
    return best;
#else
    return 0;
#endif
}

void I_BeginRead (void)
{
}

void I_EndRead (void)
{
}

void I_SetWindowTitle (char *title)
{
}

void I_GraphicsCheckCommandLine (void)
{
}

void I_SetGrabMouseCallback (grabmouse_callback_t func)
{
}

void I_EnableLoadingDisk(void)
{
}

void I_BindVideoVariables (void)
{
}

void I_DisplayFPSDots (boolean dots_on)
{
}

void I_CheckIsScreensaver (void)
{
}
