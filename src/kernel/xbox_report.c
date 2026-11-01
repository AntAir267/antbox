/*
 * xbox_report.c - the bug report archive; see xbox_report.h.
 *
 * Four parts, in this order below: the redaction, the zip writer (with its
 * own deflate), the system summary, and xbox_BugReport, which gathers the
 * members. Nothing here opens a socket or starts a process.
 *
 * The deflate is the simplest that is worth having: one block with the fixed
 * Huffman codes (RFC 1951, 3.2.6) over a hash-chain LZ77 match finder. Logs
 * shrink to about a fifth with it, which is the difference between an archive
 * of 300 KB and one of 1.5 MB; a member it cannot shrink is stored.
 */
#include "xbox_report.h"
#include "recomp_env.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <process.h>
#define rp_mkdir(p) _mkdir(p)
#define rp_getpid() ((long)_getpid())
#define rp_environ _environ
#else
#include <dirent.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#define rp_mkdir(p) mkdir((p), 0755)
#define rp_getpid() ((long)getpid())
extern char **environ;
#define rp_environ environ
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#endif

#if defined(XR_HAVE_BUILD_ID)
#include "xr_build_id.h"        /* XR_BUILD_COMMIT, made at build time */
#endif
#ifndef XR_BUILD_COMMIT
#define XR_BUILD_COMMIT "unknown"
#endif

#define LOGP "[REPORT] "

/* What one member may hold before it is cut (its head and its tail kept). */
#define LOG_HEAD        (64u * 1024u)
#define LOG_TAIL        (256u * 1024u)
#define PREV_LOG_TAIL   (128u * 1024u)
#define PREV_RUNS       3
#define CRASH_REPORTS   5
#define CRASH_TEXT_MAX  (128u * 1024u)
#define CRASH_LOG_MAX   (64u * 1024u)
#define FRAME_MAX       (8u * 1024u * 1024u)

/* ── a growing text ────────────────────────────────────────────────── */

struct xbox_report_buf {
    char *p;
    size_t n, cap;
};

static int buf_room(xbox_report_buf *b, size_t more)
{
    if (b->n + more + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        char *p;
        while (cap < b->n + more + 1)
            cap *= 2;
        p = (char *)realloc(b->p, cap);
        if (!p)
            return 0;
        b->p = p;
        b->cap = cap;
    }
    return 1;
}

static void buf_add(xbox_report_buf *b, const char *s, size_t n)
{
    if (!n || !buf_room(b, n))
        return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_str(xbox_report_buf *b, const char *s) { buf_add(b, s, strlen(s)); }

void xbox_report_printf(xbox_report_buf *b, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof tmp)
        n = (int)sizeof tmp - 1;
    buf_add(b, tmp, (size_t)n);
}

