/* xnet_lobby test: see CMakeLists.txt. */
#include <stdio.h>
#include <string.h>

#include "xnet_lobby.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* a packet as the title builds it: command, status 0, length, body, NUL */
static int packet(uint8_t *b, const char *cmd, const char *body)
{
    int n = 12 + (int)strlen(body) + 1;
    memcpy(b, cmd, 4);
    memset(b + 4, 0, 4);
    b[8] = (uint8_t)(n >> 24); b[9] = (uint8_t)(n >> 16); b[10] = (uint8_t)(n >> 8); b[11] = (uint8_t)n;
    memcpy(b + 12, body, strlen(body) + 1);
    return n;
}

static int len_of(const uint8_t *b) { return b[8] << 24 | b[9] << 16 | b[10] << 8 | b[11]; }

static void test_code_clean(void)
{
    char c[16];
    CHECK(xl_code_clean("kz7mq", c, sizeof c) && !strcmp(c, "KZ7MQ"));
    CHECK(xl_code_clean("KZ7-MQ", c, sizeof c) && !strcmp(c, "KZ7MQ"));
    CHECK(xl_code_clean("kz7 mq", c, sizeof c) && !strcmp(c, "KZ7MQ"));
    CHECK(!xl_code_clean("", c, sizeof c) && c[0] == 0);           /* none: public */
    CHECK(!xl_code_clean("AB", c, sizeof c) && c[0] == 0);         /* too short */
    /* BO-337 L2: up to 15, as the server takes */
    CHECK(xl_code_clean("ABCDEFGHJ", c, sizeof c) && !strcmp(c, "ABCDEFGHJ"));
    CHECK(xl_code_clean("abcdefghjkmnpqr", c, sizeof c) && !strcmp(c, "ABCDEFGHJKMNPQR"));
    CHECK(!xl_code_clean("ABCDEFGHJKMNPQRS", c, sizeof c) && c[0] == 0);  /* too long */
    CHECK(!xl_code_clean("ABCDEFGHJKMNPQR", c, 10) && c[0] == 0);         /* no room */
    CHECK(!xl_code_clean("AB=CD", c, sizeof c) && c[0] == 0);      /* never a field separator */
    CHECK(!xl_code_clean("AB\nPASS=x", c, sizeof c) && c[0] == 0);
    CHECK(!xl_code_clean(NULL, c, sizeof c));
}

static void test_code_make(void)
{
    char a[8], b[8], c[8];
    int i;
    xl_code_make(a, 1);
    xl_code_make(b, 2);
    xl_code_make(c, 1);
    CHECK(strlen(a) == 5 && strcmp(a, b) && !strcmp(a, c));
    for (i = 0; i < 5; i++)
        CHECK(strchr(XL_CODE_ALPHABET, a[i]) != NULL);
}

