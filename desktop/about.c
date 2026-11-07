/* About box -- the smallest possible application, and therefore the
 * template for the rest: one paint routine for the client area and one
 * down routine for the single button.  No state, no allocation. */
#include "desktop.h"

#define BTN_W 76
#define BTN_H 26

/* The button's position is derived from the client rectangle rather than
 * stored, so the window can be resized without the two drifting apart. */
static void btn_pos(const struct win *w, int *bx, int *by)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    *bx = x + (cw - BTN_W) / 2;
    *by = y + ch - BTN_H - 10;
}

static void about_paint(struct win *w)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);

    gfx_fill(x, y, cw, ch, COL_FACE);

    /* banner: the same navy->blue gradient as an active title bar, so the
     * window reads as part of one system */
    gfx_hgrad(x + 8, y + 8, cw - 16, 58, COL_TITLE0, COL_TITLE1);
    gfx_out(x + 8, y + 8, cw - 16, 58);
    /* The mark first, the name beside it -- centred between them on the
     * banner's own middle (y+37), so the disc and the two lines of type
     * hang off one axis instead of each finding its own. */
    logo_paint(x + 34, y + 37, 20);
    gfx_text_bold(x + 62, y + 20, "LnxrmOS", COL_HILITE);
    gfx_text(x + 62, y + 40, "a tiny unix-like kernel", COL_HILITE);

    static const char *lines[] = {
        "Win98-style desktop, user space",
        "PS/2 mouse driver in the kernel",
        "one backbuffer, dirty rectangles",
        "every draw goes through fb_fill",
        "",
        "Nothing here is drawn by the kernel.",
    };

    int bx, by;
    btn_pos(w, &bx, &by);
    int ly = y + 78;
    int maxl = (by - ly - 6) / 18;
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    if (maxl < n) n = maxl;
    for (int i = 0; i < n; i++) gfx_text(x + 14, ly + i * 18, lines[i], COL_TEXT);

    gfx_glass_button(bx, by, BTN_W, BTN_H, 0, "OK");
}

static void about_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    (void)cw;
    (void)ch;
    (void)clicks;
    if (btn != LNXRM_MB_LEFT) return;
    int bx, by;
    btn_pos(w, &bx, &by);
    int px = x + cx, py = y + cy;
    if (px < bx || py < by || px >= bx + BTN_W || py >= by + BTN_H) return;
    win_close(w);
}

const struct app app_about = {
    .name = "About LnxrmOS...",
    .title = "About LnxrmOS",
    .w = 352,
    .h = 268,
    .open = 0,
    .paint = about_paint,
    .down = about_down,
    .drag = 0,
    .up = 0,
    .key = 0,
    .close = 0,
};
