/* edit.c — console text editor for TigxOS 1.1.12
 *
 * Keys:
 *   arrows / Home / End   move cursor
 *   Enter                 split line / new line
 *   Backspace / Delete    delete
 *   Ctrl-S                save
 *   Ctrl-F                find (type text, Enter)
 *   Ctrl-Q / Ctrl-X       quit (confirm if dirty)
 */
#include "libc.h"

#define KEY_LEFT   ((char)0x80)
#define KEY_RIGHT  ((char)0x81)
#define KEY_UP     ((char)0x82)
#define KEY_DOWN   ((char)0x83)
#define KEY_HOME   ((char)0x84)
#define KEY_END    ((char)0x85)
#define KEY_DELETE ((char)0x86)

#define MAX_LINES  180
#define MAX_COLS   78
#define VIEW_ROWS  22

static char lines[MAX_LINES][MAX_COLS + 1];
static int  nlines;
static int  crow, ccol;     /* cursor row/col in buffer */
static int  toprow;         /* first visible buffer row */
static int  dirty;
static char path[64];
static char status[80];
static char findbuf[40];
static int  find_len;

static void set_status(const char *s) {
    int i = 0;
    if (!s) s = "";
    while (s[i] && i < 79) {
        status[i] = s[i];
        i++;
    }
    status[i] = 0;
}

static void clear_screen(void) {
    for (int i = 0; i < 30; i++)
        write(STDOUT_FILENO, "\n", 1);
}

static int line_len(int r) {
    int n = 0;
    if (r < 0 || r >= nlines) return 0;
    while (lines[r][n] && n < MAX_COLS) n++;
    return n;
}

static void clamp_cursor(void) {
    if (crow < 0) crow = 0;
    if (crow >= nlines) crow = nlines > 0 ? nlines - 1 : 0;
    int ll = line_len(crow);
    if (ccol < 0) ccol = 0;
    if (ccol > ll) ccol = ll;
    if (crow < toprow) toprow = crow;
    if (crow >= toprow + VIEW_ROWS) toprow = crow - VIEW_ROWS + 1;
    if (toprow < 0) toprow = 0;
}

static void draw(void) {
    clear_screen();
    for (int i = 0; i < VIEW_ROWS; i++) {
        int r = toprow + i;
        char prefix[12];
        int pn = 0;
        int num = r + 1;
        if (r < nlines) {
            char tmp[8];
            int t = 0;
            int v = num > 0 ? num : 1;
            if (v == 0) tmp[t++] = '0';
            while (v > 0 && t < 6) {
                tmp[t++] = (char)('0' + (v % 10));
                v /= 10;
            }
            /* right-align into 4 columns */
            int pad = 4 - t;
            if (pad < 0) pad = 0;
            while (pad--) prefix[pn++] = ' ';
            while (t > 0) prefix[pn++] = tmp[--t];
            prefix[pn++] = ' ';
            prefix[pn] = 0;
            write(STDOUT_FILENO, prefix, (size_t)pn);
            write(STDOUT_FILENO, lines[r], (size_t)line_len(r));
        } else {
            write(STDOUT_FILENO, "    ~", 5);
        }
        write(STDOUT_FILENO, "\n", 1);
    }
    /* status bar */
    char bar[90];
    int bi = 0;
    const char *name = path[0] ? path : "[no name]";
    while (*name && bi < 24) bar[bi++] = *name++;
    bar[bi++] = ' ';
    if (dirty) bar[bi++] = '*';
    bar[bi++] = ' ';
    /* line:col */
    int ln = crow + 1, cl = ccol + 1;
    char num[16];
    int nt = 0, v;
    v = ln; if (v == 0) num[nt++] = '0';
    while (v > 0) { num[nt++] = (char)('0' + v % 10); v /= 10; }
    while (nt > 0) bar[bi++] = num[--nt];
    bar[bi++] = ':';
    nt = 0; v = cl; if (v == 0) num[nt++] = '0';
    while (v > 0) { num[nt++] = (char)('0' + v % 10); v /= 10; }
    while (nt > 0) bar[bi++] = num[--nt];
    bar[bi++] = ' ';
    const char *st = status;
    while (*st && bi < 78) bar[bi++] = *st++;
    while (bi < 78) bar[bi++] = ' ';
    bar[bi] = 0;
    write(STDOUT_FILENO, bar, (size_t)bi);
    write(STDOUT_FILENO, "\n", 1);
    /* Do not clear status here — callers that need a sticky prompt
       (find) keep it; normal path re-sets status before next draw. */
}

