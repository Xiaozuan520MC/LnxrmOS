/* Minesweeper.
 *
 * The field is generated on the first click rather than at start-up, and
 * the clicked cell plus its eight neighbours are kept clear: a game that
 * can end before you have made a decision is not a game.  The generator
 * is a linear congruential walk seeded from the uptime, good enough for
 * hiding mines and a great deal smaller than a PRNG worth the name.
 */
#include "desktop.h"

#define MW 9
#define MH 9
#define NMINES 10
#define NC (MW * MH)

#define CS_HIDDEN 0
#define CS_OPEN 1
#define CS_FLAG 2

struct mine {
    u8 cell[NC];  /* 0..8 neighbours, 9 = mine */
    u8 st[NC];    /* CS_*                      */
    int started;
    int over, lost;
    int flags, opened;
    long t0;
    long shown_sec; /* last second drawn, so tick() only dirties on change */
};

static const u32 numcol[9] = {
    0, RGB(0, 0, 255), RGB(0, 128, 0), RGB(255, 0, 0), RGB(0, 0, 128),
    RGB(128, 0, 0), RGB(0, 128, 128), RGB(0, 0, 0), RGB(128, 128, 128),
};

static int nbrs(int i, int j) /* are i and j the same or adjacent cells? */
{
    int ax = i % MW, ay = i / MW, bx = j % MW, by = j / MW;
    int dx = ax - bx, dy = ay - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= 1 && dy <= 1;
}

static void neighbours(int i, int out[8], int *n)
{
    int x = i % MW, y = i / MW;
    *n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= MW || ny >= MH) continue;
            out[(*n)++] = ny * MW + nx;
        }
}

static void generate(struct mine *m, int safe)
{
    unsigned seed = (unsigned)uptime_ms() | 1u;
    int placed = 0;
    while (placed < NMINES) {
        seed = seed * 1103515245u + 12345u;
        int idx = (int)((seed >> 16) & 0x7fff) % NC;
        if (nbrs(idx, safe)) continue; /* first click is never a loss */
        if (m->cell[idx] == 9) continue;
        m->cell[idx] = 9;
        placed++;
    }
    for (int i = 0; i < NC; i++) {
        if (m->cell[i] == 9) continue;
        int adj[8], n;
        neighbours(i, adj, &n);
        int c = 0;
        for (int k = 0; k < n; k++)
            if (m->cell[adj[k]] == 9) c++;
        m->cell[i] = (u8)c;
    }
}

static void open_at(struct mine *m, int start)
{
    int stack[NC];
    int sp = 0;
    stack[sp++] = start;
    while (sp) {
        int c = stack[--sp];
        if (c < 0 || c >= NC || m->st[c] == CS_OPEN || m->st[c] == CS_FLAG) continue;
        m->st[c] = CS_OPEN;
        m->opened++;
        if (m->cell[c] != 0) continue;
        int adj[8], n;
        neighbours(c, adj, &n);
        for (int k = 0; k < n; k++)
            if (m->st[adj[k]] == CS_HIDDEN) stack[sp++] = adj[k];
    }
}

static void reset(struct mine *m)
{
    memset(m, 0, sizeof(*m));
    m->shown_sec = -1;
}

/* ---- geometry ---- */

static void grid_rect(const struct win *w, int *gx, int *gy, int *cell)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int c = (cw - 12) / MW;
    if (c > (ch - 46) / MH) c = (ch - 46) / MH;
    if (c < 8) c = 8;
    *cell = c;
    *gx = x + (cw - c * MW) / 2;
    *gy = y + 34;
}

static void three_digits(char *out, int v) /* "005", "-05", "123" */
{
    if (v > 999) v = 999;
    int i = 0;
    if (v < 0) {
        if (v < -99) v = -99;
        out[i++] = '-';
        v = -v;
    } else {
        out[i++] = (char)('0' + v / 100);
    }
    out[i++] = (char)('0' + (v / 10) % 10);
    out[i++] = (char)('0' + v % 10);
    out[i] = 0;
}

