/* Paint: a pencil, sixteen colours, four widths, and a line of type.
 *
 * There is no Save, and there is no Open button either.  Writing an
 * 800 KB image out through the FAT32 path a row at a time costs more
 * disk traffic than the rest of the desktop put together, and since this
 * shell is one thread drawing one frame at a time, the whole of that is
 * a frozen screen: the save was the one thing this program did that made
 * it look broken.  Drawing is what Paint is for, and the pixels live in
 * the canvas for as long as the window is open.
 *
 * A .ppm double-clicked in File Explorer still arrives here through
 * app_open_arg(), so Paint doubles as a picture viewer -- it is only the
 * writing end that has gone.
 *
 * The canvas is a fixed 640x420 block of its own rather than a mirror of
 * the window, so resizing the window crops and reveals instead of
 * destroying what was drawn, and repaint only ever walks the part of it
 * that the current damage rectangle covers -- a full-screen drag costs a
 * few hundred rows, not a quarter of a million pixels.
 *
 * Type is laid down in two stages.  The run being typed is an overlay:
 * it is walked out of a buffer and painted over the canvas every frame,
 * and baked into the canvas only when the run ends (Enter, another tool,
 * a new colour, Escape).  That is what lets Backspace work at all --
 * once a glyph is in the canvas the pixels that were under it are gone,
 * so it must not go in until the run is finished with it.  Each run
 * carries the colour picked when it started, which is how one line can
 * be red and the next blue.  The window closing is not one of the
 * endings: without a Save there is nowhere for the canvas to outlive
 * the window, so there is nothing to flush into it.
 *
 * Shift, held while dragging, snaps the stroke to horizontal, vertical
 * or 45 degrees.  It is measured against the point the stroke came from
 * and the snapped point becomes the new origin, so a line held straight
 * stays straight as it is extended instead of each mouse packet
 * snapping on its own.
 */
#include "desktop.h"

#define CV_W 640
#define CV_H 420
#define TOOL_H 32
#define BAR_H  22 /* the hint strip along the bottom */

/* Toolbar geometry, client-relative.  The palette owns x+4..x+324; the
 * brush swatches and the Text button take the gap after it; Clear keeps
 * the right-hand end where it has always been.  Both strips are laid out
 * from constants rather than from cw, so resizing the window moves only
 * Clear. */
#define PT_SW_X   (4 + 16 * 20 + 6)           /* first brush swatch  */
#define PT_SW_N   4
#define PT_TX_X   (PT_SW_X + PT_SW_N * 20 + 4) /* the Text button     */
#define PT_TX_W   46
#define PT_BTN_H  18

enum { PT_PENCIL = 0, PT_TEXT = 1 };

/* The widths the swatches promise, in pixels.  A swatch shows a dot of
 * exactly this size, so what you see on the button is the line you get. */
static const int pt_brush[PT_SW_N] = {1, 2, 4, 8};

#define PT_HINT_PENCIL "Pencil: drag to draw.  Hold Shift for a straight line."
#define PT_HINT_TEXT   "Text: click the canvas, then type.  Esc finishes."

static const u32 pal[16] = {
    RGB(0, 0, 0),       RGB(128, 128, 128), RGB(128, 0, 0),   RGB(255, 0, 0),
    RGB(0, 128, 0),     RGB(0, 255, 0),     RGB(128, 128, 0), RGB(255, 255, 0),
    RGB(0, 0, 128),     RGB(0, 0, 255),     RGB(128, 0, 128), RGB(255, 0, 255),
    RGB(0, 128, 128),   RGB(0, 255, 255),   RGB(192, 192, 192), RGB(255, 255, 255),
};

