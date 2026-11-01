/*
 * kernel_console_keys.h - the console keys a title's saves are signed with.
 *
 * A title signs its save files with two keys it reads from the kernel:
 *     signature = HMAC-SHA1(XboxHDKey, HMAC-SHA1(XboxSignatureKey, data))
 * XboxHDKey is the console's own key (the EEPROM holds it, encrypted);
 * XboxSignatureKey is the title's key, which the kernel derives from the
 * XBE certificate's signature key. A fresh port has no console, so both are
 * 16 zero bytes, and every save the port makes is signed with them.
 *
 * Opt in with a console key (a hex key or an eeprom.bin) and the port gives
 * the title the original console's key and the certificate-derived signature
 * key, so a save copied from an original Xbox or from xemu loads as it is.
 * Saves the port made before then are signed with the zero keys and the title
 * calls them "Unusable" while the key is set (the files are not touched; take
 * the key away and they load again).
 *
 * Settings: console.hdd_key (RECOMP_CONSOLE_HDKEY, 32 hex digits) or
 * console.eeprom (RECOMP_CONSOLE_EEPROM, the path of an eeprom.bin).
 */
#ifndef KERNEL_CONSOLE_KEYS_H
#define KERNEL_CONSOLE_KEYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 32 hex digits (spaces, colons and dashes between bytes are skipped). */
int xbox_ConsoleKeyParseHex(const char *text, uint8_t out[16]);

/* The HDD key of a 256-byte EEPROM image (any of the retail versions).
 * 0 when the image is not a valid encrypted EEPROM. */
int xbox_ConsoleKeyFromEeprom(const uint8_t eeprom[256], uint8_t hdkey[16]);

/* The kernel's XboxSignatureKey for a certificate's 16-byte signature key. */
void xbox_ConsoleKeyDeriveSignature(const uint8_t cert_key[16], uint8_t out[16]);

/* Reads the settings. 1: a console key is set and *hdkey holds it. 0: not
 * set, or not usable (the reason is in xbox_ConsoleKeyStatus). */
int xbox_ConsoleKeyLoad(uint8_t hdkey[16]);

/* The one-line state for the Advanced page and the log. */
const char *xbox_ConsoleKeyStatus(void);

#ifdef __cplusplus
}
#endif
#endif