static void buf_free(xbox_report_buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* ── the redaction ─────────────────────────────────────────────────── */

static int is_word(unsigned char c)
{
    return isalnum(c) || c >= 0x80;
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* `pat` at `s` (n bytes left), ASCII case-insensitive; with `path` set a
 * slash matches a backslash. */
static int match_at(const char *s, size_t n, const char *pat, size_t pn, int path)
{
    size_t i;
    if (pn > n)
        return 0;
    for (i = 0; i < pn; i++) {
        unsigned char a = (unsigned char)s[i], b = (unsigned char)pat[i];
        if (path && is_sep((char)a) && is_sep((char)b))
            continue;
        if (tolower(a) != tolower(b))
            return 0;
    }
    return 1;
}

/* Names that say nothing about anyone: a console's fixed account, a
 * prefix's, a default host name. Replacing them would only damage the text
 * ("Steam Deck" for a user called deck). */
static int generic_name(const char *s)
{
    static const char *const k[] = {
        "deck", "steamuser", "user", "root", "admin", "administrator", "guest",
        "player", "owner", "home", "users", "localhost", "steamdeck", "bazzite",
        "android", "unknown", "default", "public", "shared", NULL,
    };
    int i;
    for (i = 0; k[i]; i++)
        if (strlen(s) == strlen(k[i]) && match_at(s, strlen(s), k[i], strlen(k[i]), 0))
            return 1;
    return 0;
}

static const char *const k_home_roots[] = { "/home/", "/Users/", NULL };

/* "/home/<name>" or "\Users\<name>" at s: the length to replace with ~. The
 * name runs to the next separator, or to the first blank, quote or line end
 * before it. */
static size_t home_like(const char *s, size_t n)
{
    int i;
    if (!n || !is_sep(s[0]))
        return 0;
    for (i = 0; k_home_roots[i]; i++) {
        size_t rn = strlen(k_home_roots[i]), k, stop = 0;
        if (!match_at(s, n, k_home_roots[i], rn, 1))
            continue;
        for (k = rn; k < n && k < rn + 64; k++) {
            char c = s[k];
            if (is_sep(c)) {
                /* A blank inside the name: a Windows account ("John Smith")
                 * when the path is a Windows one; else the name ended there. */
                if (stop && !(s[0] == '\\' && c == '\\'))
                    break;
                return k > rn ? k : 0;
            }
            if (c == '\n' || c == '\r' || c == '"' || c == '\'' || c == 0)
                break;
            if (!stop && (c == ' ' || c == '\t' || c == ',' || c == ';' || c == ')' || c == ':'))
                stop = k;
        }
        if (!stop)
            stop = k;
        return stop > rn ? stop : 0;
    }
    return 0;
}

#define RP_NAMES 6
typedef struct {
    const char *home[2];
    size_t home_n[2];
    const char *name[RP_NAMES];
    size_t name_n[RP_NAMES];
    const char *with[RP_NAMES];
    int names;
} rp_patterns;

static void pat_name(rp_patterns *p, const char *s, size_t n, const char *with)
{
    char tmp[256];
    if (!s || n < 2 || n >= sizeof tmp || p->names >= RP_NAMES)
        return;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    if (generic_name(tmp))
        return;
    p->name[p->names] = s;
    p->name_n[p->names] = n;
    p->with[p->names] = with;
    p->names++;
}

static void patterns(rp_patterns *p, const xbox_report_identity *who)
{
    memset(p, 0, sizeof *p);
    if (who->home && strlen(who->home) > 1) {
        size_t n = strlen(who->home);
        while (n > 1 && is_sep(who->home[n - 1]))
            n--;
        p->home[0] = who->home;
        p->home_n[0] = n;
#if defined(__APPLE__)
        /* /private/var/... and /var/... name one place. */
        if (n > 9 && !strncmp(who->home, "/private/", 9)) {
            p->home[1] = who->home + 8;
            p->home_n[1] = n - 8;
        }
#endif
    }
    if (who->user)
        pat_name(p, who->user, strlen(who->user), "<user>");
    if (who->full_name)
        pat_name(p, who->full_name, strlen(who->full_name), "<user>");
    if (who->host) {
        size_t n = strlen(who->host), dot = strcspn(who->host, ".");
        pat_name(p, who->host, n, "<host>");
        if (dot && dot < n) {
            pat_name(p, who->host, dot, "<host>");
            /* Its domain too, when it is one (not ".local", ".lan"). */
            if (strchr(who->host + dot + 1, '.'))
                pat_name(p, who->host + dot + 1, n - dot - 1, "<host>");
        }
    }
}

char *xbox_report_redact(const xbox_report_identity *who, const char *in, size_t len,
                         size_t *out_len)
{
    xbox_report_buf b = { NULL, 0, 0 };
    rp_patterns p;
    size_t i = 0, run = 0;
    int k;

    if (!who)
        who = xbox_report_identity_self();
    patterns(&p, who);
    if (!buf_room(&b, len + 16))
        return NULL;
    b.p[0] = 0;
    while (i < len) {
        const char *s = in + i;
        size_t left = len - i, take = 0;
        const char *with = NULL;
        int prev_word = i > 0 && is_word((unsigned char)in[i - 1]);

        for (k = 0; k < 2 && !take; k++) {
            /* The home directory, when what follows does not continue its
             * last name (/home/ann is not in /home/anna). */
            if (p.home[k] && match_at(s, left, p.home[k], p.home_n[k], 1)
                    && (left == p.home_n[k] || !is_word((unsigned char)s[p.home_n[k]]))) {
                take = p.home_n[k];
                with = "~";
            }
        }
        if (!take && (take = home_like(s, left)) != 0)
            with = "~";
        for (k = 0; k < p.names && !take; k++) {
            if (!prev_word && match_at(s, left, p.name[k], p.name_n[k], 0)
                    && (left == p.name_n[k] || !is_word((unsigned char)s[p.name_n[k]]))) {
                take = p.name_n[k];
                with = p.with[k];
            }
        }
        if (take) {
            buf_add(&b, in + i - run, run);
            run = 0;
            buf_str(&b, with);
            i += take;
        } else {
            run++;
            i++;
        }
    }
    buf_add(&b, in + i - run, run);
    if (out_len)
        *out_len = b.n;
    return b.p;
}

const xbox_report_identity *xbox_report_identity_self(void)
{
    static xbox_report_identity id;
    static char home[1024], user[256], full[256], host[256];
    static int done;
    const char *e;

    if (done)
        return &id;
#if defined(_WIN32)
    {
        DWORD n = sizeof host;
        if ((e = getenv("USERPROFILE")) != NULL)
            snprintf(home, sizeof home, "%s", e);
        if ((e = getenv("USERNAME")) != NULL)
            snprintf(user, sizeof user, "%s", e);
        if (!GetComputerNameA(host, &n))
            host[0] = 0;
    }
#else
    {
        struct passwd *pw = getpwuid(getuid());
        if ((e = getenv("HOME")) != NULL && *e)
            snprintf(home, sizeof home, "%s", e);
        else if (pw && pw->pw_dir)
            snprintf(home, sizeof home, "%s", pw->pw_dir);
        if (pw && pw->pw_name)
            snprintf(user, sizeof user, "%s", pw->pw_name);
        else if ((e = getenv("USER")) != NULL)
            snprintf(user, sizeof user, "%s", e);
#if !defined(__ANDROID__)
        if (pw && pw->pw_gecos && *pw->pw_gecos) {
            snprintf(full, sizeof full, "%s", pw->pw_gecos);
            full[strcspn(full, ",")] = 0;       /* "Name,room,phone" */
        }
#endif
        if (gethostname(host, sizeof host) != 0)
            host[0] = 0;
        host[sizeof host - 1] = 0;
    }
#endif
    id.home = home[0] ? home : NULL;
    id.user = user[0] ? user : NULL;
    id.full_name = full[0] ? full : NULL;
    id.host = host[0] ? host : NULL;
    done = 1;
    return &id;
}

/* ── deflate, fixed Huffman codes ──────────────────────────────────── */

typedef struct {
    unsigned char *p;
    size_t n, cap;
    uint32_t acc;
    int bits;
    int fail;
} bitw;

static void bw_byte(bitw *w, unsigned char c)
{
    if (w->n == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 8192;
        unsigned char *p = (unsigned char *)realloc(w->p, cap);
        if (!p) {
            w->fail = 1;
            return;
        }
        w->p = p;
        w->cap = cap;
    }
    w->p[w->n++] = c;
}

/* The low `n` bits of v, least significant first (n <= 16). */
static void bw_put(bitw *w, uint32_t v, int n)
{
    w->acc |= v << w->bits;
    w->bits += n;
    while (w->bits >= 8) {
        bw_byte(w, (unsigned char)w->acc);
        w->acc >>= 8;
        w->bits -= 8;
    }
}

/* A Huffman code goes in most significant bit first. */
static void bw_code(bitw *w, uint32_t code, int n)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < n; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    bw_put(w, r, n);
}

static void put_sym(bitw *w, int s)
{
    if (s < 144)
        bw_code(w, 0x30u + (uint32_t)s, 8);
    else if (s < 256)
        bw_code(w, 0x190u + (uint32_t)(s - 144), 9);
    else if (s < 280)
        bw_code(w, (uint32_t)(s - 256), 7);
    else
        bw_code(w, 0xC0u + (uint32_t)(s - 280), 8);
}

static const uint16_t k_len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t k_len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t k_dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t k_dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(bitw *w, unsigned len, unsigned dist)
{
    int c;
    for (c = 28; len < k_len_base[c]; c--)
        ;
    put_sym(w, 257 + c);
    if (k_len_extra[c])
        bw_put(w, len - k_len_base[c], k_len_extra[c]);
    for (c = 29; dist < k_dist_base[c]; c--)
        ;
    bw_code(w, (uint32_t)c, 5);
    if (k_dist_extra[c])
        bw_put(w, dist - k_dist_base[c], k_dist_extra[c]);
}

#define DF_HASH_BITS 15
#define DF_WINDOW    32768u
#define DF_CHAIN     48

static uint32_t df_hash(const unsigned char *p)
{
    return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u
           >> (32 - DF_HASH_BITS);
}

/* `in` deflated into a malloc'd buffer, or NULL when memory ran out. */
static unsigned char *deflate_fixed(const unsigned char *in, size_t n, size_t *out_n)
{
    bitw w = { NULL, 0, 0, 0, 0, 0 };
    int32_t *head = (int32_t *)malloc(sizeof(int32_t) * (1u << DF_HASH_BITS));
    int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * DF_WINDOW);
    size_t i = 0;

    if (!head || !prev) {
        free(head);
        free(prev);
        return NULL;
    }
    memset(head, 0xFF, sizeof(int32_t) * (1u << DF_HASH_BITS));
    bw_put(&w, 1, 1);           /* the last block */
    bw_put(&w, 1, 2);           /* fixed Huffman codes */
    while (i < n && !w.fail) {
        unsigned best = 0, dist = 0;
        if (i + 3 <= n) {
            uint32_t h = df_hash(in + i);
            int32_t c = head[h];
            unsigned max = n - i < 258 ? (unsigned)(n - i) : 258u;
            int chain = DF_CHAIN;
            while (c >= 0 && i - (size_t)c <= DF_WINDOW && chain-- > 0) {
                const unsigned char *a = in + c, *b = in + i;
                unsigned l = 0;
                if (a[best] == b[best]) {
                    while (l < max && a[l] == b[l])
                        l++;
                    if (l > best) {
                        best = l;
                        dist = (unsigned)(i - (size_t)c);
                        if (l == max)
                            break;
                    }
                }
                c = prev[(size_t)c & (DF_WINDOW - 1)];
            }
            prev[i & (DF_WINDOW - 1)] = head[h];
            head[h] = (int32_t)i;
        }
        if (best >= 3) {
            unsigned k;
            put_match(&w, best, dist);
            for (k = 1; k < best; k++) {
                if (i + k + 3 <= n) {
                    uint32_t h = df_hash(in + i + k);
                    prev[(i + k) & (DF_WINDOW - 1)] = head[h];
                    head[h] = (int32_t)(i + k);
                }
            }
            i += best;
        } else {
            put_sym(&w, in[i]);
            i++;
        }
    }
    put_sym(&w, 256);
    if (w.bits)
        bw_put(&w, 0, 8 - w.bits);
    free(head);
    free(prev);
    if (w.fail) {
        free(w.p);
        return NULL;
    }
    *out_n = w.n;
    return w.p;
}

/* ── the zip ───────────────────────────────────────────────────────── */

