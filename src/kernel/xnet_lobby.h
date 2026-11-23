/*
 * xnet_lobby.h - the client's additions to the lobby protocol (BO-335,
 * BO-336), pure functions over packets and bodies (tests/xnet_lobby).
 *
 * A lobby packet: 4 command bytes, 4 status bytes, a 32-bit big-endian length
 * of the whole packet, then a body of KEY=VALUE lines ending in a NUL.
 *
 * Game codes (BO-335): with a code set, the copy's gcre, gsea and gjoi carry
 * TKCODE=<code>; the server keeps such a game private (listed and joinable
 * only with the same code). The title never sees the field.
 *
 * Server status (BO-336): the server adds TKONLINE=<players signed in> and
 * TKWAITING=<public games waiting> to records it already sends; the overlay
 * shows them.
 */
#ifndef XNET_LOBBY_H
#define XNET_LOBBY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 3 to 15 characters: the server takes 1 to 15; one or two are too few to
 * keep a game private (32 * 32 codes), so the client never sends them */
#define XL_CODE_MIN 3
#define XL_CODE_MAX 15
/* the alphabet of generated codes: no 0/O, 1/I */
#define XL_CODE_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

/* a code as the player typed it, made canonical in place (upper case);
 * 1 when it is one (3 to 15 letters or digits), 0 when not, an empty code
 * is "no code" (returns 0, out is "") */
int xl_code_clean(const char *in, char *out, int n);
/* a new 5-character code from XL_CODE_ALPHABET */
void xl_code_make(char *out, uint32_t seed);

/* A whole gcre, gsea, gjoi, move or sele packet (in, n bytes) with
 * TKCODE=code added before the body's NUL, its length fixed: the new length,
 * written to out (cap bytes). code "" adds "TKCODE=" (clear) on move and
 * sele only; NULL adds nothing. 0 when nothing is added (another request, a
 * part of a packet, no code, no room). */
int xl_add_code(const uint8_t *in, int n, const char *code, uint8_t *out, int cap);
/* A whole packet with the line "line" added before the body's NUL, its
 * length fixed (the new length, 0 when it is not one whole packet or out
 * has no room). xl_add_code is this with TKCODE=; the identity scheme's
 * TKID=1 on addr (BO-337) too. */
int xl_add_line(const uint8_t *in, int n, const char *line, uint8_t *out, int cap);

/* KEY=<digits> at the start of a line of body (n bytes): 1 with *v set */
int xl_key_int(const char *body, int n, const char *key, int *v);
/* Correlation ids (BO-340): KEY=<4 to 16 lower-case hex digits> at the start
 * of a line (TKSID in the addr answer, TKGID in a game's member records):
 * 1 with the digits in out (cap bytes), 0 when there is no such line or its
 * value is anything else (an old server sends none; a future format is not
 * logged half-read). */
#define XL_ID_MAX 16
int xl_key_hex(const char *body, int n, const char *key, char *out, int cap);
/* KEY=<text> at the start of a line (a game's name, a word like
 * TKWAITSTATE's): 1 with the text in out (cap bytes), 0 when there is no
 * such line, it is empty, longer than cap - 1, or has a byte outside
 * 0x20..0x7E or an '=' (BO-365) */
int xl_key_str(const char *body, int n, const char *key, char *out, int cap);
/* TKWAITSTATE's word as XNET_WAIT_* (xnet_hle.h): racing 1, lobby 2, open 3,
 * full 4, gone 5; 0 for anything else */
int xl_wait_state(const char *word);
/* the lobby's "no game" record (IDENT=0, COUNT=0, no players: the host
 * left, the game was deleted): 1 when body is one */
int xl_game_gone(const char *body, int n);

typedef struct {
    int seen;        /* the server has said something */
    int online;      /* -1 = not said */
    int waiting;
    int racing;      /* TKRACING of the room record (BO-365); -1 = not said */
} xl_status;

/* the status fields of one body (KEY=VALUE lines), into st */
void xl_status_body(const char *body, int n, xl_status *st);

/* A game record (gcre/gjoi reply, +mgm): its players and the third value of
 * each one's USERPARAMS (OPPARAMn): 1 = joined with a car, 3 = ready, 0 =
 * not yet. The host's Start needs every guest at 3. */
#define XL_MAX_PLAYERS 8
typedef struct {
    int seen;
    int ident;
    int np;
    char name[XL_MAX_PLAYERS][16];
    int ready[XL_MAX_PLAYERS];
    char gid[XL_ID_MAX + 1];   /* TKGID, "" when the server sends none */
} xl_game;

/* 1 when the body was a game record with players (g filled) */
int xl_game_body(const char *body, int n, xl_game *g);
/* "Host1:1 Ubu2:3 ..." into out */
void xl_game_line(const xl_game *g, char *out, int cap);

#ifdef __cplusplus
}
#endif
#endif
