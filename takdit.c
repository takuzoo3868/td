/*** includes ***/

#include "takdit.h"

static struct editorConfig E;
static struct termios orig_termios;

/*** Low level terminal handling ***/

void disableRawMode(int fd) {
    if (E.rawmode) {
        tcsetattr(fd, TCSAFLUSH, &orig_termios);
        E.rawmode = 0;
    }
}

void editorAtExit(void) {
    disableRawMode(STDIN_FILENO);
}

// Raw mode: 1960 magic shit
int enableRawMode(int fd) {
    struct termios raw;

    if (E.rawmode) return 0;
    if (!isatty(STDIN_FILENO)) goto fatal;
    atexit(editorAtExit);
    if (tcgetattr(fd, &orig_termios) == -1) goto fatal;

    raw = orig_termios;  // modify the original mode

    // input modes: no break, no CR to NL, no parity check, no strip char, no start/stop output control
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // output modes - disable post processing
    raw.c_oflag &= ~(OPOST);

    // control modes - set 8 bit chars
    raw.c_cflag |= (CS8);

    // local modes - choing off, canonical off, no extended functions, no signal chars (^Z,^C)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // control chars - set return condition: min number of bytes and timer
    raw.c_cc[VMIN] = 0;  // return each byte, or zero for timeout
    raw.c_cc[VTIME] = 1; // 100 ms timeout

    // put terminal in raw mode after flushing
    if (tcsetattr(fd, TCSAFLUSH, &raw) < 0) goto fatal;
    E.rawmode = 1;
    return 0;

    fatal:
    errno = ENOTTY;
    return -1;
}

// read a key from terminal input in raw mode and handle
int editorReadKey(int fd) {
    int nread;
    char c, seq[3];
    while ((nread = read(fd, &c, 1)) == 0);
    if (nread == -1) exit(1);

    while (1) {
        switch (c) {
            case ESC: // escape sequence
                // if its an escape then we timeout here
                if (read(fd, seq, 1) == 0) return ESC;
                if (read(fd, seq + 1, 1) == 0) return ESC;

                // ESC [ sequences
                if (seq[0] == '[') {
                    if (seq[1] >= '0' && seq[1] <= '9') {
                        // extended escape so read additional byte
                        if (read(fd, seq + 2, 1) == 0) return ESC;
                        if (seq[2] == '~') {
                            switch (seq[1]) {
                                case '3':
                                    return DEL_KEY;
                                case '5':
                                    return PAGE_UP;
                                case '6':
                                    return PAGE_DOWN;
                            }
                        }
                    } else {
                        switch (seq[1]) {
                            case 'A':
                                return ARROW_UP;
                            case 'B':
                                return ARROW_DOWN;
                            case 'C':
                                return ARROW_RIGHT;
                            case 'D':
                                return ARROW_LEFT;
                            case 'H':
                                return HOME_KEY;
                            case 'F':
                                return END_KEY;
                        }
                    }
                }

                    // ESC 0 sequences
                else if (seq[0] == 'O') {
                    switch (seq[1]) {
                        case 'H':
                            return HOME_KEY;
                        case 'F':
                            return END_KEY;
                    }
                }
                break;

            default:
                return c;
        }
    }
}

