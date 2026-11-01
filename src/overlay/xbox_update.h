/*
 * xbox_update - "Check for updates" (the overlay's Advanced page): one
 * anonymous HTTPS GET of GitHub's latest-release record, when the user
 * presses the row and not before. This header is the part that needs no
 * network: reading the few fields of the JSON that are used, comparing the
 * release tag with the running version, and turning the answer into the
 * lines the page shows. The GET itself is xbox_update_http_get(), one file
 * per platform (xbox_update_http_*.{mm,c}).
 *
 * The port registers its release repository and version with
 * xbox_OverlaySetUpdateCheck (xbox_settings.h); a port that does not has no
 * row. Nothing here reads a setting, writes a file or contacts anything on
 * its own.
 */
#ifndef XBOX_UPDATE_H
#define XBOX_UPDATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── no network: the JSON, the versions, the answer ───────────────────── */

/* The string value of a key of the JSON object's top level (nested objects
 * and arrays are skipped: "html_url" and "name" also occur in the release's
 * author and assets). Escapes are decoded (\n, \", \uXXXX with surrogate
 * pairs, as UTF-8). Returns 1 and the text in `out` (cut to `n` bytes, never
 * inside a UTF-8 sequence), 0 when the key is missing or its value is not a
 * string (null). */
int xbox_update_json_string(const char *json, const char *key, char *out, size_t n);

/* Compares a release tag ("v0.2.0", "0.2.0-rc1": a leading v and anything
 * after the digits and dots is ignored; at most four numbers) with the
 * running version ("0.1.0"). 1 when the tag is newer, 0 when equal, -1 when
 * the tag is older, -2 when either is not a version. */
int xbox_update_compare(const char *tag, const char *running);

enum xbox_update_kind {
    XU_LATEST,          /* the running version is the newest (or newer: a development build) */
    XU_NEWER,           /* a newer release is out: name, url and notes are set */
    XU_NONE,            /* no release published yet (HTTP 404) */
    XU_ERROR            /* could not check: `headline` says why */
};

#define XU_NOTE_LINES 8
#define XU_NOTE_WIDTH 74

typedef struct xbox_update_result {
    int  kind;                                   /* xbox_update_kind */
    char headline[200];                          /* "You have the latest version (0.1.0)", ... */
    char tag[64];                                /* XU_NEWER: the release's tag */
    char url[512];                               /* XU_NEWER: the release page (https://github.com/...) */
    char notes[XU_NOTE_LINES][XU_NOTE_WIDTH];    /* XU_NEWER: the first lines of the release notes */
    int  note_count;
} xbox_update_result;

/* The answer from what the GET gave: `status` is the HTTP status (0 when
 * there was no answer, `transport_error` then says why: "no connection",
 * "timed out"), `body` the response text or NULL. */
void xbox_update_evaluate(int status, const char *transport_error, const char *body,
                          const char *running_version, xbox_update_result *out);

/* ── the GET (one file per platform) ──────────────────────────────────── */

/* One HTTPS GET of `url` (Accept: application/vnd.github+json, the given
 * User-Agent), at most `timeout_ms` in all. Blocking: call it off the main
 * thread. Returns the HTTP status, or 0 with a short reason in `err`
 * ("no connection", "timed out", ...). On a status the response text is in
 * *body (malloc'd, NUL-terminated, at most 1 MB; free() it), or NULL.
 * Redirects are followed. Platforms without an implementation return 0 with
 * err "not available here". */
int xbox_update_http_get(const char *url, const char *user_agent, int timeout_ms,
                         char **body, char *err, size_t errn);

/* Whether this build can do the GET at all (false on Android: the overlay
 * has no row there). */
int xbox_update_available(void);

/* The release record's address for "owner/repo" (letters, digits, - _ .
 * and one /); 0 when the name is not that. */
int xbox_update_api_url(const char *repo, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_UPDATE_H */
