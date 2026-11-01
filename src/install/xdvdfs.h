/*
 * xdvdfs.h - read an Xbox disc image (XDVDFS: an xiso, or a redump with the
 * video partition in front), for the data installer (disc_install.h).
 *
 * Format, all little-endian, 2048-byte sectors:
 *
 *   Sector 32 of the game partition is the volume descriptor: the 20 bytes
 *   "MICROSOFT*XBOX*MEDIA", the root directory's start sector (u32 at 0x14)
 *   and its size in bytes (u32 at 0x18), and the same magic again at 0x7EC.
 *
 *   A directory is a binary search tree stored in a flat table. Each entry:
 *   left u16, right u16 (both in dwords from the table start; 0 = none),
 *   start sector u32, size u32, attributes u8 (0x10 = directory), name
 *   length u8, then the name. Entries are padded to 4 bytes with 0xFF; a
 *   slot whose left field is 0xFFFF is padding.
 *
 *   A redump of a retail disc is the whole DVD: a video partition first, the
 *   game partition at a fixed byte offset. Which offset depends on the dump,
 *   so the descriptor is looked for at each known one (xdvdfs.c).
 *
 * The same reader as the installer's tools/install/xdvdfs.py in the game
 * repository, and the toolkit's tools/xiso; everything from an image is data:
 * a name with a separator, "..", ":" or a control character fails the open,
 * so no path from an image can leave the folder it is copied into.
 */
#ifndef XDVDFS_H
#define XDVDFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XDVDFS_SECTOR 2048
#define XDVDFS_PATH   512

enum {
    XDVDFS_OK = 0,
    XDVDFS_E_OPEN,        /* the file cannot be opened or read (errno is set) */
    XDVDFS_E_NOT_XBOX,    /* no volume descriptor at any known offset */
    XDVDFS_E_TRUNCATED,   /* a directory or a file lies past the end of the image */
    XDVDFS_E_CORRUPT,     /* a directory table that cannot be one, or an unsafe name */
    XDVDFS_E_NOMEM
};

typedef struct xdvdfs_image xdvdfs_image;

/* A file of the image: "dir/sub/name" as on the disc, forward slashes. */
typedef struct {
    char     path[XDVDFS_PATH];
    uint64_t offset;            /* of its first byte, in the image file */
    uint32_t size;
} xdvdfs_file;

/* Open an image and read all of its directories. `err` gets a sentence for
 * every result but XDVDFS_OK. xdvdfs_open_fd reads a descriptor the caller
 * keeps (Android: the one the document picker gave). */
int  xdvdfs_open(const char *path, xdvdfs_image **out, char *err, size_t err_size);
int  xdvdfs_open_fd(int fd, xdvdfs_image **out, char *err, size_t err_size);
void xdvdfs_close(xdvdfs_image *img);

/* Where the game partition starts in the file (0 for an xiso). */
uint64_t xdvdfs_base(const xdvdfs_image *img);

/* Every file, sorted by path without regard to case. */
unsigned           xdvdfs_count(const xdvdfs_image *img);
const xdvdfs_file *xdvdfs_files(const xdvdfs_image *img);

/* The file at `path` (case-insensitive, either slash), or NULL. */
const xdvdfs_file *xdvdfs_find(const xdvdfs_image *img, const char *path);

/* `n` bytes of a file from `offset` in it. 1 when all were read. */
int xdvdfs_read(xdvdfs_image *img, const xdvdfs_file *f, uint64_t offset, void *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* XDVDFS_H */