static void mn_paint(struct win *w)
{
    struct mine *m = w->state;
    if (!m) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    /* header: mine counter, restart face, clock */
    int hy = y + 5, hh = 24;
    gfx_fill(x + 5, hy, cw - 10, hh, COL_FACE);
    gfx_in(x + 5, hy, cw - 10, hh);

    char buf[8];
    gfx_fill(x + 9, hy + 3, 46, 18, COL_DKSHADOW);
    gfx_in(x + 9, hy + 3, 46, 18);
    three_digits(buf, NMINES - m->flags);
    gfx_text_bold(x + 14, hy + 4, buf, RGB(255, 0, 0));

    gfx_fill(x + cw - 55, hy + 3, 46, 18, COL_DKSHADOW);
    gfx_in(x + cw - 55, hy + 3, 46, 18);
    long sec = m->started && !m->over ? (uptime_ms() - m->t0) / 1000 : 0;
    three_digits(buf, (int)sec);
    gfx_text_bold(x + cw - 50, hy + 4, buf, RGB(255, 0, 0));

    /* the face button */
    int fx = x + cw / 2 - 11, fy = hy + 1;
    gfx_glass_button(fx, fy, 22, 22, 0, 0);
    gfx_fill(fx + 4, fy + 4, 14, 14, RGB(255, 220, 0));
    gfx_box(fx + 4, fy + 4, 14, 14, COL_SHADOW);
    if (m->over && m->lost) { /* X eyes */
        gfx_line(fx + 7, fy + 8, fx + 9, fy + 10, COL_DKSHADOW);
        gfx_line(fx + 9, fy + 8, fx + 7, fy + 10, COL_DKSHADOW);
        gfx_line(fx + 13, fy + 8, fx + 15, fy + 10, COL_DKSHADOW);
        gfx_line(fx + 15, fy + 8, fx + 13, fy + 10, COL_DKSHADOW);
        gfx_hline(fx + 7, fy + 15, 8, COL_DKSHADOW);
    } else {
        gfx_fill(fx + 7, fy + 8, 2, 3, COL_DKSHADOW);
        gfx_fill(fx + 13, fy + 8, 2, 3, COL_DKSHADOW);
        gfx_hline(fx + 7, fy + 13, 2, COL_DKSHADOW);
        gfx_hline(fx + 13, fy + 13, 2, COL_DKSHADOW);
        gfx_vline(fx + 7, fy + 13, 2, COL_DKSHADOW);
        gfx_vline(fx + 14, fy + 13, 2, COL_DKSHADOW);
        gfx_hline(fx + 7, fy + 15, 8, COL_DKSHADOW);
    }

    /* field */
    int gx, gy, cell;
    grid_rect(w, &gx, &gy, &cell);
    gfx_fill(gx - 2, gy - 2, cell * MW + 4, cell * MH + 4, COL_FACE);
    gfx_out(gx - 2, gy - 2, cell * MW + 4, cell * MH + 4);

    for (int i = 0; i < NC; i++) {
        int cx = gx + (i % MW) * cell;
        int cy = gy + (i / MW) * cell;
        int open = m->st[i] == CS_OPEN;
        u32 base = COL_FACE;
        if (open && m->cell[i] == 9) base = RGB(255, 0, 0);
        gfx_fill(cx, cy, cell, cell, base);
        if (open)
            gfx_in(cx, cy, cell, cell);
        else
            gfx_out(cx, cy, cell, cell);

        if (!open && m->st[i] == CS_FLAG) {
            /* pole + red banner */
            int px = cx + cell / 2, py = cy + cell / 3;
            gfx_vline(px, py, cell / 2, COL_DKSHADOW);
            for (int r = 0; r < cell / 3; r++)
                gfx_hline(px - cell / 4 + 1, py + r, cell / 4, RGB(255, 0, 0));
        } else if (open && m->cell[i] == 9) {
            gfx_fill(cx + cell / 4, cy + cell / 4, cell / 2, cell / 2, COL_DKSHADOW);
            gfx_vline(cx + cell / 2, cy + 2, cell - 4, COL_DKSHADOW);
            gfx_hline(cx + 2, cy + cell / 2, cell - 4, COL_DKSHADOW);
        } else if (open && m->cell[i] > 0) {
            int d = m->cell[i];
            char s[2] = {(char)('0' + d), 0};
            gfx_text_bold(cx + (cell - gfx_strw(s)) / 2, cy + (cell - 16) / 2, s, numcol[d]);
        }
    }
    (void)ch;
}

static int cell_at(const struct win *w, int px, int py)
{
    int gx, gy, cell;
    grid_rect(w, &gx, &gy, &cell);
    if (px < gx || py < gy) return -1;
    int i = (px - gx) / cell, j = (py - gy) / cell;
    if (i < 0 || j < 0 || i >= MW || j >= MH) return -1;
    return j * MW + i;
}

static void check_win(struct mine *m)
{
    if (m->over) return;
    if (m->opened == NC - NMINES) {
        m->over = 1;
        m->lost = 0;
    }
}

static void mn_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    struct mine *m = w->state;
    if (!m) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    (void)ch;
    int px = x + cx, py = y + cy;
    (void)clicks;

    /* restart face */
    int fx = x + cw / 2 - 11, fy = y + 6;
    if (px >= fx && py >= fy && px < fx + 22 && py < fy + 22) {
        reset(m);
        return;
    }

    int idx = cell_at(w, px, py);
    if (idx < 0) return;
    if (m->over) return;

    if (btn == LNXRM_MB_RIGHT) {
        if (m->st[idx] == CS_HIDDEN) {
            m->st[idx] = CS_FLAG;
            m->flags++;
        } else if (m->st[idx] == CS_FLAG) {
            m->st[idx] = CS_HIDDEN;
            m->flags--;
        }
        return;
    }
    if (btn != LNXRM_MB_LEFT) return;
    if (m->st[idx] != CS_HIDDEN) return;

    if (!m->started) {
        generate(m, idx);
        m->started = 1;
        m->t0 = uptime_ms();
    }
    if (m->cell[idx] == 9) {
        m->over = 1;
        m->lost = 1;
        for (int i = 0; i < NC; i++)
            if (m->cell[i] == 9 && m->st[i] != CS_FLAG) m->st[i] = CS_OPEN;
        return;
    }
    open_at(m, idx);
    check_win(m);
}

static void mn_tick(struct win *w)
{
    struct mine *m = w->state;
    if (!m || !m->started || m->over) return;
    long sec = (uptime_ms() - m->t0) / 1000;
    if (sec == m->shown_sec) return;
    m->shown_sec = sec;
    win_invalidate_client(w);
}

static void mn_open(struct win *w)
{
    struct mine *m = (struct mine *)xcalloc(1, sizeof(struct mine));
    if (!m) return;
    reset(m);
    w->state = m;
}

static void mn_close(struct win *w)
{
    if (w->state) {
        xfree(w->state);
        w->state = 0;
    }
}

const struct app app_mines = {
    .name = "Minesweeper",
    .title = "Minesweeper",
    .w = 244,
    .h = 292,
    .open = mn_open,
    .paint = mn_paint,
    .down = mn_down,
    .drag = 0,
    .up = 0,
    .key = 0,
    .close = mn_close,
    .tick = mn_tick,
};
