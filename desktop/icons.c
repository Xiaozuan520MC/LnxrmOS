/* The desktop's one set of icons.
 *
 * Everything is authored in a 16x16 index buffer and then blitted, either
 * at 1:1 (Start menu rows, which have 20 pixels of height to spend) or at
 * 2:1 (the desktop, where a 32x32 mark reads across a whole icon cell).
 * Two sizes from one drawing is the whole point: there is exactly one
 * place where "what does the calculator look like" is answered, and a
 * second size costs a loop rather than a second table of art.
 *
 * The buffer is *not* screen pixels -- it is drawn with primitives
 * against a palette so an icon is a handful of shapes instead of 256
 * numbers, and so the same shapes can be retinted without a rewrite.
 * Index 0 is transparent, which is what lets a 32x32 icon sit on the teal
 * desktop or on a gray menu row without a box around it. */
#include "desktop.h"

#define ISZ 16

static const u32 pal[] = {
    0,                  /*  0 transparent                */
    RGB(0, 0, 0),       /*  1 outline                    */
    RGB(255, 255, 255), /*  2 white                      */
    RGB(192, 192, 192), /*  3 face                       */
    RGB(128, 128, 128), /*  4 shadow                     */
    RGB(0, 0, 128),     /*  5 navy                       */
    RGB(0, 64, 255),    /*  6 blue                       */
    RGB(200, 0, 0),     /*  7 red                        */
    RGB(0, 140, 0),     /*  8 green                      */
    RGB(255, 200, 0),   /*  9 yellow                     */
    RGB(80, 160, 255),  /* 10 sky                        */
    RGB(230, 120, 0),   /* 11 orange                     */
    RGB(0, 96, 96),     /* 12 dark teal                  */
    RGB(128, 0, 160),   /* 13 purple                     */
    RGB(228, 228, 228), /* 14 light grey                 */
    RGB(192, 144, 80),  /* 15 tan                        */
    RGB(150, 200, 150), /* 16 LCD green                  */
    RGB(0, 128, 128),   /* 17 teal                       */
    RGB(240, 140, 140), /* 18 salmon                     */
    RGB(255, 180, 40),  /* 19 gold                       */
};

static unsigned char b[ISZ][ISZ];

/* ---- the tiny drawing vocabulary an icon is built from ---- */

static void P(int x, int y, int c)
{
    if (x >= 0 && x < ISZ && y >= 0 && y < ISZ) b[y][x] = (unsigned char)c;
}

static void R(int x, int y, int w, int h, int c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) P(i, j, c);
}

static void H(int x, int y, int w, int c)
{
    for (int i = 0; i < w; i++) P(x + i, y, c);
}

static void V(int x, int y, int h, int c)
{
    for (int j = 0; j < h; j++) P(x, y + j, c);
}

static void O(int x, int y, int w, int h, int c)
{
    H(x, y, w, c);
    H(x, y + h - 1, w, c);
    V(x, y, h, c);
    V(x + w - 1, y, h, c);
}

/* Crude DDA, which is all a 16-pixel canvas needs. */
static void L(int x0, int y0, int x1, int y1, int c)
{
    int ddx = x1 > x0 ? 1 : (x1 < x0 ? -1 : 0);
    int ddy = y1 > y0 ? 1 : (y1 < y0 ? -1 : 0);
    int ax = x1 > x0 ? x1 - x0 : x0 - x1;
    int ay = y1 > y0 ? y1 - y0 : y0 - y1;
    int n = ax > ay ? ax : ay;
    int x = x0, y = y0;
    for (int i = 0; i <= n; i++) {
        P(x, y, c);
        x += ddx;
        y += ddy;
    }
}

/* Filled ellipse; rx/ry are half-axes. */
static void E(int cx, int cy, int rx, int ry, int c)
{
    long big = (long)rx * rx * ry * ry;
    for (int y = -ry; y <= ry; y++)
        for (int x = -rx; x <= rx; x++)
            if ((long)x * x * ry * ry + (long)y * y * rx * rx <= big) P(cx + x, cy + y, c);
}

/* ---- the drawings ---- */