struct paint {
    u32 *cv;
    int pen;             /* palette index in use                */
    int brush;           /* index into pt_brush                 */
    int tool;            /* PT_PENCIL or PT_TEXT                */
    int mode;            /* 1 = pencil down, 2 = erasing        */
    int lx, ly;          /* where the last stroke segment ended */
    char tbuf[2048];     /* the run being typed, not yet baked  */
    int tlen;
    int rx, ry;          /* where its next character goes, in canvas coordinates */
    int tpen;            /* palette index this run is typed in  */
    int vis_w, vis_h;    /* the canvas as this window shows it now */
    char status[192];
};

/* Where the canvas sits inside the window: between the palette strip and
 * the hint strip, never underneath either.  Oh/oh are what the window can
 * actually show -- the canvas is bigger, which is the whole point of
 * having a canvas of its own. */
static void cv_rect(const struct win *w, int *ox, int *oy, int *ow, int *oh)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    *ox = x + 4;
    *oy = y + TOOL_H;
    *ow = cw - 8;
    *oh = ch - TOOL_H - BAR_H - 4;
    if (*ow > CV_W) *ow = CV_W;
    if (*oh > CV_H) *oh = CV_H;
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

/* ---------------- the pencil ---------------- */

static void put(struct paint *p, int cx, int cy, u32 col)
{
    if (cx < 0 || cy < 0 || cx >= CV_W || cy >= CV_H) return;
    p->cv[(size_t)cy * CV_W + cx] = col;
}

/* One dab of the current brush: a square centred on the point, so the
 * line a stroke draws has the width the swatch promised at both ends. */
static void stamp(struct paint *p, int cx, int cy, u32 col)
{
    int t = pt_brush[p->brush];
    int x0 = cx - (t - 1) / 2, y0 = cy - (t - 1) / 2;
    for (int j = 0; j < t; j++)
        for (int i = 0; i < t; i++) put(p, x0 + i, y0 + j, col);
}

static void stroke(struct paint *p, int x0, int y0, int x1, int y1, u32 col)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        stamp(p, x0, y0, col);
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

/* ---------------- the text run ---------------- */

/* One character's worth of movement: the caret goes eight pixels right,
 * and takes the next line when this one has no room for another glyph.
 * Wrapping is against vis_w, not CV_W, because a line typed where the
 * window cannot see it is a line nobody typed. */
static void t_step(const struct paint *p, int *x, int *y)
{
    *x += 8;
    if (*x + 8 > p->vis_w) {
        *x = p->rx;
        *y += 16;
        if (*y > p->vis_h - 16) *y = p->vis_h - 16;
        if (*y < 0) *y = 0;
    }
}

static void t_caret(const struct paint *p, int *cx, int *cy)
{
    int x = p->rx, y = p->ry;
    for (int i = 0; i < p->tlen; i++) t_step(p, &x, &y);
    *cx = x;
    *cy = y;
}

/* The run over the canvas, one glyph at a time, exactly where it will be
 * baked, plus the bar that says where the next letter goes. */
static void t_paint(const struct paint *p, int ox, int oy)
{
    int x = p->rx, y = p->ry;
    u32 col = pal[p->tpen];
    for (int i = 0; i < p->tlen; i++) {
        font_draw(ox + x, oy + y, (unsigned char)p->tbuf[i], col, 0);
        t_step(p, &x, &y);
    }
    int cx, cy;
    t_caret(p, &cx, &cy);
    gfx_fill(ox + cx, oy + cy, 2, 16, COL_TEXT);
}

/* The same walk, written into the canvas for good. */
static void t_commit(struct paint *p)
{
    if (!p->tlen) return;
    int x = p->rx, y = p->ry;
    u32 col = pal[p->tpen];
    for (int i = 0; i < p->tlen; i++) {
        font_draw_buf(p->cv, CV_W, CV_H, CV_W, x, y, (unsigned char)p->tbuf[i], col);
        t_step(p, &x, &y);
    }
    p->tlen = 0;
}

/* ---------------- the window ---------------- */

static void pt_status(struct paint *p, const char *msg)
{
    p->status[0] = 0;
    str_cat_n(p->status, sizeof(p->status), msg);
}

