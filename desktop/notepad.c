/* Notepad: a scrollable, editable plain-text buffer with a File bar.
 *
 * The buffer is one NUL-less byte range with '\n' as the only line
 * terminator; lines are found by scanning rather than by an index, which
 * costs O(n) per lookup but keeps insert and delete a single memmove and
 * means there is no line table to keep in sync when a line splits in two.
 * At the sizes this window can hold (a few kilobytes) the scan is
 * invisible next to the drawing.
 *
 * Saving and loading are the point of the File bar along the top: type a
 * name, press Save, press Enter in the box to open it again.  The file
 * sits on the same FAT32 volume the shell sees, so `cat /notes.txt` on
 * the console reads back what was just written -- nothing about a save is
 * private to this process.
 */
#include "desktop.h"

#define NP_MIN_CAP 4096

/* Client layout: File bar across the top, status line along the bottom,
 * the text in between. */
#define NP_BAR_H   24
#define NP_STAT_H  20
#define NP_BTN_W   50
#define NP_BTN_H   18
#define NP_BAR_PAD 4 /* gap between buttons and before the field */
#define NP_FIELD_X 214

enum { BTN_NEW = 0, BTN_OPEN = 1, BTN_SAVE = 2, BTN_N = 3 };
static const char *const np_btn[BTN_N] = {"New", "Open", "Save"};

struct note {
    char *buf;
    int len, cap;
    int caret; /* byte offset */
    int top;   /* byte offset of the first visible line */
    struct pathedit fe;
    int dirty;      /* changed since the last save or load */
    char status[192]; /* the last thing that happened to the file */
};

static int line_of(const struct note *n, int pos)
{
    int l = 0;
    for (int i = 0; i < pos && i < n->len; i++)
        if (n->buf[i] == '\n') l++;
    return l;
}

static int line_count(const struct note *n)
{
    int l = 1;
    for (int i = 0; i < n->len; i++)
        if (n->buf[i] == '\n') l++;
    return l;
}

static int line_start(const struct note *n, int line)
{
    if (line <= 0) return 0;
    int l = 0;
    for (int i = 0; i < n->len; i++)
        if (n->buf[i] == '\n' && ++l == line) return i + 1;
    return n->len;
}

/* [start, end) of a line, excluding the '\n' */
static void line_span(const struct note *n, int line, int *s, int *e)
{
    *s = line_start(n, line);
    int i = *s;
    while (i < n->len && n->buf[i] != '\n') i++;
    *e = i;
}

/* The text area is what is left of the client after the two bars. */
static int vis_rows(const struct win *w)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int r = (ch - NP_BAR_H - NP_STAT_H - 4) / 16;
    return r > 0 ? r : 1;
}

static int vis_cols(const struct win *w)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int c = (cw - 6) / 8;
    return c > 0 ? c : 1;
}

/* Keep the caret line inside the visible band. */
static void scroll_into_view(struct win *w, struct note *n)
{
    int rows = vis_rows(w);
    int cl = line_of(n, n->caret);
    int tl = line_of(n, n->top);
    if (cl < tl)
        n->top = line_start(n, cl);
    else if (cl >= tl + rows)
        n->top = line_start(n, cl - rows + 1);
}

static void ensure(struct note *n, int extra)
{
    if (n->len + extra + 1 <= n->cap) return;
    int cap = n->cap ? n->cap : NP_MIN_CAP;
    while (cap < n->len + extra + 1) cap *= 2;
    char *b = (char *)xrealloc(n->buf, (size_t)cap);
    if (!b) return;
    n->buf = b;
    n->cap = cap;
}

static void insert(struct note *n, const char *s, int k)
{
    ensure(n, k);
    if (n->len + k + 1 > n->cap) return;
    memmove(n->buf + n->caret + k, n->buf + n->caret, (size_t)(n->len - n->caret));
    memcpy(n->buf + n->caret, s, (size_t)k);
    n->caret += k;
    n->len += k;
    n->dirty = 1;
}

static void del_before(struct note *n)
{
    if (n->caret <= 0) return;
    memmove(n->buf + n->caret - 1, n->buf + n->caret, (size_t)(n->len - n->caret));
    n->caret--;
    n->len--;
    n->dirty = 1;
}