/* My Computer: the beige box with a blue screen it always was. */
static void art_files(void)
{
    R(1, 1, 14, 9, 3);
    O(1, 1, 14, 9, 1);
    R(3, 3, 10, 5, 5);
    R(4, 4, 4, 2, 10);
    R(5, 5, 2, 1, 2);
    R(6, 10, 4, 2, 4);   /* neck   */
    R(3, 12, 10, 3, 3);
    O(3, 12, 10, 3, 1);
    H(4, 14, 8, 4);
}

/* Notepad: a sheet, a red margin, three blue lines, a folded corner. */
static void art_notepad(void)
{
    R(3, 1, 10, 14, 2);
    O(3, 1, 10, 14, 1);
    V(6, 3, 11, 7);
    H(8, 5, 4, 6);
    H(8, 8, 4, 6);
    H(8, 11, 4, 6);
    R(10, 1, 3, 3, 3);
    L(10, 1, 12, 3, 1);
}

/* Calculator: LCD at the top, a 3x3 pad below. */
static void art_calc(void)
{
    R(3, 1, 10, 14, 3);
    O(3, 1, 10, 14, 1);
    R(5, 3, 6, 3, 16);
    O(5, 3, 6, 3, 4);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            int kx = 5 + c * 3, ky = 7 + r * 3;
            R(kx, ky, 2, 2, 4);
            P(kx, ky, 1);
        }
}

/* Paint: an artist's palette -- thumb hole, four blobs of colour. */
static void art_paint(void)
{
    E(8, 9, 7, 5, 1);
    E(8, 9, 6, 4, 3);
    E(11, 11, 2, 2, 1); /* thumb hole */
    E(5, 7, 1, 1, 7);
    E(8, 6, 1, 1, 8);
    E(11, 7, 1, 1, 6);
    E(5, 11, 1, 1, 9);
}

/* Minesweeper: a raised tile with a mine sitting on it. */
static void art_mines(void)
{
    R(1, 1, 14, 14, 3);
    O(1, 1, 14, 14, 1);
    H(2, 2, 12, 2);
    V(2, 2, 12, 2);
    H(2, 13, 12, 4);
    V(13, 2, 12, 4);
    R(5, 5, 6, 6, 1);
    R(6, 6, 4, 4, 1);
    V(8, 3, 10, 1);
    H(3, 8, 10, 1);
    L(4, 4, 12, 12, 1);
    L(12, 4, 4, 12, 1);
    R(6, 6, 2, 2, 2);
}

/* Terminal: a window with a black screen and a green prompt. */
static void art_terminal(void)
{
    R(1, 2, 14, 13, 3);
    O(1, 2, 14, 13, 1);
    R(2, 3, 12, 3, 5);
    R(3, 4, 4, 1, 2);
    R(3, 6, 10, 8, 1);
    P(5, 8, 8);
    P(6, 9, 8);
    P(5, 10, 8);
    P(4, 9, 8);
    H(8, 12, 3, 8);
}

/* Mouse: an egg of a mouse -- cord up top, two buttons with a wheel
 * between them, the body filled white inside its outline. */
static void art_mouse(void)
{
    /* the cord, up and out */
    L(8, 1, 8, 3, 1);
    /* the body: an egg, outlined, filled white */
    E(8, 9, 5, 6, 1);
    E(8, 9, 4, 5, 2);
    /* the two buttons: the split, and their rule across the body */
    V(8, 4, 3, 1);
    H(4, 7, 9, 1);
    /* the wheel, between the buttons */
    P(8, 5, 1);
}

/* Task manager: a window whose client is a bar chart. */
static void art_taskmgr(void)
{
    R(1, 3, 14, 12, 3);
    O(1, 3, 14, 12, 1);
    R(2, 4, 12, 3, 5);
    R(3, 5, 5, 1, 2);
    H(3, 13, 10, 1);
    R(4, 10, 2, 3, 8);
    R(7, 7, 2, 6, 6);
    R(10, 11, 2, 2, 7);
}

/* About: the blue disc with an "i". */
static void art_about(void)
{
    E(8, 8, 7, 7, 1);
    E(8, 8, 6, 6, 5);
    R(7, 4, 2, 2, 2);
    R(7, 7, 2, 6, 2);
}

/* Clock: a white face in a black case, hands at ten past ten so the
 * dial reads as a clock at a glance instead of as a circle. */
