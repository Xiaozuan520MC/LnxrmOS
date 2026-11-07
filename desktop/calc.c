/* Calculator.
 *
 * Integer arithmetic only -- user space has no libm, no x87 helper
 * routines, and a calculator that shows 2.6666666666666665 when you ask
 * for 8/3 is not more useful than one that shows 2.  The engine is the
 * classic accumulator/entry pair: `acc` holds the running total, `cur`
 * the number being typed, `op` the operator still waiting for its right
 * hand side.  The display shows `cur` while a number is being entered
 * and `acc` the moment an operator commits it, which is what makes
 * "5 + " show 5 and "5 + 3 +" show 8.
 */
#include "desktop.h"

#define COLS 5
#define ROWS 4

static const char *const keys[ROWS][COLS] = {
    {"7", "8", "9", "/", "C"},
    {"4", "5", "6", "*", "B"},
    {"1", "2", "3", "-", "CE"},
    {"0", "00", "+/-", "+", "="},
};

struct calc {
    long acc, cur;
    int op;     /* 0 none, else '+', '-', '*', '/' */
    int has;    /* cur is a fresh entry, not a committed total */
    int acc_set;
    int fresh;  /* the answer is showing; the next digit starts over */
    int err;
    int press;  /* index of the key held down, -1 = none */
};

static void key_rect(const struct win *w, int idx, int *kx, int *ky, int *kw, int *kh)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int m = 6, disp_h = 40, gap = 6;
    int aw = cw - 2 * m - (COLS - 1) * 4;
    int ah = ch - 2 * m - disp_h - gap - (ROWS - 1) * 4;
    int kw_ = aw / COLS;
    int kh_ = ah / ROWS;
    if (kw_ < 8) kw_ = 8;
    if (kh_ < 8) kh_ = 8;
    int i = idx / COLS, j = idx % COLS;
    *kx = x + m + j * (kw_ + 4);
    *ky = y + m + disp_h + gap + i * (kh_ + 4);
    *kw = kw_;
    *kh = kh_;
}

static int key_at(const struct win *w, int px, int py)
{
    for (int i = 0; i < ROWS * COLS; i++) {
        int kx, ky, kw, kh;
        key_rect(w, i, &kx, &ky, &kw, &kh);
        if (px >= kx && py >= ky && px < kx + kw && py < ky + kh) return i;
    }
    return -1;
}

static long apply(long a, int op, long b, int *err)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/':
        if (!b) {
            *err = 1;
            return 0;
        }
        return a / b;
    }
    return b;
}

static void digit(struct calc *c, int d)
{
    if (c->err) return;
    if (c->fresh) {
        /* the answer is on the display: a digit begins a new number,
         * it does not glue itself to the old one */
        c->cur = 0;
        c->fresh = 0;
    }
    /* 19 digits is where long stops being able to keep up; stop rather
     * than wrap into a negative number the display would show as a sign. */
    if (c->cur > 999999999999999999L / 10) return;
    c->cur = c->cur * 10 + d;
    c->has = 1;
}

static void activate(struct win *w, int idx)
{
    struct calc *c = w->state;
    if (!c) return;
    const char *k = keys[idx / COLS][idx % COLS];

    if (k[0] >= '0' && k[0] <= '9' && k[1] == 0) {
        digit(c, k[0] - '0');
        return;
    }
    if (!strcmp(k, "00")) {
        digit(c, 0);
        digit(c, 0);
        return;
    }
    if (!strcmp(k, "+/-")) {
        if (!c->err) c->cur = -c->cur;
        return;
    }
    if (c->err) return; /* everything below is meaningless after an error */

    if (!strcmp(k, "C")) {
        c->acc = c->cur = 0;
        c->op = 0;
        c->has = c->acc_set = c->err = 0;
        return;
    }
    if (!strcmp(k, "CE")) {
        c->cur = 0;
        c->has = 0;
        return;
    }
    if (!strcmp(k, "B")) {
        c->cur /= 10;
        c->has = 1;
        return;
    }
    if (k[1] == 0 && (k[0] == '+' || k[0] == '-' || k[0] == '*' || k[0] == '/')) {
        if (c->has) {
            if (c->op && c->acc_set)
                c->acc = apply(c->acc, c->op, c->cur, &c->err);
            else
                c->acc = c->cur;
        }
        c->op = k[0];
        c->cur = 0; /* the next number starts fresh: 1 + 1 is 2, not 11 */
        c->acc_set = 1;
        c->has = 0;
        return;
    }
    if (!strcmp(k, "=")) {
        if (c->op) {
            long b = c->has ? c->cur : c->acc; /* "5 + =" squares the 5 */
            c->acc = apply(c->acc, c->op, b, &c->err);
            c->cur = c->acc;
            c->op = 0;
            c->acc_set = 0;
            c->has = 1;
        } else if (c->has) {
            c->acc = c->cur;
        }
        c->fresh = 1; /* a digit typed now begins a new number */
    }
    (void)w;
}