static uint32_t crc32_of(const unsigned char *p, size_t n)
{
    static uint32_t table[256];
    static int ready;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;

    if (!ready) {
        uint32_t k, j;
        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            for (j = 0; j < 8; j++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[k] = v;
        }
        ready = 1;
    }
    for (i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

#define ZIP_MAX_MEMBERS 96

typedef struct {
    char name[160];
    uint32_t crc, csize, usize, offset;
    uint16_t method;
} zip_member;

struct xbox_report {
    FILE *f;
    xbox_report_identity who;
    zip_member m[ZIP_MAX_MEMBERS];
    int n;
    uint32_t offset;
    uint16_t dos_time, dos_date;
    int fail;
};

static void le16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void le32(unsigned char *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }

static void zip_write(xbox_report *r, const void *p, size_t n)
{
    if (n && fwrite(p, 1, n, r->f) != n)
        r->fail = 1;
    r->offset += (uint32_t)n;
}

xbox_report *xbox_report_open(const char *zip_path, const xbox_report_identity *who)
{
    xbox_report *r = (xbox_report *)calloc(1, sizeof *r);
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);

    if (!r)
        return NULL;
    r->f = fopen(zip_path, "wb");
    if (!r->f) {
        free(r);
        return NULL;
    }
    r->who = who ? *who : *xbox_report_identity_self();
    if (lt && lt->tm_year >= 80) {
        r->dos_time = (uint16_t)(lt->tm_hour << 11 | lt->tm_min << 5 | lt->tm_sec / 2);
        r->dos_date = (uint16_t)((lt->tm_year - 80) << 9 | (lt->tm_mon + 1) << 5 | lt->tm_mday);
    }
    return r;
}

static int zip_add(xbox_report *r, const char *member, const void *data, size_t len)
{
    unsigned char h[30], *z = NULL;
    size_t zn = 0, name_n = strlen(member);
    zip_member *m;

    if (!r || r->n >= ZIP_MAX_MEMBERS || name_n >= sizeof r->m[0].name || len > 0x7FFFFFFFu)
        return 0;
    m = &r->m[r->n];
    memcpy(m->name, member, name_n + 1);
    m->crc = crc32_of((const unsigned char *)data, len);
    m->usize = (uint32_t)len;
    m->offset = r->offset;
    if (len > 64)
        z = deflate_fixed((const unsigned char *)data, len, &zn);
    if (z && zn < len) {
        m->method = 8;
        m->csize = (uint32_t)zn;
    } else {
        m->method = 0;
        m->csize = (uint32_t)len;
    }
    memset(h, 0, sizeof h);
    le32(h, 0x04034B50u);
    le16(h + 4, 20);
    le16(h + 6, 0x0800);        /* names are UTF-8 */
    le16(h + 8, m->method);
    le16(h + 10, r->dos_time);
    le16(h + 12, r->dos_date);
    le32(h + 14, m->crc);
    le32(h + 18, m->csize);
    le32(h + 22, m->usize);
    le16(h + 26, (uint32_t)name_n);
    zip_write(r, h, sizeof h);
    zip_write(r, member, name_n);
    zip_write(r, m->method == 8 ? (const void *)z : data, m->csize);
    free(z);
    r->n++;
    return !r->fail;
}

int xbox_report_text(xbox_report *r, const char *member, const char *text, size_t len)
{
    size_t n = 0;
    char *red;
    int ok;

    if (!r)
        return 0;
    red = xbox_report_redact(&r->who, text, len, &n);
    if (!red)
        return 0;
    ok = zip_add(r, member, red, n);
    free(red);
    return ok;
}

int xbox_report_binary(xbox_report *r, const char *member, const void *data, size_t len)
{
    return zip_add(r, member, data, len);
}

int xbox_report_count(const xbox_report *r) { return r ? r->n : 0; }

int xbox_report_close(xbox_report *r)
{
    unsigned char h[46];
    uint32_t dir_at, dir_n;
    int i, ok;

    if (!r)
        return 0;
    dir_at = r->offset;
    for (i = 0; i < r->n; i++) {
        const zip_member *m = &r->m[i];
        size_t name_n = strlen(m->name);
        memset(h, 0, sizeof h);
        le32(h, 0x02014B50u);
        le16(h + 4, 20);
        le16(h + 6, 20);
        le16(h + 8, 0x0800);
        le16(h + 10, m->method);
        le16(h + 12, r->dos_time);
        le16(h + 14, r->dos_date);
        le32(h + 16, m->crc);
        le32(h + 20, m->csize);
        le32(h + 24, m->usize);
        le16(h + 28, (uint32_t)name_n);
        le32(h + 42, m->offset);
        zip_write(r, h, 46);
        zip_write(r, m->name, name_n);
    }
    dir_n = r->offset - dir_at;
    memset(h, 0, 22);
    le32(h, 0x06054B50u);
    le16(h + 8, (uint32_t)r->n);
    le16(h + 10, (uint32_t)r->n);
    le32(h + 12, dir_n);
    le32(h + 16, dir_at);
    zip_write(r, h, 22);
    if (fclose(r->f) != 0)
        r->fail = 1;
    ok = !r->fail;
    free(r);
    return ok;
}

/* ── files ─────────────────────────────────────────────────────────── */

static long long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long long n = -1;
    if (!f)
        return -1;
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) == 0)
        n = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) == 0)
        n = (long long)ftello(f);
#endif
    fclose(f);
    return n;
}

static size_t read_at(FILE *f, long long off, char *out, size_t n)
{
#if defined(_WIN32)
    if (_fseeki64(f, off, SEEK_SET) != 0)
        return 0;
#else
    if (fseeko(f, (off_t)off, SEEK_SET) != 0)
        return 0;
#endif
    return fread(out, 1, n, f);
}

/* The file into b: all of it, or its first `head` and last `tail` bytes
 * around a line that says what is missing. 0 when it cannot be opened. */
static int buf_file(xbox_report_buf *b, const char *path, size_t head, size_t tail)
{
    long long size = file_size(path);
    FILE *f;
    size_t got;

    if (size < 0 || !(f = fopen(path, "rb")))
        return 0;
    if ((unsigned long long)size <= (unsigned long long)head + tail) {
        if (buf_room(b, (size_t)size)) {
            got = read_at(f, 0, b->p + b->n, (size_t)size);
            b->n += got;
            b->p[b->n] = 0;
        }
    } else {
        if (head && buf_room(b, head)) {
            got = read_at(f, 0, b->p + b->n, head);
            b->n += got;
            b->p[b->n] = 0;
        }
        xbox_report_printf(b, "\n[... %lld bytes left out of the report ...]\n",
                           size - (long long)head - (long long)tail);
        if (tail && buf_room(b, tail)) {
            got = read_at(f, size - (long long)tail, b->p + b->n, tail);
            b->n += got;
            b->p[b->n] = 0;
        }
    }
    fclose(f);
    return 1;
}

int xbox_report_file(xbox_report *r, const char *member, const char *path,
                     size_t head, size_t tail)
{
    xbox_report_buf b = { NULL, 0, 0 };
    int ok;

    if (!r || !path || !*path || !buf_file(&b, path, head, tail))
        return 0;
    ok = xbox_report_text(r, member, b.p ? b.p : "", b.n);
    buf_free(&b);
    return ok;
}

static void mkdir_p(const char *path)
{
    char tmp[1024];
    size_t i, n = strlen(path);

    if (!n || n >= sizeof tmp)
        return;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (is_sep(tmp[i])) {
            char c = tmp[i];
            if (i == 2 && tmp[1] == ':')
                continue;
            tmp[i] = 0;
            rp_mkdir(tmp);
            tmp[i] = c;
        }
    }
    rp_mkdir(tmp);
}

typedef struct {
    char name[200];
    int is_dir;
    unsigned long long size;
} rp_ent;

static int ent_cmp(const void *a, const void *b)
{
    return strcmp(((const rp_ent *)a)->name, ((const rp_ent *)b)->name);
}

