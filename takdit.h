#ifndef _TAKDIT_H
#define _TAKDIT_H

#define TAKDIT_VERSION "0.0.3"

#define _BSD_SOURCE
#define _GNU_SOURCE

#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <time.h>
#include <pwd.h>
#include <assert.h>

#include "modules/syntax/syntax.h"

#define EDIT_MODE 0
#define SELECTION_MODE 1
#define NORMAL_MODE 2

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_oc;
} erow;

typedef struct colourMap {
    int hl_comment_colour;
    int hl_mlcomment_colour;
    int hl_keyword_cond_colour;
    int hl_keyword_type_colour;
    int hl_keyword_pp_colour;
    int hl_keyword_return_colour;
    int hl_keyword_adapter_colour;
    int hl_keyword_loop_colour;
    int hl_string_colour;
    int hl_number_colour;
    int hl_match_colour;
    int hl_background_colour;
    int hl_default_colour;
} colourMap;

// configuration structure for the editor
struct editorConfig {
    int cx, cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    int rawmode;
    erow *row;
    int dirty;
    char *filename;
    colourMap colours;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct termios orig_termios;
};

#define CTRL_KEY(k) ((k) & 0x1f)

enum KEY_ACTION {
    KEY_NULL = 0,       /* NULL */
    CTRL_C = 3,         /* Ctrl-c */
    CTRL_D = 4,         /* Ctrl-d */
    CTRL_F = 6,         /* Ctrl-f */
    CTRL_H = 8,         /* Ctrl-h */
    TAB = 9,            /* Tab */
    CTRL_L = 12,        /* Ctrl+l */
    ENTER = 13,         /* Enter */
    CTRL_Q = 17,        /* Ctrl-q */
    CTRL_S = 19,        /* Ctrl-s */
    CTRL_U = 21,        /* Ctrl-u */
    ESC = 27,           /* Escape */
    BACKSPACE = 127,    /* Backspace */

    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

void editorSetStatusMessage(const char *fmt, ...);

#endif
