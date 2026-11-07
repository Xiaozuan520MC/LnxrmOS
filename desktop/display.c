/* Display Properties: the wallpaper dialog, minus the wallpapers.
 *
 * This system has no image decoder and no bitmap files to point one at,
 * so "background" means what Win98's Pattern list offered before it
 * offered images: a colour, a two-tone ramp, or a checker, applied to
 * the whole desktop.  The choice is made here and painted by
 * desk_bg_paint(), which wm_paint() calls first -- the window manager
 * does not know a control panel exists, it just asks what the desktop
 * is made of.
 *
 * Picking a swatch applies immediately (the desktop behind the dialog
 * changes as you click, which is the whole point of a preview), and
 * every change is one gfx_dirty_all(): unlike a window, the background
 * is not a rectangle. */
#include "desktop.h"

#define SW_N 16
#define SW_COLS 8

/* Teal first, because teal is what this desktop boots as. */
static const u32 swatch[SW_N] = {
    RGB(0, 128, 128),   RGB(0, 0, 128),     RGB(0, 64, 128),   RGB(0, 96, 96),
    RGB(75, 0, 130),    RGB(128, 0, 0),     RGB(0, 64, 0),     RGB(128, 128, 0),
    RGB(64, 64, 64),    RGB(128, 128, 128), RGB(140, 60, 160), RGB(96, 160, 200),
    RGB(200, 180, 140), RGB(255, 180, 40),  RGB(255, 255, 255), RGB(0, 0, 0),
};

static u32 bg_a = COL_BG_A;            /* the colour itself          */
static u32 bg_b = COL_BG_B;            /* its partner in 1 and 2     */
static int bg_mode = 1;                /* 0 solid, 1 ramp, 2 checker */
static int bg_sel = -1;                 /* which swatch, or -1        */

/* Half again as dark, so a ramp or a checker is a shade of the colour
 * the user picked rather than an unrelated second choice. */
static u32 darker(u32 c)
{
    int r = (int)((c >> 16) & 255) * 55 / 100;
    int g = (int)((c >> 8) & 255) * 55 / 100;
    int b = (int)(c & 255) * 55 / 100;
    return RGB(r, g, b);
}

/* The background itself, for any rectangle.  The preview in the dialog
 * and the desktop are the same function on purpose: a preview that
 * draws its own idea of the pattern is a preview of nothing.
 *
 * The clip is narrowed to the rectangle first.  gfx_fill() and
 * gfx_hgrad() stay inside the box they are given, but the checker lays
 * down whole 16-pixel tiles, and the last tile of a row or column runs
 * past the edge it belongs to -- on the preview pane that showed up as a
 * square of wallpaper sticking out of the little monitor's bezel. */
static void fill_bg(int x, int y, int w, int h)
{
    int sx, sy, sw, sh;
    gfx_clip_get(&sx, &sy, &sw, &sh);
    gfx_clip_intersect(x, y, w, h);

    if (bg_mode == 1) {
        gfx_hgrad(x, y, w, h, bg_a, bg_b);
    } else {
        gfx_fill(x, y, w, h, bg_a);
        if (bg_mode == 2) {
            /* Anchored to screen coordinates, so moving the dialog does
             * not move the cloth underneath it. */
            int t = 16;
            int yy = y - (y % t);
            int xx0 = x - (x % t);
            for (; yy < y + h; yy += t)
                for (int xx = xx0; xx < x + w; xx += t)
                    if ((((xx / t) + (yy / t)) & 1) != 0) gfx_fill(xx, yy, t, t, bg_b);
        }
    }
    gfx_clip(sx, sy, sw, sh);
}

/* Rebuilt only when the wallpaper changes; see backdrop_build(). */
static int bg_dirty = 1;
static void backdrop_build(void);

void desk_bg_paint(void)
{
    fill_bg(0, 0, g_fw, g_fh);
    if (bg_dirty) {
        bg_dirty = 0;
        backdrop_build();
    }
}

/* ---- the backdrop the frosted glass looks through ----
 * Glass has to sample a pixel the window manager may already have
 * painted a window over, so it cannot read the screen: it reads a copy
 * of the wallpaper at half resolution, kept blurred, which changes only
 * when the wallpaper does -- once at boot, once per click below.
 *
 * bg_sample() mirrors fill_bg() above and the two must agree; this one
 * runs a quarter of a million times per rebuild, so it is written as
 * arithmetic instead of as whole-screen primitives.  The solid fill and
 * the linear ramp have no edges to blur, so only the checker is box
 * blurred -- a ramp blurred against itself is still the ramp. */
