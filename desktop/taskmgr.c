/* Task Manager: the process table, read live through ps(2).
 *
 * The list is re-read on every paint rather than cached, because paint is
 * exactly the moment the window becomes visible -- reading it anywhere
 * else would mean keeping a copy in sync with a table that changes under
 * us.  A full table is 64 entries of 40 bytes; the syscall is cheaper
 * than the drawing that follows it. */
#include "desktop.h"

#define TMAX 64
#define ROW_H 16
#define STAT_H 42
#define BTN_H 22

struct tsk {
    struct lnxrm_ps_entry e[TMAX];
    int n;
    int sel;
    int top;
    char last_err[32];
    long last_ms; /* when the window last asked for a repaint */
};

static const char *state_name(int s)
{
    switch (s) {
    case LNXRM_TS_RUNNABLE: return "runnable";
    case LNXRM_TS_RUNNING: return "running";
    case LNXRM_TS_SLEEPING: return "sleeping";
    case LNXRM_TS_ZOMBIE: return "zombie";
    case LNXRM_TS_STOPPED: return "stopped";
    case LNXRM_TS_EMBRYO: return "embryo";
    default: return "-";
    }
}

static void list_rect(const struct win *w, int *lx, int *ly, int *lw, int *lh)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    *lx = x + 5;
    *ly = y + STAT_H + 18;
    *lw = cw - 10;
    *lh = ch - STAT_H - 18 - BTN_H - 14;
    if (*lh < ROW_H) *lh = ROW_H;
}

static int rows_of(const struct win *w)
{
    int lx, ly, lw, lh;
    list_rect(w, &lx, &ly, &lw, &lh);
    int r = (lh - 4) / ROW_H;
    return r > 0 ? r : 1;
}

static void refresh(struct tsk *t)
{
    long n = kps(t->e, TMAX);
    t->n = n < 0 ? 0 : (int)n;
    if (t->sel >= t->n) t->sel = t->n ? t->n - 1 : 0;
    if (t->top > t->sel) t->top = t->sel;
}

static void tm_paint(struct win *w)
{
    struct tsk *t = w->state;
    if (!t) return;
    refresh(t);

    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    /* two stat lines: what the window is actually for */
    gfx_hgrad(x + 5, y + 5, cw - 10, 16, COL_TITLE0, COL_TITLE1);
    gfx_text(x + 8, y + 5, "Task Manager", COL_HILITE);

    /* Right end of the same strip: the frame rate the main loop counts.
     * It used to be written to the console once a second, which put a
     * line of "[gui] fps=..." into whatever was reading the console --
     * diagnostics in a window the user opened on purpose.  A status line
     * is where it belonged anyway, and tm_tick() already repaints this
     * window once a second, so the number is as fresh as the uptime
     * beside it. */
    char fb[24];
    xfmt_i(fb, desktop_fps());
    str_cat(fb, " fps");
    gfx_text(x + cw - 8 - gfx_strw(fb), y + 5, fb, COL_HILITE);

    char b[40];
    long ms = uptime_ms();
    if (ms < 0) ms = 0;
    long sec = ms / 1000;
    strcpy(b, "Uptime ");
    xfmt_i(b + xstrlen(b), (long)(sec / 3600));
    {
        int n = (int)xstrlen(b);
        int mm = (int)((sec / 60) % 60);
        b[n] = ':';
        b[n + 1] = (char)('0' + (mm / 10) % 10);
        b[n + 2] = (char)('0' + mm % 10);
        b[n + 3] = 0;
    }
    gfx_text(x + 8, y + 22, b, COL_TEXT);

    strcpy(b, "Processes: ");
    xfmt_i(b + xstrlen(b), t->n);
    gfx_text(x + cw - 8 - gfx_strw(b), y + 22, b, COL_TEXT);

    /* column header */
    int lx, ly, lw, lh;
    list_rect(w, &lx, &ly, &lw, &lh);
    gfx_text(lx + 2, ly - 17, " PID  PPID  STATE      UID NAME", COL_TEXT);
    gfx_hline(lx, ly - 1, lw, COL_SHADOW);

    gfx_fill(lx, ly, lw, lh, COL_LISTBG);
    gfx_in(lx, ly, lw, lh);

    int rows = rows_of(w);
    if (t->sel < t->top) t->top = t->sel;
    if (t->sel >= t->top + rows) t->top = t->sel - rows + 1;
    if (t->top < 0) t->top = 0;

    for (int r = 0; r < rows; r++) {
        int i = t->top + r;
        if (i >= t->n) break;
        int ry = ly + 2 + r * ROW_H;
        if (i == t->sel) gfx_fill(lx + 1, ry, lw - 2, ROW_H, COL_SEL);
        u32 fg = i == t->sel ? COL_HILITE : COL_TEXT;

        char cell[48];
        int cx = lx + 3;

        strcpy(cell, "");
        xfmt_i(cell + xstrlen(cell), t->e[i].pid);
        gfx_text(cx, ry, cell, fg);
        cx += 40;

        strcpy(cell, "");
        xfmt_i(cell + xstrlen(cell), t->e[i].ppid);
        gfx_text(cx, ry, cell, fg);
        cx += 40;

        gfx_text(cx, ry, state_name(t->e[i].state), fg);
        cx += 84;

        strcpy(cell, "");
        xfmt_i(cell + xstrlen(cell), t->e[i].uid);
        gfx_text(cx, ry, cell, fg);
        cx += 30;

        gfx_text(cx, ry, t->e[i].name, fg);
    }

    /* buttons */
    int by = y + ch - BTN_H - 5;
    static const char *labels[2] = {"Refresh", "Kill"};
    for (int i = 0; i < 2; i++) {
        int bw = 82;
        int bx = x + 5 + i * (bw + 6);
        gfx_glass_button(bx, by, bw, BTN_H, 0, labels[i]);
    }
    if (t->last_err[0]) gfx_text(x + cw - 6 - gfx_strw(t->last_err), by + 3, t->last_err, COL_TEXT);
    (void)ch;
}