static void del_at(struct note *n)
{
    if (n->caret >= n->len) return;
    memmove(n->buf + n->caret, n->buf + n->caret + 1, (size_t)(n->len - n->caret - 1));
    n->len--;
    n->dirty = 1;
}

/* ---------------- the File bar ---------------- */

static void btn_rect(const struct win *w, int i, int *bx, int *by, int *bw, int *bh)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    *bx = x + NP_BAR_PAD + i * (NP_BTN_W + NP_BAR_PAD);
    *by = y + 3;
    *bw = NP_BTN_W;
    *bh = NP_BTN_H;
}

static void field_rect(const struct win *w, int *fx, int *fy, int *fw, int *fh)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    *fx = x + NP_FIELD_X;
    *fy = y + 3;
    *fw = (x + cw - NP_BAR_PAD) - *fx;
    *fh = NP_BTN_H;
    if (*fw < 40) *fw = 40; /* clipped if the window is that narrow anyway */
}

static void np_bar(struct win *w, struct note *n)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);

    gfx_fill(x, y, cw, NP_BAR_H, COL_FACE);
    gfx_hline(x, y + NP_BAR_H - 1, cw, COL_SHADOW);

    for (int i = 0; i < BTN_N; i++) {
        int bx, by, bw, bh;
        btn_rect(w, i, &bx, &by, &bw, &bh);
        gfx_glass_button(bx, by, bw, bh, 0, np_btn[i]);
    }

    gfx_text(x + 170, y + 4, "File:", COL_TEXT);

    int fx, fy, fw, fh;
    field_rect(w, &fx, &fy, &fw, &fh);
    pathedit_paint(&n->fe, fx, fy, fw, fh);
}

static void np_statusline(struct win *w, struct note *n)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int sy = y + ch - NP_STAT_H;
    gfx_fill(x, sy, cw, NP_STAT_H, COL_FACE);
    gfx_in(x, sy, cw, NP_STAT_H);

    int line = line_of(n, n->caret);
    int s, e;
    line_span(n, line, &s, &e);

    char st[256];
    st[0] = 0;
    str_cat(st, "Ln ");
    str_cat_i(st, line + 1);
    str_cat(st, ", Col ");
    str_cat_i(st, n->caret - s + 1);
    str_cat(st, "   ");
    str_cat_n(st, sizeof(st), n->fe.text[0] ? n->fe.text : "untitled");
    if (n->dirty) str_cat_n(st, sizeof(st), "  *modified*");
    if (n->status[0]) {
        str_cat_n(st, sizeof(st), "   ");
        str_cat_n(st, sizeof(st), n->status);
    }
    gfx_text_w(x + 5, sy + 3, st, COL_TEXT, cw - 10);
}

static void np_paint(struct win *w)
{
    struct note *n = w->state;
    if (!n) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);

    np_bar(w, n);

    int ty = y + NP_BAR_H;
    int th = ch - NP_BAR_H - NP_STAT_H;
    if (th < 16) th = 16;
    gfx_fill(x, ty, cw, th, COL_EDITBG);

    int cols = vis_cols(w), rows = vis_rows(w);
    int top_line = line_of(n, n->top);
    int caret_line = line_of(n, n->caret);

    for (int r = 0; r < rows; r++) {
        int line = top_line + r;
        if (line >= line_count(n)) break;
        int s, e;
        line_span(n, line, &s, &e);
        int lim = e - s;
        if (lim > cols) lim = cols;
        for (int i = 0; i < lim; i++) {
            char c = n->buf[s + i];
            if (c < 32 || c > 126) c = ' ';
            font_draw(x + 3 + i * 8, ty + 2 + r * 16, (unsigned char)c, COL_TEXT, 0);
        }
    }

    /* caret: a solid 2 px bar.  A blinking one would need a timer, and a
     * timer that repaints a window nobody is looking at is worse than a
     * caret that never blinks. */
    if (caret_line >= top_line && caret_line < top_line + rows) {
        int s, e;
        line_span(n, caret_line, &s, &e);
        int col = n->caret - s;
        if (col > cols) col = cols;
        gfx_fill(x + 3 + col * 8, ty + 2 + (caret_line - top_line) * 16, 2, 16, COL_TEXT);
    }

    np_statusline(w, n);
}

/* ---------------- the file ---------------- */

static void np_set_status(struct note *n, const char *msg)
{
    n->status[0] = 0;
    str_cat_n(n->status, sizeof(n->status), msg);
}