static void pt_status2(struct paint *p, const char *verb, const char *path)
{
    p->status[0] = 0;
    str_cat_n(p->status, sizeof(p->status), verb);
    str_cat_n(p->status, sizeof(p->status), path);
}

static void pt_paint(struct win *w)
{
    struct paint *p = w->state;
    if (!p) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    /* palette */
    for (int i = 0; i < 16; i++) {
        int sx = x + 4 + i * 20, sy = y + 5;
        gfx_fill(sx, sy, 18, 18, pal[i]);
        gfx_out(sx, sy, 18, 18);
        if (p->pen == i) gfx_box(sx - 2, sy - 2, 22, 22, COL_DKSHADOW);
    }

    /* brush widths: each swatch is a button showing a dot that big */
    for (int i = 0; i < PT_SW_N; i++) {
        int bx = x + PT_SW_X + i * 20, by = y + 5;
        int t = pt_brush[i];
        gfx_fill(bx, by, 18, 18, COL_FACE);
        if (p->brush == i)
            gfx_in(bx, by, 18, 18);
        else
            gfx_out(bx, by, 18, 18);
        gfx_fill(bx + 9 - t / 2, by + 9 - t / 2, t, t, COL_TEXT);
    }

    /* Text: down while the canvas takes type, which is how you get out
     * of it again without reaching for the keyboard */
    int tx = x + PT_TX_X, ty = y + 5;
    gfx_glass_button(tx, ty, PT_TX_W, PT_BTN_H, p->tool == PT_TEXT, "Text");

    /* Clear */
    int bx = x + cw - 54, by = y + 5, bw = 50, bh = 22;
    gfx_glass_button(bx, by, bw, bh, 0, "Clear");

    /* canvas: only the rows the current damage rectangle actually covers */
    int ox, oy, ow, oh;
    cv_rect(w, &ox, &oy, &ow, &oh);
    p->vis_w = ow;
    p->vis_h = oh;
    gfx_fill(ox - 1, oy - 1, ow + 2, oh + 2, COL_FACE);
    gfx_in(ox - 1, oy - 1, ow + 2, oh + 2);

    int cl, ct, cc, cr;
    gfx_clip_get(&cl, &ct, &cc, &cr);
    int x0 = ox > cl ? ox : cl;
    int y0 = oy > ct ? oy : ct;
    int x1 = ox + ow < cl + cc ? ox + ow : cl + cc;
    int y1 = oy + oh < ct + cr ? oy + oh : ct + cr;
    for (int j = y0; j < y1; j++) {
        u32 *dst = &g_fb[(size_t)j * g_fw + x0];
        const u32 *src = &p->cv[(size_t)(j - oy) * CV_W + (x0 - ox)];
        for (int i = x0; i < x1; i++) *dst++ = *src++;
    }

    /* the run in progress, held to the canvas rectangle: the bottom
     * strip and the palette are not part of the picture */
    if (p->tool == PT_TEXT) {
        gfx_clip_intersect(ox, oy, ow, oh);
        t_paint(p, ox, oy);
        gfx_clip(cl, ct, cc, cr);
    }

    /* the hint strip: whatever the last action had to say, or -- when it
     * has nothing to say -- the advice for the tool in use, so the bar
     * is never a dead 22 pixels */
    int fy0 = y + ch - BAR_H;
    gfx_fill(x, fy0, cw, BAR_H, COL_FACE);
    gfx_hline(x, fy0, cw, COL_HILITE);
    const char *hint = p->status[0] ? p->status
                                    : (p->tool == PT_TEXT ? PT_HINT_TEXT : PT_HINT_PENCIL);
    gfx_text_w(x + 4, fy0 + 3, hint, COL_TEXT, cw - 8);
}

/* ---------------- picking things ---------------- */

