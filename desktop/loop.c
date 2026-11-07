/* The main loop: read input, let the windows react, repaint what
 * changed, push those rectangles at the framebuffer, sleep.
 *
 * One frame is the whole story.  Nothing is threaded and nothing is
 * scheduled -- the only reason the loop sleeps at all is that a desktop
 * which repaints at 1000 Hz is indistinguishable from one that has
 * crashed, and the kernel's tick is 10 ms anyway.
 *
 * Presenting a frame is: take the damage rectangles, paint the scene
 * once clipped to their union (painting is idempotent, so one pass with
 * a big clip equals N passes with small ones), draw the cursor on top,
 * then push each rectangle through gfx_blit.  The cursor is never pushed
 * on its own -- it is drawn into the backbuffer like everything else,
 * which is why moving it only costs a repaint of where it was and where
 * it is.
 *
 * Input comes from the kernel's unified input queue (input_pop): relative
 * mouse deltas the loop integrates into an absolute pointer, button
 * transitions, the wheel, and decoded keyboard characters.  The queue has
 * no key-up, no scancodes and no Ctrl -- what that costs the desktop is
 * written where it matters (desktop.h, notepad.c's save).
 */
#include "desktop.h"

/* Frame pacing.  This is a *rate limit*, not a duration to hand to
 * nanosleep: the kernel's sleep is tick-granular and deliberately rounds
 * up (it must never wake early), so asking for "10 ms" at HZ=100 costs
 * two ticks and caps the desktop at 50 fps for no reason at all.  Asking
 * for less than one tick instead parks the task until the *next* tick,
 * and since a frame always begins on a tick boundary, "run the frame,
 * then wait for the next tick" is exactly 100 frames per second.  It
 * degrades on its own too: a frame that overruns its budget simply gets
 * the following tick, so a busy desktop drops to 50 or 33 fps instead of
 * spinning. */
#define FRAME_PACE_MS 1
#define CLICK_MS 400
#define CLICK_SLOP 6

static int mouse_x, mouse_y;
static unsigned mouse_buttons;
static int clicks;
static long click_time;
static int click_x, click_y;

/* Shift, from the flags the driver stamps on every key event: the last
 * key to arrive wins, which is the closest thing the queue offers to "is
 * shift down right now?".  Ctrl does not exist here -- the keyboard
 * driver never produces it -- so desktop_ctrl() is a constant (port.c's
 * contract keeps it callable so Notepad's save path compiles). */
static int shift_held;
int desktop_ctrl(void) { return 0; }
int desktop_shift(void) { return shift_held; }

/* The frame counter behind the "nn fps" Task Manager prints, and the
 * clock behind uptime_ms(): one tick per frame, because the sleep below
 * parks until the next tick and the frame's own work is not charged to
 * anyone.  Recomputed once a second rather than averaged over a window,
 * which is all the accuracy a status line has ever needed. */
static int fps_frames, fps;
static long fps_t0 = -1;

int desktop_fps(void) { return fps; }

static void present(void)
{
    int rects[8][4];
    int n = gfx_dirty_take(rects, 8);
    if (!n) return;

    /* One clip, one scene pass, one push of the union's bounding box.
     * The scene is a complete description of the screen, so painting it
     * once clipped to the union costs what the damage costs -- and the
     * push has to be the same box, because the scene *changed* every row
     * of it: pushing the rectangles one by one left the rows *between*
     * two far-apart rectangles painted in the backbuffer but never
     * marked, and the next frame's blit of an overlapping row uploaded
     * that stale paint to the screen as a ghost.  kfb_flip() lands the
     * frame right now, the way the fb_blit this desktop was written for
     * used to. */
    int x0 = rects[0][0], y0 = rects[0][1];
    int x1 = x0 + rects[0][2], y1 = y0 + rects[0][3];
    for (int i = 1; i < n; i++) {
        if (rects[i][0] < x0) x0 = rects[i][0];
        if (rects[i][1] < y0) y0 = rects[i][1];
        if (rects[i][0] + rects[i][2] > x1) x1 = rects[i][0] + rects[i][2];
        if (rects[i][1] + rects[i][3] > y1) y1 = rects[i][1] + rects[i][3];
    }
    gfx_clip(x0, y0, x1 - x0, y1 - y0);
    wm_paint();
    cursor_paint(mouse_x, mouse_y);
    gfx_clip_reset();

    gfx_blit(x0, y0, x1 - x0, y1 - y0);
    kfb_flip();
}

