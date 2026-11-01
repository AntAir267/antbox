/*
 * xdvdfs.c - the Xbox disc image reader. See xdvdfs.h.
 */
#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#endif

#include "xdvdfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MAGIC           "MICROSOFT*XBOX*MEDIA"
#define MAGIC_LEN       20
#define ATTR_DIRECTORY  0x10
#define DIR_TABLE_MAX   (16u * 1024 * 1024)
#define FILES_MAX       200000u
#define DEPTH_MAX       32

/* Byte offsets of the game partition in the layouts seen in the wild (the
 * list of tools/install/xdvdfs.py):
 *   0x00000000  an xiso or a trimmed image: the partition is the whole file
 *   0x0000FD90, 0x00002080, 0x00030600  rebuilt or odd images
 *   0x0FD90000  XGD2 (an Xbox 360 disc)
 *   0x18300000  a redump of an original-Xbox disc: the video partition
 *               (387 MB), then the game partition
 *   0x1FB20000  XGD3
 * The magic is checked at both ends of the descriptor, so a wrong guess is
 * cheap. */
static const uint64_t k_bases[] = {
    0x00000000, 0x0000FD90, 0x00002080, 0x00030600, 0x0FD90000, 0x18300000, 0x1FB20000
};

struct xdvdfs_image {
    int          fd, own;
    uint64_t     size, base;
    xdvdfs_file *files;
    unsigned     count, cap;
};

static int raw_read(xdvdfs_image *img, uint64_t off, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;

    while (n) {
#if defined(_WIN32)
        int got;
        if (_lseeki64(img->fd, (__int64)off, SEEK_SET) < 0)
            return 0;
        got = _read(img->fd, p, n > (1u << 30) ? (1u << 30) : (unsigned)n);
#else
        ssize_t got = pread(img->fd, p, n, (off_t)off);
        if (got < 0 && errno == EINTR)
            continue;
#endif
        if (got <= 0)
            return 0;
        p += got;
        off += (uint64_t)got;
        n -= (size_t)got;
    }
    return 1;
}