/* Anything that marks the canvas invalidates the line of hint under it:
 * advice that has already been followed is just noise. */
static void touched(struct paint *p)
{
    p->status[0] = 0;
}

/* A colour picked mid-sentence finishes the run where it stands -- in
 * the colour it was typed in -- and starts the next one here, in the new
 * one, without the caret moving. */
static void pen_pick(struct paint *p, int i)
{
    p->pen = i;
    if (p->tool == PT_TEXT && p->tlen) {
        int cx, cy;
        t_caret(p, &cx, &cy);
        t_commit(p);
        p->rx = cx;
        p->ry = cy;
    }
    p->tpen = i;
}

static void tool_pick(struct paint *p, int t)
{
    if (t == p->tool) return;
    t_commit(p);
    p->tool = t;
    pt_status(p, t == PT_TEXT ? PT_HINT_TEXT : PT_HINT_PENCIL);
}

/* ---------------- the file ---------------- */

static int rd_byte(long fd, char *c) { return kread(fd, c, 1) == 1; }

/* The next whitespace-delimited token, '#' comments skipped.  The
 * delimiter is consumed, which is what keeps the pixel data starting
 * exactly where the header said it would. */
static int rd_token(long fd, char *tok, int cap)
{
    int n = 0;
    for (;;) {
        char c;
        if (!rd_byte(fd, &c)) return 0;
        if (c == '#') {
            while (rd_byte(fd, &c) && c != '\n') {}
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (n) break;
            continue;
        }
        if (n < cap - 1)
            tok[n++] = c;
        else
            return 0;
    }
    tok[n] = 0;
    return 1;
}