static void np_set_status2(struct note *n, const char *verb, const char *path)
{
    n->status[0] = 0;
    str_cat_n(n->status, sizeof(n->status), verb);
    str_cat_n(n->status, sizeof(n->status), path);
}

static void np_title(struct win *w, struct note *n)
{
    char t[48];
    t[0] = 0;
    if (n->fe.text[0]) {
        char base[48];
        fileio_base(n->fe.text, base, sizeof(base));
        str_cat_n(t, sizeof(t), base);
        str_cat_n(t, sizeof(t), " - Notepad");
    } else {
        str_cat_n(t, sizeof(t), "Untitled - Notepad");
    }
    win_set_title(w, t);
}

static void np_save(struct win *w)
{
    struct note *n = w->state;
    if (!n) return;
    if (!n->fe.text[0]) {
        n->fe.focus = 1; /* nothing to save *to* until a name exists */
        np_set_status(n, "Type a file name, then press Save.");
        return;
    }
    char msg[96];
    msg[0] = 0;
    if (fileio_write(n->fe.text, n->buf, n->len, msg, sizeof(msg)) < 0) {
        np_set_status(n, msg);
        return;
    }
    n->dirty = 0;
    np_set_status2(n, "Saved ", n->fe.text);
    np_title(w, n);
}

static void np_load(struct win *w)
{
    struct note *n = w->state;
    if (!n) return;
    if (!n->fe.text[0]) {
        n->fe.focus = 1;
        np_set_status(n, "Type a file name, then press Open.");
        return;
    }

    char msg[96];
    msg[0] = 0;
    int len = 0, cap = 0;
    char *b = fileio_read(n->fe.text, &len, &cap, msg, sizeof(msg));
    if (!b) {
        np_set_status(n, msg);
        return;
    }

    /* CRLF is still one document: drop the carriage returns rather than
     * showing a column of boxes down the right edge. */
    int out = 0;
    for (int i = 0; i < len; i++)
        if (b[i] != '\r') b[out++] = b[i];
    b[out] = 0;
    len = out;

    xfree(n->buf);
    n->buf = b;
    n->len = len;
    n->cap = cap; /* fileio_read reports what it actually allocated */
    n->caret = 0;
    n->top = 0;
    n->dirty = 0;
    n->fe.focus = 0;
    np_set_status2(n, "Opened ", n->fe.text);
    np_title(w, n);
}

static void np_new(struct win *w)
{
    struct note *n = w->state;
    if (!n) return;
    n->len = 0;
    n->caret = 0;
    n->top = 0;
    n->dirty = 0;
    pathedit_set(&n->fe, "");
    n->fe.focus = 0;
    np_set_status(n, "New document");
    np_title(w, n);
}

/* ---------------- editing ---------------- */

static void caret_left(struct note *n)
{
    if (n->caret > 0) n->caret--;
}

static void caret_right(struct note *n)
{
    if (n->caret < n->len) n->caret++;
}

static void caret_home(struct note *n)
{
    int l = line_of(n, n->caret);
    n->caret = line_start(n, l);
}

static void caret_end(struct note *n)
{
    int l = line_of(n, n->caret);
    int s, e;
    line_span(n, l, &s, &e);
    n->caret = e;
}

static void caret_vert(struct note *n, int dir)
{
    int l = line_of(n, n->caret);
    int s, e;
    line_span(n, l, &s, &e);
    int col = n->caret - s;
    int target = l + dir;
    if (target < 0) return;
    if (target >= line_count(n)) return;
    int ts, te;
    line_span(n, target, &ts, &te);
    int len = te - ts;
    n->caret = ts + (col < len ? col : len);
}