/* The entries of `dir`, sorted by name; at most `max`. */
static int list_dir(const char *dir, rp_ent *out, int max)
{
    int n = 0;
#if defined(_WIN32)
    char pat[1100];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || n >= max)
            continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", fd.cFileName);
        out[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out[n].size = (unsigned long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char path[1300];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || n >= max)
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (lstat(path, &st) != 0)
            continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
        out[n].is_dir = S_ISDIR(st.st_mode);
        out[n].size = S_ISREG(st.st_mode) ? (unsigned long long)st.st_size : 0;
        n++;
    }
    closedir(d);
#endif
    qsort(out, (size_t)n, sizeof out[0], ent_cmp);
    return n;
}

#define DIR_LIST_MAX 256

static int is_dir(const char *path)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void dir_total(const char *dir, int depth, unsigned long long *files,
                      unsigned long long *bytes)
{
    rp_ent *e = (rp_ent *)malloc(sizeof(rp_ent) * DIR_LIST_MAX);
    int n, i;

    if (!e)
        return;
    n = list_dir(dir, e, DIR_LIST_MAX);
    for (i = 0; i < n; i++) {
        if (e[i].is_dir) {
            char sub[1300];
            snprintf(sub, sizeof sub, "%s/%s", dir, e[i].name);
            if (depth < 6)
                dir_total(sub, depth + 1, files, bytes);
        } else {
            (*files)++;
            *bytes += e[i].size;
        }
    }
    free(e);
}

void xbox_report_dir_summary(xbox_report_buf *b, const char *label, const char *dir)
{
    rp_ent *e;
    unsigned long long files = 0, bytes = 0;
    char parent[1100];
    int n, i;

    if (!dir || !*dir) {
        xbox_report_printf(b, "%s: none\n", label);
        return;
    }
    /* A file's name (a cache's own file): the folder it is in. */
    if (!is_dir(dir)) {
        size_t k = strlen(dir);
        while (k > 1 && !is_sep(dir[k - 1]))
            k--;
        if (k > 1 && k < sizeof parent) {
            memcpy(parent, dir, k - 1);
            parent[k - 1] = 0;
            if (is_dir(parent))
                dir = parent;
        }
    }
    e = (rp_ent *)malloc(sizeof(rp_ent) * DIR_LIST_MAX);
    if (!e)
        return;
    n = list_dir(dir, e, DIR_LIST_MAX);
    dir_total(dir, 0, &files, &bytes);
    xbox_report_printf(b, "%s: %s: %llu files, %llu bytes (contents not included)\n",
                       label, dir, files, bytes);
    for (i = 0; i < n && i < 40; i++) {
        if (e[i].is_dir) {
            char sub[1300];
            unsigned long long f = 0, by = 0;
            snprintf(sub, sizeof sub, "%s/%s", dir, e[i].name);
            dir_total(sub, 1, &f, &by);
            xbox_report_printf(b, "  %-32s %llu files, %llu bytes\n", e[i].name, f, by);
        } else {
            xbox_report_printf(b, "  %-32s %llu bytes\n", e[i].name, e[i].size);
        }
    }
    if (n > 40)
        xbox_report_printf(b, "  ... and %d more\n", n - 40);
    free(e);
}

/* ── what is registered ────────────────────────────────────────────── */

#define SECTIONS_MAX 24
#define ATTACH_MAX   8

static struct {
    const char *member, *heading;
    xbox_report_section_fn fn;
} s_section[SECTIONS_MAX];
static int s_sections;

static struct {
    const char *member;
    const char *(*path_fn)(void);
    size_t head, tail;
} s_attach[ATTACH_MAX];
static int s_attached;

static int (*s_capture)(const char *path, unsigned timeout_ms);
static void (*s_flush)(void);

void xbox_report_section(const char *member, const char *heading, xbox_report_section_fn fn)
{
    int i;
    if (!member || !fn)
        return;
    for (i = 0; i < s_sections; i++)
        if (s_section[i].fn == fn && !strcmp(s_section[i].member, member))
            return;
    if (s_sections == SECTIONS_MAX)
        return;
    s_section[s_sections].member = member;
    s_section[s_sections].heading = heading;
    s_section[s_sections].fn = fn;
    s_sections++;
}

void xbox_report_attach(const char *member, const char *(*path_fn)(void),
                        size_t head, size_t tail)
{
    int i;
    if (!member || !path_fn)
        return;
    for (i = 0; i < s_attached; i++)
        if (!strcmp(s_attach[i].member, member))
            return;
    if (s_attached == ATTACH_MAX)
        return;
    s_attach[s_attached].member = member;
    s_attach[s_attached].path_fn = path_fn;
    s_attach[s_attached].head = head;
    s_attach[s_attached].tail = tail;
    s_attached++;
}

void xbox_report_set_capture(int (*fn)(const char *, unsigned)) { s_capture = fn; }
void xbox_report_set_flush(void (*fn)(void)) { s_flush = fn; }

static void run_sections(xbox_report_buf *b, const char *member)
{
    int i;
    for (i = 0; i < s_sections; i++) {
        if (strcmp(s_section[i].member, member))
            continue;
        if (s_section[i].heading)
            xbox_report_printf(b, "\n[%s]\n", s_section[i].heading);
        s_section[i].fn(b);
        if (b->n && b->p[b->n - 1] != '\n')
            buf_str(b, "\n");
    }
}

/* ── the environment ───────────────────────────────────────────────── */

static char *s_env_names;       /* "\nNAME\nNAME\n": set when the process started */

void xbox_report_env_snapshot(void)
{
    xbox_report_buf b = { NULL, 0, 0 };
    char **e;

    if (s_env_names)
        return;
    buf_str(&b, "\n");
    for (e = rp_environ; e && *e; e++) {
        size_t n = strcspn(*e, "=");
        buf_add(&b, *e, n);
        buf_str(&b, "\n");
    }
    s_env_names = b.p;
}

/* 1 set at start, 0 set later, -1 unknown (no snapshot). */
static int env_at_start(const char *name)
{
    char key[160];
    if (!s_env_names)
        return -1;
    snprintf(key, sizeof key, "\n%s\n", name);
    return strstr(s_env_names, key) != NULL;
}

static const char *const k_default_prefixes[] = { "RECOMP_", NULL };

static int own_variable(const char *entry, const char *const *prefixes)
{
    int i;
    for (i = 0; prefixes[i]; i++)
        if (!strncmp(entry, prefixes[i], strlen(prefixes[i])))
            return 1;
    return 0;
}

static const char *source_text(int at_start)
{
    return at_start > 0 ? "environment"
         : at_start == 0 ? "settings file or default" : "";
}

static void write_effective(xbox_report_buf *b, const char *const *prefixes)
{
    char **e;
    int i;

    buf_str(b, "Where a value comes from, highest first: a variable set when the\n"
               "process started (environment), the settings file, the title's and the\n"
               "runtime's defaults. The sections after this one say which, per setting.\n");
    buf_str(b, "\n[runtime table: every value the runtime holds]\n");
    for (i = 0; i < RENV_COUNT; i++) {
        const char *v = recomp_env((recomp_env_id)i);
        const char *list = recomp_env_list_var((recomp_env_id)i);
        const char *name = recomp_env_name((recomp_env_id)i);
        const char *src;
        if (!v)
            continue;
        {
            /* Set under its older name (a host's default, a script): that
             * variable's source, not the list's. */
            const char *legacy = recomp_env_legacy_name((recomp_env_id)i);
            const char *lv = legacy ? getenv(legacy) : NULL;
            src = source_text(env_at_start(lv ? legacy : list ? list : name));
        }
        if (list)
            xbox_report_printf(b, "%s: %s=%s", list, name, v);
        else
            xbox_report_printf(b, "%s=%s", name, v);
        if (*src)
            xbox_report_printf(b, "   (%s)", src);
        buf_str(b, "\n");
    }
    buf_str(b, "\n[the process's own variables:");
    for (i = 0; prefixes[i]; i++)
        xbox_report_printf(b, " %s*", prefixes[i]);
    buf_str(b, "; no other variable is read for this report]\n");
    for (e = rp_environ; e && *e; e++) {
        char name[160];
        const char *src;
        size_t n = strcspn(*e, "=");
        if (!own_variable(*e, prefixes) || n >= sizeof name)
            continue;
        memcpy(name, *e, n);
        name[n] = 0;
        src = source_text(env_at_start(name));
        xbox_report_printf(b, "%s", *e);
        if (*src)
            xbox_report_printf(b, "   (%s)", src);
        buf_str(b, "\n");
    }
}