static int tok_int(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* Show a PPM.  Only ever called from pt_open, before there is a run to
 * finish or a pixel to keep, which is why it overwrites the canvas
 * outright and keeps none of it. */
static int pt_load(struct win *w, const char *path)
{
    struct paint *p = w->state;
    if (!p) return 0;

    char msg[96];
    msg[0] = 0;
    long fd = fileio_open(path, msg, sizeof(msg));
    if (fd < 0) {
        pt_status(p, msg);
        return 0;
    }

    char tok[32];
    int iw = 0, ih = 0;
    int bad = !rd_token(fd, tok, sizeof(tok)) || tok[0] != 'P' || tok[1] != '6' || tok[2];
    if (!bad && (!rd_token(fd, tok, sizeof(tok)) || (iw = tok_int(tok)) <= 0)) bad = 1;
    if (!bad && (!rd_token(fd, tok, sizeof(tok)) || (ih = tok_int(tok)) <= 0)) bad = 1;
    if (!bad && (!rd_token(fd, tok, sizeof(tok)) || tok_int(tok) != 255)) bad = 1;
    /* 4096 is the ceiling on both axes: past it the header would be
     * promising more than this program is willing to allocate. */
    if (!bad && (iw > 4096 || ih > 4096)) bad = 1;
    if (bad) {
        kclose(fd);
        pt_status(p, "Not a P6 PPM image (or too large).");
        return 0;
    }

    char *row = (char *)xmalloc((size_t)iw * 3);
    if (!row) {
        kclose(fd);
        pt_status(p, "Out of memory");
        return 0;
    }

    int wpx = iw > CV_W ? CV_W : iw;
    int hpx = ih > CV_H ? CV_H : ih;
    for (size_t i = 0; i < (size_t)CV_W * CV_H; i++) p->cv[i] = COL_EDITBG;

    int ok = 1;
    for (int ry = 0; ok && ry < hpx; ry++) {
        int need = iw * 3;
        char *d = row;
        while (need > 0) {
            long r = kread(fd, d, need);
            if (r <= 0) {
                ok = 0;
                break;
            }
            d += r;
            need -= (int)r;
        }
        if (!ok) break;
        u32 *dst = &p->cv[(size_t)ry * CV_W];
        const unsigned char *s = (const unsigned char *)row;
        for (int rx = 0; rx < wpx; rx++) {
            dst[rx] = RGB(s[0], s[1], s[2]);
            s += 3;
        }
    }
    kclose(fd);
    xfree(row);

    if (!ok) {
        pt_status(p, "The image ends early.");
        return 0;
    }

    /* the title bar names the picture, since there is no file box left
     * to show the path in */
    char t[48], base[48];
    fileio_base(path, base, sizeof(base));
    t[0] = 0;
    str_cat_n(t, sizeof(t), base);
    str_cat_n(t, sizeof(t), " - Paint");
    win_set_title(w, t);
    pt_status2(p, "Opened ", path);
    return 1;
}

/* ---------------- drawing ---------------- */

static void pt_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    struct paint *p = w->state;
    if (!p) return;
    (void)clicks;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int px = x + cx, py = y + cy;

    /* The toolbar first: a click in the chrome never lands on a pixel.
     * Anything here that is not a control falls through to the canvas
     * test below, which rejects it for being outside the canvas. */
    if (btn == LNXRM_MB_LEFT && py >= y + 5 && py < y + 27) {
        if (px >= x + 4 && px < x + 4 + 16 * 20) {
            int i = (px - (x + 4)) / 20;
            if (i >= 0 && i <= 15) pen_pick(p, i);
            return;
        }
        if (px >= x + PT_SW_X && px < x + PT_SW_X + PT_SW_N * 20) {
            int i = (px - x - PT_SW_X) / 20;
            if (i >= 0 && i < PT_SW_N) p->brush = i;
            return;
        }
        if (px >= x + PT_TX_X && px < x + PT_TX_X + PT_TX_W) {
            tool_pick(p, p->tool == PT_TEXT ? PT_PENCIL : PT_TEXT);
            return;
        }
        if (px >= x + cw - 54 && px < x + cw - 4) {
            t_commit(p);
            touched(p);
            for (size_t i = 0; i < (size_t)CV_W * CV_H; i++) p->cv[i] = COL_EDITBG;
            return;
        }
    }

    if (btn != LNXRM_MB_LEFT && btn != LNXRM_MB_RIGHT) return;

    int ox, oy, ow, oh;
    cv_rect(w, &ox, &oy, &ow, &oh);
    int gx = px - ox, gy = py - oy;
    if (gx < 0 || gy < 0 || gx >= ow || gy >= oh) return; /* the hint strip */

    if (p->tool == PT_TEXT) {
        if (btn != LNXRM_MB_LEFT) return;
        /* Move the caret: whatever was being typed is written where it
         * was, and the new run starts here. */
        t_commit(p);
        p->rx = gx;
        /* a glyph has to fit where the caret starts, or every character
         * would wrap onto a line of its own */
        if (p->rx + 8 > p->vis_w) p->rx = p->vis_w - 8;
        if (p->rx < 0) p->rx = 0;
        p->ry = gy;
        if (p->ry > p->vis_h - 16) p->ry = p->vis_h - 16;
        if (p->ry < 0) p->ry = 0;
        p->tlen = 0;
        p->tpen = p->pen;
        return;
    }

    t_commit(p); /* the pencil goes over the text, not under it */
    touched(p);
    p->mode = (btn == LNXRM_MB_LEFT) ? 1 : 2;
    p->lx = gx;
    p->ly = gy;
    stamp(p, gx, gy, p->mode == 1 ? pal[p->pen] : COL_EDITBG);
}

