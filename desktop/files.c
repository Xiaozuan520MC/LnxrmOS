/* File browser: a directory listing with a path bar, double-click to
 * descend and an Up button to climb back out.
 *
 * The listing is re-read from the filesystem every time the directory
 * changes rather than watched -- this kernel has no inotify, and a
 * desktop that lies about a directory it has not re-read is worse than
 * one that only refreshes when you navigate. */
#include "desktop.h"

#define MAX_ENT 96
#define TOOL_H 26

struct files {
    char path[128];
    struct lnxrm_dirent *ent;
    int count;
    int sel;
    int top; /* first visible row */
};

static int is_dir(const struct lnxrm_dirent *e)
{
    return e->d_type == 4;
}

static void join_path(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = xstrlen(dir);
    size_t m = xstrlen(name);
    if (n + m + 2 > cap) return;
    if (n == 1 && dir[0] == '/') {
        out[0] = '/';
        memcpy(out + 1, name, m + 1);
    } else {
        memcpy(out, dir, n);
        out[n] = '/';
        memcpy(out + n + 1, name, m + 1);
    }
}

static void parent_path(char *p)
{
    int n = (int)xstrlen(p);
    if (n <= 1) {
        p[0] = '/';
        p[1] = 0;
        return;
    }
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    while (n > 1 && p[n - 1] != '/') p[--n] = 0;
    if (n > 1) p[n - 1] = 0;
}

static void load(struct files *f, const char *path)
{
    long fd = kopen(path, 0);
    if (fd < 0) return;
    f->count = 0;
    if (!f->ent) f->ent = (struct lnxrm_dirent *)xmalloc(sizeof(*f->ent) * MAX_ENT);
    if (f->ent) {
        struct lnxrm_dirent e;
        while (f->count < MAX_ENT && kgetdent(fd, &e, sizeof(e)) > 0) f->ent[f->count++] = e;
    }
    kclose(fd);
    strncpy(f->path, path, sizeof(f->path) - 1);
    f->path[sizeof(f->path) - 1] = 0;
    f->sel = 0;
    f->top = 0;
}

/* The listing box: everything below the path bar. */
static void list_rect(const struct win *w, int *lx, int *ly, int *lw, int *lh)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    *lx = x + 4;
    *ly = y + TOOL_H + 4;
    *lw = cw - 8;
    *lh = ch - TOOL_H - 10;
    if (*lh < 16) *lh = 16;
}

static int rows_of(const struct win *w)
{
    int lx, ly, lw, lh;
    list_rect(w, &lx, &ly, &lw, &lh);
    int r = (lh - 4) / 16;
    return r > 0 ? r : 1;
}

static void scroll_into_view(struct files *f, int rows)
{
    if (f->sel < f->top) f->top = f->sel;
    if (f->sel >= f->top + rows) f->top = f->sel - rows + 1;
    if (f->top < 0) f->top = 0;
}

static void fl_paint(struct win *w)
{
    struct files *f = w->state;
    if (!f) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    /* Up button */
    gfx_glass_button(x + 4, y + 2, 44, TOOL_H - 4, 0, "Up");

    /* path */
    int px = x + 54, pw = cw - 58;
    if (pw < 20) pw = 20;
    gfx_fill(px, y + 2, pw, TOOL_H - 4, COL_EDITBG);
    gfx_in(px, y + 2, pw, TOOL_H - 4);
    gfx_text(px + 4, y + 2 + (TOOL_H - 4 - 16) / 2, f->path, COL_TEXT);

    /* listing */
    int lx, ly, lw, lh;
    list_rect(w, &lx, &ly, &lw, &lh);
    int rows = rows_of(w);
    gfx_fill(lx, ly, lw, lh, COL_LISTBG);
    gfx_in(lx, ly, lw, lh);
    scroll_into_view(f, rows);

    for (int r = 0; r < rows; r++) {
        int i = f->top + r;
        if (i >= f->count) break;
        int ry = ly + 2 + r * 16;
        if (i == f->sel) {
            gfx_fill(lx + 2, ry, lw - 4, 16, COL_SEL);
        }
        const char *nm = f->ent[i].d_name;
        char label[80];
        int n = 0;
        for (; nm[n] && n < (int)sizeof(label) - 3; n++) label[n] = nm[n];
        if (is_dir(&f->ent[i]) && n < (int)sizeof(label) - 2) label[n++] = '/';
        label[n] = 0;
        int maxch = (lw - 30) / 8;
        if (n > maxch) {
            if (maxch > 4) {
                label[maxch - 3] = '.';
                label[maxch - 2] = '.';
                label[maxch - 1] = '.';
                label[maxch] = 0;
            }
        }
        /* folder / document marks: 8x8 chips, enough to tell them apart
         * without an icon set */
        u32 chip = is_dir(&f->ent[i]) ? RGB(255, 200, 64) : RGB(220, 220, 220);
        gfx_fill(lx + 5, ry + 3, 10, 10, chip);
        gfx_box(lx + 5, ry + 3, 10, 10, COL_SHADOW);
        gfx_text_w(lx + 20, ry, label, i == f->sel ? COL_HILITE : COL_TEXT,
                   lw - 24);
    }
    (void)ch;
}

