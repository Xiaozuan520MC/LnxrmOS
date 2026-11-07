/* The application registry: the one list the Start menu and the launcher
 * share, in menu order.  Adding a program is a matter of writing its
 * `struct app` and adding a line here -- nothing else in the desktop knows
 * which programs exist.  About is deliberately *not* here: the menu draws
 * it itself, below a separator, the way Win98 pinned its footer items.
 *
 * The original seven come first and the four later additions follow, so
 * that a menu position any test or muscle memory has learned still names
 * the program it used to name (the menu grows downward from the taskbar,
 * so every row moves, but the *order* is append-only). */
#include "desktop.h"

const struct app *const apps[] = {
    &app_calc,   &app_notepad, &app_files,    &app_paint,   &app_mines,
    &app_terminal, &app_taskmgr, &app_clock,  &app_sysinfo, &app_display,
    &app_snake,  &app_mouse,
};

const int apps_count = (int)(sizeof(apps) / sizeof(apps[0]));

/* ---- launching with a file ----
 *
 * The File Explorer knows *which* file it wants opened; Notepad knows how
 * to open one.  There is no IPC here to carry the request across, so the
 * path is parked in one static slot that win_open()'s call to a->open()
 * picks up.  That call happens before win_open returns, which is the only
 * reason a static is the right shape for this: it is never a pointer that
 * outlives its buffer. */
static char launch_path[128];

void app_open_file(const struct app *a, const char *path)
{
    launch_path[0] = 0;
    if (path) {
        int i = 0;
        for (; path[i] && i < (int)sizeof(launch_path) - 1; i++) launch_path[i] = path[i];
        launch_path[i] = 0;
    }
    win_open(a);
    launch_path[0] = 0;
}

const char *app_open_arg(void)
{
    return launch_path[0] ? launch_path : 0;
}
