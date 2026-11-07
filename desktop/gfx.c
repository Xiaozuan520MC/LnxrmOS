/* Drawing surface: a private full-screen backbuffer, clipped drawing, and
 * the dirty-rectangle bookkeeping that decides what gets handed to
 * fb_blit().  Nothing here touches the hardware -- the compositor in
 * loop.c decides which rectangles changed and pushes them out.
 *
 * The clip rect is what makes a frame cost what the damage costs: the
 * scene is a complete description of the screen, so the compositor paints
 * it once clipped to the union of this frame's damage and then blits only
 * the individual damaged rectangles.  Painting is idempotent by
 * construction, which is why no module ever has to compute "what was
 * underneath". */
#include "desktop.h"

u32 *g_fb;
int g_fw, g_fh;

static int cl = 0, ct = 0, cr = 0, cb = 0; /* clip rect: [cl,cr) x [ct,cb) */

void gfx_init(void)
{
    struct lnxrm_fb_info fi;
    memset(&fi, 0, sizeof(fi));
    if (kfb_info(&fi) < 0 || !fi.width || !fi.height || fi.bpp != 32) {
        xputs("desktop: no 32bpp framebuffer\n");
        kexit(1);
    }
    g_fw = (int)fi.width;
    g_fh = (int)fi.height;
    /* Packed, not pitched: fb_blit wants h rows of exactly w pixels, and
     * a backbuffer we own keeps every module free of stride arithmetic. */
    g_fb = (u32 *)xmalloc((size_t)g_fw * (size_t)g_fh * 4);
    if (!g_fb) {
        xputs("desktop: out of memory for the backbuffer\n");
        kexit(1);
    }
    gfx_clip_reset();
    gfx_dirty_all();
}

void gfx_clip(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_fw) x1 = g_fw;
    if (y1 > g_fh) y1 = g_fh;
    if (x1 < x) x1 = x;
    if (y1 < y) y1 = y;
    cl = x;
    ct = y;
    cr = x1;
    cb = y1;
}

/* Narrow the clip to the intersection of what is already clipped and the
 * rectangle given.  gfx_clip() *replaces*, which is what the compositor
 * wants when it sets a frame's damage rectangle; a window's client paint
 * wants the other thing -- to draw only inside the damage rectangle it is
 * being repainted for, never outside it.  Handing a module the client
 * rectangle on its own lets it repaint pixels the frame does not cover,
 * and since the blit widens every damage rectangle to whole rows, that
 * stray paint (a window underneath writing over the one on top) is then
 * uploaded straight to the screen. */
void gfx_clip_intersect(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < cl) x = cl;
    if (y < ct) y = ct;
    if (x1 > cr) x1 = cr;
    if (y1 > cb) y1 = cb;
    if (x1 < x) x1 = x;
    if (y1 < y) y1 = y;
    cl = x;
    ct = y;
    cr = x1;
    cb = y1;
}

void gfx_clip_reset(void)
{
    cl = ct = 0;
    cr = g_fw;
    cb = g_fh;
}

void gfx_clip_get(int *x, int *y, int *w, int *h)
{
    if (x) *x = cl;
    if (y) *y = ct;
    if (w) *w = cr - cl;
    if (h) *h = cb - ct;
}

static inline int clipped(int x, int y)
{ return x >= cl && x < cr && y >= ct && y < cb; }

void gfx_px(int x, int y, u32 c)
{
    if (!clipped(x, y)) return;
    g_fb[(size_t)y * g_fw + x] = c;
}

/* Source over: `src` at coverage `a` (0..255) on `dst`.  Text is
 * antialiased now, so this is the arithmetic behind every edge pixel of
 * every label; it is written for the hot path -- no division, and the
 * caller short-circuits a == 255 to a plain store, which is most of a
 * stem.  Two multiplications per channel, then x/255 as x + x/256. */
u32 gfx_over(u32 dst, u32 src, int a)
{
    u32 inv = 255 - (u32)a;
    u32 r = ((src >> 16 & 255) * (u32)a + (dst >> 16 & 255) * inv);
    u32 g = ((src >> 8 & 255) * (u32)a + (dst >> 8 & 255) * inv);
    u32 b = ((src & 255) * (u32)a + (dst & 255) * inv);
    r = (r + (r >> 8) + 128) >> 8;
    g = (g + (g >> 8) + 128) >> 8;
    b = (b + (b >> 8) + 128) >> 8;
    return (r << 16) | (g << 8) | b;
}

