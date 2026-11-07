/* Snake: the oldest game a desktop ships with, and the second window
 * here whose entire point is its .tick (Task Manager is the first).
 *
 * The game advances from the tick, not from paint: a frame is 10 ms and
 * the snake moves every 130 ms, so the tick decides when to step and
 * asks for one client-sized rectangle when it does.  When the game is
 * not running the tick returns on the first line -- an idle window
 * invalidates nothing, which is why leaving Snake open costs nothing.
 *
 * Steering goes through .key, so it only ever reaches the focused
 * window: arrow keys type into Notepad while Notepad has the focus and
 * turn the snake while this one does, which is the whole reason the
 * window manager routes keys to one window at a time.
 *
 * The field is a fixed 24 x 14 cells anchored at the top-left of the
 * client rather than a grid derived from the window size: a grid that
 * changed when the window was resized would change the game. */
#include "desktop.h"

#define CELL 12
#define COLS 24
#define ROWS 14
#define FX 9   /* client-relative origin of cell (0, 0) */
#define FY 27
#define FW (COLS * CELL)
#define FH (ROWS * CELL)
#define STEP_MS 130
#define MAXLEN (COLS * ROWS)

/* 0 ready, 1 running, 2 dead, 3 paused */
#define ST_READY 0
#define ST_RUN 1
#define ST_DEAD 2
#define ST_PAUSE 3

struct snake {
    /* One slot longer than the field: the shift below writes body[len]
     * before it decrements, and a snake that has just eaten the last
     * free cell really does hold MAXLEN segments for one step. */
    unsigned short body[MAXLEN + 1]; /* head first: body[0] is the head */
    int len;
    int dir, ndir; /* 0 up, 1 right, 2 down, 3 left */
    int food;
    int score, best;
    int state;
    long next_ms;
    unsigned seed;
};

static const int dx[4] = {0, 1, 0, -1};
static const int dy[4] = {-1, 0, 1, 0};

static unsigned rnd(struct snake *s)
{
    /* A linear congruential generator: no rand() in this libc, and the
     * only thing being randomised is where the apple lands. */
    s->seed = s->seed * 1103515245u + 12345u;
    return (s->seed >> 16) & 0x7fff;
}

static void place_food(struct snake *s)
{
    for (int t = 0; t < 400; t++) {
        int c = (int)(rnd(s) % (unsigned)MAXLEN);
        int i;
        for (i = 0; i < s->len; i++)
            if (s->body[i] == (unsigned short)c) break;
        if (i == s->len) {
            s->food = c;
            return;
        }
    }
    s->food = -1; /* every cell is snake: as close to winning as this gets */
}

static void reset(struct snake *s)
{
    s->len = 3;
    s->body[0] = (unsigned short)(5 * COLS + 6);
    s->body[1] = (unsigned short)(5 * COLS + 5);
    s->body[2] = (unsigned short)(5 * COLS + 4);
    s->dir = s->ndir = 1; /* heading right, away from the left wall */
    s->score = 0;
    s->state = ST_READY;
    s->seed = (unsigned)uptime_ms() ^ 0x9e3779b9u;
    place_food(s);
    s->next_ms = 0;
}

static void start(struct snake *s, long now)
{
    reset(s);
    s->state = ST_RUN;
    s->next_ms = now + STEP_MS;
}

static void step(struct snake *s)
{
    /* One turn per step, and never a U-turn: the second check is what
     * stops two quick keypresses inside one 130 ms window from turning
     * the snake back through its own neck. */
    if ((s->ndir + 2) % 4 != s->dir) s->dir = s->ndir;

    int hx = s->body[0] % COLS, hy = s->body[0] / COLS;
    int nx = hx + dx[s->dir], ny = hy + dy[s->dir];
    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) {
        s->state = ST_DEAD;
        return;
    }
    unsigned short c = (unsigned short)(ny * COLS + nx);
    /* The tail cell vacates on this same step, so running into it is
     * legal -- that is what makes a tight turn survivable. */
    for (int i = 0; i < s->len - 1; i++) {
        if (s->body[i] == c) {
            s->state = ST_DEAD;
            return;
        }
    }

    int ate = (s->food >= 0 && c == (unsigned short)s->food);
    /* The shift writes body[len], i.e. len + 1 cells: that is the new
     * head's slot, so the length grows by one here and only goes back
     * down when the tail is allowed to leave.  Getting this the wrong
     * way round -- decrementing without having incremented -- shortens
     * the snake by one segment on every non-eating step, and after three
     * of them paint has a length of zero and draws nothing at all. */
    for (int i = s->len; i > 0; i--) s->body[i] = s->body[i - 1];
    s->body[0] = c;
    s->len++;
    if (ate) {
        s->score++;
        if (s->score > s->best) s->best = s->score;
        place_food(s);
    } else {
        s->len--; /* the old tail is now one past the end */
    }
}