/* Which program owns this file, and the extension test behind it. */
static const struct app *app_for_file(const char *nm);

static void open_entry(struct files *f, int i)
{
    if (i < 0 || i >= f->count) return;
    char np[128];
    join_path(np, sizeof(np), f->path, f->ent[i].d_name);

    if (!is_dir(&f->ent[i])) {
        /* A file: hand it to the program that knows what to do with it.
         * Directories are the only thing this browser opens itself --
         * everything else has an owner, and finding out here means
         * Notepad and Paint never have to go looking for files. */
        const struct app *a = app_for_file(f->ent[i].d_name);
        if (a) app_open_file(a, np);
        return;
    }
    load(f, np);
}

/* Extension matching, lowercased as it compares: the point is that
 * "NOTES.TXT" and "notes.txt" are the same idea to a person, so they had
 * better be the same idea here. */
static int ext_is(const char *nm, const char *ext)
{
    const char *dot = 0;
    for (const char *p = nm; *p; p++)
        if (*p == '.') dot = p;
    if (!dot) return 0;
    dot++;
    for (int i = 0; ext[i]; i++) {
        char c = dot[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != ext[i]) return 0;
    }
    return dot[xstrlen(ext)] == 0;
}

/* Which program owns this file.  Nothing owns it: 0, and a double-click
 * does nothing rather than opening a binary in a text editor. */
static const struct app *app_for_file(const char *nm)
{
    if (ext_is(nm, "ppm")) return &app_paint;
    if (ext_is(nm, "txt") || ext_is(nm, "md") || ext_is(nm, "log") ||
        ext_is(nm, "py") || ext_is(nm, "sh") || ext_is(nm, "c") ||
        ext_is(nm, "h") || ext_is(nm, "def"))
        return &app_notepad;
    return 0;
}

static void fl_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    struct files *f = w->state;
    if (!f) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    (void)cw;
    (void)ch;
    int px = x + cx, py = y + cy;

    if (btn == LNXRM_MB_LEFT && px >= x + 4 && px < x + 48 && py >= y + 2 &&
        py < y + TOOL_H - 2) {
        parent_path(f->path);
        char save[128];
        strcpy(save, f->path);
        load(f, save);
        return;
    }

    int lx, ly, lw, lh;
    list_rect(w, &lx, &ly, &lw, &lh);
    if (px < lx || py < ly || px >= lx + lw || py >= ly + lh) return;
    int r = (py - ly - 2) / 16;
    int i = f->top + r;
    if (i < 0 || i >= f->count) return;
    f->sel = i;
    if (clicks >= 2 && btn == LNXRM_MB_LEFT) open_entry(f, i);
}

static void fl_key(struct win *w, const struct dsk_event *ev)
{
    if (ev->type != LNXRM_EV_KEY_DOWN) return;
    struct files *f = w->state;
    if (!f || !f->count) return;
    int rows = rows_of(w);
    switch (ev->code) {
    case LNXRM_KEY_UP: if (f->sel > 0) f->sel--; break;
    case LNXRM_KEY_DOWN: if (f->sel < f->count - 1) f->sel++; break;
    case LNXRM_KEY_PGUP:
        f->sel -= rows;
        if (f->sel < 0) f->sel = 0;
        break;
    case LNXRM_KEY_PGDN:
        f->sel += rows;
        if (f->sel > f->count - 1) f->sel = f->count - 1;
        break;
    case LNXRM_KEY_HOME: f->sel = 0; break;
    case LNXRM_KEY_END: f->sel = f->count - 1; break;
    case LNXRM_KEY_ENTER: open_entry(f, f->sel); break;
    case LNXRM_KEY_BACKSPACE: {
        char save[128];
        parent_path(f->path);
        strcpy(save, f->path);
        load(f, save);
        break;
    }
    default: break;
    }
    scroll_into_view(f, rows);
}

static void fl_open(struct win *w)
{
    struct files *f = (struct files *)xcalloc(1, sizeof(struct files));
    if (!f) return;
    w->state = f;
    load(f, "/");
}

static void fl_close(struct win *w)
{
    struct files *f = w->state;
    if (!f) return;
    if (f->ent) xfree(f->ent);
    xfree(f);
    w->state = 0;
}

const struct app app_files = {
    .name = "File Explorer",
    .title = "Home",
    .w = 440,
    .h = 320,
    .open = fl_open,
    .paint = fl_paint,
    .down = fl_down,
    .drag = 0,
    .up = 0,
    .key = fl_key,
    .close = fl_close,
};
