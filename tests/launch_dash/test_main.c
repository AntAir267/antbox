/* What HalReturnToFirmware refuses: a request for a dashboard page. */
#include <stdio.h>
#include "kernel_launch.h"

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

int main(void)
{
    const uint32_t page = 0x80B97000u;
    /* Burnout 3: CREATE NEW ACCOUNT (7), MANAGE ACCOUNTS (9), network settings (6), 8 */
    CHECK(xbox_launch_dashboard_refused(2, page, 1, 7));
    CHECK(xbox_launch_dashboard_refused(2, page, 1, 9));
    CHECK(xbox_launch_dashboard_refused(2, page, 1, 6));
    CHECK(xbox_launch_dashboard_refused(2, page, 1, 8));
    CHECK(xbox_launch_dashboard_refused(2, page, 1, 2));    /* memory */
    /* a quit stays a quit */
    CHECK(!xbox_launch_dashboard_refused(2, page, 1, 0));   /* the main menu */
    CHECK(!xbox_launch_dashboard_refused(2, page, 1, 1));   /* XapiBootToDash: an error */
    CHECK(!xbox_launch_dashboard_refused(2, page, 0xFFFFFFFFu, 0)); /* XLaunchNewImage(NULL, NULL) */
    CHECK(!xbox_launch_dashboard_refused(2, page, 0, 7));   /* another title (LDT_TITLE) */
    CHECK(!xbox_launch_dashboard_refused(2, 0, 1, 7));      /* no page */
    CHECK(!xbox_launch_dashboard_refused(1, page, 1, 7));   /* a reboot */
    CHECK(!xbox_launch_dashboard_refused(4, page, 1, 7));
    if (s_fail) {
        printf("%d failed\n", s_fail);
        return 1;
    }
    printf("launch_dash: all passed\n");
    return 0;
}