static void snake_paint(struct win *w)
{
    struct snake *s = w->state;
    if (!s) return;
    int x, y, cw, ch;
    win_client_rect(w, &x, &y, &cw, &ch);
    gfx_fill(x, y, cw, ch, COL_FACE);

    gfx_hgrad(x + 6, y + 4, cw - 12, 16, COL_TITLE0, COL_TITLE1);
    char b[40];
    strcpy(b, "Score ");
    str_cat_i(b, s->score);
    gfx_text(x + 10, y + 4, b, COL_HILITE);
    strcpy(b, "Best ");
    str_cat_i(b, s->best);
    gfx_text(x + cw - 10 - gfx_strw(b), y + 4, b, COL_HILITE);

    /* the field: a black plate inside a raised frame, so the cells read
     * as a board set into the dialog rather than as paint on it */
    gfx_fill(x + FX, y + FY, FW, FH, RGB(0, 0, 0));
    gfx_out(x + FX - 1, y + FY - 1, FW + 2, FH + 2);

    if (s->food >= 0) {
        int px = x + FX + (s->food % COLS) * CELL + 1;
        int py = y + FY + (s->food / COLS) * CELL + 1;
        gfx_fill(px, py, CELL - 2, CELL - 2, RGB(200, 0, 0));
        gfx_px(px, py, RGB(255, 120, 120)); /* one pixel of highlight */
    }
    for (int i = 0; i < s->len; i++) {
        int c = s->body[i];
        int px = x + FX + (c % COLS) * CELL + 1;
        int py = y + FY + (c / COLS) * CELL + 1;
        gfx_fill(px, py, CELL - 2, CELL - 2,
                 i == 0 ? RGB(80, 220, 80) : RGB(0, 160, 0));
    }

    const char *msg = 0;
    if (s->state == ST_READY) msg = "Press Space to start";
    else if (s->state == ST_DEAD) msg = "Game over -- Space restarts";
    else if (s->state == ST_PAUSE) msg = "Paused";
    if (msg) {
        int mw = gfx_strw(msg) + 24, mh = 34;
        int mx = x + FX + (FW - mw) / 2, my = y + FY + (FH - mh) / 2;
        gfx_fill(mx, my, mw, mh, COL_FACE);
        gfx_out2(mx, my, mw, mh);
        gfx_text(mx + 12, my + 9, msg, COL_TEXT);
    }

    gfx_text(x + 6, y + 198, "Wheel/Space steer   Space: start/pause", COL_SHADOW);
}

/* Only ever steps when it is running, and only ever invalidates when it
 * stepped: an idle Snake is a no-op on every one of the 100 frames a
 * second the loop runs. */
static void snake_tick(struct win *w)
{
    struct snake *s = w->state;
    if (!s || w->minimized || s->state != ST_RUN) return;
    long now = uptime_ms();
    if (now < s->next_ms) return;
    s->next_ms = now + STEP_MS;
    step(s);
    win_invalidate_client(w);
}

static void snake_key(struct win *w, const struct dsk_event *ev)
{
    if (ev->type != LNXRM_EV_KEY_DOWN) return;
    struct snake *s = w->state;
    if (!s) return;
    switch (ev->code) {
    case LNXRM_KEY_UP: s->ndir = 0; return;
    case LNXRM_KEY_RIGHT: s->ndir = 1; return;
    case LNXRM_KEY_DOWN: s->ndir = 2; return;
    case LNXRM_KEY_LEFT: s->ndir = 3; return;
    case LNXRM_KEY_SPACE:
    case LNXRM_KEY_ENTER:
        if (s->state == ST_RUN)
            s->state = ST_PAUSE;
        else if (s->state == ST_PAUSE)
            s->state = ST_RUN;
        else
            start(s, uptime_ms());
        win_invalidate_client(w);
        return;
    default: return;
    }
}

/* Clicking the field does what Space does, because a window with a
 * button in it has to react to the button. */
static void snake_down(struct win *w, int cx, int cy, int btn, int clicks)
{
    (void)clicks;
    if (btn != LNXRM_MB_LEFT) return;
    struct snake *s = w->state;
    if (!s) return;
    if (cx < FX - 4 || cy < FY - 4 || cx >= FX + FW + 4 || cy >= FY + FH + 4) return;
    if (s->state == ST_RUN || s->state == ST_PAUSE) return;
    start(s, uptime_ms());
}

static void snake_open(struct win *w)
{
    struct snake *s = (struct snake *)xcalloc(1, sizeof(struct snake));
    if (!s) return;
    reset(s);
    w->state = s;
}

static void snake_close(struct win *w)
{
    if (w->state) {
        xfree(w->state);
        w->state = 0;
    }
}

const struct app app_snake = {
    .name = "Snake",
    .title = "Snake",
    .w = 314,
    .h = 242,
    .open = snake_open,
    .paint = snake_paint,
    .down = snake_down,
    .drag = 0,
    .up = 0,
    .key = snake_key,
    .close = snake_close,
    .tick = snake_tick,
};