static int load_file(const char *p) {
    int fd = open(p, O_RDONLY);
    nlines = 0;
    crow = ccol = toprow = 0;
    dirty = 0;
    for (int i = 0; i < MAX_LINES; i++)
        lines[i][0] = 0;
    if (fd < 0) {
        /* new file */
        nlines = 1;
        lines[0][0] = 0;
        int i = 0;
        while (p[i] && i < 63) { path[i] = p[i]; i++; }
        path[i] = 0;
        set_status("new file");
        return 0;
    }
    int row = 0, col = 0;
    for (;;) {
        char c;
        long r = read(fd, &c, 1);
        if (r <= 0) break;
        if (c == '\n' || c == '\r') {
            if (c == '\r') continue;
            lines[row][col] = 0;
            row++;
            col = 0;
            if (row >= MAX_LINES) break;
            continue;
        }
        if (c >= 32 && c < 127 && col < MAX_COLS) {
            lines[row][col++] = c;
            lines[row][col] = 0;
        }
    }
    close(fd);
    if (col > 0 || row == 0) {
        lines[row][col] = 0;
        row++;
    }
    nlines = row > 0 ? row : 1;
    if (nlines == 0) {
        nlines = 1;
        lines[0][0] = 0;
    }
    int i = 0;
    while (p[i] && i < 63) { path[i] = p[i]; i++; }
    path[i] = 0;
    set_status("loaded");
    return 0;
}

static int save_file(void) {
    if (!path[0]) {
        set_status("no path — quit and reopen with name");
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        set_status("save failed");
        return -1;
    }
    for (int i = 0; i < nlines; i++) {
        int n = line_len(i);
        if (n > 0)
            write(fd, lines[i], (size_t)n);
        write(fd, "\n", 1);
    }
    close(fd);
    dirty = 0;
    set_status("saved");
    return 0;
}

static void insert_char(char ch) {
    int ll = line_len(crow);
    if (ll >= MAX_COLS) return;
    for (int i = ll; i >= ccol; i--)
        lines[crow][i + 1] = lines[crow][i];
    lines[crow][ccol] = ch;
    ccol++;
    dirty = 1;
}

static void backspace(void) {
    if (ccol > 0) {
        int ll = line_len(crow);
        for (int i = ccol - 1; i < ll; i++)
            lines[crow][i] = lines[crow][i + 1];
        ccol--;
        dirty = 1;
        return;
    }
    if (crow == 0) return;
    /* merge with previous line */
    int prev = crow - 1;
    int pl = line_len(prev);
    int cl = line_len(crow);
    if (pl + cl > MAX_COLS) return;
    for (int i = 0; i < cl; i++)
        lines[prev][pl + i] = lines[crow][i];
    lines[prev][pl + cl] = 0;
    for (int r = crow; r < nlines - 1; r++) {
        int k = 0;
        while (lines[r + 1][k]) {
            lines[r][k] = lines[r + 1][k];
            k++;
        }
        lines[r][k] = 0;
    }
    nlines--;
    crow = prev;
    ccol = pl;
    dirty = 1;
}

static void delete_char(void) {
    int ll = line_len(crow);
    if (ccol < ll) {
        for (int i = ccol; i < ll; i++)
            lines[crow][i] = lines[crow][i + 1];
        dirty = 1;
        return;
    }
    if (crow + 1 >= nlines) return;
    int nl = line_len(crow + 1);
    if (ll + nl > MAX_COLS) return;
    for (int i = 0; i < nl; i++)
        lines[crow][ll + i] = lines[crow + 1][i];
    lines[crow][ll + nl] = 0;
    for (int r = crow + 1; r < nlines - 1; r++) {
        int k = 0;
        while (lines[r + 1][k]) {
            lines[r][k] = lines[r + 1][k];
            k++;
        }
        lines[r][k] = 0;
    }
    nlines--;
    dirty = 1;
}

