/* The bottom of the screen and the desktop floor: the taskbar (window
 * buttons, clock) and the desktop icons.  Every program in the registry
 * sits on the desktop, so there is no Start button and no menu -- the
 * bar is just the row of windows that are open, plus the clock that
 * says how long this session has been up.
 *
 * The clock is session uptime (uptime_ms(), fed by the main loop), not
 * wall time: user space has no clock, and a desktop that lies about the
 * time is worse than one that counts.  taskbar_tick() is what makes it
 * move, because nothing else would repaint the corner of the screen on
 * its own. */
#include "desktop.h"

#define TB_Y (g_fh - TASKBAR_H)
#define CLOCK_W 72
#define TBB_H 24
#define TBB_R 6                  /* the buttons' corner radius   */

static int hover_tbb = -1;  /* hovered taskbar button index, -1 = none */
static int sel_icon = -1;   /* selected desktop icon                  */
static char clock_str[8];
static int clock_ready;

/* ---- geometry ---- */

/* A dotted handle before the clock -- the taskbar in Win98 was a rebar,
 * and a rebar was a set of grips with controls between them; without one
 * the right end is just a well on a glass stripe. */
#define GRIP_W 10

static int grip_b_x(void)
{
    return g_fw - 2 - CLOCK_W - GRIP_W - 4;
}

/* Every window gets a button, but the row is a fixed width: shrink them
 * all before any of them falls off the left edge. */
static void task_btn_rect(int i, int *x, int *y, int *w, int *h)
{
    int first = 6;
    int avail = grip_b_x() - 4 - first;
    if (avail < 60) avail = 60;
    int n = wm_count();
    int bw = 150;
    if (n > 0 && (long)bw * n > avail) bw = avail / n;
    if (bw < 36) bw = 36;
    *x = first + i * (bw + 2);
    *y = TB_Y + 3;
    *w = bw;
    *h = TBB_H;
}

static void clock_rect(int *x, int *y, int *w, int *h)
{
    *x = g_fw - 2 - CLOCK_W;
    *y = TB_Y + 3;
    *w = CLOCK_W;
    *h = TBB_H;
}

/* ---- taskbar ---- */

/* A pair of dotted columns: highlight on the left of each dot, a dark
 * grey on the right, so it reads as recessed rather than printed -- and
 * dark enough to survive being drawn on glass instead of on grey. */
static void grip(int x, int y, int w, int h)
{
    for (int c = 1; c + 1 < w - 1; c += 3)
        for (int r = 3; r + 1 < h - 3; r += 3) {
            gfx_px(x + c, y + r, COL_HILITE);
            gfx_px(x + c + 1, y + r, RGB(116, 120, 132));
        }
}

/* A capsule laid on the glass: the radius leaves the four corners alone,
 * and they already hold the bar's own pixels, so there is nothing to hand
 * back afterwards.  The pressed state is the focused window's button --
 * an outline in the accent colour rather than a Win98 sunken box, because
 * a well drawn in the middle of a translucent bar has nothing behind it
 * to sink into. */
static void draw_tb_button(int x, int y, int w, int h, int pressed, int hovered,
                           const char *label)
{
    gfx_glass_button(x, y, w, h, pressed, label);
    if (hovered && !pressed) {
        /* dotted focus rectangle, the Win98 way of saying "this one" */
        for (int i = 2; i < w - 2; i += 2) {
            gfx_px(x + i, y + 2, COL_DKSHADOW);
            gfx_px(x + i, y + h - 3, COL_DKSHADOW);
        }
        for (int i = 2; i < h - 2; i += 2) {
            gfx_px(x + 2, y + i, COL_DKSHADOW);
            gfx_px(x + w - 3, y + i, COL_DKSHADOW);
        }
    }
}

