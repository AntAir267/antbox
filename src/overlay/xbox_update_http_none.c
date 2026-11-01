/* xbox_update_http_get where there is no simple way to do it (Android: the
 * Java activity would have to do it over JNI; the overlay has no row there). */
#include "xbox_update.h"

#include <stdio.h>

int xbox_update_available(void) { return 0; }

int xbox_update_http_get(const char *url, const char *user_agent, int timeout_ms,
                         char **body, char *err, size_t errn)
{
    (void)url; (void)user_agent; (void)timeout_ms;
    *body = NULL;
    snprintf(err, errn, "not available here");
    return 0;
}
