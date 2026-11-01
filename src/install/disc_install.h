/*
 * disc_install.h - the data installer: a title's files from the user's own
 * disc (an image, xdvdfs.h, or a folder already extracted from one) into the
 * folder the port reads them from, every file checked against a manifest.
 *
 * A port ships its program and none of the title's data. Its first start
 * hands a disc to this; what the program was built for is one exact disc, so
 * the manifest lists that disc's files (path, size, SHA-256) and anything
 * else is refused with what it is.
 *
 *   <dest>/<subdir>/...       the disc's files, as the manifest names them
 *   <dest>/install.journal    the files that are whole, a line each, while
 *                             an install is under way
 *   <dest>/.complete          written last: the install is whole
 *
 * A file is hashed while it is copied to <name>.part, compared, and renamed;
 * then its line goes into the journal. A run that was stopped (the process
 * killed, the machine switched off, the cancel callback) goes on where it
 * was: a file the journal lists is kept as it is, a file that is there
 * without a line is hashed where it lies, the rest are copied. The marker is
 * made when every file of the manifest is in place and the journal goes.
 * Files in the source that the manifest does not list are not copied.
 *
 * No runtime, no thread, no global state: the same code in a game's
 * executable (its --install), in an Android library behind a document
 * picker (source_fd), and in the tests.
 */
#ifndef XR_DISC_INSTALL_H
#define XR_DISC_INSTALL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *path;           /* as on the disc, forward slashes */
    uint64_t    size;
    const char *sha256;         /* 64 hex digits */
} xr_manifest_file;

typedef struct {
    const char *id;             /* names this list: in the journal and the marker */
    const char *title;          /* "Burnout 3: Takedown" */
    const char *build;          /* "North America (NTSC-U) retail" */
    uint32_t    title_id;       /* of the executable's certificate */
    const char *exe;            /* the file that says which disc it is: "default.xbe" */
    const xr_manifest_file *files;
    unsigned    count;
} xr_manifest;

enum {
    XR_INSTALL_OK = 0,
    XR_INSTALL_NO_SOURCE,       /* the path does not exist */
    XR_INSTALL_SOURCE_UNREADABLE, /* it does, and cannot be read */
    XR_INSTALL_NOT_XBOX,        /* a file that is not an Xbox disc image */
    XR_INSTALL_XBOX360,         /* an Xbox 360 disc */
    XR_INSTALL_NO_EXE,          /* a folder or image without the executable */
    XR_INSTALL_WRONG_GAME,      /* another title */
    XR_INSTALL_WRONG_VERSION,   /* this title, another region or revision */
    XR_INSTALL_TRUNCATED,       /* the image ends early */
    XR_INSTALL_DAMAGED,         /* a file is missing, or is not what the disc holds */
    XR_INSTALL_DISK_FULL,
    XR_INSTALL_NO_PERMISSION,   /* the destination cannot be written */
    XR_INSTALL_WRITE_FAILED,    /* any other error writing */
    XR_INSTALL_CANCELLED,
    XR_INSTALL_INTERNAL,        /* out of memory, a path too long */
    XR_INSTALL_CODES
};

/* The code as a word for a front end: "ok", "wrong-version", "disk-full" ... */
const char *xr_install_code_name(int code);

typedef struct {
    const char *source;         /* an image file or a folder */
    int         source_fd;      /* >= 0: an image on this descriptor instead */
    const char *dest;           /* created */
    const char *subdir;         /* under dest, for the files; NULL: dest itself */
    const xr_manifest *manifest;
    const char *version;        /* the program's, into the marker */
    uint64_t    free_needed;    /* bytes a whole install wants free; 0: no check */
    int         identify_only;  /* say what the source is, write nothing */
    void       *ctx;
    void      (*progress)(void *ctx, double fraction, const char *text);
    void      (*log)(void *ctx, const char *text);
    int       (*cancelled)(void *ctx);  /* asked between blocks of 1 MiB */
    /* A folder the program cannot walk by path (an Android document tree: a
     * content:// URI has no file name). When set it stands for the extracted
     * folder, `source` and `source_fd` are not looked at, and the installer
     * asks it for each file by its path as on the disc (forward slashes, the
     * manifest's spelling; the callback matches the case itself and also
     * finds the executable one folder below the tree's root). Returns an
     * open descriptor to read (the installer closes it) and its size in
     * *size; -1 when the source has no such file; -2 when it has and cannot
     * be read. Called with ctx, on the installing thread. The checks, the
     * journal and the messages are those of a folder given by path. */
    int       (*source_open)(void *ctx, const char *rel, uint64_t *size);
} xr_install_opts;

typedef struct {
    int      code;
    char     message[1024];     /* a sentence for the user, for every code */
    int      image;             /* the source was an image */
    uint64_t image_base;        /* its game partition's offset */
    int      already;           /* the marker was there and the files with it */
    unsigned copied, verified, kept;    /* files: copied; hashed in place; from the journal */
    uint64_t bytes_copied, bytes_verified;
    unsigned extra;             /* files in the source the manifest does not list */
} xr_install_result;

/* The whole install, or what is left of one. Returns r->code. */
int xr_install(const xr_install_opts *o, xr_install_result *r);

/* What <dest>/.complete says. 1 when it is there and well formed. */
typedef struct {
    char     manifest[80];
    char     version[80];
    unsigned files;
    uint64_t bytes;
} xr_install_marker;

int xr_install_marker_read(const char *dest, xr_install_marker *m);

/* Total size of the manifest's files. */
uint64_t xr_manifest_bytes(const xr_manifest *m);

#ifdef __cplusplus
}
#endif

#endif /* XR_DISC_INSTALL_H */
