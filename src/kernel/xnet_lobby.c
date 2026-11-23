/*
 * xnet_lobby.c - the client's additions to the lobby protocol; see
 * xnet_lobby.h.
 */
#include "xnet_lobby.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xl_code_clean(const char *in, char *out, int n)
{
    int i, k = 0;
    if (n > 0)
        out[0] = 0;
    if (!in)
        return 0;
    for (i = 0; in[i]; i++) {
        char c = in[i];
        if (c == ' ' || c == '-')
            continue;   /* "KZ7 MQ", "KZ7-MQ" as said aloud or pasted */
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return out[0] = 0, 0;
        if (k >= XL_CODE_MAX || k + 1 >= n)
            return out[0] = 0, 0;
        out[k++] = c;
    }
    out[k] = 0;
    if (k < XL_CODE_MIN) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

void xl_code_make(char *out, uint32_t seed)
{
    static const char a[] = XL_CODE_ALPHABET;
    uint32_t x = seed ? seed : 0x9E3779B9u;
    int i;
    for (i = 0; i < 5; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        out[i] = a[x % (sizeof a - 1)];
    }
    out[5] = 0;
}

int xl_add_code(const uint8_t *in, int n, const char *code, uint8_t *out, int cap)
{
    char field[32];
    int fl;
    int room;
    if (!code || n < 12)
        return 0;
    /* move and sele tell the server the code too (it then pushes the room's
     * private games with that code); an empty code clears it there */
    room = !memcmp(in, "move", 4) || !memcmp(in, "sele", 4);
    if (!room && memcmp(in, "gcre", 4) && memcmp(in, "gsea", 4) && memcmp(in, "gjoi", 4))
        return 0;
    if (!code[0] && !room)
        return 0;
    fl = snprintf(field, sizeof field, "TKCODE=%.*s", XL_CODE_MAX, code);
    if (fl < 0 || fl >= (int)sizeof field)
        return 0;
    return xl_add_line(in, n, field, out, cap);
}

int xl_add_line(const uint8_t *in, int n, const char *line, uint8_t *out, int cap)
{
    int fl, body_end, m;
    uint32_t len;
    if (!line || n < 12)
        return 0;
    len = (uint32_t)in[8] << 24 | (uint32_t)in[9] << 16 | (uint32_t)in[10] << 8 | in[11];
    if (len != (uint32_t)n)
        return 0;   /* not one whole packet: leave it alone */
    fl = (int)strlen(line);
    /* the body ends in a NUL (sometimes after a newline): the field goes in
     * front of it; without one, at the end, with one added */
    body_end = n;
    if (n > 12 && in[n - 1] == 0)
        body_end = n - 1;
    m = body_end + fl + 2;   /* the line, its newline, the NUL */
    if (m > cap)
        return 0;
    memcpy(out, in, (size_t)body_end);
    /* a body whose last line has no newline gets one first */
    if (body_end > 12 && out[body_end - 1] != '\n') {
        if (m + 1 > cap)
            return 0;
        out[body_end++] = '\n';
        m++;
    }
    memcpy(out + body_end, line, (size_t)fl);
    out[body_end + fl] = '\n';
    out[body_end + fl + 1] = 0;
    out[8] = (uint8_t)(m >> 24); out[9] = (uint8_t)(m >> 16);
    out[10] = (uint8_t)(m >> 8); out[11] = (uint8_t)m;
    return m;
}

/* the digits at body[i..n) as a number (at most 9 of them), 0 when none */
static int num(const char *body, int i, int n)
{
    int x = 0, k = 0;
    while (i < n && body[i] >= '0' && body[i] <= '9' && k++ < 9)
        x = x * 10 + (body[i++] - '0');
    return x;
}

int xl_key_int(const char *body, int n, const char *key, int *v)
{
    int kl = (int)strlen(key), i = 0;
    while (i < n && body[i]) {
        while (i < n && (body[i] == '\n' || body[i] == '\r' || body[i] == ' ' || body[i] == '\t'))
            i++;
        if (i + kl < n && !memcmp(body + i, key, (size_t)kl) && body[i + kl] == '=') {
            int j = i + kl + 1, x = 0, any = 0;
            while (j < n && body[j] >= '0' && body[j] <= '9' && x < 100000000) {
                x = x * 10 + (body[j] - '0');
                j++;
                any = 1;
            }
            if (any) {
                *v = x;
                return 1;
            }
        }
        while (i < n && body[i] && body[i] != '\n' && body[i] != ' ' && body[i] != '\t')
            i++;
    }
    return 0;
}

int xl_key_hex(const char *body, int n, const char *key, char *out, int cap)
{
    int kl = (int)strlen(key), i = 0;
    if (cap > 0)
        out[0] = 0;
    while (i < n && body[i]) {
        while (i < n && (body[i] == '\n' || body[i] == '\r' || body[i] == ' ' || body[i] == '\t'))
            i++;
        if (i + kl < n && !memcmp(body + i, key, (size_t)kl) && body[i + kl] == '=') {
            int j = i + kl + 1, k = 0;
            while (j < n && ((body[j] >= '0' && body[j] <= '9') || (body[j] >= 'a' && body[j] <= 'f'))) {
                j++;
                k++;
            }
            if (k >= 4 && k <= XL_ID_MAX && k < cap && (j >= n || !body[j] || body[j] == '\n' || body[j] == '\r')) {
                memcpy(out, body + i + kl + 1, (size_t)k);
                out[k] = 0;
                return 1;
            }
        }
        while (i < n && body[i] && body[i] != '\n' && body[i] != ' ' && body[i] != '\t')
            i++;
    }
    return 0;
}

int xl_key_str(const char *body, int n, const char *key, char *out, int cap)
{
    int kl = (int)strlen(key), i = 0;
    if (cap > 0)
        out[0] = 0;
    while (i < n && body[i]) {
        while (i < n && (body[i] == '\n' || body[i] == '\r' || body[i] == ' ' || body[i] == '\t'))
            i++;
        if (i + kl < n && !memcmp(body + i, key, (size_t)kl) && body[i + kl] == '=') {
            int j = i + kl + 1, k = 0, ok = 1;
            while (j < n && body[j] && body[j] != '\n' && body[j] != '\r') {
                if ((unsigned char)body[j] < 0x20 || (unsigned char)body[j] > 0x7E || body[j] == '=')
                    ok = 0;
                j++;
                k++;
            }
            if (ok && k > 0 && k < cap) {
                memcpy(out, body + i + kl + 1, (size_t)k);
                out[k] = 0;
                return 1;
            }
            return 0;
        }
        while (i < n && body[i] && body[i] != '\n')
            i++;
    }
    return 0;
}

int xl_wait_state(const char *w)
{
    static const char *const k[] = { "racing", "lobby", "open", "full", "gone" };
    int i;
    for (i = 0; w && i < 5; i++)
        if (!strcmp(w, k[i]))
            return i + 1;
    return 0;
}

void xl_status_body(const char *body, int n, xl_status *st)
{
    int v;
    if (!st->seen) {
        st->online = -1;
        st->waiting = -1;
        st->racing = -1;
    }
    if (xl_key_int(body, n, "TKONLINE", &v)) {
        st->online = v;
        st->seen = 1;
    }
    if (xl_key_int(body, n, "TKWAITING", &v)) {
        st->waiting = v;
        st->seen = 1;
    }
}

int xl_game_body(const char *body, int n, xl_game *g)
{
    xl_game t;
    int i = 0, any = 0;
    memset(&t, 0, sizeof t);
    t.ident = -1;
    while (i < n && body[i]) {
        int ks, ke, vs, ve;
        while (i < n && (body[i] == '\n' || body[i] == '\r' || body[i] == ' ' || body[i] == '\t'))
            i++;
        ks = i;
        while (i < n && body[i] && body[i] != '=' && body[i] != '\n')
            i++;
        ke = i;
        if (i >= n || body[i] != '=') {
            while (i < n && body[i] && body[i] != '\n')
                i++;
            continue;
        }
        vs = ++i;
        while (i < n && body[i] && body[i] != '\n' && body[i] != '\r')
            i++;
        ve = i;
        if (ke - ks == 5 && !memcmp(body + ks, "IDENT", 5)) {
            t.ident = num(body, vs, ve);
        } else if (ke - ks == 5 && !memcmp(body + ks, "TKGID", 5)) {
            int l = ve - vs, c;
            for (c = 0; c < l && c < XL_ID_MAX; c++) {
                char ch = body[vs + c];
                if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                    break;
            }
            if (c == l && l >= 4 && l <= XL_ID_MAX) {
                memcpy(t.gid, body + vs, (size_t)l);
                t.gid[l] = 0;
            }
        } else if (ke - ks > 4 && !memcmp(body + ks, "OPPO", 4) && body[ks + 4] >= '0' && body[ks + 4] <= '9') {
            int k = num(body, ks + 4, ke), l = ve - vs;
            if (k >= 0 && k < XL_MAX_PLAYERS) {
                if (l > 15) l = 15;
                memcpy(t.name[k], body + vs, (size_t)l);
                t.name[k][l] = 0;
                if (k + 1 > t.np) t.np = k + 1;
                any = 1;
            }
        } else if (ke - ks > 7 && !memcmp(body + ks, "OPPARAM", 7)) {
            int k = num(body, ks + 7, ke), commas = 0, j;
            if (k >= 0 && k < XL_MAX_PLAYERS) {
                for (j = vs; j < ve && commas < 2; j++)
                    if (body[j] == ',') commas++;
                t.ready[k] = (commas == 2 && j < ve) ? num(body, j, ve) : 0;
            }
        }
    }
    if (!any)
        return 0;
    t.seen = 1;
    *g = t;
    return 1;
}

int xl_game_gone(const char *body, int n)
{
    int ident, count = 0;
    xl_game g;
    if (!xl_key_int(body, n, "IDENT", &ident) || ident != 0)
        return 0;
    if (xl_key_int(body, n, "COUNT", &count) && count != 0)
        return 0;
    return !xl_game_body(body, n, &g);
}

void xl_game_line(const xl_game *g, char *out, int cap)
{
    int k, o = 0;
    if (cap > 0)
        out[0] = 0;
    for (k = 0; k < g->np && o < cap - 1; k++) {
        int r = snprintf(out + o, (size_t)(cap - o), "%s%s:%d", k ? " " : "", g->name[k][0] ? g->name[k] : "?",
                         g->ready[k]);
        if (r < 0)
            break;
        o += r;
        if (o >= cap)   /* cut short: the NUL is at cap - 1 */
            break;
    }
}
