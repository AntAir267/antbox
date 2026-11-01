/*
 * host_probe.c - the machine as SDL and the graphics API describe it, for
 * the bug report's system.txt (xbox_report.h): displays, controllers, GPUs.
 *
 * While a title runs this only reads what SDL already has (the window's
 * thread started video and the controllers; a headless run started neither
 * and gets a line saying so; the GPU is in the log, which the report quotes).
 * A report made without a title (a command-line switch) has nothing else
 * running, so there the subsystems are started for the listing and the
 * graphics API is asked directly: xbox_HostReportProbe(1).
 *
 * The Vulkan probe is an instance of its own, made and destroyed here; it
 * shares nothing with the backend.
 */
#include "video_player.h"
#include "xbox_report.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#if defined(XBOXRECOMP_HAVE_VULKAN) && !defined(__APPLE__) && !defined(__ANDROID__)
#include <vulkan/vulkan.h>
#define PROBE_VULKAN 1
#endif

#if defined(__APPLE__)
void xbox_host_probe_metal(xbox_report_buf *b);     /* host_probe_metal.m */
#endif

static int s_may_init;

static void displays(xbox_report_buf *b)
{
    int started = 0, n, i;

    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!s_may_init) {
            xbox_report_printf(b, "(no window in this process: a headless run)\n");
            return;
        }
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            xbox_report_printf(b, "(SDL video did not start: %s)\n", SDL_GetError());
            return;
        }
        started = 1;
    }
    xbox_report_printf(b, "video driver: %s\n", SDL_GetCurrentVideoDriver()
                                                ? SDL_GetCurrentVideoDriver() : "?");
    n = SDL_GetNumVideoDisplays();
    for (i = 0; i < n; i++) {
        SDL_DisplayMode cur, desk;
        SDL_Rect r = { 0, 0, 0, 0 };
        float dpi = 0.f;
        const char *name = SDL_GetDisplayName(i);

        memset(&cur, 0, sizeof cur);
        memset(&desk, 0, sizeof desk);
        SDL_GetCurrentDisplayMode(i, &cur);
        SDL_GetDesktopDisplayMode(i, &desk);
        SDL_GetDisplayBounds(i, &r);
        SDL_GetDisplayDPI(i, &dpi, NULL, NULL);
        xbox_report_printf(b, "display %d: \"%s\" mode %dx%d at %d Hz (desktop %dx%d at %d Hz),"
                           " %dx%d at %d,%d, %.0f dpi\n", i, name ? name : "?", cur.w, cur.h,
                           cur.refresh_rate, desk.w, desk.h, desk.refresh_rate, r.w, r.h, r.x,
                           r.y, (double)dpi);
    }
    if (!n)
        xbox_report_printf(b, "(no display)\n");
    if (started)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void controllers(xbox_report_buf *b)
{
    static const char *const type[] = { "unknown", "Xbox 360", "Xbox One", "PS3", "PS4",
        "Switch Pro", "virtual", "PS5", "Amazon Luna", "Google Stadia", "NVIDIA Shield",
        "Joy-Con left", "Joy-Con right", "Joy-Con pair" };
    int started = 0, n, i;

    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        if (!s_may_init) {
            xbox_report_printf(b, "(the controllers were not started in this process)\n");
            return;
        }
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            xbox_report_printf(b, "(SDL controllers did not start: %s)\n", SDL_GetError());
            return;
        }
        started = 1;
    }
    SDL_JoystickUpdate();
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        const char *name = SDL_JoystickNameForIndex(i);
        int t = SDL_IsGameController(i) ? (int)SDL_GameControllerTypeForIndex(i) : -1;
        xbox_report_printf(b, "pad %d: \"%s\" %04x:%04x version %04x, %s\n", i, name ? name : "?",
                           SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i),
                           SDL_JoystickGetDeviceProductVersion(i),
                           t < 0 ? "a joystick SDL has no mapping for"
                           : t < (int)(sizeof type / sizeof type[0]) ? type[t] : "game controller");
    }
    if (!n)
        xbox_report_printf(b, "(none connected)\n");
    if (started)
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

#if defined(PROBE_VULKAN)
static void vulkan_devices(xbox_report_buf *b)
{
    static const char *const kind[] = { "other", "integrated", "discrete", "virtual", "cpu" };
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice dev[8];
    uint32_t n = 8, i, loader = VK_API_VERSION_1_0;
    VkResult rc;

#if defined(XR_VK_DYNAMIC)
    if (!xr_vk_loader_ok()) {      /* BO-231: opened at run time, may be absent */
        xbox_report_printf(b, "(no Vulkan loader: %s)\n", xr_vk_loader_error());
        return;
    }
#endif
    vkEnumerateInstanceVersion(&loader);
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "xboxrecomp bug report";
    app.apiVersion = VK_API_VERSION_1_1;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    rc = vkCreateInstance(&ci, NULL, &inst);
    xbox_report_printf(b, "Vulkan loader %u.%u.%u\n", VK_API_VERSION_MAJOR(loader),
                       VK_API_VERSION_MINOR(loader), VK_API_VERSION_PATCH(loader));
    if (rc != VK_SUCCESS) {
        xbox_report_printf(b, "(no Vulkan instance: VkResult %d: no driver installed?)\n", (int)rc);
        return;
    }
    rc = vkEnumeratePhysicalDevices(inst, &n, dev);
    if (rc != VK_SUCCESS && rc != VK_INCOMPLETE)
        n = 0;
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceDriverProperties drv;
        VkPhysicalDeviceProperties2 p2;

        vkGetPhysicalDeviceProperties(dev[i], &p);
        memset(&drv, 0, sizeof drv);
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        memset(&p2, 0, sizeof p2);
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &drv;
        if (p.apiVersion >= VK_API_VERSION_1_1)
            vkGetPhysicalDeviceProperties2(dev[i], &p2);
        xbox_report_printf(b, "gpu %u: %s (%s, %04x:%04x), Vulkan %u.%u.%u, driver %s %s"
                           " (0x%X)%s\n", i, p.deviceName,
                           p.deviceType <= 4 ? kind[p.deviceType] : "?", p.vendorID, p.deviceID,
                           VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                           VK_API_VERSION_PATCH(p.apiVersion),
                           drv.driverName[0] ? drv.driverName : "?", drv.driverInfo,
                           p.driverVersion,
                           p.apiVersion < VK_API_VERSION_1_3 ? "  <- below the 1.3 the game needs"
                                                             : "");
    }
    if (!n)
        xbox_report_printf(b, "(Vulkan lists no device)\n");
    vkDestroyInstance(inst, NULL);
}
#endif

static void gpus(xbox_report_buf *b)
{
    if (!s_may_init) {
        xbox_report_printf(b, "(the device the title runs on is in the log lines below)\n");
        return;
    }
#if defined(__APPLE__)
    xbox_host_probe_metal(b);
#elif defined(PROBE_VULKAN)
    vulkan_devices(b);
#else
    xbox_report_printf(b, "(not probed on this host; see the log lines below)\n");
#endif
}

void xbox_HostReportProbe(int may_init)
{
    s_may_init = may_init;
    xbox_report_section("system.txt", "gpu", gpus);
    xbox_report_section("system.txt", "displays", displays);
    xbox_report_section("system.txt", "controllers", controllers);
}