/* The pointer is part of the scene, so it moves the way everything else
 * does: by invalidating where it was and where it is going.  Nothing else
 * in the loop reacts to a bare motion -- no window changes state, no hover
 * highlight changes -- so without this the screen would only ever repaint
 * when something unrelated went dirty and the pointer would sit perfectly
 * still while the mouse moved under it.  (Only the two ends, not the
 * path: the cursor exists at one position per frame, so the pixels in
 * between were never drawn with it.)  The box is measured with the
 * cursor's own w/h, because the Mouse window can make the sprite bigger. */
static void mouse_warp(int nx, int ny)
{
    if (nx == mouse_x && ny == mouse_y) return;
    int cw = cursor_w(), ch = cursor_h();
    int x0 = mouse_x < nx ? mouse_x : nx;
    int y0 = mouse_y < ny ? mouse_y : ny;
    int x1 = mouse_x + cw > nx + cw ? mouse_x + cw : nx + cw;
    int y1 = mouse_y + ch > ny + ch ? mouse_y + ch : ny + ch;
    gfx_dirty(x0, y0, x1 - x0, y1 - y0);
    mouse_x = nx;
    mouse_y = ny;
}

/* The Mouse window changes the pointer's size or colours in place: the
 * old sprite's pixels are still in the backbuffer, so both the largest
 * size the sprite can have and the current one get marked -- one box
 * covers every scale. */
void dsk_cursor_dirty(void)
{
    gfx_dirty(mouse_x, mouse_y, CURSOR_W * 3, CURSOR_H * 3);
}

/* One button transition, shared by press and release: the press lands
 * before the motion it came with, so a window that starts a drag sees the
 * position it was grabbed at; the release lands last, so it can tell
 * whether the button came up over the control that took it down. */
static void buttons_now(unsigned buttons)
{
    unsigned pressed = buttons & ~mouse_buttons;
    unsigned released = mouse_buttons & ~buttons;

    if (pressed & LNXRM_MB_LEFT) {
        long now = uptime_ms();
        long dt = now - click_time;
        if (dt <= CLICK_MS && (mouse_x - click_x <= CLICK_SLOP) &&
            (click_x - mouse_x <= CLICK_SLOP) && (mouse_y - click_y <= CLICK_SLOP) &&
            (click_y - mouse_y <= CLICK_SLOP))
            clicks++;
        else
            clicks = 1;
        click_time = now;
        click_x = mouse_x;
        click_y = mouse_y;
        wm_down(mouse_x, mouse_y, LNXRM_MB_LEFT, clicks);
    }
    if (pressed & LNXRM_MB_RIGHT) wm_down(mouse_x, mouse_y, LNXRM_MB_RIGHT, 1);
    if (pressed & LNXRM_MB_MIDDLE) wm_down(mouse_x, mouse_y, LNXRM_MB_MIDDLE, 1);

    if (released & LNXRM_MB_LEFT) wm_up(mouse_x, mouse_y, LNXRM_MB_LEFT);
    if (released & LNXRM_MB_RIGHT) wm_up(mouse_x, mouse_y, LNXRM_MB_RIGHT);
    if (released & LNXRM_MB_MIDDLE) wm_up(mouse_x, mouse_y, LNXRM_MB_MIDDLE);

    mouse_buttons = buttons;
}

static void handle_key(const struct lnxrm_input_event *e)
{
    struct dsk_event ev;
    int c = (int)e->a;
    shift_held = (e->flags & INPUT_MOD_SHIFT) ? 1 : 0;
    ev.type = LNXRM_EV_KEY_DOWN;
    switch (c) {
    case '\n': ev.code = LNXRM_KEY_ENTER; break;
    case '\b': ev.code = LNXRM_KEY_BACKSPACE; break;
    case '\t': ev.code = LNXRM_KEY_TAB; break;
    case 27:   ev.code = LNXRM_KEY_ESC; break;
    default:   ev.code = c; break;
    }
    ev.ch = c;
    wm_key(&ev);
}

static void run_t(void)
{
    /* Per-window timers: a window whose display advances on its own asks
     * for a repaint here rather than spinning the whole screen. */
    for (int i = 0; i < wm_count(); i++) {
        struct win *w = wm_nth(i);
        if (w && w->used && w->app->tick) w->app->tick(w);
    }
}

