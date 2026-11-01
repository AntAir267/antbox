/* The part of "Check for updates" that needs no network (xbox_update.h). */
#include "xbox_update.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── JSON: the string values of the top level ─────────────────────────── */

/* p is at an opening quote; the address after the closing one, NULL when
 * the text ends first. */
static const char *skip_string(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\' && p[1])
            p++;
        else if (*p == '"')
            return p + 1;
    }
    return NULL;
}

static int hex4(const char *p, unsigned *v)
{
    unsigned x = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        x <<= 4;
        if (c >= '0' && c <= '9') x |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') x |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') x |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *v = x;
    return 1;
}

static int put_utf8(unsigned cp, char *b)
{
    if (cp < 0x80) { b[0] = (char)cp; return 1; }
    if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63));
        b[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | ((cp >> 12) & 63));
    b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* Decodes the string whose opening quote is at p into out[n]. */
static void decode_string(const char *p, char *out, size_t n)
{
    size_t at = 0;
    for (p++; *p && *p != '"'; p++) {
        char b[4];
        int len = 1;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': b[0] = '\n'; break;
            case 'r': b[0] = '\r'; break;
            case 't': b[0] = '\t'; break;
            case 'b': b[0] = '\b'; break;
            case 'f': b[0] = '\f'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(p + 1, &cp)) { b[0] = '?'; break; }
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                    unsigned lo;
                    if (hex4(p + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                if (cp >= 0xD800 && cp < 0xE000)
                    cp = '?';
                len = put_utf8(cp, b);
                break;
            }
            case 0: goto done;
            default: b[0] = *p; break;              /* \" \\ \/ */
            }
        } else {
            unsigned char lead = (unsigned char)*p;
            int want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            b[0] = *p;
            /* A sequence of the source is taken whole or not at all. */
            while (len < want && ((unsigned char)p[len] & 0xC0) == 0x80) {
                b[len] = p[len];
                len++;
            }
            p += len - 1;
        }
        if (at + (size_t)len + 1 > n)
            break;
        memcpy(out + at, b, (size_t)len);
        at += (size_t)len;
    }
done:
    out[at] = 0;
}

int xbox_update_json_string(const char *json, const char *key, char *out, size_t n)
{
    int depth = 0;
    size_t klen = strlen(key);
    const char *p = json;
    if (n)
        out[0] = 0;
    if (!json || !n)
        return 0;
    while (*p) {
        if (*p == '{' || *p == '[') {
            depth++, p++;
        } else if (*p == '}' || *p == ']') {
            depth--, p++;
        } else if (*p == '"') {
            const char *s = p, *q;
            p = skip_string(p);
            if (!p)
                return 0;
            if (depth != 1)
                continue;
            for (q = p; *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'; q++) {}
            if (*q != ':' || (size_t)(p - s) != klen + 2 || strncmp(s + 1, key, klen))
                continue;
            for (q++; *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'; q++) {}
            if (*q != '"')
                return 0;
            decode_string(q, out, n);
            return 1;
        } else {
            p++;
        }
    }
    return 0;
}

/* ── versions ─────────────────────────────────────────────────────────── */

static int parse_version(const char *s, int v[4])
{
    int n = 0;
    while (*s == ' ' || *s == 'v' || *s == 'V')
        s++;
    memset(v, 0, 4 * sizeof v[0]);
    while (n < 4 && isdigit((unsigned char)*s)) {
        long x = 0;
        while (isdigit((unsigned char)*s)) {
            if (x < 100000000)
                x = x * 10 + (*s - '0');
            s++;
        }
        v[n++] = (int)x;
        if (*s != '.' || !isdigit((unsigned char)s[1]))
            break;
        s++;
    }
    return n;
}

int xbox_update_compare(const char *tag, const char *running)
{
    int a[4], b[4];
    if (!tag || !running || !parse_version(tag, a) || !parse_version(running, b))
        return -2;
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i])
            return a[i] > b[i] ? 1 : -1;
    return 0;
}

/* ── the answer ───────────────────────────────────────────────────────── */

/* One line of the release notes for the page: Markdown's heading marks and
 * bullets made plain, characters the overlay's font lacks as '?', cut to
 * `n`-1 bytes with "..." when long. Returns whether the line has any text. */
