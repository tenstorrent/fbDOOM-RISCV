//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
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
// DESCRIPTION:
//	DOOM keyboard input via linux input events tunnelled over UART
//


#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/input-event-codes.h>

#include "config.h"
#include "deh_str.h"
#include "doomtype.h"
#include "doomkeys.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_swap.h"
#include "i_timer.h"
#include "i_video.h"
#include "i_scale.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

int vanilla_keyboard_mapping = 1;

static int initialized = 0;

// Is the shift key currently down?

static int shiftdown = 0;

static int serial_fd = -1;

#define SERIAL_DEVICE "/dev/ttyS1"

static int input_init(void)
{
    serial_fd = open(SERIAL_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_fd < 0) {
        perror("open");
        return -1;
    }

    struct termios tty;
    if (tcgetattr(serial_fd, &tty) < 0) {
        perror("tcgetattr");
        close(serial_fd);
        return -1;
    }

    cfmakeraw(&tty);

    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);

    tty.c_cflag &= ~PARENB;   // no parity
    tty.c_cflag &= ~CSTOPB;   // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;       // 8 bits
    tty.c_cflag |= CREAD | CLOCAL; // enable receiver, ignore modem control lines

    tty.c_iflag = IGNPAR;     // ignore parity errors
    tty.c_oflag = 0;
    tty.c_lflag = 0;

    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 0;

    if (tcflush(serial_fd, TCIOFLUSH) < 0) {
        perror("tcflush");
        close(serial_fd);
        return -1;
    }

    if (tcsetattr(serial_fd, TCSANOW, &tty) < 0) {
        perror("tcsetattr");
        close(serial_fd);
        return -1;
    }

    int flags = fcntl(serial_fd, F_GETFL, 0);
    fcntl(serial_fd, F_SETFL, flags | O_NONBLOCK);

    printf("Ready to read input. Press Backspace to exit.\n");

    initialized = 1;

    return 0;
}

static unsigned char TranslateBasePS2Key(unsigned char code)
{
    switch (code) {
        case 0x0D: return DOOM_KEY_TAB;          // Tab
        case 0x5A: return DOOM_KEY_ENTER;        // Enter
        case 0x76: return DOOM_KEY_ESCAPE;       // Esc
        case 0x29: return DOOM_KEY_USE;          // Space
        case 0x52: return '\'';                  // Apostrophe
        case 0x41: return ',';                   // Comma
        case 0x4E: return DOOM_KEY_MINUS;        // '-'
        case 0x49: return '.';                   // Period
        case 0x4A: return '/';                   // Slash

        case 0x45: return '0';
        case 0x16: return '1';
        case 0x1E: return '2';
        case 0x26: return '3';
        case 0x25: return '4';
        case 0x2E: return '5';
        case 0x36: return '6';
        case 0x3D: return '7';
        case 0x3E: return '8';
        case 0x46: return '9';

        case 0x4C: return ';';
        case 0x55: return DOOM_KEY_EQUALS;       // '='
        case 0x54: return '[';
        case 0x5B: return ']';

        case 0x1C: return 'a';
        case 0x32: return 'b';
        case 0x21: return 'c';
        case 0x23: return 'd';
        case 0x24: return 'e';
        case 0x2B: return 'f';
        case 0x34: return 'g';
        case 0x33: return 'h';
        case 0x43: return 'i';
        case 0x3B: return 'j';
        case 0x42: return 'k';
        case 0x4B: return 'l';
        case 0x3A: return 'm';
        case 0x31: return 'n';
        case 0x44: return 'o';
        case 0x4D: return 'p';
        case 0x15: return 'q';
        case 0x2D: return 'r';
        case 0x1B: return 's';
        case 0x2C: return 't';
        case 0x3C: return 'u';
        case 0x2A: return 'v';
        case 0x1D: return 'w';
        case 0x22: return 'x';
        case 0x35: return 'y';
        case 0x1A: return 'z';

        case 0x66: return DOOM_KEY_BACKSPACE;
        case 0x14: return DOOM_KEY_FIRE;         // Left Ctrl

        case 0x12: return DOOM_KEY_RSHIFT;       // Left Shift
        case 0x59: return DOOM_KEY_RSHIFT;       // Right Shift
        case 0x11: return DOOM_KEY_LALT;         // Left Alt

        case 0x05: return DOOM_KEY_F1;
        case 0x06: return DOOM_KEY_F2;
        case 0x04: return DOOM_KEY_F3;
        case 0x0C: return DOOM_KEY_F4;
        case 0x03: return DOOM_KEY_F5;
        case 0x0B: return DOOM_KEY_F6;
        case 0x83: return DOOM_KEY_F7;
        case 0x0A: return DOOM_KEY_F8;
        case 0x01: return DOOM_KEY_F9;
        case 0x09: return DOOM_KEY_F10;
        case 0x78: return DOOM_KEY_F11;
        case 0x07: return DOOM_KEY_F12;

        default: return 0;
    }
}