static void np_key(struct win *w, const struct dsk_event *ev)
{
    if (ev->type != LNXRM_EV_KEY_DOWN) return;
    struct note *n = w->state;
    if (!n) return;

    if (n->fe.focus) {
        int enter = 0;
        pathedit_key(&n->fe, ev, &enter);
        if (enter) np_load(w); /* Enter in a file box opens, everywhere else */
        return;                 /* the box keeps every other key to itself */
    }

    switch (ev->code) {
    case LNXRM_KEY_ENTER: {
        char nl = '\n';
        insert(n, &nl, 1);
        break;
    }
    case LNXRM_KEY_BACKSPACE: del_before(n); break;
    case LNXRM_KEY_DELETE: del_at(n); break;
    case LNXRM_KEY_LEFT: caret_left(n); break;
    case LNXRM_KEY_RIGHT: caret_right(n); break;
    case LNXRM_KEY_HOME: caret_home(n); break;
    case LNXRM_KEY_END: caret_end(n); break;
    case LNXRM_KEY_UP: caret_vert(n, -1); break;
    case LNXRM_KEY_DOWN: caret_vert(n, 1); break;
    case LNXRM_KEY_PGUP: caret_vert(n, -vis_rows(w)); break;
    case LNXRM_KEY_PGDN: caret_vert(n, vis_rows(w)); break;
    default: {
        char c = (char)ev->ch;
        if (c >= 32 && c < 127) insert(n, &c, 1);
        break;
    }
    }
    scroll_into_view(w, n);
}

static void np_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    struct note *n = w->state;
    (void)clicks;
    if (!n || btn != LNXRM_MB_LEFT) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    int px = x + cx, py = y + cy;

    if (cy < NP_BAR_H) {
        for (int i = 0; i < BTN_N; i++) {
            int bx, by, bw, bh;
            btn_rect(w, i, &bx, &by, &bw, &bh);
            if (px < bx || px >= bx + bw || py < by || py >= by + bh) continue;
            if (i == BTN_NEW) np_new(w);
            else if (i == BTN_OPEN) np_load(w);
            else np_save(w);
            return;
        }
        int fx, fy, fw, fh;
        field_rect(w, &fx, &fy, &fw, &fh);
        if (pathedit_down(&n->fe, px, py, fx, fy, fw, fh)) return;
        n->fe.focus = 0; /* clicked the bar but not the box: keys go to the text */
        return;
    }

    if (cy >= ch - NP_STAT_H) return; /* the status line explains, it does not act */

    int cols = vis_cols(w);
    n->fe.focus = 0;
    int top_line = line_of(n, n->top);
    int row = (cy - NP_BAR_H - 2) / 16;
    if (row < 0) row = 0;
    int line = top_line + row;
    if (line >= line_count(n)) line = line_count(n) - 1;
    int s, e;
    line_span(n, line, &s, &e);
    int col = (cx - 3) / 8;
    if (col < 0) col = 0;
    if (col > cols) col = cols;
    int len = e - s;
    n->caret = s + (col < len ? col : len);
    scroll_into_view(w, n);
}

static void np_open(struct win *w)
{
    struct note *n = (struct note *)xcalloc(1, sizeof(struct note));
    if (!n) return;
    n->cap = NP_MIN_CAP;
    n->buf = (char *)xmalloc((size_t)n->cap);
    if (!n->buf) {
        xfree(n);
        return;
    }
    /* A name from the start, so the very first Save has somewhere to go
     * and nobody meets a status line asking them to invent one. */
    pathedit_init(&n->fe, "/notes.txt");
    n->dirty = 0;
    np_set_status(n, "Save writes, Enter in the box opens.");

    const char *arg = app_open_arg(); /* File Explorer asked for this one */
    if (arg) {
        pathedit_set(&n->fe, arg);
        w->state = n;
        np_load(w);
        return;
    }

    static const char welcome[] =
        "LnxrmOS Notepad\n\n"
        "Type here.  The mouse wheel moves the caret a line at a time,\n"
        "Backspace erases, and Enter starts a new line.\n\n"
        "The File bar at the top is where the document is named: type a\n"
        "path, press Save to write it, press Enter in the box to open it.\n"
        "New clears the window for a fresh document.\n\n"
        "The window can be dragged by its title bar and resized from the\n"
        "bottom-right corner.";
    int len = 0;
    while (welcome[len]) len++;
    memcpy(n->buf, welcome, (size_t)len);
    n->len = len;
    n->caret = 0;
    n->dirty = 0;
    w->state = n;
}

static void np_close(struct win *w)
{
    struct note *n = w->state;
    if (!n) return;
    if (n->buf) xfree(n->buf);
    xfree(n);
    w->state = 0;
}

const struct app app_notepad = {
    .name = "Notepad",
    .title = "Untitled - Notepad",
    .w = 460,
    .h = 320,
    .open = np_open,
    .paint = np_paint,
    .down = np_down,
    .drag = 0,
    .up = 0,
    .key = np_key,
    .close = np_close,
};