void gfx_px_a(int x, int y, u32 c, int a)
{
    if (a <= 0) return;
    if (a >= 255) {
        gfx_px(x, y, c);
        return;
    }
    if (!clipped(x, y)) return;
    u32 *p = &g_fb[(size_t)y * g_fw + x];
    *p = gfx_over(*p, c, a);
}

void gfx_hline(int x, int y, int w, u32 c)
{
    if (y < ct || y >= cb) return;
    if (x < cl) {
        w -= cl - x;
        x = cl;
    }
    if (x + w > cr) w = cr - x;
    if (w <= 0) return;
    u32 *p = &g_fb[(size_t)y * g_fw + x];
    for (int i = 0; i < w; i++) p[i] = c;
}

void gfx_vline(int x, int y, int h, u32 c)
{
    if (x < cl || x >= cr) return;
    if (y < ct) {
        h -= ct - y;
        y = ct;
    }
    if (y + h > cb) h = cb - y;
    if (h <= 0) return;
    for (int i = 0; i < h; i++) g_fb[(size_t)(y + i) * g_fw + x] = c;
}

void gfx_fill(int x, int y, int w, int h, u32 c)
{
    int x1 = x + w, y1 = y + h;
    if (x < cl) x = cl;
    if (y < ct) y = ct;
    if (x1 > cr) x1 = cr;
    if (y1 > cb) y1 = cb;
    if (x1 <= x || y1 <= y) return;
    for (int j = y; j < y1; j++) {
        u32 *p = &g_fb[(size_t)j * g_fw + x];
        for (int i = x; i < x1; i++) *p++ = c;
    }
}

/* The same, over a rectangle -- the block form the scaled logo and the
 * drop shadows need. */
void gfx_fill_a(int x, int y, int w, int h, u32 c, int a)
{
    if (a <= 0) return;
    if (a >= 255) {
        gfx_fill(x, y, w, h, c);
        return;
    }
    int x1 = x + w, y1 = y + h;
    if (x < cl) x = cl;
    if (y < ct) y = ct;
    if (x1 > cr) x1 = cr;
    if (y1 > cb) y1 = cb;
    if (x1 <= x || y1 <= y) return;
    for (int j = y; j < y1; j++) {
        u32 *p = &g_fb[(size_t)j * g_fw + x];
        for (int i = x; i < x1; i++) {
            u32 d = *p;
            *p++ = gfx_over(d, c, a);
        }
    }
}

/* ---- rounded rectangles ----
 * The corners are the cheapest part of a modern window and the only part
 * that has to respect the compositor's rule: a pixel a rounded corner
 * leaves out belongs to whatever is underneath, so a fill skips it and
 * paint_win() saves it and puts it back after the module has painted the
 * client.  Every shape here derives its row inset from the same circle,
 * so the fill and that save/restore cannot disagree by a pixel. */
static int round_inset(int r, int dy)
{
    /* The first column of row `dy` whose centre lies inside the circle
     * of radius r centred r in and r down. */
    int b = 2 * dy + 1 - 2 * r;
    for (int i = 0; i <= r; i++) {
        int a = 2 * i + 1 - 2 * r;
        if (a * a + b * b <= 4 * r * r) return i;
    }
    return r;
}

/* How far in the rounded edge cuts on row `j` of a `rows` tall shape.
 * Callers clamp the radius with gfx_round_r() first so that every user
 * of the same rectangle agrees. */
int gfx_row_inset(int r, int j, int rows)
{
    if (r <= 0 || j < 0 || j >= rows) return 0;
    int ins = 0;
    if (j < r) ins = round_inset(r, j);
    if (j >= rows - r) {
        int k = round_inset(r, rows - 1 - j);
        if (k > ins) ins = k;
    }
    return ins;
}

int gfx_round_r(int w, int h, int r)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    return r < 0 ? 0 : r;
}

/* Does any of this rectangle fall inside the current clip?  A shape built
 * from many small pieces -- a drop shadow's rings, a rounded outline, a
 * row of traffic lights -- would otherwise run every one of them through
 * the clip just to throw the result away, and with several windows up
 * that walk is the frame whenever a single cursor blinks anywhere on the
 * screen.  Everything is drawn inside the clip by construction, so a
 * whole shape may be skipped when nothing of it is being repainted. */
int gfx_clip_hit(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < cl) x = cl;
    if (y < ct) y = ct;
    if (x1 > cr) x1 = cr;
    if (y1 > cb) y1 = cb;
    return x1 > x && y1 > y;
}