static int btn_at(const struct win *w, int px, int py)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int by = y + ch - BTN_H - 5;
    if (py < by || py >= by + BTN_H) return -1;
    for (int i = 0; i < 2; i++) {
        int bw = 82;
        int bx = x + 5 + i * (bw + 6);
        if (px >= bx && px < bx + bw) return i;
    }
    return -1;
}

static void tm_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    struct tsk *t = w->state;
    if (!t || btn != LNXRM_MB_LEFT) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int px = x + cx, py = y + cy;

    int b = btn_at(w, px, py);
    if (b == 0) {
        refresh(t);
        t->last_err[0] = 0;
        return;
    }
    if (b == 1) {
        if (t->n <= 0) return;
        long r = kkill(t->e[t->sel].pid, SIGTERM);
        if (r < 0) {
            strcpy(t->last_err, r == LNXRM_EACCES  ? "permission denied"
                                : r == LNXRM_ESRCH ? "no such process"
                                                   : "kill failed");
        } else {
            strcpy(t->last_err, "signal sent");
        }
        refresh(t);
        return;
    }

    int lx, ly, lw, lh;
    list_rect(w, &lx, &ly, &lw, &lh);
    if (px < lx || py < ly || px >= lx + lw || py >= ly + lh) return;
    int i = t->top + (py - ly - 2) / ROW_H;
    if (i >= 0 && i < t->n) {
        t->sel = i;
        t->last_err[0] = 0;
    }
    (void)clicks;
}

static void tm_key(struct win *w, const struct dsk_event *ev)
{
    if (ev->type != LNXRM_EV_KEY_DOWN) return;
    struct tsk *t = w->state;
    if (!t || !t->n) return;
    int rows = rows_of(w);
    switch (ev->code) {
    case LNXRM_KEY_UP: if (t->sel > 0) t->sel--; break;
    case LNXRM_KEY_DOWN: if (t->sel < t->n - 1) t->sel++; break;
    case LNXRM_KEY_HOME: t->sel = 0; break;
    case LNXRM_KEY_END: t->sel = t->n - 1; break;
    case LNXRM_KEY_PGUP: t->sel -= rows; if (t->sel < 0) t->sel = 0; break;
    case LNXRM_KEY_PGDN:
        t->sel += rows;
        if (t->sel > t->n - 1) t->sel = t->n - 1;
        break;
    case LNXRM_KEY_DELETE:
    case LNXRM_KEY_ENTER: {
        long r = kkill(t->e[t->sel].pid, SIGTERM);
        strcpy(t->last_err,
               r < 0 ? (r == LNXRM_EACCES  ? "permission denied"
                        : r == LNXRM_ESRCH ? "no such process" : "kill failed")
                     : "signal sent");
        refresh(t);
        break;
    }
    default: break;
    }
}

static void tm_open(struct win *w)
{
    struct tsk *t = (struct tsk *)xcalloc(1, sizeof(struct tsk));
    if (!t) return;
    refresh(t);
    w->state = t;
}

/* Uptime and the process states move on their own; a window nobody has
 * touched is a window nobody repaints, so the table has to ask for the
 * repaint itself.  Once a second is what the clock it prints is worth
 * anyway, and one client-sized rectangle a second costs nothing. */
static void tm_tick(struct win *w)
{
    struct tsk *t = w->state;
    if (!t || w->minimized) return;
    long now = uptime_ms();
    if (t->last_ms && now - t->last_ms < 1000) return;
    t->last_ms = now;
    win_invalidate_client(w);
}

static void tm_close(struct win *w)
{
    if (w->state) {
        xfree(w->state);
        w->state = 0;
    }
}

const struct app app_taskmgr = {
    .name = "Task Manager",
    .title = "Task Manager",
    .w = 470,
    .h = 340,
    .open = tm_open,
    .paint = tm_paint,
    .down = tm_down,
    .drag = 0,
    .up = 0,
    .key = tm_key,
    .close = tm_close,
    .tick = tm_tick,
};
