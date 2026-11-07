/* System Properties: what this machine actually is, read from the
 * kernel rather than written into the dialog at build time.  uname(2)
 * answers with the release and machine, uptime(37) with how long it has
 * been up, ps(2) with how many tasks exist -- three syscalls and a
 * string each, so paint can simply ask again every time it runs.  That
 * is the bargain Task Manager makes too: paint is the moment the window
 * becomes visible, and the table it reads from moves anyway.
 *
 * The layout is Win98's General tab: a raised panel, a rule under the
 * heading, label/value pairs on a fixed column, and the small print
 * underneath that says where the drawing comes from. */
#include "desktop.h"

#define ROW_H 19

static void kv(int x, int y, int colw, const char *k, const char *v)
{
    gfx_text(x, y, k, COL_TEXT);
    gfx_text(x + colw, y, v, COL_TEXT);
}

static void sys_paint(struct win *w)
{
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    struct lnxrm_utsname u;
    memset(&u, 0, sizeof(u));
    if (kuname(&u) < 0) {
        strcpy(u.sysname, "LnxrmOS");
        strcpy(u.release, "unknown");
        strcpy(u.machine, "unknown");
    }

    /* heading */
    gfx_hgrad(x + 6, y + 6, cw - 12, 18, COL_TITLE0, COL_TITLE1);
    gfx_text(x + 9, y + 7, "General", COL_HILITE);
    icon_paint(ICO_SYSINFO, x + cw - 24, y + 7, 1);

    /* the panel the fields sit in */
    gfx_out(x + 6, y + 30, cw - 12, ch - 76);

    int col = x + cw / 2 - 6;
    int colw = col - x - 14; /* width available to a label */
    int row = 0;
    char val[96];

    val[0] = 0;
    str_cat(val, u.sysname);
    str_cat(val, "  ");
    str_cat(val, u.release);
    kv(x + 14, y + 40 + row * ROW_H, colw, "System:", val);
    row++;

    kv(x + 14, y + 40 + row * ROW_H, colw, "Machine:", u.machine);
    row++;

    /* uname()'s version carries the compiler's date and time, longer
     * than the column: gfx_text_w() ellipsises rather than running into
     * the edge of the panel. */
    gfx_text(x + 14, y + 40 + row * ROW_H, "Built:", COL_TEXT);
    gfx_text_w(col, y + 40 + row * ROW_H, u.version, COL_TEXT, cw - (col - x) - 14);
    row++;

    long ms = uptime_ms();
    if (ms < 0) ms = 0;
    long sec = ms / 1000;
    val[0] = 0;
    str_cat_i(val, sec / 3600);
    str_cat(val, ":");
    {
        int mm = (int)((sec / 60) % 60);
        char two[3];
        two[0] = (char)('0' + (mm / 10) % 10);
        two[1] = (char)('0' + mm % 10);
        two[2] = 0;
        str_cat(val, two);
    }
    str_cat(val, ":");
    {
        int ss = (int)(sec % 60);
        char two[3];
        two[0] = (char)('0' + (ss / 10) % 10);
        two[1] = (char)('0' + ss % 10);
        two[2] = 0;
        str_cat(val, two);
    }
    kv(x + 14, y + 40 + row * ROW_H, colw, "Uptime:", val);
    row++;

    /* One syscall returns the count it wrote, so the buffer has to be
     * the size of a full table; static, because 2.5 KB should not be
     * asked of the stack on every paint. */
    static struct lnxrm_ps_entry procs[64];
    long n = kps(procs, 64);
    val[0] = 0;
    str_cat_i(val, n < 0 ? 0 : n);
    kv(x + 14, y + 40 + row * ROW_H, colw, "Processes:", val);
    row++;

    val[0] = 0;
    str_cat_i(val, g_fw);
    str_cat(val, " x ");
    str_cat_i(val, g_fh);
    str_cat(val, ", 32 bpp");
    kv(x + 14, y + 40 + row * ROW_H, colw, "Display:", val);
    row++;

    /* the small print, which on Win98 was a copyright and here is the
     * one claim this system can actually back up */
    gfx_text(x + 10, y + ch - 38, "The window, the taskbar and every program",
              COL_SHADOW);
    gfx_text(x + 10, y + ch - 22, "are user space: they draw through fb_fill.",
              COL_SHADOW);
}

const struct app app_sysinfo = {
    .name = "System",
    .title = "System Properties",
    .w = 372,
    .h = 244,
    .open = 0,
    .paint = sys_paint,
    .down = 0,
    .drag = 0,
    .up = 0,
    .key = 0,
    .close = 0,
    .tick = 0,
};
