/* A one-line editable file name, shared by Notepad's File bar and
 * Paint's status strip.
 *
 * The box is a real single-line edit: caret, backspace, delete, arrows,
 * Home/End, and a view that follows the caret rather than the text.  It
 * shows the *tail* of a path that is longer than the box, which is the
 * whole point -- a path you are typing is being typed at its end, and a
 * box that shows the first 29 characters of a 40-character path shows you
 * what you already know.
 *
 * Focus lives in the field itself (focus == 1 means "keys come here"), so
 * the owning application needs exactly one flag, not two that can
 * disagree. */
#include "desktop.h"

void pathedit_init(struct pathedit *pe, const char *text)
{
    pe->text[0] = 0;
    pe->caret = 0;
    pe->focus = 0;
    if (text) pathedit_set(pe, text);
}

void pathedit_set(struct pathedit *pe, const char *text)
{
    int i = 0;
    if (text) {
        for (; text[i] && i < (int)sizeof(pe->text) - 1; i++) pe->text[i] = text[i];
    }
    pe->text[i] = 0;
    pe->caret = i;
}

/* How many characters fit, and which one is at the left edge. */
static int field_cols(int w)
{
    int c = (w - 8) / 8;
    return c > 0 ? c : 1;
}

static int view_start(const struct pathedit *pe, int cols)
{
    int len = (int)xstrlen(pe->text);
    int s = len > cols ? len - cols : 0;
    if (pe->caret < s) s = pe->caret;
    if (pe->caret > s + cols - 1) s = pe->caret - cols + 1;
    if (s < 0) s = 0;
    return s;
}

void pathedit_paint(const struct pathedit *pe, int x, int y, int w, int h)
{
    gfx_fill(x, y, w, h, COL_EDITBG);
    gfx_in(x, y, w, h);

    int cols = field_cols(w);
    int start = view_start(pe, cols);
    int len = (int)xstrlen(pe->text);
    for (int i = 0; i < cols && start + i < len; i++) {
        char c = pe->text[start + i];
        if (c < 32 || c > 126) c = ' ';
        font_draw(x + 4 + i * 8, y + (h - 16) / 2, (unsigned char)c, COL_TEXT, 0);
    }

    if (pe->focus)
        gfx_fill(x + 4 + (pe->caret - start) * 8, y + 2, 2, h - 4, COL_TEXT);
}

/* Returns 1 when the click landed in the box (and took the focus). */
int pathedit_down(struct pathedit *pe, int cx, int cy, int x, int y, int w, int h)
{
    if (cx < x || cy < y || cx >= x + w || cy >= y + h) return 0;
    pe->focus = 1;
    int cols = field_cols(w);
    int start = view_start(pe, cols);
    int col = (cx - x - 4) / 8;
    if (col < 0) col = 0;
    int len = (int)xstrlen(pe->text);
    int c = start + col;
    if (c > len) c = len;
    pe->caret = c;
    return 1;
}

/* Returns 1 when the key belonged to the box.  *enter is set when that
 * key was Enter -- what "confirm a file name" means is the caller's
 * business, because Notepad and Paint do not agree on it. */
int pathedit_key(struct pathedit *pe, const struct dsk_event *ev, int *enter)
{
    if (enter) *enter = 0;
    if (ev->type != LNXRM_EV_KEY_DOWN) return 0;

    int len = (int)xstrlen(pe->text);
    switch (ev->code) {
    case LNXRM_KEY_LEFT:
        if (pe->caret > 0) pe->caret--;
        return 1;
    case LNXRM_KEY_RIGHT:
        if (pe->caret < len) pe->caret++;
        return 1;
    case LNXRM_KEY_HOME:
        pe->caret = 0;
        return 1;
    case LNXRM_KEY_END:
        pe->caret = len;
        return 1;
    case LNXRM_KEY_BACKSPACE:
        if (pe->caret > 0) {
            memmove(pe->text + pe->caret - 1, pe->text + pe->caret,
                    (size_t)(len - pe->caret + 1));
            pe->caret--;
        }
        return 1;
    case LNXRM_KEY_DELETE:
        if (pe->caret < len) memmove(pe->text + pe->caret, pe->text + pe->caret + 1,
                                     (size_t)(len - pe->caret));
        return 1;
    case LNXRM_KEY_ENTER:
        if (enter) *enter = 1;
        return 1;
    case LNXRM_KEY_ESC:
        pe->focus = 0;
        return 1;
    default: break;
    }

    char c = (char)ev->ch;
    if (c >= 32 && c < 127 && len < (int)sizeof(pe->text) - 1) {
        memmove(pe->text + pe->caret + 1, pe->text + pe->caret,
                (size_t)(len - pe->caret + 1));
        pe->text[pe->caret] = c;
        pe->caret++;
        return 1;
    }
    return 0;
}