static int has_descriptor(xdvdfs_image *img, uint64_t base, uint8_t *d)
{
    uint64_t off = base + 32 * XDVDFS_SECTOR;

    if (off + XDVDFS_SECTOR > img->size || !raw_read(img, off, d, XDVDFS_SECTOR))
        return 0;
    return !memcmp(d, MAGIC, MAGIC_LEN) && !memcmp(d + 0x7EC, MAGIC, MAGIC_LEN);
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A name from the image is data, not a path. Latin-1 to UTF-8 on the way. */
static int safe_name(const uint8_t *s, unsigned n, char *out, size_t out_size)
{
    size_t w = 0;
    unsigned i;

    if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
        return 0;
    for (i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':')
            return 0;
        if (w + 3 > out_size)
            return 0;
        if (c < 0x80) {
            out[w++] = (char)c;
        } else {
            out[w++] = (char)(0xC0 | c >> 6);
            out[w++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[w] = '\0';
    return 1;
}

static int fail(char *err, size_t n, int code, const char *fmt, const char *arg)
{
    if (err && n)
        snprintf(err, n, fmt, arg);
    return code;
}

static int add_file(xdvdfs_image *img, const char *path, uint64_t offset, uint32_t size)
{
    if (img->count == img->cap) {
        unsigned cap = img->cap ? img->cap * 2 : 1024;
        xdvdfs_file *f = (xdvdfs_file *)realloc(img->files, cap * sizeof *f);
        if (!f)
            return 0;
        img->files = f;
        img->cap = cap;
    }
    snprintf(img->files[img->count].path, XDVDFS_PATH, "%s", path);
    img->files[img->count].offset = offset;
    img->files[img->count].size = size;
    img->count++;
    return 1;
}

/* One directory table and everything below it. The tree's links are followed
 * with a list of the slots already seen, so a table that loops ends. */
static int walk(xdvdfs_image *img, uint32_t sector, uint32_t size, const char *prefix, int depth,
                char *err, size_t err_size)
{
    uint64_t at = img->base + (uint64_t)sector * XDVDFS_SECTOR;
    uint8_t *table, *seen;
    uint32_t *todo;
    unsigned slots = size / 4, ntodo = 0;
    int rc = XDVDFS_OK;

    if (!size)
        return XDVDFS_OK;
    if (depth > DEPTH_MAX || size > DIR_TABLE_MAX)
        return fail(err, err_size, XDVDFS_E_CORRUPT, "the image's directory \"%s\" cannot be one "
                    "(the image is damaged)", prefix);
    if (at + size > img->size)
        return fail(err, err_size, XDVDFS_E_TRUNCATED, "the image ends before its directory \"%s\" "
                    "(a truncated download or copy?)", prefix);
    table = (uint8_t *)malloc(size);
    seen = (uint8_t *)calloc(slots + 1, 1);
    todo = (uint32_t *)malloc((slots + 1) * sizeof *todo);
    if (!table || !seen || !todo) {
        rc = fail(err, err_size, XDVDFS_E_NOMEM, "out of memory reading the image%s", "");
    } else if (!raw_read(img, at, table, size)) {
        rc = fail(err, err_size, XDVDFS_E_OPEN, "the image cannot be read: %s", strerror(errno));
    } else {
        todo[ntodo++] = 0;
    }
    while (rc == XDVDFS_OK && ntodo) {
        uint32_t slot = todo[--ntodo], off = slot * 4, left, right, start, length;
        char name[XDVDFS_PATH], path[XDVDFS_PATH];
        unsigned nlen;

        if (slot >= slots || seen[slot] || off + 14 > size)
            continue;
        seen[slot] = 1;
        left = table[off] | (uint32_t)table[off + 1] << 8;
        right = table[off + 2] | (uint32_t)table[off + 3] << 8;
        if (left == 0xFFFF)                 /* padding, not an entry */
            continue;
        start = le32(table + off + 4);
        length = le32(table + off + 8);
        nlen = table[off + 13];
        if (off + 14 + nlen > size || !safe_name(table + off + 14, nlen, name, sizeof name)
            || snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", name)
               >= (int)sizeof path) {
            rc = fail(err, err_size, XDVDFS_E_CORRUPT, "the image has an unusable file name in "
                      "\"%s\" (the image is damaged)", prefix);
            break;
        }
        if (table[off + 12] & ATTR_DIRECTORY) {
            rc = walk(img, start, length, path, depth + 1, err, err_size);
        } else {
            uint64_t fo = img->base + (uint64_t)start * XDVDFS_SECTOR;
            if (length && fo + length > img->size)
                rc = fail(err, err_size, XDVDFS_E_TRUNCATED, "the image ends before the end of "
                          "\"%s\" (a truncated download or copy?)", path);
            else if (img->count >= FILES_MAX)
                rc = fail(err, err_size, XDVDFS_E_CORRUPT, "the image lists too many files%s", "");
            else if (!add_file(img, path, fo, length))
                rc = fail(err, err_size, XDVDFS_E_NOMEM, "out of memory reading the image%s", "");
        }
        if (left)
            todo[ntodo++] = left;
        if (right)
            todo[ntodo++] = right;
    }
    free(table);
    free(seen);
    free(todo);
    return rc;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Without regard to case, and either slash. */
static int path_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = *a == '\\' ? '/' : lower((unsigned char)*a);
        int cb = *b == '\\' ? '/' : lower((unsigned char)*b);
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (!ca)
            return 0;
    }
}

static int file_cmp(const void *a, const void *b)
{
    return path_cmp(((const xdvdfs_file *)a)->path, ((const xdvdfs_file *)b)->path);
}

static int open_image(int fd, int own, xdvdfs_image **out, char *err, size_t err_size)
{
    xdvdfs_image *img = (xdvdfs_image *)calloc(1, sizeof *img);
    uint8_t d[XDVDFS_SECTOR];
    unsigned i;
    int rc;

    *out = NULL;
    if (!img) {
        if (own)
#if defined(_WIN32)
            _close(fd);
#else
            close(fd);
#endif
        return fail(err, err_size, XDVDFS_E_NOMEM, "out of memory reading the image%s", "");
    }
    img->fd = fd;
    img->own = own;
    {
#if defined(_WIN32)
        __int64 size = _filelengthi64(fd);
        img->size = size > 0 ? (uint64_t)size : 0;
#else
        struct stat st;
        img->size = fstat(fd, &st) == 0 && st.st_size > 0 ? (uint64_t)st.st_size : 0;
#endif
    }
    for (i = 0; i < sizeof k_bases / sizeof k_bases[0]; i++)
        if (has_descriptor(img, k_bases[i], d))
            break;
    if (i == sizeof k_bases / sizeof k_bases[0]) {
        xdvdfs_close(img);
        return fail(err, err_size, XDVDFS_E_NOT_XBOX, "this is not an Xbox disc image (no Xbox "
                    "volume descriptor at any known partition offset)%s", "");
    }
    img->base = k_bases[i];
    rc = walk(img, le32(d + 0x14), le32(d + 0x18), "", 0, err, err_size);
    if (rc != XDVDFS_OK) {
        xdvdfs_close(img);
        return rc;
    }
    if (img->count)
        qsort(img->files, img->count, sizeof img->files[0], file_cmp);
    *out = img;
    return XDVDFS_OK;
}

int xdvdfs_open(const char *path, xdvdfs_image **out, char *err, size_t err_size)
{
#if defined(_WIN32)
    int fd = _open(path, _O_RDONLY | _O_BINARY);
#else
    int fd = open(path, O_RDONLY);
#endif
    *out = NULL;
    if (fd < 0)
        return fail(err, err_size, XDVDFS_E_OPEN, "the image cannot be opened: %s", strerror(errno));
    return open_image(fd, 1, out, err, err_size);
}

int xdvdfs_open_fd(int fd, xdvdfs_image **out, char *err, size_t err_size)
{
    return open_image(fd, 0, out, err, err_size);
}

void xdvdfs_close(xdvdfs_image *img)
{
    if (!img)
        return;
    if (img->own)
#if defined(_WIN32)
        _close(img->fd);
#else
        close(img->fd);
#endif
    free(img->files);
    free(img);
}

uint64_t xdvdfs_base(const xdvdfs_image *img) { return img->base; }
unsigned xdvdfs_count(const xdvdfs_image *img) { return img->count; }
const xdvdfs_file *xdvdfs_files(const xdvdfs_image *img) { return img->files; }

const xdvdfs_file *xdvdfs_find(const xdvdfs_image *img, const char *path)
{
    unsigned lo = 0, hi = img->count;

    while (*path == '/' || *path == '\\')
        path++;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = path_cmp(path, img->files[mid].path);
        if (!c)
            return &img->files[mid];
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

int xdvdfs_read(xdvdfs_image *img, const xdvdfs_file *f, uint64_t offset, void *buf, size_t n)
{
    if (offset > f->size || n > f->size - offset)
        return 0;
    return raw_read(img, f->offset + offset, buf, n);
}