/* ── the system ────────────────────────────────────────────────────── */

#if !defined(_WIN32) && !defined(__APPLE__)
#if !defined(__ANDROID__)
/* The first line of a small file, trimmed; "" when it cannot be read. */
static void first_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f)
        return;
    if (!fgets(out, (int)n, f))
        out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}
#endif

/* "key<sep>value" from a /proc or os-release style file: the first match. */
static void file_key(const char *path, const char *key, char sep, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    char line[512];
    size_t kn = strlen(key);

    out[0] = 0;
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *v;
        if (strncmp(line, key, kn))
            continue;
        v = line + kn;
        while (*v == ' ' || *v == '\t')
            v++;
        if (*v != sep)
            continue;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '"')
            v++;
        v[strcspn(v, "\r\n")] = 0;
        while (*v && v[strlen(v) - 1] == '"')
            v[strlen(v) - 1] = 0;
        snprintf(out, n, "%s", v);
        break;
    }
    fclose(f);
}
#endif

#if defined(_WIN32)
static void reg_string(const char *key, const char *value, char *out, DWORD n)
{
    HKEY h;
    DWORD type = 0, size = n - 1;

    out[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(h, value, NULL, &type, (LPBYTE)out, &size) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ))
        out[size < n ? size : n - 1] = 0;
    else
        out[0] = 0;
    RegCloseKey(h);
}
#endif

#if defined(__ANDROID__)
static void sysprop(const char *name, char *out)     /* PROP_VALUE_MAX bytes */
{
    if (__system_property_get(name, out) <= 0)
        out[0] = 0;
}
#endif

static void write_os(xbox_report_buf *b)
{
#if defined(_WIN32)
    static const char *const cv = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    static const char *const bios = "HARDWARE\\DESCRIPTION\\System\\BIOS";
    char product[128], display[64], build[32], cpu[128], maker[128], model[128];
    MEMORYSTATUSEX ms;
    SYSTEM_INFO si;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    const char *(*wine_version)(void) =
        ntdll ? (const char *(*)(void))(void *)GetProcAddress(ntdll, "wine_get_version") : NULL;

    reg_string(cv, "ProductName", product, sizeof product);
    reg_string(cv, "DisplayVersion", display, sizeof display);
    reg_string(cv, "CurrentBuild", build, sizeof build);
    reg_string("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
               cpu, sizeof cpu);
    reg_string(bios, "SystemManufacturer", maker, sizeof maker);
    reg_string(bios, "SystemProductName", model, sizeof model);
    /* ProductName still says "Windows 10" on 11; the build number tells. */
    xbox_report_printf(b, "os: %s %s, build %s%s\n", product[0] ? product : "Windows", display,
                       build, atoi(build) >= 22000 ? " (Windows 11)" : "");
    if (wine_version)
        xbox_report_printf(b, "wine: %s (a Windows build under Wine or Proton)\n", wine_version());
    if (maker[0] || model[0])
        xbox_report_printf(b, "device: %s %s\n", maker, model);
    GetSystemInfo(&si);
    xbox_report_printf(b, "cpu: %s, %lu logical cores\n", cpu[0] ? cpu : "?",
                       (unsigned long)si.dwNumberOfProcessors);
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        xbox_report_printf(b, "memory: %llu MiB\n", (unsigned long long)(ms.ullTotalPhys >> 20));
#elif defined(__APPLE__)
    char s[256];
    size_t n;
    uint64_t mem = 0;
    int cores = 0;
    struct utsname u;

    n = sizeof s;
    if (sysctlbyname("kern.osproductversion", s, &n, NULL, 0) == 0)
        xbox_report_printf(b, "os: macOS %s", s);
    else
        buf_str(b, "os: macOS");
    n = sizeof s;
    if (sysctlbyname("kern.osversion", s, &n, NULL, 0) == 0)
        xbox_report_printf(b, " (%s)", s);
    if (uname(&u) == 0)
        xbox_report_printf(b, ", %s %s %s", u.sysname, u.release, u.machine);
    buf_str(b, "\n");
    n = sizeof s;
    if (sysctlbyname("hw.model", s, &n, NULL, 0) == 0)
        xbox_report_printf(b, "device: %s\n", s);
    n = sizeof s;
    if (sysctlbyname("machdep.cpu.brand_string", s, &n, NULL, 0) != 0)
        snprintf(s, sizeof s, "?");
    n = sizeof cores;
    sysctlbyname("hw.ncpu", &cores, &n, NULL, 0);
    xbox_report_printf(b, "cpu: %s, %d logical cores\n", s, cores);
    n = sizeof mem;
    if (sysctlbyname("hw.memsize", &mem, &n, NULL, 0) == 0)
        xbox_report_printf(b, "memory: %llu MiB\n", (unsigned long long)(mem >> 20));
#else
    char s[256];
#if !defined(__ANDROID__)
    char t[256];
#endif
    struct utsname u;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);

#if defined(__ANDROID__)
    {
        char rel[PROP_VALUE_MAX], sdk[PROP_VALUE_MAX], maker[PROP_VALUE_MAX],
             model[PROP_VALUE_MAX], dev[PROP_VALUE_MAX], soc[PROP_VALUE_MAX],
             socm[PROP_VALUE_MAX], board[PROP_VALUE_MAX], rom[PROP_VALUE_MAX];
        sysprop("ro.build.version.release", rel);
        sysprop("ro.build.version.sdk", sdk);
        sysprop("ro.product.manufacturer", maker);
        sysprop("ro.product.model", model);
        sysprop("ro.product.device", dev);
        sysprop("ro.soc.manufacturer", socm);
        sysprop("ro.soc.model", soc);
        sysprop("ro.board.platform", board);
        sysprop("ro.build.display.id", rom);
        xbox_report_printf(b, "os: Android %s (API %s), build %s\n", rel, sdk, rom);
        xbox_report_printf(b, "device: %s %s (%s)\n", maker, model, dev);
        xbox_report_printf(b, "soc: %s %s, platform %s\n", socm, soc, board);
    }
#else
    file_key("/etc/os-release", "PRETTY_NAME", '=', s, sizeof s);
    file_key("/etc/os-release", "VARIANT_ID", '=', t, sizeof t);
    xbox_report_printf(b, "os: %s%s%s\n", s[0] ? s : "Linux", t[0] ? ", variant " : "", t);
    first_line("/sys/devices/virtual/dmi/id/sys_vendor", s, sizeof s);
    first_line("/sys/devices/virtual/dmi/id/product_name", t, sizeof t);
    if (s[0] || t[0])
        xbox_report_printf(b, "device: %s %s\n", s, t);
#endif
    if (uname(&u) == 0)
        xbox_report_printf(b, "kernel: %s %s %s\n", u.sysname, u.release, u.machine);
    file_key("/proc/cpuinfo", "model name", ':', s, sizeof s);
    if (!s[0])
        file_key("/proc/cpuinfo", "Hardware", ':', s, sizeof s);
    xbox_report_printf(b, "cpu: %s, %ld logical cores\n", s[0] ? s : "?", cores);
    file_key("/proc/meminfo", "MemTotal", ':', s, sizeof s);
    if (s[0])
        xbox_report_printf(b, "memory: %lu MiB\n", strtoul(s, NULL, 10) >> 10);
