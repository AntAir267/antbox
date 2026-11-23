/*
 * kernel_ps2save.h - a Burnout 3 save from the PlayStation 2, read out of the
 * files PS2 players have and turned into the Xbox profile (BO-323).
 *
 * Where it comes from: a PS2 memory card image (PCSX2 .ps2, with or without
 * the ECC bytes), a PCSX2 folder memory card or a plain save folder, or one
 * of the single-save files: .psu (uLaunchELF/EMS), .max (Action Replay MAX,
 * LZARI packed), .cbs (CodeBreaker, RC4 + zlib), .xps/.sps (X-Port and
 * SharkPort), .psv (a PS3's PS2 save). The formats were worked out from
 * Ross Ridge's mymc (public domain) and Haruhiko Okumura's LZARI; no code of
 * either is in here.
 *
 * The save: a folder named B<region><serial> (BASLUS-21050, BESLES-52584,
 * BESLES-52585, BISLPM-65719, ...) holding a file of the same name, 61772
 * bytes, laid out as the Xbox profile is (both little-endian):
 *     0x0000  u32 version, 0x10
 *     0x0004  the records table (0xE330 bytes), as on the Xbox
 *     0xE334  the settings (0x40 bytes), as on the Xbox
 *     0xE374  three profiles of 0x49C bytes (the Xbox file has one), the
 *             last byte of each is 1 when the slot is empty
 *     0xF148  u32 sum of the 32-bit words before it
 * The Xbox "Profile 1" (59904 bytes) has the version, the table, the
 * settings, one profile, the same sum at 0xE810, zeros, and the 20-byte
 * signature at 0xE9EC. The profile is the same structure on both (name,
 * play time, points, medals, takedowns, records); what is unlocked is worked
 * out by the game from it at load. US, PAL and Japanese saves are alike.
 */
#ifndef KERNEL_PS2SAVE_H
#define KERNEL_PS2SAVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2SAVE_DATA_SIZE    0xF14Cu   /* the PS2 data file */
#define PS2SAVE_XBOX_SIZE    0xEA00u   /* the Xbox "Profile 1" */
#define PS2SAVE_SLOTS        3
#define PS2SAVE_SETTINGS     0xE334u   /* the settings, 0x40 bytes, same place on both */
#define PS2SAVE_SETTINGS_LEN 0x40u

typedef struct {
    int   used;
    char  name[40];       /* UTF-8 */
    float hours;          /* time played */
    int   gold;           /* gold medals, of 173 events */
    int   medals;         /* events with any medal */
} ps2save_profile;

typedef struct {
    uint8_t data[PS2SAVE_DATA_SIZE];
    char    dir[40];      /* the save's folder name, e.g. BASLUS-21050 */
    char    kind[48];     /* what it was read from, for the player */
    char    file[1024];   /* the file (or folder) it was read from */
    int     found;        /* Burnout 3 saves seen; the first is the one here */
    int     used;         /* profiles in use */
    ps2save_profile prof[PS2SAVE_SLOTS];
} ps2save_b3;

/* A look may be cut short: `spent` is asked at each file and folder looked
 * at, and 1 stops the look (NULL: no bound). `shallow` (may be NULL) says a
 * folder is not to be looked below. */
typedef struct {
    int (*spent)(void *ctx);
    int (*shallow)(const char *dir, void *ctx);
    void *ctx;
} ps2save_bound;

/* A Burnout 3 PS2 save at `path`: a save file or card image, a save folder,
 * or a folder holding one of those up to `depth` levels below. 1 and `out`
 * filled; 0 with a sentence in `err` (none, damaged, not Burnout 3); -1 when
 * the bound ran out before an answer. */
int ps2save_find(const char *path, int depth, const ps2save_bound *bound, ps2save_b3 *out,
                 char *err, unsigned err_size);

/* The same for a file already in memory (`name` for the messages). */
int ps2save_read_buffer(const uint8_t *buf, size_t n, const char *name, ps2save_b3 *out,
                        char *err, unsigned err_size);

/* Whether a 61772-byte data file is a Burnout 3 save: size, version, sum. */
int ps2save_data_ok(const uint8_t *d, size_t n);

/* The Xbox "Profile 1" made from profile `slot` (0..2) of `ps2`: version,
 * records table, the profile, sum. `settings` (0x40 bytes) are the ones to
 * keep (the player's current ones), or NULL for the PS2's own with the PS2
 * screen offset cleared. The last 20 bytes (the signature) are left zero for
 * the caller to sign. 0 when the slot is empty or out of range. */
int ps2save_to_xbox(const uint8_t ps2[PS2SAVE_DATA_SIZE], int slot, const uint8_t *settings,
                    uint8_t out[PS2SAVE_XBOX_SIZE]);

/* The sum the game keeps: the 32-bit little-endian words of `d` added. */
uint32_t ps2save_sum(const uint8_t *d, size_t n);

/* Raw deflate into exactly `outlen` bytes (kernel_save_import.c). */
int xbox_SaveImportInflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);

#ifdef __cplusplus
}
#endif
#endif