static void test_add_code(void)
{
    uint8_t in[512], out[512];
    int n, m;
    /* gcre: the field before the NUL, the length fixed */
    n = packet(in, "gcre", "NAME=Host1\nPASS=\nPARAMS=-1622913024,0,0\nMINSIZE=2\n");
    m = xl_add_code(in, n, "KZ7MQ", out, sizeof out);
    CHECK(m == n + 13 && len_of(out) == m && out[m - 1] == 0);
    CHECK(!memcmp(out, "gcre", 4) && strstr((char *)out + 12, "MINSIZE=2\nTKCODE=KZ7MQ\n") != NULL);
    /* gsea and gjoi too; a body without a closing newline gets one */
    n = packet(in, "gsea", "START=0\nCOUNT=500");
    m = xl_add_code(in, n, "KZ7MQ", out, sizeof out);
    CHECK(m > 0 && !strcmp((char *)out + 12, "START=0\nCOUNT=500\nTKCODE=KZ7MQ\n") && len_of(out) == m);
    n = packet(in, "gjoi", "NAME=Host1\nPASS=\nUSERPARAMS=0,0,0,\n");
    CHECK(xl_add_code(in, n, "KZ7MQ", out, sizeof out) > n);
    /* other requests, no code, part of a packet, no room: nothing */
    n = packet(in, "gset", "NAME=Host1\nUSERPARAMS=0,0,1,\n");
    CHECK(xl_add_code(in, n, "KZ7MQ", out, sizeof out) == 0);
    n = packet(in, "gcre", "NAME=Host1\n");
    CHECK(xl_add_code(in, n, "", out, sizeof out) == 0);
    CHECK(xl_add_code(in, n, NULL, out, sizeof out) == 0);
    CHECK(xl_add_code(in, n - 3, "KZ7MQ", out, sizeof out) == 0);
    CHECK(xl_add_code(in, n, "KZ7MQ", out, n + 5) == 0);
    CHECK(xl_add_code(in, 8, "KZ7MQ", out, sizeof out) == 0);
    /* move and sele carry it too; an empty code clears it there, and only there */
    n = packet(in, "move", "IDENT=1\n");
    m = xl_add_code(in, n, "KZ7MQ", out, sizeof out);
    CHECK(m > 0 && !strcmp((char *)out + 12, "IDENT=1\nTKCODE=KZ7MQ\n"));
    m = xl_add_code(in, n, "", out, sizeof out);
    CHECK(m > 0 && !strcmp((char *)out + 12, "IDENT=1\nTKCODE=\n") && len_of(out) == m);
    n = packet(in, "sele", "MYGAME=1 GAMES=1 ROOMS=1");
    CHECK(xl_add_code(in, n, "KZ7MQ", out, sizeof out) > n);
    n = packet(in, "gsea", "START=0\nCOUNT=500\n");
    CHECK(xl_add_code(in, n, "", out, sizeof out) == 0);
    /* an empty body */
    n = packet(in, "gsea", "");
    m = xl_add_code(in, n, "ABC", out, sizeof out);
    CHECK(m == 12 + 11 + 1 && !strcmp((char *)out + 12, "TKCODE=ABC\n"));
}

static void test_status(void)
{
    xl_status st;
    const char *b1 = "I=1\nN=Takedawn\nT=3\nTKONLINE=7\nTKWAITING=2\n";
    memset(&st, 0, sizeof st);
    xl_status_body("I=1\nN=Takedawn\n", 16, &st);
    CHECK(!st.seen && st.online == -1 && st.waiting == -1);
    xl_status_body(b1, (int)strlen(b1) + 1, &st);
    CHECK(st.seen && st.online == 7 && st.waiting == 2);
    /* a later record with only one of them keeps the other */
    xl_status_body("TKONLINE=8\n", 12, &st);
    CHECK(st.online == 8 && st.waiting == 2);
    /* blanks as separators; not a number; a longer key that starts the same */
    xl_status_body("A=1 TKWAITING=0", 15, &st);
    CHECK(st.waiting == 0);
    xl_status_body("TKONLINE=x\nTKONLINEX=99\n", 24, &st);
    CHECK(st.online == 8);
}

static void test_game(void)
{
    xl_game g;
    char line[128];
    const char *b = "IDENT=10\nNAME=Host1\nHOST=Host1\nCOUNT=3\nOPID0=14\nOPPO0=Host1\nADDR0=77.234.95.82\n"
                    "OPPARAM0=-1779744768,1532330907,1,\nOPPO1=Ubu2\nOPPARAM1=-1777184768,1532330907,3,\n"
                    "OPPO2=Dee2\nOPPARAM2=-1777184768,1532330907,1,\n";
    memset(&g, 0, sizeof g);
    CHECK(xl_game_body(b, (int)strlen(b) + 1, &g) == 1);
    CHECK(g.ident == 10 && g.np == 3 && !strcmp(g.name[1], "Ubu2") && g.ready[0] == 1 && g.ready[1] == 3 && g.ready[2] == 1);
    xl_game_line(&g, line, sizeof line);
    CHECK(!strcmp(line, "Host1:1 Ubu2:3 Dee2:1"));
    /* a room record or a body without players: nothing */
    CHECK(xl_game_body("I=1\nN=Takedawn\nT=3\n", 20, &g) == 0 && g.np == 3);
    /* a player that has not picked a car yet */
    CHECK(xl_game_body("OPPO0=A\nOPPARAM0=0,0,0,\nOPPO1=B\n", 34, &g) == 1 && g.np == 2 && g.ready[0] == 0 && g.ready[1] == 0);
}

