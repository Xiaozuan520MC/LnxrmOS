/* The window manager: the z-order, the title bar (gradient, three
 * buttons, drag and resize) and the routing that decides which module
 * sees a click.
 *
 * Everything is single-threaded and event driven, so none of this needs
 * a lock -- but it does need one invariant to hold: `zorder` is the only
 * thing that decides what is on top, and `focused` is always its last
 * non-minimised entry.  Painting walks zorder bottom-up, hit testing walks
 * it top-down, and every operation that changes it repaints the taskbar
 * too because the taskbar is where the same window is also visible. */
#include "desktop.h"

static struct win wins[WIN_MAX];
static struct win *zorder[WIN_MAX]; /* [0] = bottom of the stack */
static int zcount;
static int id_seq;
static struct win *focused;
static int cascade;

/* pointer gesture state */
enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CLIENT, DRAG_BTN };
enum { BTN_MIN = 1, BTN_MAX = 2, BTN_CLOSE = 3 };

static int drag;
static struct win *drag_win;  /* window the gesture belongs to          */
static int drag_ox, drag_oy;  /* pointer -> window origin, for moving    */
static int drag_sx, drag_sy;  /* pointer position when the gesture began */
static int start_w, start_h;  /* outer size when a resize began          */
static int tbtn;              /* title bar button under the press        */

/* ---- geometry ---- */

void win_client_rect(const struct win *w, int *x, int *y, int *cw, int *ch)
{
    *x = w->x + FRAME_W + CLIENT_B;
    *y = w->y + FRAME_W + TITLE_H + CLIENT_B;
    *cw = w->w - 2 * (FRAME_W + CLIENT_B);
    *ch = w->h - 2 * FRAME_W - TITLE_H - 2 * CLIENT_B;
    if (*cw < 0) *cw = 0;
    if (*ch < 0) *ch = 0;
}

static void clamp_pos(const struct win *w, int *px, int *py)
{
    int x = *px, y = *py;
    /* The title bar must stay reachable and the whole bar must stay above
     * the taskbar; the rest of the window is allowed to hang off the
     * edges, exactly like dragging it behind another window. */
    if (x > g_fw - 32) x = g_fw - 32;
    if (x < 32 - w->w) x = 32 - w->w;
    if (y > g_fh - TASKBAR_H - TITLE_H - FRAME_W) y = g_fh - TASKBAR_H - TITLE_H - FRAME_W;
    if (y < 0) y = 0;
    *px = x;
    *py = y;
}

static void dirty_chrome(void)
{ gfx_dirty(0, g_fh - TASKBAR_H, g_fw, TASKBAR_H); }

/* The drop shadow falls outside the window, so every operation that
 * moves, resizes, opens or hides one has to invalidate that much more
 * than the window's own rectangle -- otherwise the previous frame's
 * shadow is left hanging on the desktop where the window used to be.
 * gfx_dirty() clamps to the screen, so a window against an edge is
 * simply dirtied by a smaller margin. */
static void dirty_box(int x, int y, int ww, int hh)
{
    gfx_dirty(x - WIN_SH, y - WIN_SH, ww + 2 * WIN_SH, hh + 2 * WIN_SH);
}

static void dirty_win(const struct win *w) { dirty_box(w->x, w->y, w->w, w->h); }

void win_invalidate(struct win *w)
{
    if (w) dirty_win(w);
}

void win_invalidate_client(struct win *w)
{
    if (!w) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_dirty(x, y, cw, ch);
}

/* ---- z-order ---- */

struct win *wm_nth(int i)
{
    if (i < 0 || i >= zcount) return 0;
    return zorder[i];
}

int wm_count(void)
{
    return zcount;
}

struct win *win_focused(void)
{
    return focused;
}

struct win *win_at(int x, int y)
{
    for (int i = zcount - 1; i >= 0; i--) {
        struct win *w = zorder[i];
        if (w->minimized) continue;
        if (x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h) return w;
    }
    return 0;
}

static void z_remove(struct win *w)
{
    for (int i = 0; i < zcount; i++)
        if (zorder[i] == w) {
            for (int j = i; j + 1 < zcount; j++) zorder[j] = zorder[j + 1];
            zcount--;
            return;
        }
}

static void z_append(struct win *w)
{
    if (zcount < WIN_MAX) zorder[zcount++] = w;
}

