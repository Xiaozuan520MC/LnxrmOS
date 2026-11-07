/* Terminal: the console's window.  The desktop takes the console while
 * it runs (con_capture(1) detaches the shell from the keyboard and the
 * screen in one move), and this window is where the console comes back:
 * the ring the muted output keeps reaching is drained once per frame
 * into a scrollback, the shell behind it is typed into through
 * kbd_inject, and its output lands here one line at a time.
 *
 * The shell itself is the desktop's child (loop.c forked it when the
 * console was captured): prompt, echo, commands and their output all
 * arrive through the same ring, which is why this window has no shell
 * of its own and never runs a command itself.  Closing it closes a
 * view, not the console -- there is one console and it outlives every
 * window that looks at it.
 *
 * The scrollback is a flat byte range with '\n' as the only terminator,
 * exactly like Notepad's buffer: lines are found by scanning, the view
 * is a line index, and PageUp/PageDown walk it.  The kernel's ring is
 * 4 KiB and drops its oldest bytes when it wraps, so what survives here
 * is bounded by what the console printed, not by what this window can
 * hold -- a scrollback that claimed to remember everything would be
 * lying about a console that does not.
 */
#include "desktop.h"

#define SB_CAP  8192 /* scrollback bytes; halved from the front when full */
#define ROW_H   16
#define STAT_H  20

struct term {
    char sb[SB_CAP]; /* the console's output, '\n'-terminated lines */
    int sb_len;
    int sb_view;     /* line index at the top of the window */
    int drop;        /* the first pump discards the boot log */
    int blink;       /* the caret phase the last paint drew */
};

/* One console, one ring, one reader: every Terminal window shows the
 * same scrollback, so the state is file-scope and the windows' state
 * pointers all point here. */
static struct term g_term;

/* Which line does byte offset i start?  Scanned like Notepad's, because
 * the buffer is the same shape. */
static int line_count(void)
{
    int l = 1;
    for (int i = 0; i < g_term.sb_len; i++)
        if (g_term.sb[i] == '\n') l++;
    return l;
}

/* Byte offset where line `n` starts; the end when n is past the last. */
static int line_start(int n)
{
    if (n <= 0) return 0;
    int l = 0;
    for (int i = 0; i < g_term.sb_len; i++)
        if (g_term.sb[i] == '\n' && ++l == n) return i + 1;
    return g_term.sb_len;
}

/* The scrollback is full: give the front half back, line-aligned, and
 * walk the view down by the lines that went, so what the user was
 * reading stays under their eyes. */
static void sb_compact(void)
{
    int cut = 0;
    while (cut < SB_CAP / 2 && cut < g_term.sb_len)
        if (g_term.sb[cut++] == '\n') break;
    if (cut >= g_term.sb_len) { /* pathological: one enormous line */
        cut = g_term.sb_len;
        g_term.sb_len = 0;
    } else {
        int lines = 0;
        for (int i = 0; i < cut; i++)
            if (g_term.sb[i] == '\n') lines++;
        memmove(g_term.sb, g_term.sb + cut, (size_t)(g_term.sb_len - cut));
        g_term.sb_len -= cut;
        g_term.sb_view -= lines;
    }
    if (g_term.sb_view < 0) g_term.sb_view = 0;
}

/* Returns 1 when the buffer changed (bytes appended, or a clear from an
 * ANSI J sequence) -- the caller invalidates the windows that are
 * looking. */
static int term_append(const char *s, int n)
{
    int changed = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        /* The shell's own erase ("\b \b") trims the current line; a bare
         * CR rewrites it from the left edge. */
        if (c == '\b') {
            if (g_term.sb_len > 0 && g_term.sb[g_term.sb_len - 1] != '\n')
                g_term.sb_len--;
            continue;
        }
        if (c == '\r') {
            while (g_term.sb_len > 0 && g_term.sb[g_term.sb_len - 1] != '\n')
                g_term.sb_len--;
            continue;
        }
        if (c == '\t') {
            /* spaces to the next 8th column, the way the mono pen counts */
            int col = 0;
            for (int j = g_term.sb_len; j > 0 && g_term.sb[j - 1] != '\n'; j--)
                col++;
            int stop = (col / 8 + 1) * 8;
            while (col < stop) {
                if (g_term.sb_len + 1 >= SB_CAP) sb_compact();
                g_term.sb[g_term.sb_len++] = ' ';
                col++;
            }
            changed = 1;
            continue;
        }
        if (c == '\033') { /* an ANSI sequence: the ones that mean
                            * something to a scrolling terminal are the
                            * clears -- `clear` writes ESC[2J ESC[H
                            * ESC[3J -- and everything else (colours,
                            * cursor moves) is skipped whole */
            if (i + 1 < n && s[i + 1] == '[') {
                for (i += 2; i < n; i++) {
                    char f = s[i];
                    if (f >= '@' && f <= '~') {
                        if (f == 'J') { /* 0/2/3J: clear the screen and
                                          * the scrollback behind it */
                            g_term.sb_len = 0;
                            g_term.sb_view = -1;
                            changed = 1;
                        }
                        break;
                    }
                }
            } else if (i + 1 < n) {
                i++;
            }
            continue;
        }
        if (g_term.sb_len + 1 >= SB_CAP) sb_compact();
        g_term.sb[g_term.sb_len++] = c;
        changed = 1;
    }
    return changed;
}

/* Drained once per frame from the main loop, never from paint: the
 * console ring has a single read cursor and only one reader may walk
 * it.  The first pump discards what the ring holds -- the boot log, and
 * the shell session this desktop was launched from -- so the scrollback
 * starts with the console's own next word, which is the prompt the
 * desktop's shell prints the moment it is forked. */