static int note_line(const char *s, size_t len, char *out, size_t n)
{
    char buf[512];
    size_t at = 0;
    const char *e = s + len;
    while (s < e && (*s == ' ' || *s == '\t' || *s == '#'))
        s++;
    if (e - s >= 2 && (*s == '-' || *s == '*' || *s == '+') && s[1] == ' ') {
        buf[at++] = '-';
        s++;
    }
    for (; s < e && at < sizeof buf - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\t' || c == '\r')
            c = ' ';
        if (c < 0x20)
            continue;
        if (c == 0xE2 && e - s >= 3) {          /* common punctuation of U+2000..U+206F */
            unsigned char c1 = (unsigned char)s[1], c2 = (unsigned char)s[2];
            if (c1 == 0x80) {
                char r = (c2 == 0x93 || c2 == 0x94 || c2 == 0xA2) ? '-'
                       : (c2 == 0x98 || c2 == 0x99) ? '\''
                       : (c2 == 0x9C || c2 == 0x9D) ? '"' : '?';
                buf[at++] = r;
                s += 2;
                continue;
            }
        }
        if (c >= 0x80) {                        /* other non-ASCII: kept when it is Latin, else '?' */
            if (c >= 0xC2 && c <= 0xCA && e - s >= 2) {
                buf[at++] = (char)c;
                buf[at++] = s[1];
                s += 1;
            } else {
                buf[at++] = '?';
                while (s + 1 < e && ((unsigned char)s[1] & 0xC0) == 0x80)
                    s++;
            }
            continue;
        }
        buf[at++] = (char)c;
    }
    buf[at] = 0;
    while (at > 0 && buf[at - 1] == ' ')
        buf[--at] = 0;
    if (!at || (at == 1 && buf[0] == '-'))
        return 0;
    if (at >= n) {
        size_t cut = n - 4;
        while (cut > 0 && ((unsigned char)buf[cut] & 0xC0) == 0x80)
            cut--;
        memcpy(out, buf, cut);
        memcpy(out + cut, "...", 4);
    } else {
        memcpy(out, buf, at + 1);
    }
    return 1;
}

static void set_error(xbox_update_result *r, const char *fmt, const char *why, int num)
{
    r->kind = XU_ERROR;
    if (why)
        snprintf(r->headline, sizeof r->headline, fmt, why);
    else
        snprintf(r->headline, sizeof r->headline, fmt, num);
}

void xbox_update_evaluate(int status, const char *transport_error, const char *body,
                          const char *running_version, xbox_update_result *r)
{
    char tag[64], name[128], url[512], notes[4096];
    memset(r, 0, sizeof *r);
    if (!status) {
        set_error(r, "Could not check: %s",
                  transport_error && transport_error[0] ? transport_error : "no answer", 0);
        return;
    }
    if (status == 404) {
        r->kind = XU_NONE;
        snprintf(r->headline, sizeof r->headline, "No release published yet");
        return;
    }
    if (status == 403 || status == 429) {
        set_error(r, "Could not check: GitHub is limiting requests (HTTP %d), try again later", NULL, status);
        return;
    }
    if (status != 200 || !body) {
        set_error(r, "Could not check: GitHub answered HTTP %d", NULL, status);
        return;
    }
    if (!xbox_update_json_string(body, "tag_name", tag, sizeof tag)) {
        set_error(r, "Could not check: %s", "the answer has no release in it", 0);
        return;
    }
    const char *shown = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
    int cmp = xbox_update_compare(tag, running_version);
    if (cmp == -2) {
        set_error(r, "Could not check: the release tag \"%s\" is not a version", tag, 0);
        return;
    }
    if (cmp <= 0) {
        r->kind = XU_LATEST;
        snprintf(r->headline, sizeof r->headline, "You have the latest version (%s)", running_version);
        return;
    }
    r->kind = XU_NEWER;
    snprintf(r->tag, sizeof r->tag, "%s", tag);
    snprintf(r->headline, sizeof r->headline, "Version %.60s is out (you have %.40s)", shown, running_version);
    if (xbox_update_json_string(body, "name", name, sizeof name) && name[0]
        && strcmp(name, tag) != 0 && strstr(r->headline, name) == NULL
        && strstr(name, shown) == NULL) {
        size_t at = strlen(r->headline);
        snprintf(r->headline + at, sizeof r->headline - at, ": %.80s", name);
    }
    /* Only a page on github.com is ever opened. */
    if (xbox_update_json_string(body, "html_url", url, sizeof url)
        && !strncmp(url, "https://github.com/", 19))
        snprintf(r->url, sizeof r->url, "%s", url);
    if (xbox_update_json_string(body, "body", notes, sizeof notes)) {
        const char *p = notes;
        while (*p && r->note_count < XU_NOTE_LINES) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (note_line(p, len, r->notes[r->note_count], XU_NOTE_WIDTH))
                r->note_count++;
            p += len + (nl ? 1 : 0);
        }
    }
}

int xbox_update_api_url(const char *repo, char *out, size_t n)
{
    int slashes = 0;
    size_t len = repo ? strlen(repo) : 0;
    if (!len || len > 120 || repo[0] == '/' || repo[len - 1] == '/')
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = repo[i];
        if (c == '/')
            slashes++;
        else if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.')
            return 0;
    }
    if (slashes != 1 || strstr(repo, ".."))
        return 0;
    snprintf(out, n, "https://api.github.com/repos/%s/releases/latest", repo);
    return 1;
}