int getCursorPosition(int ifd, int ofd, int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(ofd, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(ifd, buf + i, 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != ESC || buf[1] != '[') return -1;
    if (sscanf(buf + 2, "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int ifd, int ofd, int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        int orig_row, orig_col, retval;

        retval = getCursorPosition(ifd, ofd, &orig_row, &orig_col);
        if (retval == -1) goto failed;

        if (write(ofd, "\x1b[999C\x1b[999B", 12) != 12) goto failed;
        retval = getCursorPosition(ifd, ofd, rows, cols);
        if (retval == -1) goto failed;

        char seq[32];
        snprintf(seq, 32, "\x1b[%d;%dH", orig_row, orig_col);
        if (write(ofd, seq, strlen(seq)) == -1) {
            /* hogehoge */
        }
        return 0;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }

    failed:
    return -1;
}

/*** Syntax highlight color scheme ***/

int is_separator(int c) {
    return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];", c) != NULL;
}

int editorRowHasOpenComment(erow *row) {
    if (row->hl && row->rsize && row->hl[row->rsize - 1] == HL_MLCOMMENT &&
        (row->rsize < 2 || (row->render[row->rsize - 2] != '*' ||
                            row->render[row->rsize - 1] != '/')))
        return 1;
    return 0;
}

void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    //no syntax in this line, everything is HL_NORMAL
    if (E.syntax == NULL) return;

    char *p;
    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    // point to the first non space char
    p = row->render;
    // current char offset in row
    int i = 0;
    while (*p && isspace(*p)) {
        p++;
        i++;
    }

    // tell the parser if 'i' points to the start of a word
    int prev_sep = 1;
    // is the character  in a string "" or ''
    int in_string = 0;
    // is the char in an open multiline comment
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_oc);

    //if the previous line has open open comment then this line start with an open comment state
    if (row->idx > 0 && editorRowHasOpenComment(&E.row[row->idx - 1])) in_comment = 1;

    while (*p) {
        //handle single line comments
        if (prev_sep && *p == scs[0] && *(p + 1) == scs[1]) {
            // from here to the end of the row is a comment
            memset(row->hl + i, HL_COMMENT, row->rsize - i);
            return;
        }

        // handle multiline comments
        if (in_comment) {
            row->hl[i] = HL_MLCOMMENT;
            if (*p == mce[0] && *(p + 1) == mce[1]) {
                row->hl[i + 1] = HL_MLCOMMENT;
                p += 2;
                i += 2;
                in_comment = 0;
                prev_sep = 1;
                continue;
            } else {
                prev_sep = 0;
                p++;
                i++;
                continue;
            }
        } else if (*p == mcs[0] && *(p + 1) == mcs[1]) {
            row->hl[i] = HL_MLCOMMENT;
            row->hl[i + 1] = HL_MLCOMMENT;
            p += 2;
            i += 2;
            in_comment = 1;
            prev_sep = 0;
            continue;
        }


        // handle "" and '' */
        if (in_string) {
            row->hl[i] = HL_STRING;
            if (*p == '\\') {
                row->hl[i + 1] = HL_STRING;
                p += 2;
                i += 2;
                prev_sep = 0;
                continue;
            }
            if (*p == in_string) in_string = 0;
            p++;
            i++;
            continue;
        } else {
            if (*p == '"' || *p == '\'') {
                in_string = *p;
                row->hl[i] = HL_STRING;
                p++;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        // handle non printable chars
        if (!isprint(*p)) {
            row->hl[i] = HL_NONPRINT;
            p++;
            i++;
            prev_sep = 0;
            continue;
        }

        //handle numbers
        if ((isdigit(*p) && (prev_sep || row->hl[i - 1] == HL_NUMBER)) ||
            (*p == '.' && i > 0 && row->hl[i - 1] == HL_NUMBER)) {
            row->hl[i] = HL_NUMBER;
            p++;
            i++;
            prev_sep = 0;
            continue;
        }

        //handle keywords and lib calls
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int pp = keywords[j][klen - 1] == '|';   // preprocessor keyword
                int cond = keywords[j][klen - 1] == '~'; // condition
                int retu = keywords[j][klen - 1] == '#';
                int adapter = keywords[j][klen - 1] == '^'; //adapter keywords
                int loopy = keywords[j][klen - 1] == '@';
                if (pp || cond || retu || adapter || loopy) klen--;

                if (!memcmp(p, keywords[j], klen) && is_separator(*(p + klen))) {
                    // keyword
                    if (pp) memset(row->hl + i, HL_KEYWORD_PP, klen);
                    else if (cond) memset(row->hl + i, HL_KEYWORD_COND, klen);
                    else if (retu) memset(row->hl + i, HL_KEYWORD_RETURN, klen);
                    else if (adapter) memset(row->hl + i, HL_KEYWORD_ADAPTER, klen);
                    else if (loopy) memset(row->hl + i, HL_KEYWORD_LOOP, klen);
                    else memset(row->hl + i, HL_KEYWORD_TYPE, klen);
                    p += klen;
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue; // we had a keyword match
            }
        }

        // not special chars
        prev_sep = is_separator(*p);
        p++;
        i++;
    }

    int oc = editorRowHasOpenComment(row);
    if (row->hl_oc != oc && row->idx + 1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx + 1]);
    row->hl_oc = oc;
}