/* The console's shell: the Terminal window types into it and reads its
 * output back.  Forked by the desktop because the shell that launched it
 * is blocked in wait4 -- a shell that cannot read cannot answer -- and
 * fds cross the fork, so the child reads the same console the kernel's
 * capture muted.  Killed from desktop_term() below; the kernel releases
 * the framebuffer and the console capture when the desktop exits either
 * way. */
static long sh_child;

static void desktop_term(int sig)
{
    (void)sig;
    if (sh_child > 0) kkill(sh_child, SIGKILL);
    kexit(0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    gfx_init();
    wm_init();

    /* The queue says what the pointer is, not where it starts; the middle
     * of the screen is where a mouse that has not moved yet lives. */
    mouse_x = g_fw / 2;
    mouse_y = g_fh / 2;
    gfx_dirty_all();

    /* The keyboard we typed the launch command into fed the same unified
     * queue this desktop is about to read, and nothing else consumes it
     * (the shell reads the console ring, not this queue) -- so every key
     * pressed since boot is still in there, and replaying it into the
     * GUI would open whatever row happened to be selected.  The desktop
     * starts its input history here. */
    struct lnxrm_input_event stale;
    while (kinput_pop(&stale) == 1) {}

    /* Take the screen and the keyboard in one move: from here the PS/2
     * keyboard feeds the GUI's event stream instead of the shell, and the
     * framebuffer console stops drawing over the desktop. */
    if (kcon_capture(1) < 0) xputs("desktop: con_capture failed\n");

    /* The console ring still holds the boot log at this point; the first
     * term_pump() drops it and seeds the shell's prompt instead (see the
     * comment there). */
    term_pump();

    sh_child = kfork();
    if (sh_child == 0) {
        char *sh_argv[] = {"/bin/sh", 0};
        char *envp[] = {0};
        if (kexecve("/bin/sh", sh_argv, envp) < 0) {
            xputs("desktop: exec /bin/sh failed\n");
            kexit(1);
        }
    }
    struct lnxrm_sigaction term;
    memset(&term, 0, sizeof(term));
    term.sa_handler = (void *)desktop_term;
    ksigaction(SIGTERM, &term, 0);

    /* One window up front: an empty desktop looks like a hung machine. */
    if (!win_open(&app_terminal)) xputs("desktop: no window slots\n");

    for (;;) {
        term_pump();
        taskbar_tick();
        run_t();

        struct lnxrm_input_event e;
        struct dsk_event ev;
        while (kinput_pop(&e) == 1) {
            switch (e.type) {
            case INPUT_EV_REL: {
                /* The Mouse window's speed: a percent of the raw delta,
                 * truncating toward the pointer -- small motions under
                 * 100% still move at least a pixel's worth of intent. */
                int gain = mouse_speed();
                int nx = mouse_x + ((int)e.a * gain) / 100;
                int ny = mouse_y + ((int)e.b * gain) / 100;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx > g_fw - 1) nx = g_fw - 1;
                if (ny > g_fh - 1) ny = g_fh - 1;
                int dx = nx - mouse_x, dy = ny - mouse_y;
                mouse_warp(nx, ny);
                if (dx || dy) wm_move(nx, ny, dx, dy);
                break;
            }
            case INPUT_EV_BTN:
                buttons_now((unsigned)e.a);
                break;
            case INPUT_EV_WHEEL:
                /* No arrows reach this queue (extended keys never enter
                 * it), so the wheel stands in for them: one notch, one
                 * line.  Lists scroll, Notepad's caret walks, and the
                 * wheel that a PS/2 mouse has had for decades is finally
                 * the navigation key. */
                ev.type = LNXRM_EV_KEY_DOWN;
                ev.code = e.a > 0 ? LNXRM_KEY_DOWN : LNXRM_KEY_UP;
                ev.ch = 0;
                wm_key(&ev);
                break;
            case INPUT_EV_KEY:
                handle_key(&e);
                break;
            default:
                break;
            }
        }

        present();

        fps_frames++;
        long now = uptime_ms();
        if (fps_t0 < 0) fps_t0 = now;
        if (now - fps_t0 >= 1000) {
            fps = (int)(fps_frames * 1000 / (now - fps_t0));
            fps_frames = 0;
            fps_t0 = now;
        }

        dsk_frame(); /* one tick of uptime per frame */
        ksleep_ms(FRAME_PACE_MS, 0);
    }
}