static u32 *bd_src, *bd_tmp;
static int bd_w, bd_h;
static int bd_ok; /* 0 not built yet, -1 out of memory, 1 fine */

#define BD_S 1 /* backdrop is kept at half the screen resolution */

static u32 bg_sample(int x, int y)
{
    if (bg_mode == 1) {
        int w = g_fw > 0 ? g_fw : 1;
        int ar = (bg_a >> 16) & 255, ag = (bg_a >> 8) & 255, ab = bg_a & 255;
        int dr = (int)((bg_b >> 16) & 255) - ar;
        int dg = (int)((bg_b >> 8) & 255) - ag;
        int db = (int)(bg_b & 255) - ab;
        return RGB(ar + dr * x / w, ag + dg * x / w, ab + db * x / w);
    }
    if (bg_mode == 2 && ((((x >> 4) + (y >> 4)) & 1)) != 0) return bg_b;
    return bg_a;
}

/* Eight samples along one line, edges repeated: a shift instead of a
 * division, and no pale rim where the checker reaches the screen's
 * border.  `dd`/`ds` are the element strides, so the same routine walks
 * a row and a column. */
static void blur8_line(u32 *dst, int n, int dd, const u32 *src, int ds)
{
    int sr = 0, sg = 0, sb = 0;
    if (n <= 0) return;
#define CL(k) ((k) < 0 ? 0 : ((k) >= n ? n - 1 : (k)))
    for (int k = -3; k <= 4; k++) {
        u32 c = src[(size_t)CL(k) * ds];
        sr += (c >> 16) & 255;
        sg += (c >> 8) & 255;
        sb += c & 255;
    }
    for (int i = 0; i < n; i++) {
        dst[(size_t)i * dd] = RGB(sr >> 3, sg >> 3, sb >> 3);
        u32 out = src[(size_t)CL(i - 3) * ds];
        u32 in = src[(size_t)CL(i + 4) * ds];
        sr += ((in >> 16) & 255) - ((out >> 16) & 255);
        sg += ((in >> 8) & 255) - ((out >> 8) & 255);
        sb += (in & 255) - (out & 255);
    }
#undef CL
}

static void backdrop_build(void)
{
    int w = (g_fw + 1) >> BD_S, h = (g_fh + 1) >> BD_S;
    if (w < 4 || h < 4) return;
    if (bd_ok == 0) {
        /* One buffer, not two: only the checker has edges worth blurring,
         * so the second one is asked for if and when the checker is
         * picked -- which keeps the default wallpaper's footprint to a
         * single 900 KiB buffer and leaves the glass working on a tint
         * over what is already behind it if even that cannot be had. */
        bd_src = (u32 *)xmalloc((size_t)w * h * 4);
        if (!bd_src) {
            bd_ok = -1;
            return;
        }
        bd_w = w;
        bd_h = h;
        bd_ok = 1;
    }
    if (bd_ok < 0) return;

    for (int j = 0; j < bd_h; j++)
        for (int i = 0; i < bd_w; i++)
            bd_src[(size_t)j * bd_w + i] = bg_sample(i << BD_S, j << BD_S);

    if (bg_mode == 2) { /* the only pattern that has edges to soften */
        if (!bd_tmp)
            bd_tmp = (u32 *)xmalloc((size_t)bd_w * bd_h * 4);
        if (bd_tmp) {
            for (int j = 0; j < bd_h; j++)
                blur8_line(bd_tmp + (size_t)j * bd_w, bd_w, 1,
                           bd_src + (size_t)j * bd_w, 1);
            for (int i = 0; i < bd_w; i++)
                blur8_line(bd_src + i, bd_h, bd_w, bd_tmp + i, bd_w);
        } else {
            /* Handing the unblurred checker over would show a chessboard
             * through frosted glass; handing nothing over makes the panel
             * tint what is behind it instead, which is only missing the
             * frost, not the glass. */
            gfx_backdrop(0, 0, 0);
            return;
        }
    }
    gfx_backdrop(bd_src, bd_w, bd_h);
}

void desk_bg_reset(void)
{
    bg_a = COL_BG_A;
    bg_b = COL_BG_B;
    bg_mode = 1;
    bg_sel = -1;   /* the default ramp is not one of the swatches */
}

/* ---- layout, in client coordinates ----
 * Paint adds the client origin; the mouse handler does not have to
 * think about screen coordinates at all, which is why the two can share
 * these. */
static void preview_rect(int *rx, int *ry, int *rw, int *rh)
{
    *rx = 10;
    *ry = 30;
    *rw = 132;
    *rh = 84;
}

