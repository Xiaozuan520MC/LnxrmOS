/* The port layer: the bits of the old kernel's userland this desktop used
 * to inherit from its ulib that the current one does not ship.
 *
 * Three jobs live here.
 *
 * Heap -- a first-fit free-list allocator over kbrk.  The kernel meters
 * brk against a 64 MiB quota, the backbuffer wants 8 MiB of that, and
 * nothing else here is more than a few pages, so the list never grows
 * long enough for first-fit to matter.  Blocks are kept in address order
 * because morecore() only ever appends at the top, which is what makes
 * the free-path coalesce a two-pointer check.
 *
 * Strings -- the small libc the modules call by name (memcpy, strcmp,
 * str_cat...).  LLVM is free to emit calls to memset/memcpy/memmove for
 * struct copies and large initialisations, so those three exist even
 * where no module names them.
 *
 * Time -- there is no uptime syscall on this kernel, so the main loop
 * reports one tick (10 ms at HZ=100) per frame through dsk_frame() and
 * everything that used to read kuptime() reads the same counter here.
 * A frame is paced by a sleep that parks until the next tick, so the
 * count tracks wall time to within whatever a busy frame overruns; a
 * desktop clock that drifts slow under load is honest about being
 * busy, which is all the accuracy a status line ever needed. */
#include "desktop.h"

/* ---- heap ---- */

#define BLK_AL   16
#define MIN_BLK  32 /* smallest payload a split will leave behind */

struct blk {
    size_t size;       /* payload bytes, header not included */
    struct blk *next;  /* address order: morecore() appends at the top */
    int used;
};

static struct blk *heap;

static struct blk *morecore(size_t n)
{
    long cur = kbrk(0);
    if (cur < 0) return 0;
    long pad = (-(long)cur) & (BLK_AL - 1);
    /* The header sits before the payload, so the region to ask brk for is
     * the header plus the payload -- asking for `n` alone left the last
     * 24 bytes of every allocation unmapped, and the first program to
     * touch them (the backbuffer's final row) took a SIGSEGV. */
    if (kbrk(cur + pad + (long)(sizeof(struct blk) + n)) < 0) return 0;
    struct blk *b = (struct blk *)(cur + pad);
    b->size = n;
    b->used = 0;
    b->next = 0;
    if (!heap) {
        heap = b;
    } else {
        struct blk *e = heap;
        while (e->next) e = e->next;
        e->next = b;
    }
    return b;
}

void *xmalloc(size_t n)
{
    if (n == 0) n = 1;
    n = (n + BLK_AL - 1) & ~(size_t)(BLK_AL - 1);
    if (n < MIN_BLK) n = MIN_BLK;

    for (struct blk *b = heap; b; b = b->next) {
        if (b->used || b->size < n) continue;
        if (b->size >= n + sizeof(struct blk) + MIN_BLK) {
            /* split: the tail keeps the remainder */
            struct blk *t = (struct blk *)((char *)b + sizeof(struct blk) + n);
            t->size = b->size - n - sizeof(struct blk);
            t->used = 0;
            t->next = b->next;
            b->next = t;
            b->size = n;
        }
        b->used = 1;
        return (char *)b + sizeof(struct blk);
    }
    struct blk *b = morecore(n);
    if (!b) return 0;
    b->used = 1;
    return (char *)b + sizeof(struct blk);
}

void xfree(void *p)
{
    if (!p) return;
    struct blk *b = (struct blk *)((char *)p - sizeof(struct blk));
    b->used = 0;
    /* absorb free successors while they are physically adjacent */
    while (b->next && !b->next->used &&
           (char *)b + sizeof(struct blk) + b->size == (char *)b->next) {
        b->size += sizeof(struct blk) + b->next->size;
        b->next = b->next->next;
    }
}

void *xcalloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    char *p = (char *)xmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    if (!p) return xmalloc(n);
    if (n == 0) {
        xfree(p);
        return 0;
    }
    struct blk *b = (struct blk *)((char *)p - sizeof(struct blk));
    size_t old = b->size;
    void *q = xmalloc(n);
    if (!q) return 0;
    memcpy(q, p, old < n ? old : n);
    xfree(p);
    return q;
}

/* ---- strings ---- */

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    if (dp == sp || !n) return d;
    if (dp < sp) {
        while (n--) *dp++ = *sp++;
    } else {
        dp += n;
        sp += n;
        while (n--) *--dp = *--sp;
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (; n; n--, pa++, pb++)
        if (*pa != *pb) return *pa - *pb;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) break;
    }
    return 0;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && (*d++ = *s++)) n--;
    while (n--) *d++ = 0;
    return r;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

/* ---- the desktop's own string helpers ---- */

void str_cat(char *dst, const char *src)
{
    while (*dst) dst++;
    while ((*dst++ = *src++)) {}
}

void str_cat_n(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(dst);
    if (n + 1 >= cap) return;
    size_t left = cap - n - 1;
    while (left-- && *src) dst[n++] = *src++;
    dst[n] = 0;
}

static void fmt_i(char *dst, long v)
{
    char buf[24];
    int i = (int)sizeof(buf);
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    buf[--i] = 0;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg) buf[--i] = '-';
    strcpy(dst, &buf[i]);
}

void str_cat_i(char *dst, long v)
{
    char buf[24];
    fmt_i(buf, v);
    str_cat(dst, buf);
}

void xfmt_i(char *dst, long v)
{
    fmt_i(dst, v);
}

/* ---- time ---- */

static long dsk_ms;

void dsk_frame(void) { dsk_ms += 10; }

long uptime_ms(void) { return dsk_ms; }

/* ---- the kernel surface the old ulib shipped ----
 * fb_blit presents a rectangle of the backbuffer (gfx.c's one push, and
 * the reason the desktop never had to speak in fills); con_capture takes
 * the console in one move -- its framebuffer backend stands down and its
 * keystrokes stop racing the desktop's queue -- and con_read drains the
 * ring that output keeps flowing into, which is the Terminal window's
 * scrollback source.  Typing into the console rides the existing
 * kbd_inject (35). */
void kfb_blit(const struct lnxrm_fb_blit *b)
{
    sys_call3(SYS_fb_blit, (long)b, 0, 0);
}

long kcon_capture(int on)
{
    return sys_call3(SYS_con_capture, on, 0, 0);
}

long kcon_read(char *buf, int len)
{
    return sys_call3(SYS_con_read, (long)buf, len, 0);
}

long kcon_inject(char c)
{
    return kkbd_inject(&c, 1);
}
