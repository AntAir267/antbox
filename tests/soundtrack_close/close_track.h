/* Force-included into xst_decode.c (and the stb_vorbis it includes) by this
 * test: every fopen and fclose goes through the counters in test_main.c. */
#include <stdio.h>
FILE *track_fopen(const char *path, const char *mode);
int   track_fclose(FILE *f);
#define fopen  track_fopen
#define fclose track_fclose