static void newline(void) {
    if (nlines >= MAX_LINES) return;
    int ll = line_len(crow);
    /* shift lines down */
    for (int r = nlines; r > crow + 1; r--) {
        int k = 0;
        while (lines[r - 1][k]) {
            lines[r][k] = lines[r - 1][k];
            k++;
        }
        lines[r][k] = 0;
    }
    /* split current */
    int j = 0;
    for (int i = ccol; i < ll; i++)
        lines[crow + 1][j++] = lines[crow][i];
    lines[crow + 1][j] = 0;
    lines[crow][ccol] = 0;
    nlines++;
    crow++;
    ccol = 0;
    dirty = 1;
}

static int match_at(int r, int c, const char *pat, int plen) {
    for (int i = 0; i < plen; i++) {
        if (lines[r][c + i] != pat[i]) return 0;
    }
    return 1;
}

static void do_find(void) {
    if (find_len <= 0) {
        set_status("find: empty");
        return;
    }
    /* search from next column */
    int r = crow, c = ccol + 1;
    for (;;) {
        if (r >= nlines) {
            set_status("find: no match");
            return;
        }
        int ll = line_len(r);
        while (c + find_len <= ll) {
            if (match_at(r, c, findbuf, find_len)) {
                crow = r;
                ccol = c;
                set_status("find: hit");
                return;
            }
            c++;
        }
        r++;
        c = 0;
    }
}

static int read_key(void) {
    char c;
    long r = read(STDIN_FILENO, &c, 1);
    if (r <= 0) return -1;
    return (unsigned char)c;
}

static void find_prompt(void) {
    find_len = 0;
    findbuf[0] = 0;
    set_status("find: ");
    draw();
    for (;;) {
        int k = read_key();
        if (k < 0) break;
        if (k == '\n' || k == '\r') break;
        if (k == 8 || k == 127) {
            if (find_len > 0) {
                findbuf[--find_len] = 0;
                set_status("find: ");
                /* append current find text to status */
                int si = 0;
                while (status[si]) si++;
                int j = 0;
                while (findbuf[j] && si < 79) status[si++] = findbuf[j++];
                status[si] = 0;
                draw();
            }
            continue;
        }
        if (k >= 32 && k < 127 && find_len < 39) {
            findbuf[find_len++] = (char)k;
            findbuf[find_len] = 0;
            set_status("find: ");
            int si = 0;
            while (status[si]) si++;
            int j = 0;
            while (findbuf[j] && si < 79) status[si++] = findbuf[j++];
            status[si] = 0;
            draw();
        }
    }
    do_find();
}

int main(int argc, char **argv) {
    path[0] = 0;
    status[0] = 0;
    findbuf[0] = 0;
    find_len = 0;
    nlines = 1;
    lines[0][0] = 0;
    crow = ccol = toprow = 0;
    dirty = 0;

    if (argc >= 2)
        load_file(argv[1]);
    else {
        nlines = 1;
        set_status("Ctrl-S save  Ctrl-F find  Ctrl-Q quit");
    }

    for (;;) {
        clamp_cursor();
        draw();
        int k = read_key();
        if (k < 0) break;

        if (k == 17 || k == 24) { /* Ctrl-Q / Ctrl-X */
            static int quit_armed;
            if (dirty && !quit_armed) {
                quit_armed = 1;
                set_status("unsaved — Ctrl-S to save, Ctrl-Q again to quit");
                continue;
            }
            clear_screen();
            return 0;
        }
        if (k == 19) { /* Ctrl-S */
            save_file();
            /* allow clean quit after save without double Ctrl-Q */
            continue;
        }
        if (k == 6) { /* Ctrl-F */
            find_prompt();
            continue;
        }
        if (k == KEY_LEFT)  { ccol--; continue; }
        if (k == KEY_RIGHT) { ccol++; continue; }
        if (k == KEY_UP)    { crow--; continue; }
        if (k == KEY_DOWN)  { crow++; continue; }
        if (k == KEY_HOME)  { ccol = 0; continue; }
        if (k == KEY_END)   { ccol = line_len(crow); continue; }
        if (k == KEY_DELETE){ delete_char(); continue; }
        if (k == 8 || k == 127) { backspace(); continue; }
        if (k == '\n' || k == '\r') { newline(); continue; }
        if (k >= 32 && k < 127) { insert_char((char)k); continue; }
    }
    return 0;
}