/* BO-337: TKID=1 on addr (identity scheme), the lobby's "no game" record,
 * keys only at the start of a line, numbers that end with the body */
static void test_add_line(void)
{
    uint8_t in[256], out[256];
    int n, m;
    n = packet(in, "addr", "ADDR=192.168.0.108\nPORT=0\n");
    m = xl_add_line(in, n, "TKID=1", out, sizeof out);
    CHECK(m == n + 7 && len_of(out) == m && !strcmp((char *)out + 12, "ADDR=192.168.0.108\nPORT=0\nTKID=1\n"));
    /* then the code on a request that has both */
    n = packet(in, "gjoi", "IDENT=3");
    m = xl_add_line(in, n, "TKID=1", out, sizeof out);
    CHECK(m > 0 && !strcmp((char *)out + 12, "IDENT=3\nTKID=1\n"));
    CHECK(xl_add_line(in, n - 1, "TKID=1", out, sizeof out) == 0);
    CHECK(xl_add_line(in, n, "TKID=1", out, n + 3) == 0);
}

static void test_gone(void)
{
    int v;
    const char *gone = "IDENT=0\nNAME=\nHOST=\nCOUNT=0";
    CHECK(xl_game_gone(gone, (int)strlen(gone) + 1));
    CHECK(xl_game_gone("IDENT=0\n", 9));
    CHECK(!xl_game_gone("IDENT=4\nCOUNT=0\n", 17));
    CHECK(!xl_game_gone("IDENT=0\nCOUNT=2\nOPPO0=A\nOPPO1=B\n", 34));
    CHECK(!xl_game_gone("NAME=x\n", 7));
    /* a key inside a value is not that key (names have no '=') */
    CHECK(!xl_key_int("NAME=a COUNTX=0\n", 16, "COUNT", &v));
    CHECK(xl_key_int("NAME=a\nCOUNT=12\n", 16, "COUNT", &v) && v == 12);
    /* a record with no NUL at its end: the number stops at the end */
    {
        char b[12];
        memcpy(b, "IDENT=12345", 11);
        b[11] = '9';
        CHECK(xl_key_int(b, 11, "IDENT", &v) && v == 12345);
    }
}