static void art_clock(void)
{
    E(8, 8, 7, 7, 1);
    E(8, 8, 6, 6, 2);
    P(8, 3, 1); /* 12 */
    P(13, 8, 1); /* 3  */
    P(8, 13, 1); /* 6  */
    P(3, 8, 1);  /* 9  */
    L(8, 8, 11, 6, 1);  /* hour   */
    L(8, 8, 5, 11, 7);  /* minute */
    P(8, 8, 1);
}

/* System Properties: the chip, drawn from above -- package, die and the
 * pins that make it a part rather than a square. */
static void art_sysinfo(void)
{
    for (int i = 5; i <= 11; i += 2) {
        V(i, 2, 2, 1);
        V(i, 12, 2, 1);
        H(2, i, 2, 1);
        H(12, i, 2, 1);
    }
    R(4, 4, 8, 8, 4);
    O(4, 4, 8, 8, 1);
    R(6, 6, 4, 4, 10);
    P(6, 6, 2);
}

/* Display Properties: a monitor with two colour bars on its screen --
 * "what comes out of this" rather than "which machine is this". */
static void art_display(void)
{
    R(1, 2, 14, 10, 3);
    O(1, 2, 14, 10, 1);
    R(3, 4, 10, 6, 5);
    R(4, 5, 4, 4, 7);
    R(9, 5, 3, 4, 9);
    R(6, 12, 4, 2, 4);
    R(3, 14, 10, 1, 3);
}

/* Snake: an S of green blocks in a bordered field, with the apple it is
 * heading for in the corner. */
static void art_snake(void)
{
    R(1, 1, 14, 14, 1);
    O(1, 1, 14, 14, 4);
    R(3, 11, 7, 2, 8);
    R(3, 7, 2, 6, 8);
    R(5, 7, 6, 2, 8);
    R(9, 9, 2, 2, 8);
    P(10, 7, 2);      /* eye   */
    E(11, 4, 2, 2, 7); /* apple, clear of the field's border */
    P(11, 2, 8);
}

static void build(int id)
{
    for (int j = 0; j < ISZ; j++)
        for (int i = 0; i < ISZ; i++) b[j][i] = 0;
    switch (id) {
    case ICO_NOTEPAD: art_notepad(); break;
    case ICO_CALC: art_calc(); break;
    case ICO_PAINT: art_paint(); break;
    case ICO_MINES: art_mines(); break;
    case ICO_TERMINAL: art_terminal(); break;
    case ICO_TASKMGR: art_taskmgr(); break;
    case ICO_ABOUT: art_about(); break;
    case ICO_CLOCK: art_clock(); break;
    case ICO_SYSINFO: art_sysinfo(); break;
    case ICO_DISPLAY: art_display(); break;
    case ICO_SNAKE: art_snake(); break;
    case ICO_MOUSE: art_mouse(); break;
    default: art_files(); break;
    }
}

void icon_paint(int id, int x, int y, int scale)
{
    if (scale < 1) scale = 1;
    build(id);
    for (int r = 0; r < ISZ; r++) {
        for (int c = 0; c < ISZ; c++) {
            int v = b[r][c];
            if (!v) continue;
            if (scale == 1) {
                gfx_px(x + c, y + r, pal[v]);
            } else {
                int px = x + c * scale, py = y + r * scale;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++) gfx_px(px + dx, py + dy, pal[v]);
            }
        }
    }
}

int icon_id_for(const struct app *a)
{
    static const struct {
        const struct app *a;
        int id;
    } tab[] = {
        {&app_calc, ICO_CALC},       {&app_notepad, ICO_NOTEPAD},
        {&app_files, ICO_FILES},     {&app_paint, ICO_PAINT},
        {&app_mines, ICO_MINES},     {&app_terminal, ICO_TERMINAL},
        {&app_taskmgr, ICO_TASKMGR}, {&app_about, ICO_ABOUT},
        {&app_clock, ICO_CLOCK},     {&app_sysinfo, ICO_SYSINFO},
        {&app_display, ICO_DISPLAY}, {&app_snake, ICO_SNAKE},
        {&app_mouse, ICO_MOUSE},
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (tab[i].a == a) return tab[i].id;
    return ICO_FILES;
}
