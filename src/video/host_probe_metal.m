/*
 * host_probe_metal.m - the Metal devices of this Mac, for the bug report's
 * system.txt (host_probe.c). Asks Metal for its device list; makes nothing.
 * Manual retain/release, as the rest of this target.
 */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "xbox_report.h"

void xbox_host_probe_metal(xbox_report_buf *b)
{
    @autoreleasepool {
        NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
        NSUInteger i;

        for (i = 0; i < devs.count; i++) {
            id<MTLDevice> d = devs[i];
            const char *family = "?";
            if (@available(macOS 13.0, *))
                family = [d supportsFamily:MTLGPUFamilyMetal3] ? "Metal 3" : "Metal 2";
            xbox_report_printf(b, "gpu %lu: %s (%s%s%s), %s, working set %llu MiB\n",
                               (unsigned long)i, d.name.UTF8String,
                               d.hasUnifiedMemory ? "unified memory" : "discrete memory",
                               d.isLowPower ? ", low power" : "", d.isRemovable ? ", external" : "",
                               family, (unsigned long long)(d.recommendedMaxWorkingSetSize >> 20));
        }
        if (!devs.count)
            xbox_report_printf(b, "(Metal lists no device)\n");
        xbox_report_printf(b, "(Metal's driver is the system's: its version is the OS version)\n");
        [devs release];
    }
}