/* BO-340: correlation ids (TKSID, TKGID) */
static void test_ids(void)
{
    char id[XL_ID_MAX + 1];
    xl_game g;
    const char *rec = "IDENT=3\nNAME=Gm\nTKSECRET=00112233445566778899aabbccddeeff\nTKGID=aec4ed3eaf2b\nOPPO0=A\nOPPARAM0=0,0,1,\n";
    CHECK(xl_key_hex("TKADDR=10.64.0.2\nTKSID=d8fa84fd\n", 33, "TKSID", id, sizeof id) && !strcmp(id, "d8fa84fd"));
    /* the last line without a newline or with a NUL; a CRLF */
    CHECK(xl_key_hex("TKSID=0123abcd", 14, "TKSID", id, sizeof id) && !strcmp(id, "0123abcd"));
    CHECK(xl_key_hex("TKSID=0123abcd\r\n", 17, "TKSID", id, sizeof id) && !strcmp(id, "0123abcd"));
    /* an old server sends none: nothing, and out is cleared */
    snprintf(id, sizeof id, "stale");
    CHECK(!xl_key_hex("TKADDR=10.64.0.2\n", 18, "TKSID", id, sizeof id) && id[0] == 0);
    /* not hex, upper case, too short, too long, trailing junk, a longer key that starts the same */
    CHECK(!xl_key_hex("TKSID=xyz12345\n", 15, "TKSID", id, sizeof id));
    CHECK(!xl_key_hex("TKSID=D8FA84FD\n", 15, "TKSID", id, sizeof id));
    CHECK(!xl_key_hex("TKSID=abc\n", 10, "TKSID", id, sizeof id));
    CHECK(!xl_key_hex("TKSID=0123456789abcdef0\n", 24, "TKSID", id, sizeof id));
    CHECK(!xl_key_hex("TKSID=0123abcd!\n", 16, "TKSID", id, sizeof id));
    CHECK(!xl_key_hex("TKSIDX=0123abcd\n", 16, "TKSID", id, sizeof id));
    CHECK(!xl_key_hex("TKSID=0123abcd\n", 15, "TKSID", id, 8));   /* no room: refused, not cut */
    /* a game record */
    memset(&g, 0, sizeof g);
    CHECK(xl_game_body(rec, (int)strlen(rec) + 1, &g) == 1 && g.ident == 3 && !strcmp(g.gid, "aec4ed3eaf2b"));
    CHECK(xl_game_body("OPPO0=A\nOPPARAM0=0,0,1,\n", 25, &g) == 1 && g.gid[0] == 0);
    CHECK(xl_game_body("TKGID=zz\nOPPO0=A\n", 17, &g) == 1 && g.gid[0] == 0);
}

/* BO-365: a gsea answer with the race keys, as the lobby sends it */
static void test_racewait(void)
{
    const char *b = "COUNT=0\nTKWAITSTATE=lobby\nTKWAITPLAYERS=2\nTKWAITQUEUE=1\nTKWAITNAME=Host One\n"
                    "TKRACING=1\nTKRACEID=42\nTKRACENAME=RaceA\n";
    char s[32];
    int n = (int)strlen(b) + 1, v;
    xl_status st;
    CHECK(xl_key_str(b, n, "TKWAITSTATE", s, sizeof s) && !strcmp(s, "lobby") && xl_wait_state(s) == 2);
    CHECK(xl_key_str(b, n, "TKWAITNAME", s, sizeof s) && !strcmp(s, "Host One"));
    CHECK(xl_key_str(b, n, "TKRACENAME", s, sizeof s) && !strcmp(s, "RaceA"));
    CHECK(xl_key_int(b, n, "TKRACEID", &v) && v == 42 && xl_key_int(b, n, "TKWAITQUEUE", &v) && v == 1);
    CHECK(!xl_key_str(b, n, "TKWAIT", s, sizeof s) && s[0] == 0);            /* a key that only starts the same */
    CHECK(!xl_key_str(b, n, "TKWAITNAME", s, 5) && s[0] == 0);               /* longer than out */
    CHECK(!xl_key_str("A=x=y\n", 7, "A", s, sizeof s));                     /* never an '=' */
    CHECK(!xl_key_str("A=\n", 4, "A", s, sizeof s));                        /* empty */
    CHECK(!xl_key_str("A=\x01b\n", 6, "A", s, sizeof s));                  /* control bytes */
    CHECK(xl_wait_state("racing") == 1 && xl_wait_state("open") == 3 && xl_wait_state("full") == 4 &&
          xl_wait_state("gone") == 5 && xl_wait_state("x") == 0 && xl_wait_state(NULL) == 0);
    memset(&st, 0, sizeof st);
    xl_status_body("TKONLINE=1\n", 12, &st);
    CHECK(st.racing == -1);                  /* the room's count is set from +rom only (xnet_hle.c) */
}

int main(void)
{
    test_racewait();
    test_code_clean();
    test_code_make();
    test_add_code();
    test_status();
    test_game();
    test_add_line();
    test_gone();
    test_ids();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