static void mode_rect(int i, int *rx, int *ry, int *rw, int *rh)
{
    *rx = 152;
    *ry = 30 + i * 24;
    *rw = 170;
    *rh = 22;
}

static void swatch_rect(int i, int *rx, int *ry, int *rw, int *rh)
{
    *rx = 10 + (i % SW_COLS) * 39;
    *ry = 126 + (i / SW_COLS) * 24;
    *rw = 34;
    *rh = 18;
}

static void btn_rect(int which, int cw, int ch, int *rx, int *ry, int *rw, int *rh)
{
    *rw = 112;
    *rh = 26;
    *ry = ch - 34;
    *rx = which == 0 ? 10 : cw - 122;
}

static int inside(int px, int py, int rx, int ry, int rw, int rh)
{
    return px >= rx && py >= ry && px < rx + rw && py < ry + rh;
}

static void button(int x, int y, int rx, int ry, int rw, int rh, const char *label)
{
    gfx_glass_button(x + rx, y + ry, rw, rh, 0, label);
}

static void dp_paint(struct win *w)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    gfx_hgrad(x + 6, y + 6, cw - 12, 18, COL_TITLE0, COL_TITLE1);
    gfx_text(x + 9, y + 7, "Background", COL_HILITE);
    icon_paint(ICO_DISPLAY, x + cw - 24, y + 7, 1);

    /* the preview: a monitor-sized pane showing the real background,
     * raised bezel and all */
    int rx, ry, rw, rh;
    preview_rect(&rx, &ry, &rw, &rh);
    fill_bg(x + rx + 3, y + ry + 3, rw - 6, rh - 6);
    gfx_out(x + rx, y + ry, rw, rh);

    static const char *modes[3] = {"Solid colour", "Colour ramp", "Checker"};
    for (int i = 0; i < 3; i++) {
        mode_rect(i, &rx, &ry, &rw, &rh);
        gfx_glass_button(x + rx, y + ry, rw, rh, i == bg_mode, modes[i]);
    }

    for (int i = 0; i < SW_N; i++) {
        swatch_rect(i, &rx, &ry, &rw, &rh);
        gfx_fill(x + rx, y + ry, rw, rh, swatch[i]);
        gfx_out(x + rx, y + ry, rw, rh);
        if (i == bg_sel) gfx_box(x + rx - 3, y + ry - 3, rw + 6, rh + 6, COL_DKSHADOW);
    }

    int bx, by, bw, bh;
    btn_rect(0, cw, ch, &bx, &by, &bw, &bh);
    button(x, y, bx, by, bw, bh, "Use default");
    btn_rect(1, cw, ch, &bx, &by, &bw, &bh);
    button(x, y, bx, by, bw, bh, "Close");
}

static void apply(void)
{
    /* The background is not a rectangle, so there is no cheaper honest
     * answer than "everything changed" -- and the glass reads its own
     * copy of the wallpaper, which has to be rebuilt to match. */
    bg_dirty = 1;
    gfx_dirty_all();
}

static void dp_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    (void)clicks;
    if (btn != LNXRM_MB_LEFT) return;
    int rx, ry, rw, rh;

    for (int i = 0; i < 3; i++) {
        mode_rect(i, &rx, &ry, &rw, &rh);
        if (inside(cx, cy, rx, ry, rw, rh)) {
            bg_mode = i;
            apply();
            return;
        }
    }
    for (int i = 0; i < SW_N; i++) {
        swatch_rect(i, &rx, &ry, &rw, &rh);
        if (inside(cx, cy, rx, ry, rw, rh)) {
            bg_sel = i;
            bg_a = swatch[i];
            bg_b = darker(swatch[i]);
            apply();
            return;
        }
    }
    int cw, ch, ox, oy;
    win_client_rect(w, &ox, &oy, &cw, &ch);
    btn_rect(0, cw, ch, &rx, &ry, &rw, &rh);
    if (inside(cx, cy, rx, ry, rw, rh)) {
        desk_bg_reset();
        apply();
        return;
    }
    btn_rect(1, cw, ch, &rx, &ry, &rw, &rh);
    if (inside(cx, cy, rx, ry, rw, rh)) win_close(w);
}

static void dp_key(struct win *w, const struct dsk_event *ev)
{
    if (ev->type != LNXRM_EV_KEY_DOWN) return;
    if (ev->code == LNXRM_KEY_ESC) win_close(w);
}

const struct app app_display = {
    .name = "Display",
    .title = "Display Properties",
    .w = 340,
    .h = 260,
    .open = 0,
    .paint = dp_paint,
    .down = dp_down,
    .drag = 0,
    .up = 0,
    .key = dp_key,
    .close = 0,
    .tick = 0,
};