#endif
}

static const char *arch_name(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "?";
#endif
}

/* Lines of a run's log that describe the machine as the runtime found it:
 * the GPU and its driver, the display mode, the controllers, the audio
 * device. The backends print them at start; quoting them here keeps this
 * file out of the backends. */
static void log_facts(xbox_report_buf *b, const char *log, size_t len)
{
    static const char *const want[] = {
        "device up:", "[PRESENT] display", "[PRESENT] window", "[PRESENT] CAMetalLayer",
        "[INPUT] controller", "[SDL2] audio", "[APU] speakers", "[ENHANCE] render.scale",
        "[ENHANCE] display.", "GPU driver", "Vulkan driver", NULL,
    };
    size_t i = 0;
    int lines = 0;

    while (i < len && lines < 60) {
        size_t e = i, n;
        int k;
        while (e < len && log[e] != '\n')
            e++;
        n = e - i;
        for (k = 0; want[k] && n; k++) {
            size_t wn = strlen(want[k]), j;
            int hit = 0;
            for (j = 0; j + wn <= n && j < 48 && !hit; j++)
                hit = !memcmp(log + i + j, want[k], wn);
            if (hit) {
                buf_add(b, log + i, n > 400 ? 400 : n);
                buf_str(b, "\n");
                lines++;
                break;
            }
        }
        i = e + 1;
    }
    if (!lines)
        buf_str(b, "(none: no run's log names them yet)\n");
}

/* What is in effect, from the runtime's table, by variable name. */
static void write_in_effect(xbox_report_buf *b)
{
    static const char *const names[] = {
        "RECOMP_PB_BACKEND", "RECOMP_RENDER_SCALE", "RECOMP_PRESENT_VSYNC",
        "RECOMP_PRESENT_PACING", "RECOMP_PRESENT_SPIN_LEAD", "RECOMP_FLIP_PACING",
        "RECOMP_VBLANK_SYNC", "RECOMP_PRESENT_FILTER", "RECOMP_PRESENT_FULLSCREEN",
        "RECOMP_PRESENT_ASPECT", "RECOMP_DISPLAY_WIDESCREEN", "RECOMP_HEADLESS", NULL,
    };
    int i, k;

    for (k = 0; names[k]; k++) {
        const char *v = NULL;
        for (i = 0; i < RENV_COUNT; i++)
            if (!strcmp(recomp_env_name((recomp_env_id)i), names[k])) {
                v = recomp_env((recomp_env_id)i);
                break;
            }
        if (i < RENV_COUNT)
            xbox_report_printf(b, "%s: %s\n", names[k], v ? v : "(unset: the runtime's default)");
    }
}

/* ── the logs ──────────────────────────────────────────────────────── */

/* One run's log as xbox_crash.c keeps it: <prefix>.log, its first part, and
 * <prefix>.tail.old.log + <prefix>.tail.log, what came after. 0 when the run
 * left no file. */
static int run_log(xbox_report_buf *b, const char *dir, const char *prefix, size_t tail_max)
{
    char head[1300], t0[1300], t1[1300];
    long long n0, n1;
    int any;

    snprintf(head, sizeof head, "%s/%s.log", dir, prefix);
    snprintf(t0, sizeof t0, "%s/%s.tail.old.log", dir, prefix);
    snprintf(t1, sizeof t1, "%s/%s.tail.log", dir, prefix);
    any = buf_file(b, head, LOG_HEAD, 0);
    n0 = file_size(t0);
    n1 = file_size(t1);
    if (n0 < 0)
        n0 = 0;
    if (n1 < 0)
        n1 = 0;
    if (n0 + n1 > 0) {
        any = 1;
        if ((unsigned long long)(n0 + n1) <= tail_max) {
            buf_file(b, t0, (size_t)n0, 0);
            buf_file(b, t1, (size_t)n1, 0);
        } else {
            xbox_report_printf(b, "\n[... %lld bytes left out of the report ...]\n",
                               n0 + n1 - (long long)tail_max);
            if ((unsigned long long)n1 < tail_max) {
                /* buf_file's own "left out" line would count twice: read the
                 * end of the older file by hand. */
                FILE *f = fopen(t0, "rb");
                size_t want = tail_max - (size_t)n1;
                if (f && buf_room(b, want)) {
                    size_t got = read_at(f, n0 - (long long)want, b->p + b->n, want);
                    b->n += got;
                    b->p[b->n] = 0;
                }
                if (f)
                    fclose(f);
                buf_file(b, t1, (size_t)n1, 0);
            } else {
                FILE *f = fopen(t1, "rb");
                if (f && buf_room(b, tail_max)) {
                    size_t got = read_at(f, n1 - (long long)tail_max, b->p + b->n, tail_max);
                    b->n += got;
                    b->p[b->n] = 0;
                }
                if (f)
                    fclose(f);
            }
        }
    }
    return any;
}

/* ── the crash folder ──────────────────────────────────────────────── */

/* A crash report's settings.txt lists SDL_ and sanitizer variables as well
 * (xbox_crash.c); the bug report keeps the host's own only. */
static void filter_crash_settings(xbox_report_buf *out, const char *in, size_t len,
                                  const char *const *prefixes)
{
    size_t i = 0;
    int in_env = 0;

    while (i < len) {
        size_t e = i;
        while (e < len && in[e] != '\n')
            e++;
        if (e > i && in[i] == '[')
            in_env = e - i > 13 && !strncmp(in + i, "[environment:", 13);
        if (!in_env || in[i] == '[' || e == i) {
            buf_add(out, in + i, e - i);
            buf_str(out, "\n");
        } else {
            char line[64];
            size_t n = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
            memcpy(line, in + i, n);
            line[n] = 0;
            if (own_variable(line, prefixes)) {
                buf_add(out, in + i, e - i);
                buf_str(out, "\n");
            }
        }
        i = e + 1;
    }
}

static int add_crashes(xbox_report *r, xbox_report_buf *readme, const char *dir,
                       const char *const *prefixes)
{
    rp_ent *e = (rp_ent *)malloc(sizeof(rp_ent) * DIR_LIST_MAX);
    int n, i, added = 0, dirs = 0;

    if (!e)
        return 0;
    n = list_dir(dir, e, DIR_LIST_MAX);
    for (i = 0; i < n; i++)
        dirs += e[i].is_dir;
    /* The names start with the time: the last ones are the newest. */
    for (i = n - 1; i >= 0 && added < CRASH_REPORTS; i--) {
        static const char *const texts[] = { "report.txt", "log.txt", NULL };
        char path[1500], member[400];
        xbox_report_buf b = { NULL, 0, 0 }, f = { NULL, 0, 0 };
        long long dump;
        int k;

        if (!e[i].is_dir)
            continue;
        for (k = 0; texts[k]; k++) {
            snprintf(path, sizeof path, "%s/%s/%s", dir, e[i].name, texts[k]);
            snprintf(member, sizeof member, "crash/%.150s/%s", e[i].name, texts[k]);
            xbox_report_file(r, member, path, k ? 0 : CRASH_TEXT_MAX / 2,
                             k ? CRASH_LOG_MAX : CRASH_TEXT_MAX / 2);
        }
        snprintf(path, sizeof path, "%s/%s/settings.txt", dir, e[i].name);
        if (buf_file(&b, path, 32768, 0)) {
            filter_crash_settings(&f, b.p ? b.p : "", b.n, prefixes);
            snprintf(member, sizeof member, "crash/%.150s/settings.txt", e[i].name);
            xbox_report_text(r, member, f.p ? f.p : "", f.n);
        }
        buf_free(&b);
        buf_free(&f);
        snprintf(path, sizeof path, "%s/%s/minidump.dmp", dir, e[i].name);
        dump = file_size(path);
        if (dump >= 0)
            xbox_report_printf(readme, "crash/%s: a minidump of %lld bytes is in the crash"
                               " folder; it is not in this archive.\n", e[i].name, dump);
        added++;
    }
    xbox_report_printf(readme, "crash reports: %d in the crash folder, the newest %d here.\n",
                       dirs, added);
    free(e);
    return added;
}