void taskbar_paint(void)
{
    int x, y, w, h;

    /* Nothing along the bottom of the screen is being repainted -- the
     * most common case by far, since most damage is a window in the
     * middle of the desktop. */
    if (!gfx_clip_hit(0, TB_Y, g_fw, TASKBAR_H)) return;

    /* A sheet of glass laid across the bottom of the screen: the same
     * blur the captions use, so the bar belongs to the material the
     * windows are made of rather than being a grey stripe cut through
     * the desktop.  The white hairline on top and its shadow under it are
     * all that is left of the old raised edge, and they are what keeps
     * the bar from melting into the wallpaper where the two meet. */
    gfx_glass(0, TB_Y, g_fw, TASKBAR_H, GLASS_BAR_T, GLASS_BAR_A);
    gfx_hline(0, TB_Y, g_fw, COL_HILITE);
    gfx_fill_a(0, TB_Y + 1, g_fw, 1, RGB(70, 74, 86), 96);

    grip(grip_b_x(), TB_Y + 3, GRIP_W, TBB_H);

    int n = wm_count();
    for (int i = 0; i < n; i++) {
        struct win *wn = wm_nth(i);
        if (!wn) continue;
        int bx, by, bw, bh;
        task_btn_rect(i, &bx, &by, &bw, &bh);
        int pressed = (win_focused() == wn && !wn->minimized);
        draw_tb_button(bx, by, bw, bh, pressed, hover_tbb == i, wn->title);
    }

    clock_rect(&x, &y, &w, &h);
    /* No fill: the tray is a well cut into the glass, not a box set on
     * it, and the glass behind it is the right colour already. */
    gfx_in(x, y, w, h);
    if (clock_ready) {
        /* Right-aligned against the tray's own edge, in the bold the
         * clock is drawn in -- not the regular width gfx_strw() reports. */
        gfx_text_bold(x + w - 6 - gfx_strw_bold(clock_str), y + (h - 16) / 2,
                      clock_str, COL_TEXT);
    }
}

void taskbar_tick(void)
{
    long ms = uptime_ms();
    if (ms < 0) ms = 0;
    long sec = ms / 1000;
    int hh = (int)((sec / 3600) % 24);
    int mm = (int)((sec / 60) % 60);
    char s[8];
    s[0] = '0' + (hh / 10) % 10;
    s[1] = '0' + hh % 10;
    s[2] = ':';
    s[3] = '0' + (mm / 10) % 10;
    s[4] = '0' + mm % 10;
    s[5] = 0;
    if (clock_ready && !strcmp(s, clock_str)) return;
    strcpy(clock_str, s);
    clock_ready = 1;
    int x, y, w, h;
    clock_rect(&x, &y, &w, &h);
    gfx_dirty(x, y, w, h);
}

void taskbar_hover(int x, int y)
{
    int nt = -1;
    if (y >= TB_Y) {
        int n = wm_count();
        for (int i = 0; i < n; i++) {
            int bx, by, bw, bh;
            task_btn_rect(i, &bx, &by, &bw, &bh);
            if (x >= bx && x < bx + bw) {
                nt = i;
                break;
            }
        }
    }
    if (nt != hover_tbb) {
        if (hover_tbb >= 0) {
            int bx, by, bw, bh;
            task_btn_rect(hover_tbb, &bx, &by, &bw, &bh);
            gfx_dirty(bx, by, bw, bh);
        }
        hover_tbb = nt;
        if (hover_tbb >= 0) {
            int bx, by, bw, bh;
            task_btn_rect(hover_tbb, &bx, &by, &bw, &bh);
            gfx_dirty(bx, by, bw, bh);
        }
    }
}

int taskbar_hit(int x, int y, int btn, int clicks)
{
    int n = wm_count();
    for (int i = 0; i < n; i++) {
        int bx, by, bw, bh;
        task_btn_rect(i, &bx, &by, &bw, &bh);
        if (x < bx || x >= bx + bw || y < by || y >= by + bh) continue;
        struct win *w = wm_nth(i);
        if (!w) return 1;
        /* Clicking the button you are already looking at minimises it --
         * the one bit of taskbar behaviour people actually rely on. */
        if (win_focused() == w && !w->minimized)
            win_minimize(w);
        else
            win_focus(w);
        return 1;
    }
    (void)btn;
    (void)clicks;
    return 1; /* the taskbar never lets a click through to the desktop */
}

/* ---- desktop icons ----
 * Every program in the registry, one 32x32 icon apiece, double-click to
 * open.  Six to a column from the top-left, which on any screen this
 * system supports holds the whole registry twice over. */

#define ICON_N 13
#define ICON_W 76
#define ICON_H 74
#define ICON_X 8
#define ICON_Y 8

