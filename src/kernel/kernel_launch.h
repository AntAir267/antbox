/* kernel_launch.h - what HalReturnToFirmware does with a title's request to
 * start the Xbox Dashboard.
 *
 * XLaunchNewImage(NULL, &LD_LAUNCH_DASHBOARD) writes the launch data page
 * (XWriteTitleInfo: LaunchDataType at +0, the title id at +4, the image
 * path at +8, the LAUNCH_DATA at +0x400, whose first dword is the
 * dashboard's dwReason) and reboots with HalReturnToFirmware(2). On a console
 * the dashboard then opens the page the reason names: 2 memory, 3 settings,
 * 6 network settings, 7 new account, 9 account management, ... There is no
 * dashboard here, and ending the process for such a request looks like a
 * crash to the player: the game vanishes from a menu.
 *
 * So a request for a dashboard page is refused: HalReturnToFirmware returns
 * STATUS_NOT_SUPPORTED, XLaunchNewImage hands the title the error
 * (RtlNtStatusToDosError), and the title stays where it was. A title that
 * just quits (XLaunchNewImage(NULL, NULL), type -1; the main menu, reason 0)
 * or gives up (XapiBootToDash's reason 1, XLD_LAUNCH_DASHBOARD_ERROR) still
 * ends the process, as do every other routine and launch type. */
#ifndef XBOX_KERNEL_LAUNCH_H
#define XBOX_KERNEL_LAUNCH_H

#include <stdint.h>

#define XBOX_LDT_LAUNCH_DASHBOARD          1u
#define XBOX_XLD_LAUNCH_DASHBOARD_MAIN_MENU 0u
#define XBOX_XLD_LAUNCH_DASHBOARD_ERROR     1u
#define XBOX_RETURN_FIRMWARE_QUICK_REBOOT   2u

/* 1: refuse (return to the title with an error), 0: end the process. */
static inline int xbox_launch_dashboard_refused(uint32_t routine, uint32_t page,
                                                uint32_t type, uint32_t reason)
{
    if (routine != XBOX_RETURN_FIRMWARE_QUICK_REBOOT || !page)
        return 0;
    if (type != XBOX_LDT_LAUNCH_DASHBOARD)
        return 0;
    return reason != XBOX_XLD_LAUNCH_DASHBOARD_MAIN_MENU &&
           reason != XBOX_XLD_LAUNCH_DASHBOARD_ERROR;
}

#endif