int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_COMMENT:
            return E.colours.hl_comment_colour;
        case HL_MLCOMMENT:
            return E.colours.hl_mlcomment_colour;
        case HL_KEYWORD_COND:
            return E.colours.hl_keyword_cond_colour;
        case HL_KEYWORD_TYPE:
            return E.colours.hl_keyword_type_colour;
        case HL_KEYWORD_PP:
            return E.colours.hl_keyword_pp_colour;
        case HL_KEYWORD_RETURN:
            return E.colours.hl_keyword_return_colour;
        case HL_KEYWORD_ADAPTER:
            return E.colours.hl_keyword_adapter_colour;
        case HL_KEYWORD_LOOP:
            return E.colours.hl_keyword_loop_colour;
        case HL_STRING:
            return E.colours.hl_string_colour;
        case HL_NUMBER:
            return E.colours.hl_number_colour;
        case HL_MATCH:
            return E.colours.hl_match_colour;
        case HL_BACKGROUND_DEFAULT:
            return E.colours.hl_background_colour;
        default:
            return E.colours.hl_default_colour;
    }
}

void editorSelectSyntaxHighlight(char *filename) {
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = HLDB + j;
        unsigned int i = 0;
        while (s->filematch[i]) {
            char *p;
            int patlen = strlen(s->filematch[i]);
            if ((p = strstr(filename, s->filematch[i])) != NULL) {
                if (s->filematch[i][0] != '.' || p[patlen] == '\0') {
                    E.syntax = s;
                    return;
                }
            }
            i++;
        }
    }
}

/*** Editor rows implementation ***/

// Update the rendered version and the syntax highlight of a row
void editorUpdateRow(erow *row) {
    int tabs = 0;
    int nonprint = 0;
    int j;

    free(row->render);
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == TAB) tabs++;

    row->render = malloc(row->size + tabs * 8 + nonprint * 9 + 1);
    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == TAB) {
            row->render[idx++] = ' ';
            while ((idx + 1) % 8 != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->rsize = idx;
    row->render[idx] = '\0';

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    if (at != E.numrows) {
        memmove(E.row + at + 1, E.row + at, sizeof(E.row[0]) * (E.numrows - at));
        for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;
    }
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len + 1);
    E.row[at].hl = NULL;
    E.row[at].hl_oc = 0;
    E.row[at].render = NULL;
    E.row[at].rsize = 0;
    E.row[at].idx = at;
    editorUpdateRow(E.row + at);
    E.numrows++;
    E.dirty++;
}

// Free row's heap allocated stuff
void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at) {
    erow *row;

    if (at >= E.numrows) return;
    row = E.row + at;
    editorFreeRow(row);
    memmove(E.row + at, E.row + at + 1, sizeof(E.row[0]) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx++;
    E.numrows--;
    E.dirty++;
}

char *editorRowsToString(int *buflen) {
    char *buf = NULL, *p;
    int totlen = 0;
    int j;

    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;

    p = buf = malloc(totlen);
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    *p = '\0';
    return buf;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at > row->size) {
        int padlen = at - row->size;
        row->chars = realloc(row->chars, row->size + padlen + 2);
        memset(row->chars + row->size, ' ', padlen);
        row->chars[row->size + padlen + 1] = '\0';
        row->size += padlen + 1;
    } else {
        row->chars = realloc(row->chars, row->size + 2);
        memmove(row->chars + at + 1, row->chars + at, row->size - at + 1);
        row->size++;
    }
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(row->chars + row->size, s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (row->size <= at) return;
    memmove(row->chars + at, row->chars + at + 1, row->size - at);
    editorUpdateRow(row);
    row->size--;
    E.dirty++;
}

void editorInsertChar(int c) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row) {
        while (E.numrows <= filerow)
            editorInsertRow(E.numrows, "", 0);
    }
    row = &E.row[filerow];
    editorRowInsertChar(row, filecol, c);
    if (E.cx == E.screencols - 1)
        E.coloff++;
    else
        E.cx++;
    E.dirty++;
}