static const struct app *icon_app(int i)
{
    /* apps.c holds the registry in menu order; the desktop shows all of
     * it plus About (which the registry excludes, the way the Start
     * menu's footer used to pin it) in a reading order of its own */
    extern const struct app app_files;
    extern const struct app app_notepad;
    extern const struct app app_calc;
    extern const struct app app_paint;
    extern const struct app app_mines;
    extern const struct app app_terminal;
    extern const struct app app_taskmgr;
    extern const struct app app_clock;
    extern const struct app app_sysinfo;
    extern const struct app app_display;
    extern const struct app app_snake;
    extern const struct app app_about;
    extern const struct app app_mouse;
    switch (i) {
    case 0: return &app_files;
    case 1: return &app_notepad;
    case 2: return &app_calc;
    case 3: return &app_paint;
    case 4: return &app_mines;
    case 5: return &app_terminal;
    case 6: return &app_taskmgr;
    case 7: return &app_clock;
    case 8: return &app_sysinfo;
    case 9: return &app_display;
    case 10: return &app_snake;
    case 11: return &app_mouse;
    default: return &app_about;
    }
}

static const char *icon_label(int i)
{
    static const char *const labels[ICON_N] = {
        "Home",     "Notepad",  "Calculator", "Paint",
        "Minesweeper", "Terminal", "Task Manager", "Clock",
        "System",      "Display",  "Snake",      "Mouse",
        "About",
    };
    return labels[i];
}

void desk_paint(void)
{
    for (int i = 0; i < ICON_N; i++) {
        int x = ICON_X + (i / 6) * ICON_W;
        int y = ICON_Y + (i % 6) * ICON_H;
        int sel = (i == sel_icon);
        if (sel) gfx_fill(x, y, ICON_W - 6, ICON_H - 8, RGB(0, 96, 96));
        /* 32x32: the very same drawing the icon art was authored against,
         * so an app looks like itself wherever it appears */
        icon_paint(icon_id_for(icon_app(i)), x + (ICON_W - 6 - 32) / 2, y + 2, 2);

        const char *lab = icon_label(i);
        int tw = gfx_strw(lab);
        int lx = x + (ICON_W - 6 - tw) / 2, ly = y + 38;
        if (sel) {
            /* the selected label gets a filled box, the way Win98 marked a
             * chosen icon once the wallpaper made plain text unreliable */
            gfx_fill(lx - 4, ly - 1, tw + 8, 18, COL_SEL);
            gfx_text(lx, ly, lab, COL_HILITE);
        } else {
            /* one pixel of black under the white: on teal it costs
             * nothing, and on a patterned wallpaper it is what keeps the
             * label from dissolving into it */
            gfx_text(lx + 1, ly + 1, lab, COL_DKSHADOW);
            gfx_text(lx, ly, lab, COL_HILITE);
        }
        if (sel) {
            /* 1 px marching-ants box in white: visible on teal without
             * needing an alpha channel we do not have */
            for (int k = 0; k < ICON_W - 6; k += 2) {
                gfx_px(x + k, y, COL_HILITE);
                gfx_px(x + k, y + ICON_H - 9, COL_HILITE);
            }
            for (int k = 0; k < ICON_H - 8; k += 2) {
                gfx_px(x, y + k, COL_HILITE);
                gfx_px(x + ICON_W - 7, y + k, COL_HILITE);
            }
        }
    }
}

int desk_hit(int x, int y, int btn, int clicks)
{
    int hit = -1;
    for (int i = 0; i < ICON_N; i++) {
        int ix = ICON_X + (i / 6) * ICON_W;
        int iy = ICON_Y + (i % 6) * ICON_H;
        if (x >= ix && y >= iy && x < ix + ICON_W - 6 && y < iy + ICON_H - 8) hit = i;
    }
    if (hit < 0) {
        if (sel_icon != -1) {
            sel_icon = -1;
            gfx_dirty(ICON_X, ICON_Y, ICON_W * 3, ICON_H * 6);
        }
        return 0;
    }
    if (sel_icon != hit) {
        /* Only mark the old icon when there was one: with sel_icon == -1
         * the row/column arithmetic below points at row -1, above the top
         * of the screen, and the dirty would land on a strip of desktop
         * nobody touched. */
        if (sel_icon >= 0) {
            int oldx = ICON_X + (sel_icon / 6) * ICON_W;
            int oldy = ICON_Y + (sel_icon % 6) * ICON_H;
            gfx_dirty(oldx, oldy, ICON_W, ICON_H);
        }
        sel_icon = hit;
        gfx_dirty(ICON_X + (hit / 6) * ICON_W, ICON_Y + (hit % 6) * ICON_H, ICON_W, ICON_H);
    }
    if (clicks >= 2 && btn == LNXRM_MB_LEFT) win_open(icon_app(hit));
    return 1;
}