void gfx_fill_round(int x, int y, int w, int h, int r, u32 c)
{
    if (w <= 0 || h <= 0) return;
    r = gfx_round_r(w, h, r);
    if (r <= 0) {
        gfx_fill(x, y, w, h, c);
        return;
    }
    for (int j = 0; j < h; j++) {
        int ins = gfx_row_inset(r, j, h);
        gfx_hline(x + ins, y + j, w - 2 * ins, c);
    }
}

/* The hairline that makes the shape: the first and last rows are spans,
 * the rows between are the two pixels the circle puts at that height.
 * Same inset as gfx_fill_round(), so an outline and the fill it encloses
 * cannot disagree by a pixel. */
void gfx_box_round(int x, int y, int w, int h, int r, u32 c)
{
    if (w <= 0 || h <= 0) return;
    r = gfx_round_r(w, h, r);
    if (r <= 0) {
        gfx_box(x, y, w, h, c);
        return;
    }
    for (int j = 0; j < h; j++) {
        int ins = gfx_row_inset(r, j, h);
        if (j == 0 || j == h - 1)
            gfx_hline(x + ins, y + j, w - 2 * ins, c);
        else {
            gfx_px(x + ins, y + j, c);
            gfx_px(x + w - 1 - ins, y + j, c);
        }
    }
}

/* The band between the rectangle grown by `od` and the same rectangle
 * grown by `id` -- a drop shadow, one concentric ring at a time.  Rings
 * are how a shadow stays affordable: painting six solid copies of the
 * window to darken their edges would cost six windows' worth of fill on
 * every frame the window moves. */
void gfx_ring_round(int x, int y, int w, int h, int r, int od, int id,
                    u32 c, int a)
{
    if (od <= id || a <= 0) return;
    int ro = gfx_round_r(w + 2 * od, h + 2 * od, r + od);
    int ri = gfx_round_r(w + 2 * id, h + 2 * id, r + id);
    for (int j = -od; j < h + od; j++) {
        int ol = gfx_row_inset(ro, j + od, h + 2 * od);
        int ox0 = x - od + ol, ox1 = x + w + od - 1 - ol;
        if (ox1 < ox0) continue;
        if (j < -id || j >= h + id) {          /* clear of the window  */
            gfx_fill_a(ox0, y + j, ox1 - ox0 + 1, 1, c, a);
            continue;
        }
        int il = gfx_row_inset(ri, j + id, h + 2 * id);
        int ix0 = x - id + il, ix1 = x + w + id - 1 - il;
        if (ox0 < ix0) gfx_fill_a(ox0, y + j, ix0 - ox0, 1, c, a);
        if (ox1 > ix1) gfx_fill_a(ix1 + 1, y + j, ox1 - ix1, 1, c, a);
    }
}

/* ---- frosted glass ----
 * A half-resolution copy of the wallpaper, box-blurred, is kept by the
 * module that owns the wallpaper (display.c) and handed over with
 * gfx_backdrop().  It is read one pixel per texel rather than
 * interpolated: the copy is blurred already, so there is nothing finer
 * than a texel left to recover, and the caption of a moving window is
 * the last place to spend four reads a pixel.
 *
 * What is left over after the tint is a quarter of what the compositor
 * has already painted underneath and three quarters of that blurred
 * backdrop.  The backdrop term is what makes it frost; the small sharp
 * term is what stops a window behind the glass from reappearing as a
 * hard rectangle while still letting it tint the panel. */
static const u32 *bgl;
static int bgl_w, bgl_h;

void gfx_backdrop(const u32 *px, int w, int h)
{
    bgl = px;
    bgl_w = w;
    bgl_h = h;
}