void editorInsertNewline(void) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row) {
        if (filerow == E.numrows) {
            editorInsertRow(filerow, "", 0);
            goto fixcursor;
        }
        return;
    }

    if (filecol >= row->size) filecol = row->size;
    if (filecol == 0) {
        editorInsertRow(filerow, "", 0);
    } else {
        editorInsertRow(filerow + 1, row->chars + filecol, row->size - filecol);
        row = &E.row[filerow];
        row->chars[filecol] = '\0';
        row->size = filecol;
        editorUpdateRow(row);
    }

    fixcursor:
    if (E.cy == E.screenrows - 1) {
        E.rowoff++;
    } else {
        E.cy++;
    }
    E.cx = 0;
    E.coloff = 0;
}

// Delete the char at the current prompt position
void editorDelChar() {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row || (filecol == 0 && filerow == 0)) return;
    if (filecol == 0) {
        filecol = E.row[filerow - 1].size;
        editorRowAppendString(&E.row[filerow - 1], row->chars, row->size);
        editorDelRow(filerow);
        row = NULL;
        if (E.cy == 0)
            E.rowoff--;
        else
            E.cy--;
        E.cx = filecol;
        if (E.cx >= E.screencols) {
            int shift = (E.screencols - E.cx) + 1;
            E.cx -= shift;
            E.coloff += shift;
        }
    } else {
        editorRowDelChar(row, filecol - 1);
        if (E.cx == 0 && E.coloff)
            E.coloff--;
        else
            E.cx--;
    }
    if (row) editorUpdateRow(row);
    E.dirty++;
}