/* ── the report ────────────────────────────────────────────────────── */

static const char k_readme[] =
    "In this archive:\n"
    "  system.txt              OS, device, CPU, memory, GPU and driver, displays,\n"
    "                          controllers, the build\n"
    "  settings.toml           the settings file, as it is\n"
    "  settings-effective.txt  every setting and where its value comes from\n"
    "  stats.txt               frame times of the session, shader compile counts\n"
    "  logs/                   this run's log and the runs before it\n"
    "  crash/                  the newest crash and hang reports\n"
    "  gaps.bin                the pipeline keys the game had to build while drawing\n"
    "                          (keys only, no compiled code), when there is one\n"
    "  frame.bmp               the last frame (only when asked for)\n"
    "\n"
    "Never in it: save files, disc data, shader or pipeline cache contents (sizes\n"
    "and counts only), minidumps, environment variables other than the game's own.\n"
    "The home directory is written ~, the user name <user>, the host name <host>.\n"
    "Nothing was sent anywhere: this file is on your disk only, and it is yours to\n"
    "read before you attach it to an issue.\n";

int xbox_BugReport(const xbox_bug_report_opts *o, char *out_path, size_t n)
{
    const char *const *prefixes = o && o->env_prefixes ? o->env_prefixes : k_default_prefixes;
    char out_dir[1024], log_dir[1024], crash_dir[1024], stamp[32], path[1200], part[1210];
    xbox_report_buf readme = { NULL, 0, 0 }, sys = { NULL, 0, 0 }, text = { NULL, 0, 0 },
                    cur = { NULL, 0, 0 };
    const char *name = o && o->name && *o->name ? o->name : "game";
    xbox_report *r;
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    int i, have_cur, frame = 0, members, ok;
    long long size;

    if (!o || !o->save_dir || !*o->save_dir) {
        snprintf(out_path, n, "no save folder to write the report to");
        return 0;
    }
    snprintf(out_dir, sizeof out_dir, "%s", o->out_dir && *o->out_dir ? o->out_dir : "");
    if (!out_dir[0])
        snprintf(out_dir, sizeof out_dir, "%s/reports", o->save_dir);
    snprintf(log_dir, sizeof log_dir, "%s", o->log_dir && *o->log_dir ? o->log_dir : "");
    if (!log_dir[0])
        snprintf(log_dir, sizeof log_dir, "%s/logs", o->save_dir);
    snprintf(crash_dir, sizeof crash_dir, "%s", o->crash_dir && *o->crash_dir ? o->crash_dir : "");
    if (!crash_dir[0])
        snprintf(crash_dir, sizeof crash_dir, "%s/crash", o->save_dir);
    mkdir_p(out_dir);
    if (!lt || !strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", lt))
        snprintf(stamp, sizeof stamp, "%lld", (long long)t);
    snprintf(path, sizeof path, "%s/%s-report-%s.zip", out_dir, name, stamp);
    snprintf(part, sizeof part, "%s.part", path);
    r = xbox_report_open(part, o->who);
    if (!r) {
        snprintf(out_path, n, "cannot write %s", part);
        return 0;
    }
    if (s_flush)
        s_flush();

    /* The logs first: system.txt quotes this run's. */
    have_cur = run_log(&cur, log_dir, "run", LOG_TAIL);
    if (!have_cur && !o->no_current_run) {
        /* No tee in this process (a Windows build without a console): the
         * file RECOMP_STDIO_LOG sends the output to, when there is one. */
        const char *file = recomp_env(RENV_STDIO_LOG);
        if (file && *file)
            have_cur = buf_file(&cur, file, LOG_HEAD, LOG_TAIL);
    }
    if (have_cur)
        xbox_report_text(r, o->no_current_run ? "logs/last-run.txt" : "logs/current.txt",
                         cur.p ? cur.p : "", cur.n);
    for (i = 1; i <= PREV_RUNS; i++) {
        char prefix[16], member[40];
        text.n = 0;
        snprintf(prefix, sizeof prefix, "prev%d", i);
        snprintf(member, sizeof member, "logs/previous-%d.txt", i);
        if (run_log(&text, log_dir, prefix, PREV_LOG_TAIL)) {
            xbox_report_text(r, member, text.p ? text.p : "", text.n);
            if (!have_cur && !cur.n)
                buf_add(&cur, text.p ? text.p : "", text.n);   /* for the facts below */
        }
    }

    xbox_report_printf(&readme, "%s\nBug report, format 1, made %s (local time)%s%s\n\n",
                       o->title && *o->title ? o->title : name, stamp,
                       o->note && *o->note ? "\n" : "", o->note && *o->note ? o->note : "");
    buf_str(&readme, k_readme);
    buf_str(&readme, "\n");
    if (!have_cur)
        xbox_report_printf(&readme, "logs: no log of %s was found in %s.\n",
                           o->no_current_run ? "an earlier run" : "this run", log_dir);
    /* The print gate (xr_log.h, BO-125): a quiet run's log has errors and
     * the start-up facts only. Said, with the way to a full one. */
    if (!XR_DIAG_ON())
        xbox_report_printf(&readme, "logs: diagnostic logging was off in this run (%s): the log has"
                           " only errors and the start-up facts. For a full log switch on"
                           " Advanced > Diagnostic logging in the overlay (Back+Start or F1),"
                           " restart the game, make the problem happen again, then create the"
                           " report again.\n", xr_log_source());
    else
        xbox_report_printf(&readme, "logs: diagnostic logging was on (%s).\n", xr_log_source());

    /* system.txt */
    xbox_report_printf(&sys, "[build]\ntoolkit: xboxrecomp %s, built %s, %s\n", XR_BUILD_COMMIT,
                       __DATE__, arch_name());
    if (o->title && *o->title)
        xbox_report_printf(&sys, "title: %s\n", o->title);
    buf_str(&sys, "\n[system]\n");
    write_os(&sys);
    run_sections(&sys, "system.txt");
    buf_str(&sys, "\n[in effect]\n");
    write_in_effect(&sys);
    xbox_report_printf(&sys, "\n[as the %s found it: lines of its log]\n",
                       have_cur && !o->no_current_run ? "run" : "last run");
    log_facts(&sys, cur.p ? cur.p : "", cur.n);
    xbox_report_text(r, "system.txt", sys.p, sys.n);

    /* The attached files (settings.toml), then the settings with sources. */
    for (i = 0; i < s_attached; i++) {
        const char *p = s_attach[i].path_fn();
        if (!p || !*p || !xbox_report_file(r, s_attach[i].member, p, s_attach[i].head,
                                           s_attach[i].tail))
            xbox_report_printf(&readme, "%s: no such file%s.\n", s_attach[i].member,
                               p && *p ? "" : " in use");
    }
    text.n = 0;
    write_effective(&text, prefixes);
    run_sections(&text, "settings-effective.txt");
    xbox_report_text(r, "settings-effective.txt", text.p, text.n);

    text.n = 0;
    run_sections(&text, "stats.txt");
    if (!text.n)
        buf_str(&text, "(no title ran in this process: no session to count)\n");
    xbox_report_text(r, "stats.txt", text.p, text.n);

    /* Members of the host's own. */
    for (i = 0; i < s_sections; i++) {
        const char *m = s_section[i].member;
        int k, first = 1;
        if (!strcmp(m, "system.txt") || !strcmp(m, "settings-effective.txt")
                || !strcmp(m, "stats.txt"))
            continue;
        for (k = 0; k < i; k++)
            if (!strcmp(s_section[k].member, m))
                first = 0;
        if (!first)
            continue;
        text.n = 0;
        run_sections(&text, m);
        xbox_report_text(r, m, text.p ? text.p : "", text.n);
    }

    add_crashes(r, &readme, crash_dir, prefixes);

    if (o->screenshot) {
        char bmp[1300];
        snprintf(bmp, sizeof bmp, "%s/.frame-%ld.bmp", out_dir, rp_getpid());
        if (s_capture && s_capture(bmp, 1500)) {
            long long fsz = file_size(bmp);
            FILE *f = fsz > 0 && fsz <= (long long)FRAME_MAX ? fopen(bmp, "rb") : NULL;
            void *px = f ? malloc((size_t)fsz) : NULL;
            if (px && fread(px, 1, (size_t)fsz, f) == (size_t)fsz)
                frame = xbox_report_binary(r, "frame.bmp", px, (size_t)fsz);
            free(px);
            if (f)
                fclose(f);
        }
        remove(bmp);
        xbox_report_printf(&readme, "frame.bmp: %s.\n", frame
                           ? "the last frame the title presented, at the title's own size"
                           : "asked for, but no frame could be had (no title running, or"
                             " none presented in time)");
    } else {
        buf_str(&readme, "frame.bmp: not asked for.\n");
    }

    xbox_report_text(r, "README.txt", readme.p, readme.n);
    members = xbox_report_count(r);
    ok = xbox_report_close(r);
    buf_free(&readme);
    buf_free(&sys);
    buf_free(&text);
    buf_free(&cur);
    if (!ok) {
        remove(part);
        snprintf(out_path, n, "cannot write %s", path);
        return 0;
    }
    remove(path);
    if (rename(part, path) != 0) {
        snprintf(out_path, n, "cannot name %s", path);
        return 0;
    }
    size = file_size(path);
    fprintf(stderr, LOGP "wrote %s (%lld bytes, %d files%s)\n", path, size, members,
            frame ? ", with the frame" : "");
    snprintf(out_path, n, "%s", path);
    return 1;
}

/* ── RECOMP_DEBUG=bug_report ───────────────────────────────────────── */

static xbox_bug_report_fn s_maker;

void xbox_BugReportSetMaker(xbox_bug_report_fn fn) { s_maker = fn; }
int xbox_BugReportHasMaker(void) { return s_maker != NULL; }

int xbox_BugReportMake(int screenshot, char *out_path, unsigned n)
{
    if (!s_maker) {
        snprintf(out_path, n, "this build has no bug report");
        return 0;
    }
    return s_maker(screenshot, out_path, n);
}

static double s_arm_after;
static const char *s_arm_how = "";
static const char *s_arm_file;

static void arm_sleep(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

#if !defined(XBOX_REPORT_STANDALONE)
void xbox_HostPause(int on);            /* kernel_pause.h, included below */
#endif

/* `how` has "shot" for the frame, and "paused" to pause the title around
 * the report as the settings overlay does (the frame then comes from the
 * executor's pause hold, not from a flip). */
static void arm_make(const char *how)
{
    char path[1200] = "";
    int paused = strstr(how, "paused") != NULL, ok;

#if !defined(XBOX_REPORT_STANDALONE)
    if (paused) {
        xbox_HostPause(1);
        arm_sleep(300);
    }
#else
    (void)paused;
#endif
    ok = xbox_BugReportMake(strstr(how, "shot") != NULL, path, sizeof path);
#if !defined(XBOX_REPORT_STANDALONE)
    if (paused)
        xbox_HostPause(0);
#endif
    if (ok)
        fprintf(stderr, LOGP "bug_report: done: %s\n", path);
    else
        XR_ERROR( LOGP "bug_report: failed: %s\n", path);
}

static void arm_run(void)
{
    if (!s_arm_file) {
        arm_sleep((unsigned)(s_arm_after * 1000.0));
        arm_make(s_arm_how);
        return;
    }
    /* =@file: a report each time the file appears (a script makes it when
     * the title is where it wants the report); "shot" in it asks for the
     * frame. The file is removed first, so it can be made again. */
    for (;;) {
        FILE *f = fopen(s_arm_file, "rb");
        if (f) {
            char word[32] = "";
            size_t n = fread(word, 1, sizeof word - 1, f);
            word[n] = 0;
            fclose(f);
            remove(s_arm_file);
            arm_make(word);
        }
        arm_sleep(100);
    }
}

#if defined(_WIN32)
static DWORD WINAPI arm_thread(LPVOID p) { (void)p; arm_run(); return 0; }
#else
static void *arm_thread(void *p) { (void)p; arm_run(); return NULL; }
#endif

void xbox_BugReportArm(void)
{
    static int armed;
    const char *e = recomp_env(RENV_BUG_REPORT);

    if (!e || armed)
        return;
    armed = 1;
    if (e[0] == '@' && e[1]) {
        s_arm_file = e + 1;
        fprintf(stderr, LOGP "bug_report: a report whenever %s appears\n", s_arm_file);
    } else {
        s_arm_after = atof(e);
        s_arm_how = strchr(e, ':') ? strchr(e, ':') : "";
        fprintf(stderr, LOGP "bug_report: a report in %.1f s%s\n", s_arm_after,
                strstr(s_arm_how, "shot") ? ", with the frame" : "");
    }
    {
#if defined(_WIN32)
        HANDLE h = CreateThread(NULL, 0, arm_thread, NULL, 0, NULL);
        if (h)
            CloseHandle(h);
#else
        pthread_t th;
        if (pthread_create(&th, NULL, arm_thread, NULL) == 0)
            pthread_detach(th);
#endif
    }
}

/* ── the runtime's parts ───────────────────────────────────────────── */

#if !defined(XBOX_REPORT_STANDALONE)
#include "kernel_pause.h"
#include "xbox_crash.h"
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

int nv2a_pb_capture_frame(const char *path, unsigned timeout_ms);   /* nv2a_pb_state.h */

static void flip_section(xbox_report_buf *b)
{
    xbox_flip_stats s;

    xbox_HostFlipStats(&s);
    if (!s.intervals) {
        xbox_report_printf(b, "flips: %llu (no frame times yet)\n", (unsigned long long)s.flips);
        return;
    }
    xbox_report_printf(b, "flips: %llu over %.1f s since the first (%.1f per second);"
                       " %u pauses (the settings overlay)\n", (unsigned long long)s.flips,
                       s.span_s, s.span_s > 0 ? (double)s.flips / s.span_s : 0.0, s.pauses);
    xbox_report_printf(b, "frame time, flip to flip, ms: mean %.2f, p50 %.2f, p90 %.2f,"
                       " p95 %.2f, p99 %.2f, max %.1f\n", s.mean_ms, s.p50_ms, s.p90_ms,
                       s.p95_ms, s.p99_ms, s.max_ms);
    xbox_report_printf(b, "late frames (longer than 1.5 x the median): %u of %llu (%.2f %%)\n",
                       s.late, (unsigned long long)s.intervals,
                       100.0 * s.late / (double)s.intervals);
    xbox_report_printf(b, "frames over 20 ms: %u, over 25 ms: %u, over 34 ms: %u, over 50 ms:"
                       " %u, over 100 ms: %u\n", s.over[0], s.over[1], s.over[2], s.over[3],
                       s.over[4]);
    xbox_report_printf(b, "the last %u frames: p50 %.2f, p95 %.2f, max %.1f ms\n", s.recent,
                       s.recent_p50_ms, s.recent_p95_ms, s.recent_max_ms);
    buf_str(b, "(every flip counts: menus, loading screens and movies are in these too)\n");
}

static void report_flush(void) { xbox_crash_log_flush(200); }

void xbox_BugReportRuntime(void)
{
    xbox_report_section("stats.txt", "frame times of this session", flip_section);
    xbox_report_set_capture(nv2a_pb_capture_frame);
    xbox_report_set_flush(report_flush);
}
#endif