void term_pump(void)
{
    char buf[256];
    int moved = 0;
    for (;;) {
        long n = kcon_read(buf, (int)sizeof(buf));
        if (n <= 0) break;
        if (!g_term.drop) moved |= term_append(buf, (int)n);
    }
    g_term.drop = 0;
    if (!moved) return;
    /* the console writes below the view, the view follows it: a terminal
     * that sat still while its own output ran past would have to be
     * scrolled by hand */
    g_term.sb_view = -1;
    for (int i = 0; i < wm_count(); i++) {
        struct win *w = wm_nth(i);
        if (w && w->used && w->app == &app_terminal && !w->minimized)
            win_invalidate_client(w);
    }
}

static int rows_of(const struct win *w)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int r = (ch - STAT_H - 2) / ROW_H;
    return r > 0 ? r : 1;
}

static int cols_of(const struct win *w)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int c = (cw - 6) / 8;
    return c > 0 ? c : 1;
}

/* The view: line 0 when the user scrolled up, otherwise the bottom.  A
 * view of -1 means "pinned" -- new output moves it, scroll keys give it
 * a number and following ends until Enter or End puts it back. */
static int view_top(const struct win *w)
{
    if (g_term.sb_view < 0) {
        int rows = rows_of(w);
        int top = line_count() - rows;
        return top < 0 ? 0 : top;
    }
    return g_term.sb_view;
}

static void tm_paint(struct win *w)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, RGB(14, 16, 20));

    int rows = rows_of(w), cols = cols_of(w);
    int top = view_top(w);

    for (int r = 0; r < rows; r++) {
        int s = line_start(top + r);
        if (top + r >= line_count() - 1 && s >= g_term.sb_len) break;
        int e = s;
        while (e < g_term.sb_len && g_term.sb[e] != '\n') e++;
        int lim = e - s;
        if (lim > cols) lim = cols;
        for (int i = 0; i < lim; i++) {
            char c = g_term.sb[s + i];
            if (c < 32 || c > 126) c = ' ';
            font_draw(x + 3 + i * 8, y + 2 + r * ROW_H, (unsigned char)c,
                      RGB(220, 224, 232), 0);
        }
    }

    /* the caret: the shell's echo lands a character at a time, so the
     * block at the end of the bottom line is where typing is.  Its row
     * is the last line's own row -- not the window's bottom, which is
     * where it used to blink when the scrollback was shorter than the
     * window.  Typed keys go to the focused window, so the caret only
     * shows there. */
    int last_row = (line_count() - 1) - top;
    if (last_row >= 0 && last_row < rows && win_focused() == w &&
        ((uptime_ms() >> 8) & 1)) {
        int s = line_start(line_count() - 1);
        int col = g_term.sb_len - s;
        if (col > cols) col = cols;
        gfx_fill(x + 3 + col * 8, y + 2 + last_row * ROW_H, 8, ROW_H,
                 RGB(220, 224, 232));
    }

    /* status line: what the window is for, and where the view is */
    int sy = y + ch - STAT_H;
    gfx_fill(x, sy, cw, STAT_H, COL_FACE);
    gfx_in(x, sy, cw, STAT_H);
    char st[64];
    st[0] = 0;
    str_cat(st, g_term.sb_view < 0 ? "Console   "
                                   : "Console  (scrolled)   ");
    str_cat_i(st + xstrlen(st), line_count() - 1);
    str_cat(st, " lines");
    gfx_text_w(x + 5, sy + 3, st, COL_TEXT, cw - 10);
}

static void tm_scroll(struct win *w, int delta)
{
    int rows = rows_of(w);
    if (delta > 0 && g_term.sb_view < 0)
        g_term.sb_view = view_top(w); /* leaving the bottom: take a number */
    g_term.sb_view += delta;
    int last = line_count() - rows;
    if (last < 0) last = 0;
    if (g_term.sb_view > last) g_term.sb_view = -1; /* pinned again */
    if (g_term.sb_view < 0) g_term.sb_view = 0;
    win_invalidate_client(w);
}

static void tm_key(struct win *w, const struct dsk_event *ev)
{
    if (ev->type != LNXRM_EV_KEY_DOWN) return;
    switch (ev->code) {
    case LNXRM_KEY_PGUP: tm_scroll(w, -rows_of(w)); return;
    case LNXRM_KEY_PGDN: tm_scroll(w, rows_of(w)); return;
    case LNXRM_KEY_UP:   tm_scroll(w, -1); return;
    case LNXRM_KEY_DOWN: tm_scroll(w, 1); return;
    case LNXRM_KEY_ENTER: kcon_inject('\n'); return;
    case LNXRM_KEY_BACKSPACE: kcon_inject('\b'); return;
    case LNXRM_KEY_TAB: kcon_inject('\t'); return;
    case LNXRM_KEY_ESC: kcon_inject(27); return;
    default: {
        char c = (char)ev->ch;
        if (c >= 32 && c < 127) kcon_inject(c);
        return;
    }
    }
}

static void tm_tick(struct win *w)
{
    struct term *t = &g_term;
    if (w->minimized) return;
    /* the caret only matters while the bottom line is on screen */
    if (t->sb_view >= 0) return;
    int phase = (int)((uptime_ms() >> 8) & 1);
    if (phase == t->blink) return;
    t->blink = phase;
    win_invalidate_client(w);
}

const struct app app_terminal = {
    .name = "Terminal",
    .title = "Terminal",
    .w = 460,
    .h = 300,
    .open = 0,
    .paint = tm_paint,
    .down = 0,
    .drag = 0,
    .up = 0,
    .key = tm_key,
    .close = 0,
    .tick = tm_tick,
};