void win_raise(struct win *w)
{
    if (!w || !w->used) return;
    z_remove(w);
    z_append(w);
}

void win_focus(struct win *w)
{
    if (!w || !w->used) return;
    w->minimized = 0;
    if (focused && focused->used) win_invalidate(focused);
    win_raise(w);
    focused = w;
    win_invalidate(w);
    dirty_chrome();
}

/* ---- lifecycle ---- */

struct win *win_open(const struct app *a)
{
    struct win *w = 0;
    for (int i = 0; i < WIN_MAX; i++)
        if (!wins[i].used) {
            w = &wins[i];
            break;
        }
    if (!w) return 0;

    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->app = a;
    w->id = ++id_seq;
    strncpy(w->title, a->title, sizeof(w->title) - 1);

    int ww = a->w, hh = a->h;
    if (ww > g_fw - 20) ww = g_fw - 20;
    if (hh > g_fh - TASKBAR_H - 20) hh = g_fh - TASKBAR_H - 20;
    if (ww < MIN_W) ww = MIN_W;
    if (hh < MIN_H) hh = MIN_H;
    w->w = ww;
    w->h = hh;
    w->x = 110 + (cascade % 8) * 30;
    w->y = 56 + (cascade % 8) * 26;
    cascade++;
    clamp_pos(w, &w->x, &w->y);
    w->rx = w->x;
    w->ry = w->y;
    w->rw = w->w;
    w->rh = w->h;

    if (a->open) a->open(w);

    z_append(w);
    if (focused && focused->used) win_invalidate(focused);
    focused = w;
    win_invalidate(w);
    dirty_chrome();
    return w;
}

void win_close(struct win *w)
{
    if (!w || !w->used) return;
    if (w->app->close) w->app->close(w);
    z_remove(w);
    if (drag_win == w) drag = DRAG_NONE;
    if (tbtn && drag_win == w) drag = DRAG_NONE;
    memset(w, 0, sizeof(*w));
    focused = zcount ? zorder[zcount - 1] : 0;
    /* The stack moved, so anything that was showing through a window now
     * has to show something else: one full repaint is cheaper than
     * computing which rectangles actually moved. */
    gfx_dirty_all();
    dirty_chrome();
}

void win_minimize(struct win *w)
{
    if (!w || !w->used) return;
    w->minimized = 1;
    focused = 0;
    for (int i = zcount - 1; i >= 0; i--)
        if (!zorder[i]->minimized) {
            focused = zorder[i];
            break;
        }
    if (focused) win_invalidate(focused);
    dirty_win(w);
    dirty_chrome();
}

void win_maximize(struct win *w)
{
    if (!w || !w->used) return;
    dirty_win(w);
    if (!w->maximized) {
        w->rx = w->x;
        w->ry = w->y;
        w->rw = w->w;
        w->rh = w->h;
        w->x = 0;
        w->y = 0;
        w->w = g_fw;
        w->h = g_fh - TASKBAR_H;
        w->maximized = 1;
    } else {
        w->x = w->rx;
        w->y = w->ry;
        w->w = w->rw;
        w->h = w->rh;
        w->maximized = 0;
        clamp_pos(w, &w->x, &w->y);
    }
    w->minimized = 0;
    dirty_win(w);
    dirty_chrome();
}