static void pt_drag(struct win *w, int cx, int cy, int dx, int dy)
{
    struct paint *p = w->state;
    if (!p || !p->mode) return;
    (void)dx;
    (void)dy;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int gx = x + cx;
    int gy = y + cy;
    int ox, oy, ow, oh;
    cv_rect(w, &ox, &oy, &ow, &oh);
    gx -= ox;
    gy -= oy;

    /* Shift: the segment, not the packet, is snapped -- to horizontal,
     * to vertical, or to the diagonal when the two are close.  The
     * snapped point then becomes the origin for the next segment, so a
     * line held straight stays straight however long it gets. */
    if (desktop_shift()) {
        int ddx = gx - p->lx, ddy = gy - p->ly;
        int ax = ddx < 0 ? -ddx : ddx, ay = ddy < 0 ? -ddy : ddy;
        if (ax >= ay * 2) {
            gy = p->ly;
        } else if (ay >= ax * 2) {
            gx = p->lx;
        } else {
            int m = ax < ay ? ax : ay;
            gx = p->lx + (ddx < 0 ? -m : m);
            gy = p->ly + (ddy < 0 ? -m : m);
        }
    }

    /* Line between the last and this point: without it a fast drag draws
     * a dotted trail, because the mouse generates packets, not pixels. */
    u32 col = p->mode == 1 ? pal[p->pen] : COL_EDITBG;
    stroke(p, p->lx, p->ly, gx, gy, col);
    p->lx = gx;
    p->ly = gy;
}

static void pt_up(struct win *w, int cx, int cy, int btn)
{
    struct paint *p = w->state;
    if (!p) return;
    (void)cx;
    (void)cy;
    (void)btn;
    p->mode = 0;
}

static void pt_key(struct win *w, const struct dsk_event *ev)
{
    struct paint *p = w->state;
    if (!p || ev->type != LNXRM_EV_KEY_DOWN) return;
    if (p->tool != PT_TEXT) return; /* the pencil has nothing to type into */

    switch (ev->code) {
    case LNXRM_KEY_BACKSPACE:
        if (p->tlen) p->tlen--;
        return;
    case LNXRM_KEY_ENTER: {
        /* The line ends here; the next one starts under this run's left
         * edge, one glyph down, in whatever colour is picked by then. */
        int cx, cy;
        t_caret(p, &cx, &cy);
        t_commit(p);
        p->rx = cx;
        p->ry = cy + 16;
        if (p->ry > p->vis_h - 16) p->ry = p->vis_h - 16;
        if (p->ry < 0) p->ry = 0;
        p->tpen = p->pen;
        return;
    }
    case LNXRM_KEY_ESC:
        t_commit(p);
        tool_pick(p, PT_PENCIL);
        return;
    default:
        break;
    }

    char c = (char)ev->ch;
    if (c >= 32 && c < 127 && p->tlen < (int)sizeof(p->tbuf) - 1)
        p->tbuf[p->tlen++] = c;
}

static void pt_open(struct win *w)
{
    struct paint *p = (struct paint *)xcalloc(1, sizeof(struct paint));
    if (!p) return;
    p->cv = (u32 *)xmalloc((size_t)CV_W * CV_H * 4);
    if (!p->cv) {
        xfree(p);
        return;
    }
    for (size_t i = 0; i < (size_t)CV_W * CV_H; i++) p->cv[i] = COL_EDITBG;
    p->pen = 0;
    p->brush = 0;
    p->tool = PT_PENCIL;
    p->tpen = 0;
    p->vis_w = CV_W;
    p->vis_h = CV_H;
    w->state = p;

    const char *arg = app_open_arg(); /* File Explorer handed us a .ppm */
    if (arg) pt_load(w, arg);
}

static void pt_close(struct win *w)
{
    struct paint *p = w->state;
    if (!p) return;
    /* Nothing to flush: without a Save, the canvas and the run in it
     * both belong to this window and go with it. */
    if (p->cv) xfree(p->cv);
    xfree(p);
    w->state = 0;
}

const struct app app_paint = {
    .name = "Paint",
    .title = "untitled - Paint",
    .w = 560,
    .h = 420,
    .open = pt_open,
    .paint = pt_paint,
    .down = pt_down,
    .drag = pt_drag,
    .up = pt_up,
    .key = pt_key,
    .close = pt_close,
};