void gfx_glass(int x, int y, int w, int h, u32 tint, int a)
{
    if (w <= 0 || h <= 0) return;
    if (!bgl || bgl_w <= 0 || bgl_h <= 0) {
        /* No backdrop to read yet: the panel is still glass, only over a
         * sharper world.  The blur is what is missing, not the
         * translucency, and a flat tint here would turn the taskbar from
         * a wash over the wallpaper into a stripe of paint on it. */
        gfx_fill_a(x, y, w, h, tint, a > 255 ? 255 : a);
        return;
    }
    if (a < 0) a = 0;
    if (a > 256) a = 256;
    int rest = 256 - a;
    int k1 = rest / 4, k2 = rest - k1;
    int tr = (tint >> 16) & 255, tg = (tint >> 8) & 255, tb = tint & 255;
    int x1 = x + w, y1 = y + h;
    if (x < cl) x = cl;
    if (y < ct) y = ct;
    if (x1 > cr) x1 = cr;
    if (y1 > cb) y1 = cb;
    if (x1 <= x || y1 <= y) return;

    for (int j = y; j < y1; j++) {
        int by = j >> 1;
        const u32 *row;
        u32 *out;
        if (by >= bgl_h) by = bgl_h - 1;
        row = bgl + (size_t)by * bgl_w;
        out = &g_fb[(size_t)j * g_fw + x];
        for (int i = x; i < x1; i++, out++) {
            int bx = i >> 1;
            u32 bl, d;
            int r, g, b;
            if (bx >= bgl_w) bx = bgl_w - 1;
            bl = row[bx];
            d = *out;
            r = (((d >> 16) & 255) * k1 + ((bl >> 16) & 255) * k2 + tr * a + 128) >> 8;
            g = (((d >> 8) & 255) * k1 + ((bl >> 8) & 255) * k2 + tg * a + 128) >> 8;
            b = ((d & 255) * k1 + (bl & 255) * k2 + tb * a + 128) >> 8;
            *out = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }
}

/* ---- the glass button ----
 * The one shape every command button in the desktop draws through: the
 * caption's frosted material with a hairline capsule edge.  `pushed`
 * darkens the tint and turns the edge the selection blue, the way the
 * taskbar's buttons already read; the label centres itself and sits a
 * pixel lower while pushed. */
void gfx_glass_button(int x, int y, int w, int h, int pushed, const char *label)
{
    if (w <= 0 || h <= 0) return;
    gfx_glass(x, y, w, h, pushed ? GLASS_BTN_ON_T : GLASS_BTN_T,
              pushed ? GLASS_BTN_ON_A : GLASS_BTN_A);
    gfx_box_round(x, y, w, h, 5, pushed ? COL_SEL : COL_EDGE);
    int off = pushed ? 1 : 0;
    if (label)
        gfx_text_w(x + (w - gfx_strw(label)) / 2 + off,
                   y + (h - FONT_CELL_H) / 2 + off, label, COL_TEXT, w - 8);
}

void gfx_box(int x, int y, int w, int h, u32 c)
{
    if (w <= 0 || h <= 0) return;
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

void gfx_line(int x0, int y0, int x1, int y1, u32 c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Raised: light on the top/left, dark on the bottom/right.  Everything a
 * Win98 window, button and status bar is made of. */
void gfx_out(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    gfx_hline(x, y, w, COL_HILITE);
    gfx_vline(x, y, h, COL_HILITE);
    gfx_hline(x, y + h - 1, w, COL_SHADOW);
    gfx_vline(x + w - 1, y, h, COL_SHADOW);
}

/* Sunken: the same frame, mirrored. */
void gfx_in(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    gfx_hline(x, y, w, COL_SHADOW);
    gfx_vline(x, y, h, COL_SHADOW);
    gfx_hline(x, y + h - 1, w, COL_HILITE);
    gfx_vline(x + w - 1, y, h, COL_HILITE);
}

/* Raised, two pixels deep: white on the very edge, light grey just inside
 * it, and the mirror on the far side.  A single hairline reads as a
 * drawn line; the second band is what makes a rectangle look like an
 * object with thickness.  Only the window frame uses this -- buttons are
 * too small for two bands to mean anything but "thick". */
void gfx_out2(int x, int y, int w, int h)
{
    if (w <= 2 || h <= 2) {
        gfx_out(x, y, w, h);
        return;
    }
    gfx_out(x, y, w, h);
    gfx_hline(x + 1, y + 1, w - 2, RGB(224, 224, 224));
    gfx_vline(x + 1, y + 1, h - 2, RGB(224, 224, 224));
    gfx_hline(x + 1, y + h - 2, w - 2, COL_SHADOW);
    gfx_vline(x + w - 2, y + 1, h - 2, COL_SHADOW);
}

void gfx_hgrad(int x, int y, int w, int h, u32 a, u32 b)
{
    if (w <= 0 || h <= 0) return;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (int)((b >> 16) & 255) - ar;
    int bg = (int)((b >> 8) & 255) - ag;
    int bb = (int)(b & 255) - ab;
    int ox = x; /* the gradient's real left edge, kept across the clip */
    int x1 = x + w, y1 = y + h;
    if (x < cl) x = cl;
    if (y < ct) y = ct;
    if (x1 > cr) x1 = cr;
    if (y1 > cb) y1 = cb;
    if (x1 <= x || y1 <= y) return;
    /* Column colours are computed once and shared by every row: a title
     * bar is 18 rows of the same w interpolations.  t is measured from
     * ox, not from the clipped x -- a title bar that is only partly
     * repainted (the cursor passing over it is enough) must keep the
     * colour it already had on screen, otherwise the visible part
     * restarts the ramp at the clip edge and a navy bar grows a navy
     * patch where the pointer went. */
    for (int i = x; i < x1; i++) {
        int t = i - ox;
        u32 col = ((u32)(ar + br * t / w) << 16) | ((u32)(ag + bg * t / w) << 8) |
                  (u32)(ab + bb * t / w);
        for (int j = y; j < y1; j++) g_fb[(size_t)j * g_fw + i] = col;
    }
}

/* ---- dirty rectangles ----
 * A handful of rectangles that are merged whenever they touch, so a
 * dragging cursor over a window produces one rectangle, not two, and a
 * screen with damage everywhere still collapses to "just repaint the
 * lot". */
#define DIRTY_MAX 8
static int dcount;
static int drect[DIRTY_MAX][4];

static int intersects(const int *a, const int *b)
{
    return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] &&
           b[1] < a[1] + a[3];
}

static void grow(int *a, const int *b)
{
    int x0 = a[0] < b[0] ? a[0] : b[0];
    int y0 = a[1] < b[1] ? a[1] : b[1];
    int x1 = a[0] + a[2] > b[0] + b[2] ? a[0] + a[2] : b[0] + b[2];
    int y1 = a[1] + a[3] > b[1] + b[3] ? a[1] + a[3] : b[1] + b[3];
    a[0] = x0;
    a[1] = y0;
    a[2] = x1 - x0;
    a[3] = y1 - y0;
}

/* Collapse any pair that now overlaps; repeated because merging A into B
 * can make B overlap C. */
static void dirty_compact(void)
{
    for (int again = 0; again < DIRTY_MAX; again++) {
        int merged = 0;
        for (int i = 0; i < dcount && !merged; i++)
            for (int j = i + 1; j < dcount; j++)
                if (intersects(drect[i], drect[j])) {
                    grow(drect[i], drect[j]);
                    for (int k = 0; k < 4; k++) drect[j][k] = drect[dcount - 1][k];
                    dcount--;
                    merged = 1;
                    break;
                }
        if (!merged) break;
    }
}

void gfx_dirty(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    int r[4] = {x, y, w, h};
    int x1 = r[0] + r[2], y1 = r[1] + r[3];
    if (r[0] < 0) r[0] = 0;
    if (r[1] < 0) r[1] = 0;
    if (x1 > g_fw) x1 = g_fw;
    if (y1 > g_fh) y1 = g_fh;
    if (x1 <= r[0] || y1 <= r[1]) return;
    r[2] = x1 - r[0];
    r[3] = y1 - r[1];

    for (int i = 0; i < dcount; i++)
        if (intersects(drect[i], r)) {
            grow(drect[i], r);
            dirty_compact();
            return;
        }
    if (dcount == DIRTY_MAX) { /* out of slots: fall back to one big one */
        grow(drect[0], r); /* r has to join the merge, or it is lost */
        for (int i = 1; i < dcount; i++) grow(drect[0], drect[i]);
        dcount = 1;
        return;
    }
    drect[dcount][0] = r[0];
    drect[dcount][1] = r[1];
    drect[dcount][2] = r[2];
    drect[dcount][3] = r[3];
    dcount++;
}

void gfx_dirty_all(void)
{
    dcount = 1;
    drect[0][0] = 0;
    drect[0][1] = 0;
    drect[0][2] = g_fw;
    drect[0][3] = g_fh;
}

int gfx_dirty_take(int rects[][4], int max)
{
    if (max <= 0) return 0;
    if (dcount > max) { /* caller cannot take them all: hand over one */
        for (int i = 1; i < dcount; i++) grow(drect[0], drect[i]);
        dcount = 1;
    }
    int n = dcount;
    if (n > max) n = max;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 4; k++) rects[i][k] = drect[i][k];
    dcount = 0;
    return n;
}

void gfx_blit(int x, int y, int w, int h)
{
    (void)x;
    if (w <= 0 || h <= 0) return;
    /* fb_blit takes packed rows of exactly `w` pixels, so a sub-rectangle
     * of the backbuffer cannot be handed over as-is -- it would read the
     * rest of each row as if it were the next one.  Widening the copy to
     * whole rows is both correct and cheap: the rows are contiguous in
     * memory and a full-width copy is one loop per row either way. */
    struct lnxrm_fb_blit b;
    b.x = 0;
    b.y = (u32)y;
    b.w = (u32)g_fw;
    b.h = (u32)h;
    b.pixels = &g_fb[(size_t)y * g_fw];
    kfb_blit(&b);
}