// load the specified program in the editor memory
int editorOpen(char *filename) {
    FILE *fp;

    E.dirty = 0;
    free(E.filename);
    E.filename = strdup(filename);

    fp = fopen(filename, "r");
    if (!fp) {
        if (errno != ENOENT) {
            perror("Opening file");
            exit(1);
        }
        return 1;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        if (linelen && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            line[--linelen] = '\0';
        editorInsertRow(E.numrows, line, linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
    return 0;
}

// save the current file on disk
int editorSave(void) {
    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) goto writeerr;

    if (ftruncate(fd, len) == -1) goto writeerr;
    if (write(fd, buf, len) != len) goto writeerr;

    close(fd);
    free(buf);
    E.dirty = 0;
    editorSetStatusMessage("%d bytes written on disk", len);
    return 0;

    writeerr:
    free(buf);
    if (fd != -1) close(fd);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
    return 1;
}

/*** Terminal update ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(new + ab->len, s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

void editorRefreshScreen(void) {
    int y;
    erow *r;
    char buf[32];
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    for (y = 0; y < E.screenrows; y++) {
        int filerow = E.rowoff + y;

        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "TAKDIT editor -- version %s\x1b[0K\r\n", TAKDIT_VERSION);
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(&ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(&ab, " ", 1);
                abAppend(&ab, welcome, welcomelen);
            } else {
                abAppend(&ab, "~\x1b[0K\r\n", 7);
            }
            continue;
        }

        r = &E.row[filerow];

        int len = r->rsize - E.coloff;
        int current_color = -1;
        if (len > 0) {
            if (len > E.screencols) len = E.screencols;
            char *c = r->render + E.coloff;
            unsigned char *hl = r->hl + E.coloff;
            int j;
            for (j = 0; j < len; j++) {
                if (hl[j] == HL_NONPRINT) {
                    char sym;
                    abAppend(&ab, "\x1b[7m", 4);
                    if (c[j] <= 26)
                        sym = '@' + c[j];
                    else
                        sym = '?';
                    abAppend(&ab, &sym, 1);
                    abAppend(&ab, "\x1b[0m", 4);
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(&ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(&ab, c + j, 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        current_color = color;
                        abAppend(&ab, buf, clen);
                    }
                    abAppend(&ab, c + j, 1);
                }
            }
        }
        abAppend(&ab, "\x1b[39m", 5);
        abAppend(&ab, "\x1b[0K", 4);
        abAppend(&ab, "\r\n", 2);
    }

    abAppend(&ab, "\x1b[0K", 4);
    abAppend(&ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename, E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus),
                        "%d/%d", E.rowoff + E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(&ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(&ab, rstatus, rlen);
            break;
        } else {
            abAppend(&ab, " ", 1);
            len++;
        }
    }
    abAppend(&ab, "\x1b[0m\r\n", 6);

    abAppend(&ab, "\x1b[0K", 4);
    int msglen = strlen(E.statusmsg);
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(&ab, E.statusmsg, msglen <= E.screencols ? msglen : E.screencols);

    int j;
    int cx = 1;
    int filerow = E.rowoff + E.cy;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    if (row) {
        for (j = E.coloff; j < (E.cx + E.coloff); j++) {
            if (j < row->size && row->chars[j] == TAB) cx += 7 - ((cx) % 8);
            cx++;
        }
    }
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, cx);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** Find mode ***/

#define TAKDIT_QUERY_LEN 256

void editorFind(int fd) {
    char query[TAKDIT_QUERY_LEN + 1] = {0};
    int qlen = 0;
    int last_match = -1;
    int find_next = 0;
    int saved_hl_line = -1;
    char *saved_hl = NULL;

#define FIND_RESTORE_HL do { \
    if (saved_hl) { \
        memcpy(E.row[saved_hl_line].hl,saved_hl, E.row[saved_hl_line].rsize); \
        saved_hl = NULL; \
    } \
} while (0)

    // Save the cursor position in order to restore it later
    int saved_cx = E.cx, saved_cy = E.cy;
    int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

    while (1) {
        editorSetStatusMessage(
                "Search: %s (Use ESC/Arrows/Enter)", query);
        editorRefreshScreen();

        int c = editorReadKey(fd);
        if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
            if (qlen != 0) query[--qlen] = '\0';
            last_match = -1;
        } else if (c == ESC || c == ENTER) {
            if (c == ESC) {
                E.cx = saved_cx;
                E.cy = saved_cy;
                E.coloff = saved_coloff;
                E.rowoff = saved_rowoff;
            }
            FIND_RESTORE_HL;
            editorSetStatusMessage("");
            return;
        } else if (c == ARROW_RIGHT || c == ARROW_DOWN) {
            find_next = 1;
        } else if (c == ARROW_LEFT || c == ARROW_UP) {
            find_next = -1;
        } else if (isprint(c)) {
            if (qlen < TAKDIT_QUERY_LEN) {
                query[qlen++] = c;
                query[qlen] = '\0';
                last_match = -1;
            }
        }

        // Search occurrence
        if (last_match == -1) find_next = 1;
        if (find_next) {
            char *match = NULL;
            int match_offset = 0;
            int i, current = last_match;

            for (i = 0; i < E.numrows; i++) {
                current += find_next;
                if (current == -1) current = E.numrows - 1;
                else if (current == E.numrows) current = 0;
                match = strstr(E.row[current].render, query);
                if (match) {
                    match_offset = match - E.row[current].render;
                    break;
                }
            }
            find_next = 0;

            /* Highlight */
            FIND_RESTORE_HL;

            if (match) {
                erow *row = &E.row[current];
                last_match = current;
                if (row->hl) {
                    saved_hl_line = current;
                    saved_hl = malloc(row->rsize);
                    memcpy(saved_hl, row->hl, row->rsize);
                    memset(row->hl + match_offset, HL_MATCH, qlen);
                }
                E.cy = 0;
                E.cx = match_offset;
                E.rowoff = current;
                E.coloff = 0;
                if (E.cx > E.screencols) {
                    int diff = E.cx - E.screencols;
                    E.cx -= diff;
                    E.coloff += diff;
                }
            }
        }
    }
}

