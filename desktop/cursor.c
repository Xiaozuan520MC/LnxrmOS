/* The pointer.  A 1-bit arrow with an outline and a fill, drawn the way
 * Windows drew it: into the same backbuffer as everything else, on top
 * of the scene, never into the framebuffer on its own.  That is what
 * lets the compositor repaint a rectangle under the old cursor position
 * simply by painting the scene there again.
 *
 * Three of its qualities are settings, owned here and edited by the
 * Mouse window (mouse.c): the speed the loop multiplies motion deltas
 * by, the scale the sprite is drawn at, and the two colours it is drawn
 * with.  They are session settings -- the desktop has nowhere to keep
 * them across boots, and a pointer that forgets between sessions is
 * honest about that. */
#include "desktop.h"

/* 0 = transparent, 1 = outline, 2 = fill */
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

/* ---- the settings (mouse.c writes; loop.c and cursor_paint read) ---- */

static int ms_speed = 100; /* motion gain, percent of the raw delta */
static int ms_scale = 1;   /* sprite magnification: 1, 2 or 3          */
static u32 ms_fill = COL_HILITE;
static u32 ms_outline = COL_DKSHADOW;

int mouse_speed(void) { return ms_speed; }

void mouse_set_speed(int pct)
{
    if (pct < 25) pct = 25;
    if (pct > 400) pct = 400;
    ms_speed = pct;
}

int mouse_scale(void) { return ms_scale; }

void mouse_set_scale(int s)
{
    if (s < 1) s = 1;
    if (s > 3) s = 3;
    ms_scale = s;
}

void mouse_set_color(u32 fill, u32 outline)
{
    ms_fill = fill;
    ms_outline = outline;
}

void mouse_colors(u32 *fill, u32 *outline)
{
    if (fill) *fill = ms_fill;
    if (outline) *outline = ms_outline;
}

/* The sprite's size on screen at the current scale -- the compositor
 * measures its dirty boxes with these, so a bigger pointer dirties a
 * bigger box. */
int cursor_w(void) { return CURSOR_W * ms_scale; }
int cursor_h(void) { return CURSOR_H * ms_scale; }

void cursor_paint(int x, int y)
{
    int s = ms_scale;
    for (int r = 0; r < CURSOR_H; r++)
        for (int c = 0; c < CURSOR_W; c++) {
            unsigned char v = arrow[r][c];
            if (!v) continue;
            u32 col = v == 1 ? ms_outline : ms_fill;
            for (int dy = 0; dy < s; dy++)
                for (int dx = 0; dx < s; dx++)
                    gfx_px(x + c * s + dx, y + r * s + dy, col);
        }
}