void win_set_title(struct win *w, const char *t)
{
    if (!w || !w->used) return;
    if (!strcmp(w->title, t)) return;
    strncpy(w->title, t, sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = 0;
    win_invalidate(w);
}

void win_move(struct win *w, int x, int y)
{
    if (!w || !w->used || w->maximized) return;
    int ox = w->x, oy = w->y;
    clamp_pos(w, &x, &y);
    if (x == ox && y == oy) return;
    w->x = x;
    w->y = y;
    dirty_box(ox, oy, w->w, w->h);
    dirty_box(x, y, w->w, w->h);
}

void win_client_size(struct win *w, int cw, int ch)
{
    if (!w || !w->used) return;
    int ow = w->w, oh = w->h, ox = w->x, oy = w->y;
    int nw = cw + 2 * (FRAME_W + CLIENT_B);
    int nh = ch + 2 * FRAME_W + TITLE_H + 2 * CLIENT_B;
    if (nw < MIN_W) nw = MIN_W;
    if (nh < MIN_H) nh = MIN_H;
    if (nw > g_fw) nw = g_fw;
    if (nh > g_fh - TASKBAR_H) nh = g_fh - TASKBAR_H;
    w->w = nw;
    w->h = nh;
    clamp_pos(w, &w->x, &w->y);
    dirty_box(ox, oy, ow, oh);
    dirty_win(w);
}

/* ---- painting ----
 *
 * A window is a rounded rectangle with a drop shadow, a wash of glass
 * for a caption and three traffic lights on the left: the three things a
 * current desktop is recognised by, laid over the Win98 frame that the
 * rest of the code still measures against.  FRAME_W, TITLE_H and the
 * sunken client border are untouched, so no module had to move a pixel.
 *
 * The corners are the part that needs care.  The compositor paints the
 * whole scene clipped to this frame's damage, and a module's client area
 * is a plain rectangle: fill it and white runs straight past the rounded
 * corner onto the desktop underneath.  So the frame saves every pixel the
 * outline leaves out before anything is painted and puts them back after
 * the module has finished -- about ninety pixels a frame, instead of a
 * second non-rectangular clip for every module to combine with the
 * damage rectangle. */

#define CAP_H    (TITLE_H + 1) /* the glass wash, starting at y+FRAME_W  */
#define LIGHT_X0 10            /* first traffic light, from the left edge */
#define LIGHT_GAP 20           /* pitch of the three                      */
#define LIGHT_TOT 70           /* the caption starts clear of them        */

static u32 light_col(int kind, int on)
{
    if (!on) return COL_LIGHT_OFF;
    if (kind == BTN_CLOSE) return COL_LIGHT_ON_R;
    if (kind == BTN_MIN) return COL_LIGHT_ON_Y;
    return COL_LIGHT_ON_G;
}

static u32 shade(u32 c, int mul) /* mul is a percentage of the colour */
{
    return RGB(((c >> 16) & 255) * mul / 100, ((c >> 8) & 255) * mul / 100,
               (c & 255) * mul / 100);
}

static u32 lift(u32 c, int pct) /* that far toward white */
{
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    r += (255 - r) * pct / 100;
    g += (255 - g) * pct / 100;
    b += (255 - b) * pct / 100;
    return RGB(r, g, b);
}

/* One traffic light: a disc of colour with a darker rim and a highlight
 * across its top, so it reads as a bead of glass rather than a sticker.
 * 144 pixels, and only when the window paints. */
static void draw_light(int kind, int bx, int by, int pressed, int on)
{
    u32 c = light_col(kind, on);
    int d = LIGHT_D, r2 = d * d;
    if (pressed) c = shade(c, 76);
    for (int j = 0; j < d; j++)
        for (int i = 0; i < d; i++) {
            int a = 2 * i + 1 - d, b = 2 * j + 1 - d;
            int v = a * a + b * b;
            u32 p;
            if (v > r2) continue;
            p = c;
            if (v >= r2 - 2 * d) p = shade(c, 66); /* the rim      */
            else if (b < -d / 4) p = lift(c, 42);  /* the bead     */
            gfx_px(bx + i, by + j, p);
        }
}

/* Left to right: close, minimise, maximise -- the order a mac user's
 * hand already knows, which is the whole point of moving them. */
static void title_btn_pos(const struct win *w, int kind, int *bx, int *by)
{
    int i = (kind == BTN_CLOSE) ? 0 : (kind == BTN_MIN) ? 1 : 2;
    *bx = w->x + LIGHT_X0 + i * LIGHT_GAP;
    *by = w->y + FRAME_W + (CAP_H - LIGHT_D) / 2;
}

static int title_btn_at(const struct win *w, int x, int y)
{
    for (int k = BTN_MIN; k <= BTN_CLOSE; k++) {
        int bx, by;
        title_btn_pos(w, k, &bx, &by);
        /* a pixel of slack each side: 12 is a small target on its own */
        if (x >= bx - 1 && x < bx + LIGHT_D + 1 &&
            y >= by - 1 && y < by + LIGHT_D + 1)
            return k;
    }
    return 0;
}

/* The caption, shortened to the room there is for it: centred when it
 * fits, pinned against the lights with an ellipsis when it does not, so
 * a narrow window shows "Display Pr..." instead of type running across
 * the desktop. */
static void draw_title(const struct win *w, int tx, int ty, int avail, u32 fg)
{
    char buf[sizeof(w->title) + 4]; /* the title, plus an ellipsis */
    char first;
    int n, k, dots;

    if (avail < 8) avail = 8;
    strncpy(buf, w->title, sizeof(w->title) - 1);
    buf[sizeof(w->title) - 1] = 0;
    first = buf[0];
    n = (int)xstrlen(buf);
    dots = gfx_strw_bold("...");

    k = n;
    while (k > 0) {
        int wide;
        buf[k] = 0;
        wide = gfx_strw_bold(buf);
        if (k < n) wide += dots;
        if (wide <= avail) break;
        k--;
    }
    if (k < 1) { /* not even one glyph: one glyph anyway */
        buf[0] = first;
        buf[1] = 0;
    } else if (k < n) {
        buf[k] = '.';
        buf[k + 1] = '.';
        buf[k + 2] = '.';
        buf[k + 3] = 0;
    }
    gfx_text_bold(tx + (avail - gfx_strw_bold(buf)) / 2, ty, buf, fg);
}

/* Every pixel inside the window's rectangle but outside its rounded
 * outline: they belong to whatever is underneath, and the client area's
 * plain rectangle is about to paint over them.  Reading the backbuffer
 * directly is right even outside the clip -- those pixels were not
 * touched this frame, so the buffer and the screen still agree. */
static int corners_save(const struct win *w, int r, u32 *buf)
{
    int n = 0;
    if (r <= 0) return 0;
    for (int j = 0; j < w->h; j++) {
        int ins = gfx_row_inset(r, j, w->h);
        for (int i = 0; i < ins && n < 4 * WIN_R * WIN_R; i++)
            buf[n++] = g_fb[(size_t)(w->y + j) * g_fw + (w->x + i)];
        for (int i = 0; i < ins && n < 4 * WIN_R * WIN_R; i++)
            buf[n++] = g_fb[(size_t)(w->y + j) * g_fw + (w->x + w->w - 1 - i)];
    }
    return n;
}

static void corners_load(const struct win *w, int r, const u32 *buf, int n)
{
    int k = 0;
    if (n <= 0) return;
    for (int j = 0; j < w->h && k < n; j++) {
        int ins = gfx_row_inset(r, j, w->h);
        for (int i = 0; i < ins && k < n; i++)
            gfx_px(w->x + i, w->y + j, buf[k++]);
        for (int i = 0; i < ins && k < n; i++)
            gfx_px(w->x + w->w - 1 - i, w->y + j, buf[k++]);
    }
}

static void paint_win(struct win *w)
{
    int x = w->x, y = w->y, ww = w->w, hh = w->h;
    int on = (focused == w);
    int r = w->maximized ? 0 : gfx_round_r(ww, hh, WIN_R);
    u32 corners[4 * WIN_R * WIN_R];
    int nc = 0;

    /* Nothing of this window, and nothing of the shadow it casts, is in
     * this frame's damage.  Every primitive below would clip its own
     * writes away on its own -- but not before walking six shadow rings
     * and a rounded outline once apiece, and with five windows up and a
     * cursor blinking somewhere else on the screen that walk is the
     * frame.  (It is also the honest place to stop: the module's own
     * bookkeeping, "what the last paint put on screen", is only updated
     * by a paint that really drew.) */
    if (!gfx_clip_hit(x - WIN_SH, y - WIN_SH, ww + 2 * WIN_SH, hh + 2 * WIN_SH))
        return;

    /* The shadow lives outside the window, so it goes down first: the
     * corner save has to see it, or restoring a corner would erase part
     * of it and leave a notch in the falloff.  A maximised window has no
     * margin to fall into and gets none. */
    if (r > 0) {
        static const int sh_a[WIN_SH + 1] = {0, 118, 80, 54, 36, 24, 14};
        for (int d = WIN_SH; d >= 1; d--)
            gfx_ring_round(x, y, ww, hh, r, d, d - 1, COL_DKSHADOW, sh_a[d]);
    }

    nc = corners_save(w, r, corners);

    /* The caption first, because what is behind glass is the whole point:
     * right now these pixels hold the desktop and the windows below, and
     * the frame has not touched them yet. */
    gfx_glass(x + 1, y + 1, ww - 2, CAP_H,
              on ? GLASS_TITLE_T : GLASS_TITLE_IT,
              on ? GLASS_TITLE_A : GLASS_TITLE_IA);

    for (int k = BTN_MIN; k <= BTN_CLOSE; k++) {
        int bx, by;
        title_btn_pos(w, k, &bx, &by);
        draw_light(k, bx, by, drag == DRAG_BTN && drag_win == w && tbtn == k, on);
    }
    /* Optically centred, not boxed: the glyph cell carries two rows of
     * leading above the cap height, so the caps have to line up with the
     * middles of the lights rather than the middles of the two boxes. */
    draw_title(w, x + LIGHT_TOT,
               y + FRAME_W + (CAP_H - FONT_CELL_H) / 2 + 1,
               ww - LIGHT_TOT - 6, on ? COL_HILITE : RGB(226, 231, 240));

    /* The frame the module's sheet sits in, painted as seven lines
     * rather than as one rectangle: the white sheet covers the middle
     * two calls later, so filling that first would write a quarter of a
     * million pixels that are about to be written again -- more than the
     * drop shadow costs, on a machine that has to hold ninety frames a
     * second while a window is being dragged.  Every band between the
     * sunken border and the sheet is one of these seven. */
    int fx = x + FRAME_W, fy = y + FRAME_W + TITLE_H;
    int fw = ww - 2 * FRAME_W, fh = hh - 2 * FRAME_W - TITLE_H;

    gfx_vline(x + 1, fy, fh + 1, COL_FACE);
    gfx_vline(x + ww - 2, fy, fh + 1, COL_FACE);
    gfx_hline(x + 1, y + hh - 2, ww - 2, COL_FACE);
    gfx_hline(fx, fy + 1, fw, COL_FACE);
    gfx_hline(fx, fy + fh - 2, fw, COL_FACE);
    gfx_vline(fx + 1, fy + CLIENT_B, fh - 2 * CLIENT_B, COL_FACE);
    gfx_vline(fx + fw - 2, fy + CLIENT_B, fh - 2 * CLIENT_B, COL_FACE);

    /* client: a sunken border, then a white sheet for the module to fill */
    gfx_in(fx, fy, fw, fh);

    int cx, cy, cw, ch;
    win_client_rect(w, &cx, &cy, &cw, &ch);
    gfx_fill(cx, cy, cw, ch, COL_EDITBG);

    if (w->app->paint) {
        int sx, sy, sw, sh;
        gfx_clip_get(&sx, &sy, &sw, &sh);
        /* Intersect, not replace: the module may only touch the pixels
         * this frame is actually repainting.  Replacing the damage
         * rectangle with the client rectangle let a window underneath
         * paint over the window on top -- the lower window's content
         * showed through the upper one's frame -- because the blit then
         * widens those rows to the full screen width and uploads it. */
        gfx_clip_intersect(cx, cy, cw, ch);
        w->app->paint(w);
        gfx_clip(sx, sy, sw, sh);
    }

    /* Last, so nothing the module did can paint over the edge of its own
     * window -- and before the corners go back, so the curve is not cut
     * short by pixels that are not this window's to keep.  r is 0 when
     * the window is maximised, and then this is the plain box edge a
     * window parked against the screen's own edge should have. */
    gfx_box_round(x, y, ww, hh, r, COL_EDGE);
    corners_load(w, r, corners, nc);
}

void wm_paint(void)
{
    desk_bg_paint();
    desk_paint();
    for (int i = 0; i < zcount; i++)
        if (!zorder[i]->minimized) paint_win(zorder[i]);
    taskbar_paint();
}

/* ---- input routing ---- */

void wm_down(int x, int y, int btn, int clicks)
{
    if (y >= g_fh - TASKBAR_H) {
        taskbar_hit(x, y, btn, clicks);
        return;
    }

    struct win *w = win_at(x, y);
    if (!w) {
        desk_hit(x, y, btn, clicks);
        return;
    }
    if (focused != w) win_focus(w);

    if (!w->maximized && x >= w->x + w->w - 18 && y >= w->y + w->h - 18) {
        drag = DRAG_RESIZE;
        drag_win = w;
        drag_sx = x;
        drag_sy = y;
        start_w = w->w;
        start_h = w->h;
        return;
    }
    if (y < w->y + FRAME_W + TITLE_H) {
        int b = title_btn_at(w, x, y);
        if (b) {
            tbtn = b;
            drag = DRAG_BTN;
            drag_win = w;
            win_invalidate(w);
            return;
        }
        /* Win98 zoomed a window when its caption was double-clicked.
         * Handled before a drag is armed so the second click cannot start
         * moving the window by the couple of pixels between the two
         * presses; win_maximize() invalidates both geometries itself. */
        if (btn == LNXRM_MB_LEFT && clicks >= 2) {
            win_maximize(w);
            return;
        }
        if (btn == LNXRM_MB_LEFT) {
            drag = DRAG_MOVE;
            drag_win = w;
            drag_ox = x - w->x;
            drag_oy = y - w->y;
        }
        return;
    }

    int cx, cy, cw, ch;
    win_client_rect(w, &cx, &cy, &cw, &ch);
    if (x < cx || y < cy || x >= cx + cw || y >= cy + ch) return;
    drag = DRAG_CLIENT;
    drag_win = w;
    if (w->app->down) w->app->down(w, x - cx, y - cy, btn, clicks);
    /* Whatever the app just did to its state -- a pushed button, a caret
     * that moved, a row that lit up -- exists only in memory until this
     * window is painted again, and nothing else repaints a window the
     * user is working in.  Every app would otherwise have to remember to
     * invalidate itself, and most of them (Notepad, Files, Paint, Task
     * Manager) forgot: the screen kept showing what was there before the
     * click.  Painting happens later, in present(), so this is a mark,
     * not a draw -- and it is safe even if the callback closed the
     * window, because win_client_rect() on a zeroed slot is empty. */
    win_invalidate_client(w);
}

void wm_move(int x, int y, int dx, int dy)
{
    switch (drag) {
    case DRAG_MOVE:
        if (drag_win) win_move(drag_win, x - drag_ox, y - drag_oy);
        break;
    case DRAG_RESIZE: {
        struct win *w = drag_win;
        if (!w || !w->used) break;
        int ow = w->w, oh = w->h, ox = w->x, oy = w->y;
        int nw = start_w + (x - drag_sx);
        int nh = start_h + (y - drag_sy);
        if (nw < MIN_W) nw = MIN_W;
        if (nh < MIN_H) nh = MIN_H;
        if (nw > g_fw) nw = g_fw;
        if (nh > g_fh - TASKBAR_H) nh = g_fh - TASKBAR_H;
        w->w = nw;
        w->h = nh;
        clamp_pos(w, &w->x, &w->y);
        dirty_box(ox, oy, ow, oh);
        dirty_win(w);
        break;
    }
    case DRAG_CLIENT:
        if (drag_win && drag_win->used && drag_win->app->drag) {
            int cx, cy, cw, ch;
            win_client_rect(drag_win, &cx, &cy, &cw, &ch);
            drag_win->app->drag(drag_win, x - cx, y - cy, dx, dy);
            win_invalidate_client(drag_win);
        }
        break;
    default:
        taskbar_hover(x, y);
        break;
    }
}

void wm_up(int x, int y, int btn)
{
    if (drag == DRAG_BTN) {
        struct win *w = drag_win;
        int kind = tbtn;
        int hit = (w && w->used) ? title_btn_at(w, x, y) : 0;
        drag = DRAG_NONE;
        drag_win = 0;
        tbtn = 0;
        if (!w || !w->used) return;
        win_invalidate(w); /* lifts the pressed look */
        if (hit != kind) return;
        if (kind == BTN_CLOSE) win_close(w);
        else if (kind == BTN_MIN) win_minimize(w);
        else if (kind == BTN_MAX) win_maximize(w);
        return;
    }

    int d = drag;
    struct win *w = drag_win;
    drag = DRAG_NONE;
    drag_win = 0;
    if (d == DRAG_CLIENT && w && w->used && w->app->up) {
        int cx, cy, cw, ch;
        win_client_rect(w, &cx, &cy, &cw, &ch);
        w->app->up(w, x - cx, y - cy, btn);
        win_invalidate_client(w); /* same rule as the press */
    }
}

void wm_key(const struct dsk_event *ev)
{
    if (focused && focused->used && focused->app->key) {
        focused->app->key(focused, ev);
        /* Typing is the same story as clicking: the app changed its own
         * buffer and expects the caret, the text or the list to follow.
         * Without this Notepad painted whatever was on screen when it was
         * last focused by someone else. */
        if (focused && focused->used) win_invalidate_client(focused);
    }
}

void wm_init(void)
{
    memset(wins, 0, sizeof(wins));
    zcount = 0;
    focused = 0;
    cascade = 0;
    gfx_dirty_all();
}