/*** Editor events handling ***/

void editorMoveCursor(int key) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    int rowlen;

    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch (key) {
        case ARROW_LEFT:
            if (E.cx == 0) {
                if (E.coloff) {
                    E.coloff--;
                } else {
                    if (filerow > 0) {
                        E.cy--;
                        E.cx = E.row[filerow - 1].size;
                        if (E.cx > E.screencols - 1) {
                            E.coloff = E.cx - E.screencols + 1;
                            E.cx = E.screencols - 1;
                        }
                    }
                }
            } else {
                E.cx -= 1;
            }
            break;
        case ARROW_RIGHT:
            if (row && filecol < row->size) {
                if (E.cx == E.screencols - 1) {
                    E.coloff++;
                } else {
                    E.cx += 1;
                }
            } else if (row && filecol == row->size) {
                E.cx = 0;
                E.coloff = 0;
                if (E.cy == E.screenrows - 1) {
                    E.rowoff++;
                } else {
                    E.cy += 1;
                }
            }
            break;
        case ARROW_UP:
            if (E.cy == 0) {
                if (E.rowoff) E.rowoff--;
            } else {
                E.cy -= 1;
            }
            break;
        case ARROW_DOWN:
            if (filerow < E.numrows) {
                if (E.cy == E.screenrows - 1) {
                    E.rowoff++;
                } else {
                    E.cy += 1;
                }
            }
            break;
    }

    filerow = E.rowoff + E.cy;
    filecol = E.coloff + E.cx;
    row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    rowlen = row ? row->size : 0;
    if (filecol > rowlen) {
        E.cx -= filecol - rowlen;
        if (E.cx < 0) {
            E.coloff += E.cx;
            E.cx = 0;
        }
    }
}

#define TAKDIT_QUIT_TIMES 3

void editorProcessKeypress(int fd) {
    static int quit_times = TAKDIT_QUIT_TIMES;

    int c = editorReadKey(fd);

    switch (c) {
        case ENTER:
            editorInsertNewline();
            break;

        case CTRL_Q:
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                               "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_S:
            editorSave();
            break;

        case CTRL_F:
            editorFind(fd);
            break;
        case BACKSPACE:
        case CTRL_H:
        case DEL_KEY:
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            if (c == PAGE_UP && E.cy != 0)
                E.cy = 0;
            else if (c == PAGE_DOWN && E.cy != E.screenrows - 1)E.cy = E.screenrows - 1;
            {
                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_L:
            break;

        case ESC:
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = TAKDIT_QUIT_TIMES;
}

int editorFileWasModified(void) {
    return E.dirty;
}

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

    E.colours.hl_comment_colour = 33;
    E.colours.hl_mlcomment_colour = 33;
    E.colours.hl_keyword_cond_colour = 36;
    E.colours.hl_keyword_type_colour = 32;
    E.colours.hl_keyword_pp_colour = 34;
    E.colours.hl_keyword_return_colour = 35;
    E.colours.hl_keyword_adapter_colour = 94;
    E.colours.hl_keyword_loop_colour = 36;
    E.colours.hl_string_colour = 31;
    E.colours.hl_number_colour = 34;
    E.colours.hl_match_colour = 101;
    E.colours.hl_background_colour = 49;
    E.colours.hl_default_colour = 37;

    if (getWindowSize(STDIN_FILENO, STDOUT_FILENO, &E.screenrows, &E.screencols) == -1) {
        perror("Unable to query the screen for size (columns / rows)");
        exit(1);
    }
    E.screenrows -= 2;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: takdit <filename>\n");
        exit(1);
    }

    initEditor();
    editorSelectSyntaxHighlight(argv[1]);
    editorOpen(argv[1]);
    enableRawMode(STDIN_FILENO);
    editorSetStatusMessage(
            "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress(STDIN_FILENO);
    }
    return 0;
}
