/*
 * kernel_save_import.h - bring a save from an original Xbox (or xemu) into
 * the port's save layout.
 *
 * What the original title wrote lives under E:\UDATA\<title id>\<save id>\
 * (SaveMeta.xbx and the profile, here "Profile 1"), with the title's
 * TitleMeta.xbx, TitleImage.xbx and SaveImage.xbx one level up, and
 * sometimes E:\TDATA\<title id>. The profile ends in a 20-byte signature,
 *     HMAC-SHA1(XboxHDKey, HMAC-SHA1(XboxSignatureKey, file minus signature))
 * and the port keeps both keys at zero, so the profile is signed again with
 * zeros. SaveMeta.xbx and the other files carry no signature.
 *
 * The input is a save folder (the <save id> folder), a folder that holds it
 * (the title id folder, UDATA, or a whole E: drive copy), or a .zip of any of
 * them. The result is a folder shaped like a save export (recomp_save_dir.h:
 * UDATA/<title id>/... plus save-export.txt), which the port's own import
 * puts in place at the next start, with the save in use set aside first.
 *
 * A Burnout 3 save from the PlayStation 2 is taken the same way (BO-323,
 * kernel_ps2save.h): a memory card image, a .psu/.max/.cbs/.xps/.sps/.psv
 * file, a PS2 save folder, a folder or .zip holding one of them. One of its
 * profiles becomes the Xbox "Profile 1", made from nothing but the PS2 data
 * and, when there is a save in use, its settings (the player's own settings
 * win), then signed for the port.
 */
#ifndef KERNEL_SAVE_IMPORT_H
#define KERNEL_SAVE_IMPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Whether `path` (a folder) holds an Xbox save of this title or a Burnout 3
 * PS2 save: a quick look, no copy made. Bounded as
 * xbox_SaveImportCheckBounded with no limits given; "not known" is 0. */
int xbox_SaveImportCheck(const char *path, const char *title_id);

/* The same look with a bound (BO-289: a folder browser asks it of every
 * row, and a network share must not hold it). It looks at most max_entries
 * entries for at most max_ms (0: 50000 and 3000 ms), asks `stop` at each
 * one, and below a folder `shallow` says is on a slow volume it does not
 * look (the folder's own files it does). 1: an Xbox save of this title;
 * 2: a Burnout 3 PS2 save; 0: none; -1: not known (the bound ran out, `stop`
 * said so, or a folder was left unwalked). */
typedef struct xbox_save_check_limits {
    int   max_entries;
    int   max_ms;
    int (*stop)(void *ctx);
    int (*shallow)(const char *dir, void *ctx);
    void *ctx;
} xbox_save_check_limits;
int xbox_SaveImportCheckBounded(const char *path, const char *title_id, const xbox_save_check_limits *lim);

/*
 * Make the export-shaped folder <work_root>/import-work/out from `input`.
 *   keep_root  the save root in use, or NULL: its title-level files (title
 *              image and meta) fill in what the input lacks.
 * 1 and the folder in `out`; `info` gets a line for the player (profile
 * name, files, how it was signed). 0 with a sentence in `err`; nothing in
 * the save root is touched either way.
 */
int xbox_SaveImportPrepare(const char *input, const char *title_id, const char *keep_root,
                           const char *work_root, char *out, unsigned out_size,
                           char *info, unsigned info_size, char *err, unsigned err_size);

/* The profiles of a PS2 save to choose from (BO-323). */
typedef struct xbox_ps2_choice {
    int  count;                 /* profiles in use */
    int  slot[3];               /* their numbers, 1..3 */
    char line[3][96];           /* "NAME, 120 gold medals, 30.9 hours" */
} xbox_ps2_choice;

/*
 * xbox_SaveImportPrepare with the profile of a PS2 save to take:
 *   ps2_slot   0: the one in use (a save with several is refused, their
 *              list in `choice` and `err`); 1..3: that one. An Xbox save
 *              does not look at it.
 *   choice     may be NULL.
 */
int xbox_SaveImportPrepareSlot(const char *input, const char *title_id, const char *keep_root,
                               const char *work_root, int ps2_slot, xbox_ps2_choice *choice,
                               char *out, unsigned out_size, char *info, unsigned info_size,
                               char *err, unsigned err_size);

/* Remove the work folder (after the import has been applied). */
void xbox_SaveImportCleanup(const char *work_root);

#ifdef __cplusplus
}
#endif
#endif
