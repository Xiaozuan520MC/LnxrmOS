/* Mouse Properties: the pointer's speed, size and colours, edited live.
 *
 * A setting made here takes effect the moment the button comes up --
 * the pointer is the one thing in this desktop that is visible while
 * you are still dragging the mouse, so a preview pane would be a lie:
 * the preview IS the pointer.  The changes are the desktop's own
 * (cursor.c holds the state, the loop applies the gain), which is why
 * there is nothing to apply or cancel; every click is already done.
 *
 * The speed is a slider, because a pointer's speed is a feel and a
 * feel is dragged, not stepped: grab the thumb, sweep until the
 * pointer answers the way your hand does, let go.  The layout is
 * Display Properties' -- a raised panel, one row per setting -- because
 * two control panels that disagree about what a button is are two
 * systems wearing one name. */
#include "desktop.h"

#define ROW_H 26

/* speed: 50%..400% in 25% steps; the slider walks the same stops */
static const int speeds[] = {50, 75, 100, 125, 150, 200, 250, 300, 350, 400};
#define SPEED_N (int)(sizeof(speeds) / sizeof(speeds[0]))

/* fill colours the pointer can wear; the outline follows as near-black
 * unless the fill is dark, in which case the outline goes white -- a
 * pointer you cannot see against the wallpaper is a pointer you have
 * lost */
static const u32 fill_cols[] = {
    RGB(255, 255, 255), RGB(24, 24, 28),   RGB(255, 200, 40),
    RGB(96, 160, 255),  RGB(90, 200, 90),  RGB(255, 120, 160),
};
#define COL_N (int)(sizeof(fill_cols) / sizeof(fill_cols[0]))

static int drag_thumb; /* the speed slider is being dragged */

static int speed_stop(int pct)
{
    int best = 0;
    for (int i = 0; i < SPEED_N; i++)
        if (speeds[i] <= pct) best = i;
    return best;
}

/* ---- layout, in client coordinates ---- */

#define SLIDER_X0 78
#define THUMB_W 12

static void slider_geom(int cw, int *gx0, int *gx1, int *gy)
{
    *gx0 = SLIDER_X0;
    *gx1 = cw - 66;
    *gy = 34 + ROW_H / 2; /* the groove's centre line */
}

/* the thumb's left edge for a speed stop */
static int thumb_x(int gx0, int gx1, int stop)
{
    int travel = gx1 - gx0 - THUMB_W;
    if (travel < 0) travel = 0;
    return gx0 + stop * travel / (SPEED_N - 1);
}

static void speed_box(int cw, int *rx, int *ry, int *rw, int *rh)
{
    *rx = cw - 58;
    *ry = 34;
    *rw = 46;
    *rh = ROW_H;
}

static void size_rect(int i, int *rx, int *ry, int *rw, int *rh)
{
    *rx = 78 + i * 62;
    *ry = 66;
    *rw = 58;
    *rh = ROW_H;
}

static void col_rect(int i, int *rx, int *ry, int *rw, int *rh)
{
    *rx = 78 + i * 30;
    *ry = 98;
    *rw = 26;
    *rh = ROW_H - 4;
}

static int inside(int px, int py, int rx, int ry, int rw, int rh)
{
    return px >= rx && py >= ry && px < rx + rw && py < ry + rh;
}

static void button(int x, int y, int rx, int ry, int rw, int rh, const char *label,
                   int pushed)
{
    gfx_glass_button(x + rx, y + ry, rw, rh, pushed, label);
}

/* speed <- slider x, snapped to the nearest stop */
static void speed_from_x(int cx, int gx0, int gx1)
{
    int travel = gx1 - gx0 - THUMB_W;
    if (travel <= 0) return;
    int p = (cx - gx0 - THUMB_W / 2) * (SPEED_N - 1) / travel;
    if (p < 0) p = 0;
    if (p > SPEED_N - 1) p = SPEED_N - 1;
    mouse_set_speed(speeds[p]);
}

/* The arrow, drawn as cursor_paint draws it -- same sprite, same scale,
 * same colours -- so the pane shows exactly what the pointer has become. */
static void draw_arrow(int x, int y, int scale, u32 fill, u32 outline)
{
    static const unsigned char arrow[CURSOR_H][CURSOR_W] = {
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0},
        {1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0},
        {1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 0},
        {1, 2, 2, 1, 2, 2, 1, 0, 0, 0, 0, 0},
        {1, 2, 1, 0, 1, 2, 2, 1, 0, 0, 0, 0},
        {1, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0},
        {1, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0},
        {0, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0},
        {0, 0, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0},
    };
    for (int r = 0; r < CURSOR_H; r++)
        for (int c = 0; c < CURSOR_W; c++) {
            unsigned char v = arrow[r][c];
            if (!v) continue;
            u32 col = v == 1 ? outline : fill;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    gfx_px(x + c * scale + dx, y + r * scale + dy, col);
        }
}

