/* File I/O from the GUI: whole-file reads for Notepad, chunked writes
 * for Notepad and Paint.
 *
 * Two kernel facts shape this file.  sys_read/sys_write copy through a
 * fixed 4 KiB kernel buffer, so anything larger has to arrive as a series
 * of calls rather than one hopeful one.  And open() takes no mode
 * argument, so a file is created with O_CREAT|O_WRONLY|O_TRUNC spelled in
 * octal -- which is what every other user program in this tree does, and
 * O_TRUNC matters: without it a shorter save would keep the tail of what
 * was there before, and a text file would end in its own leftovers.
 *
 * Errors come back as negative LNXRM_E* values.  They are turned into one
 * line of words here, because a status bar that says "-28" is a status
 * bar nobody reads. */
#include "desktop.h"

#define FIO_CHUNK 4096
#define FIO_MAX   (4 * 1024 * 1024) /* refuse anything bigger than this */

static const char *err_name(long e)
{
    switch (e) {
    case LNXRM_ENOENT: return "no such file";
    case LNXRM_EACCES: return "permission denied";
    case LNXRM_EEXIST: return "already exists";
    case LNXRM_ENOSPC: return "disk full";
    case LNXRM_ENAMETOOLONG: return "name too long";
    case LNXRM_ENOMEM: return "out of memory";
    case LNXRM_EIO: return "i/o error";
    case LNXRM_EINVAL: return "invalid argument";
    case LNXRM_EFAIL: return "write failed";
    default: return "error";
    }
}

/* "<verb><path>: <reason>" -- every message these helpers produce has the
 * same shape, so the shape lives in one place.  Bounded: a path can be as
 * long as the field it was typed into. */
static void seterr(char *err, int cap, const char *verb, const char *path, long e)
{
    if (!err || cap <= 0) return;
    err[0] = 0;
    str_cat_n(err, (size_t)cap, verb);
    str_cat_n(err, (size_t)cap, path ? path : "");
    str_cat_n(err, (size_t)cap, ": ");
    str_cat_n(err, (size_t)cap, err_name(e));
}

/* Read a whole file.  Returns a malloc'd buffer holding len bytes plus a * NUL (the file may itself contain NULs -- that is why the length comes
 * back separately), or NULL with *err explaining why.
 *
 * cap_out is not optional for callers that keep editing the result: it
 * reports the *allocated* size, so np_load() can hand the buffer to the
 * normal grow path instead of lying about how much room is in it. */
char *fileio_read(const char *path, int *len, int *cap_out, char *err, int errcap)
{
    long fd = kopen(path, 0);
    if (fd < 0) {
        seterr(err, errcap, "Cannot open ", path, fd);
        return 0;
    }

    int cap = FIO_CHUNK, n = 0;
    char *buf = (char *)xmalloc((size_t)cap + 1);
    if (!buf) {
        kclose(fd);
        seterr(err, errcap, "Cannot open ", path, LNXRM_ENOMEM);
        return 0;
    }

    for (;;) {
        long r = kread(fd, buf + n, FIO_CHUNK);
        if (r < 0) {
            xfree(buf);
            kclose(fd);
            seterr(err, errcap, "Cannot read ", path, r);
            return 0;
        }
        if (r == 0) break; /* end of file */
        n += (int)r;
        if (n > FIO_MAX) {
            xfree(buf);
            kclose(fd);
            seterr(err, errcap, "Cannot read ", path, LNXRM_EINVAL);
            return 0;
        }
        if (n + FIO_CHUNK + 1 > cap) {
            int ncap = cap * 2;
            while (ncap < n + FIO_CHUNK + 1) ncap *= 2;
            char *nb = (char *)xrealloc(buf, (size_t)ncap + 1);
            if (!nb) {
                xfree(buf);
                kclose(fd);
                seterr(err, errcap, "Cannot read ", path, LNXRM_ENOMEM);
                return 0;
            }
            buf = nb;
            cap = ncap;
        }
    }

    kclose(fd);
    buf[n] = 0;
    if (len) *len = n;
    if (cap_out) *cap_out = cap + 1;
    return buf;
}

/* Open an existing file for reading.  -1 on failure, with *err set --
 * the streaming readers (Paint, which must not hold 800 KB of image in
 * memory just to look at it) want the descriptor rather than the data. */
long fileio_open(const char *path, char *err, int errcap)
{
    long fd = kopen(path, 0);
    if (fd < 0) seterr(err, errcap, "Cannot open ", path, fd);
    return fd < 0 ? -1 : fd;
}

/* Create (or truncate) a file for writing.  -1 on failure, with *err set. */
long fileio_create(const char *path, char *err, int errcap)
{
    long fd = kopen(path, 0100 | 1 | 01000); /* O_CREAT|O_WRONLY|O_TRUNC */
    if (fd < 0) seterr(err, errcap, "Cannot save ", path, fd);
    return fd < 0 ? -1 : fd;
}

/* Write every one of len bytes.  Returns len, or -1 with *err set.  A
 * short write is retried rather than treated as an error -- the kernel
 * caps each call at its own buffer size, so Paint's row-at-a-time output
 * would otherwise look like failure on every row. */
long fileio_write_fd(long fd, const char *path, const void *buf, int len, char *err, int errcap)
{
    const char *p = (const char *)buf;
    int left = len;
    while (left > 0) {
        int chunk = left > FIO_CHUNK ? FIO_CHUNK : left;
        long w = kwrite(fd, p, chunk);
        if (w <= 0) {
            seterr(err, errcap, "Cannot save ", path, w < 0 ? w : LNXRM_EIO);
            return -1;
        }
        p += w;
        left -= (int)w;
    }
    return len;
}

/* One shot: create, write, close. */
long fileio_write(const char *path, const void *buf, int len, char *err, int errcap)
{
    long fd = fileio_create(path, err, errcap);
    if (fd < 0) return -1;
    long r = fileio_write_fd(fd, path, buf, len, err, errcap);
    kclose(fd);
    return r;
}

/* The part of a path a title bar shows: "notes.txt" out of
 * "/notes.txt".  Out is always terminated; a path with no slash is used
 * as it stands. */
void fileio_base(const char *path, char *out, int cap)
{
    const char *slash = 0;
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!path) return;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p + 1;
    str_cat_n(out, (size_t)cap, slash ? slash : path);
}
