/* Clock: an analog dial with a digital readout under it -- the accessory
 * Win98 shipped in the menu, and the only window here whose face has to
 * change without anyone touching it.  Like Task Manager it carries a
 * .tick, but it asks for a repaint only when the second hand has actually
 * moved: one client-sized rectangle a second is free, a window that
 * repainted every frame for a hand that moves once a second would not be.
 *
 * There is no real-time clock in user space, so "the time" is uptime
 * read through uptime_ms() -- the same number the taskbar clock adds to
 * "midnight".  Both hands and the readout come from that one call, which
 * is why the dial and the strip along the bottom of the screen agree. */
#include "desktop.h"

struct clk {
    long last_sec; /* what was on screen at the last paint */
};

static long now_sec(void)
{
    long ms = uptime_ms();
    if (ms < 0) ms = 0;
    return ms / 1000;
}

/* sin() in thousandths of the radius, 0..90 degrees in 6-degree steps.
 * Integer only: this system has no floating point to spend (-mno-mmx
 * -mno-sse), and a clock hand does not need more resolution than this. */
static const short sin_tab[16] = {
    0, 105, 208, 309, 407, 500, 588, 669,
    743, 809, 866, 914, 951, 978, 995, 1000
};

static int isin(int deg)
{
    int sign = 1;
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg > 180) {
        deg -= 180;
        sign = -1;
    }
    if (deg > 90) deg = 180 - deg;
    int i = deg / 6, f = deg % 6;
    int v = (i >= 15) ? 1000 : sin_tab[i] + (sin_tab[i + 1] - sin_tab[i]) * f / 6;
    return sign * v;
}

static int icos(int deg) { return isin(deg + 90); }

static long isqrt(long n)
{
    if (n <= 0) return 0;
    long x = n, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

/* One hand, thickened along the direction at right angles to itself --
 * offsetting both endpoints by (1,1) instead would leave a diagonal
 * hand one pixel wide, which is exactly the hand nobody can see. */
static void hand(int cx0, int cy0, int len, int deg, int th, u32 col)
{
    if (len < 1) return;
    int tx = cx0 + len * isin(deg) / 1000;
    int ty = cy0 - len * icos(deg) / 1000;
    int nx = -(ty - cy0), ny = tx - cx0;
    long nl = isqrt((long)nx * nx + (long)ny * ny);
    if (nl < 1) nl = 1;
    for (int k = -(th / 2); k <= th / 2; k++) {
        int ox = (int)((long)nx * k / nl), oy = (int)((long)ny * k / nl);
        gfx_line(cx0 + ox, cy0 + oy, tx + ox, ty + oy, col);
    }
}

/* The dial edge: 0.5 degree steps, because a 78-pixel radius has a
 * circumference of nearly 500 pixels and one sample per degree would
 * leave a dotted ring. */
static void dial(int cx0, int cy0, int r, u32 col)
{
    for (int a = 0; a < 720; a++) {
        int deg = a / 2;
        gfx_px(cx0 + r * isin(deg) / 1000, cy0 - r * icos(deg) / 1000, col);
    }
}

static void clock_paint(struct win *w)
{
    struct clk *c = w->state;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    long sec = now_sec();
    if (c) c->last_sec = sec;
    int hh = (int)((sec / 3600) % 24);
    int mm = (int)((sec / 60) % 60);
    int ss = (int)(sec % 60);

    /* The dial takes whatever fits above a line for the readout; capping
     * it means a resized window grows the *frame* around the clock
     * rather than a clock no longer in the middle of its window. */
    int r = cw / 2 - 8;
    int ry = (ch - 26) / 2 - 4;
    if (ry < r) r = ry;
    if (r > 84) r = 84;
    if (r < 16) r = 16;
    int cx0 = x + cw / 2, cy0 = y + 6 + r;

    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)isqrt((long)(r * r - dy * dy));
        gfx_hline(cx0 - dx, cy0 + dy, dx * 2 + 1, COL_EDITBG);
    }
    dial(cx0, cy0, r, COL_DKSHADOW);

    /* Sixty marks: the hours are longer and black, the rest shadow. */
    for (int i = 0; i < 60; i++) {
        int deg = i * 6;
        int hour = (i % 5) == 0;
        int r0 = r - (hour ? 10 : 5);
        int r1 = r - 2;
        gfx_line(cx0 + r0 * isin(deg) / 1000, cy0 - r0 * icos(deg) / 1000,
                 cx0 + r1 * isin(deg) / 1000, cy0 - r1 * icos(deg) / 1000,
                 hour ? COL_DKSHADOW : COL_SHADOW);
    }

    hand(cx0, cy0, r * 55 / 100, (hh % 12) * 30 + mm / 2, 4, COL_TEXT);
    hand(cx0, cy0, r * 82 / 100, mm * 6 + ss / 10, 2, COL_TEXT);
    hand(cx0, cy0, r * 92 / 100, ss * 6, 1, RGB(200, 0, 0));
    gfx_fill(cx0 - 2, cy0 - 2, 5, 5, COL_TEXT);

    char t[12];
    t[0] = (char)('0' + (hh / 10) % 10);
    t[1] = (char)('0' + hh % 10);
    t[2] = ':';
    t[3] = (char)('0' + (mm / 10) % 10);
    t[4] = (char)('0' + mm % 10);
    t[5] = ':';
    t[6] = (char)('0' + (ss / 10) % 10);
    t[7] = (char)('0' + ss % 10);
    t[8] = 0;
    /* The bold table has its own advances now, so the width the clock is
     * centred against is the width it is drawn at. */
    int tw = gfx_strw_bold(t);
    gfx_text_bold(x + (cw - tw) / 2, y + ch - 18, t, COL_TEXT);
}

/* Nothing else repaints a window nobody has touched, so the clock has
 * to notice on its own that the second it is showing has gone.  Skipping
 * a minimised window matters too: a hidden clock would otherwise keep
 * invalidating a rectangle nothing draws, forever. */
static void clock_tick(struct win *w)
{
    struct clk *c = w->state;
    if (!c || w->minimized) return;
    long sec = now_sec();
    if (sec == c->last_sec) return;
    c->last_sec = sec;
    win_invalidate_client(w);
}

static void clock_open(struct win *w)
{
    struct clk *c = (struct clk *)xcalloc(1, sizeof(struct clk));
    if (!c) return;
    c->last_sec = now_sec();
    w->state = c;
}

static void clock_close(struct win *w)
{
    if (w->state) {
        xfree(w->state);
        w->state = 0;
    }
}

const struct app app_clock = {
    .name = "Clock",
    .title = "Clock",
    .w = 236,
    .h = 232,
    .open = clock_open,
    .paint = clock_paint,
    .down = 0,
    .drag = 0,
    .up = 0,
    .key = 0,
    .close = clock_close,
    .tick = clock_tick,
};