static void mp_paint(struct win *w)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    gfx_hgrad(x + 6, y + 6, cw - 12, 18, COL_TITLE0, COL_TITLE1);
    gfx_text(x + 9, y + 7, "Pointer", COL_HILITE);
    icon_paint(ICO_MOUSE, x + cw - 24, y + 7, 1);

    int gx0, gx1, gy;
    slider_geom(cw, &gx0, &gx1, &gy);

    /* speed: the groove, the stops' ticks, the thumb, the readout */
    gfx_text(x + 14, y + 40, "Speed:", COL_TEXT);
    gfx_hline(x + gx0, y + gy, gx1 - gx0, COL_SHADOW);
    gfx_hline(x + gx0, y + gy + 1, gx1 - gx0, COL_HILITE);
    for (int i = 0; i < SPEED_N; i++) {
        int tx = thumb_x(gx0, gx1, i) + THUMB_W / 2;
        gfx_vline(x + tx, y + gy - 3, 3, COL_SHADOW);
    }
    int stop = speed_stop(mouse_speed());
    int tx = thumb_x(gx0, gx1, stop);
    gfx_glass_button(x + tx, y + gy - ROW_H / 2, THUMB_W, ROW_H, drag_thumb, 0);
    for (int k = 2; k < ROW_H - 2; k += 2)
        gfx_hline(x + tx + 3, y + gy - ROW_H / 2 + k, THUMB_W - 6, COL_SHADOW);

    int rx, ry, rw, rh;
    speed_box(cw, &rx, &ry, &rw, &rh);
    gfx_fill(x + rx, y + ry, rw, rh, COL_EDITBG);
    gfx_in(x + rx, y + ry, rw, rh);
    char v[8];
    v[0] = 0;
    str_cat_i(v, mouse_speed());
    str_cat(v, "%");
    gfx_text(x + rx + (rw - gfx_strw(v)) / 2, y + ry + (rh - 16) / 2, v, COL_TEXT);

    /* size */
    gfx_text(x + 14, y + 72, "Size:", COL_TEXT);
    static const char *const sizes[3] = {"Small", "Medium", "Large"};
    for (int i = 0; i < 3; i++) {
        size_rect(i, &rx, &ry, &rw, &rh);
        button(x, y, rx, ry, rw, rh, sizes[i], mouse_scale() == i + 1);
    }

    /* colours */
    gfx_text(x + 14, y + 104, "Color:", COL_TEXT);
    u32 fill, outline;
    mouse_colors(&fill, &outline);
    for (int i = 0; i < COL_N; i++) {
        col_rect(i, &rx, &ry, &rw, &rh);
        gfx_fill(x + rx, y + ry, rw, rh, fill_cols[i]);
        gfx_out(x + rx, y + ry, rw, rh);
        if (fill_cols[i] == fill) gfx_box(x + rx - 3, y + ry - 3, rw + 6, rh + 6, COL_DKSHADOW);
    }

    /* the pane the arrow lives in: a well, because the pointer is drawn
     * against whatever it happens to be over, and here it is over white */
    gfx_fill(x + 14, y + 132, cw - 28, ch - 168, COL_EDITBG);
    gfx_in(x + 14, y + 132, cw - 28, ch - 168);
    int sc = mouse_scale();
    draw_arrow(x + 30, y + 144, sc * 2, fill, outline);
}

static void mp_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    (void)clicks;
    if (btn != LNXRM_MB_LEFT) return;
    int rx, ry, rw, rh;

    /* the speed slider: grab anywhere in its band */
    int cw, ch, ox, oy, gx0, gx1, gy;
    win_client_rect(w, &ox, &oy, &cw, &ch);
    (void)ox;
    (void)oy;
    (void)ch;
    slider_geom(cw, &gx0, &gx1, &gy);
    if (cy >= 34 - 4 && cy < 34 + ROW_H + 4 && cx >= gx0 - THUMB_W / 2 && cx < gx1 + THUMB_W / 2) {
        drag_thumb = 1;
        speed_from_x(cx, gx0, gx1);
        dsk_cursor_dirty();
        win_invalidate_client(w);
        return;
    }

    for (int i = 0; i < 3; i++) {
        size_rect(i, &rx, &ry, &rw, &rh);
        if (inside(cx, cy, rx, ry, rw, rh)) {
            mouse_set_scale(i + 1);
            dsk_cursor_dirty();
            win_invalidate_client(w);
            return;
        }
    }

    u32 fill, outline;
    mouse_colors(&fill, &outline);
    for (int i = 0; i < COL_N; i++) {
        col_rect(i, &rx, &ry, &rw, &rh);
        if (inside(cx, cy, rx, ry, rw, rh)) {
            fill = fill_cols[i];
            /* a dark fill wants a light outline, or the pointer melts
             * into dark wallpaper; the palette pairs them */
            outline = (fill_cols[i] == RGB(24, 24, 28)) ? RGB(255, 255, 255)
                                                        : RGB(24, 24, 28);
            mouse_set_color(fill, outline);
            dsk_cursor_dirty();
            win_invalidate_client(w);
            return;
        }
    }
}

static void mp_drag(struct win *w, int cx, int cy, int dx, int dy)
{
    (void)cy;
    (void)dx;
    (void)dy;
    if (!drag_thumb) return;
    int cw, ch, ox, oy, gx0, gx1, gy;
    win_client_rect(w, &ox, &oy, &cw, &ch);
    (void)ox;
    (void)oy;
    (void)ch;
    slider_geom(cw, &gx0, &gx1, &gy);
    speed_from_x(cx, gx0, gx1);
    win_invalidate_client(w);
}

static void mp_up(struct win *w, int cx, int cy, int btn)
{
    (void)cx;
    (void)cy;
    (void)btn;
    drag_thumb = 0;
}

static void mp_key(struct win *w, const struct dsk_event *ev)
{
    if (ev->type != LNXRM_EV_KEY_DOWN) return;
    if (ev->code == LNXRM_KEY_ESC) win_close(w);
}

const struct app app_mouse = {
    .name = "Mouse",
    .title = "Mouse Properties",
    .w = 340,
    .h = 300,
    .open = 0,
    .paint = mp_paint,
    .down = mp_down,
    .drag = mp_drag,
    .up = mp_up,
    .key = mp_key,
    .close = 0,
    .tick = 0,
};