static void calc_paint(struct win *w)
{
    struct calc *c = w->state;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    int m = 6, disp_h = 40, gap = 6;

    /* display: sunken, right aligned, like the real thing */
    int dx = x + m, dy = y + m, dw = cw - 2 * m;
    gfx_fill(dx, dy, dw, disp_h, COL_EDITBG);
    gfx_in(dx, dy, dw, disp_h);

    char buf[40];
    const char *txt = buf;
    if (c->err) {
        txt = "Error";
    } else {
        long v = c->has ? c->cur : c->acc;
        xfmt_i(buf, v);
    }
    int tw = gfx_strw(txt);
    int tx = dx + dw - 8 - tw;
    if (tx < dx + 4) tx = dx + 4;
    gfx_text(tx, dy + (disp_h - 16) / 2, txt, COL_TEXT);

    /* the pending operation, at the left edge: "1 +" while the right
     * hand side is being typed, so the sum on screen is the sum in mind */
    if (c->op && !c->err) {
        char expr[44];
        expr[0] = 0;
        xfmt_i(expr, c->acc);
        str_cat(expr, " ");
        int n = xstrlen(expr);
        expr[n] = (char)c->op;
        expr[n + 1] = 0;
        gfx_text(dx + 8, dy + (disp_h - 16) / 2, expr, COL_SHADOW);
    }

    for (int i = 0; i < ROWS * COLS; i++) {
        int kx, ky, kw, kh;
        key_rect(w, i, &kx, &ky, &kw, &kh);
        int down = (c && c->press == i);
        gfx_glass_button(kx, ky, kw, kh, down, keys[i / COLS][i % COLS]);
    }
    (void)gap;
}

static void calc_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    struct calc *c = w->state;
    (void)clicks;
    if (!c || btn != LNXRM_MB_LEFT) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int idx = key_at(w, x + cx, y + cy);
    if (idx < 0) return;
    c->press = idx;
    win_invalidate_client(w);
}

static void calc_up(struct win *w, int cx, int cy, int btn)
{
    struct calc *c = w->state;
    if (!c || btn != LNXRM_MB_LEFT) return;
    int held = c->press;
    c->press = -1;
    win_invalidate_client(w);
    if (held < 0) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    if (key_at(w, x + cx, y + cy) == held) activate(w, held);
}

/* Typing on the keyboard does what typing on the keypad does. */
static void calc_key(struct win *w, const struct dsk_event *ev)
{
    if (ev->type != LNXRM_EV_KEY_DOWN) return;
    struct calc *c = w->state;
    if (!c) return;
    if (ev->ch >= '0' && ev->ch <= '9') {
        digit(c, ev->ch - '0');
        return;
    }
    if (ev->code == LNXRM_KEY_BACKSPACE) {
        c->cur /= 10;
        c->has = 1;
        return;
    }
    if (ev->code == LNXRM_KEY_ENTER || ev->ch == '=') {
        /* the last key of the grid, which is "=" -- the old 4*COLS+4
           walked off the array and activated whatever byte came next */
        activate(w, ROWS * COLS - 1);
        return;
    }
    const char *ops = "+-*/";
    char ch = (char)ev->ch;
    if (ch && strchr(ops, ch)) {
        for (int i = 0; i < ROWS * COLS; i++)
            if (keys[i / COLS][i % COLS][0] == ch && keys[i / COLS][i % COLS][1] == 0) {
                activate(w, i);
                return;
            }
    }
}

static void calc_open(struct win *w)
{
    struct calc *c = (struct calc *)xcalloc(1, sizeof(struct calc));
    if (!c) return;
    c->press = -1;
    w->state = c;
}

static void calc_close(struct win *w)
{
    if (w->state) {
        xfree(w->state);
        w->state = 0;
    }
}

const struct app app_calc = {
    .name = "Calculator",
    .title = "Calculator",
    .w = 274,
    .h = 264,
    .open = calc_open,
    .paint = calc_paint,
    .down = calc_down,
    .drag = 0,
    .up = calc_up,
    .key = calc_key,
    .close = calc_close,
};