static unsigned char TranslateExtendedPS2Key(unsigned char code)
{
    switch (code) {
        case 0x11: return DOOM_KEY_RALT;
        case 0x14: return DOOM_KEY_RCTRL;
        case 0x6C: return DOOM_KEY_HOME;
        case 0x70: return DOOM_KEY_INS;
        case 0x7D: return DOOM_KEY_PGUP;
        case 0x7A: return DOOM_KEY_PGDN;
        case 0x69: return DOOM_KEY_END;

        case 0x6B: return DOOM_KEY_LEFTARROW;    // Left
        case 0x75: return DOOM_KEY_UPARROW;      // Up
        case 0x74: return DOOM_KEY_RIGHTARROW;   // Right
        case 0x72: return DOOM_KEY_DOWNARROW;    // Down

        default: return 0;
    }
}

#define PS2_KEY_EXTENDED 0xe0
#define PS2_KEY_UP 0xf0

// Print screen and pause not handled

static int input_read(int *pressed, unsigned char *key)
{
    int extended = 0;
    *pressed = 1;
    unsigned char data;
    ssize_t len;

    if (!initialized)
        return 0;

again:
    len = read(serial_fd, &data, 1);

    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;

    if (len == 0)
        return 0;

    if (len != 1) {
        perror("read");
        printf("bad length %ld\n", len);
        return 0;
    }

    if (data == PS2_KEY_EXTENDED) {
        extended = 1;
        goto again;
    }

    if (data == PS2_KEY_UP) {
        *pressed = 0;
        goto again;
    }

    if (extended) {
        data = TranslateExtendedPS2Key(data);
    } else {
        data = TranslateBasePS2Key(data);
    }

    *key = data;

    printf("%s: 0x%2X (%i)\n", *pressed ? " Pressed" : "Released", (unsigned int)*key, (unsigned int)*key);

    return 1;
}

int get_shifted_key(int ascii_char) {
    // Handle letters
    if (ascii_char >= 'a' && ascii_char <= 'z') {
        return ascii_char - 32; // Convert to uppercase
    }

    // Handle numbers and symbols
    switch (ascii_char) {
        case '`': return '~';
        case '1': return '!';
        case '2': return '@';
        case '3': return '#';
        case '4': return '$';
        case '5': return '%';
        case '6': return '^';
        case '7': return '&';
        case '8': return '*';
        case '9': return '(';
        case '0': return ')';
        case '-': return '_';
        case '=': return '+';
        case '[': return '{';
        case ']': return '}';
        case '\\': return '|';
        case ';': return ':';
        case '\'': return '"';
        case ',': return '<';
        case '.': return '>';
        case '/': return '?';
        default:
            return ascii_char; // No shift equivalent
    }
}

// Get the equivalent ASCII (Unicode?) character for a keypress.

static unsigned char GetTypedChar(unsigned char key)
{
    // Is shift held down?  If so, perform a translation.

    if (shiftdown > 0) {
        key = get_shifted_key(key);
    }

    return key;
}

static void UpdateShiftStatus(int pressed, unsigned char key)
{
    int change;

    if (pressed) {
        change = 1;
    } else {
        change = -1;
    }

    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
        shiftdown += change;
    }
}

void I_GetEvent(void)
{
    event_t event;
    int pressed;
    unsigned char key;

    while (input_read(&pressed, &key)) {
        UpdateShiftStatus(pressed, key);

        if (pressed) {
            // data1 has the key pressed, data2 has the character
            // (shift-translated, etc)
            event.type = ev_keydown;
            event.data1 = key;
            event.data2 = GetTypedChar(key);

            if (event.data1 != 0) {
                D_PostEvent(&event);
            }
        } else {
            event.type = ev_keyup;
            event.data1 = key;

            // data2 is just initialized to zero for ev_keyup.
            // For ev_keydown it's the shifted Unicode character
            // that was typed, but if something wants to detect
            // key releases it should do so based on data1
            // (key ID), not the printable char.

            event.data2 = 0;

            if (event.data1 != 0) {
                D_PostEvent(&event);
            }

            break;
        }
    }
}

void I_InitInput(void)
{
    input_init();
}
