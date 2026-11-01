/*
 * vk_dyn.c - the Vulkan loader the runtime calls on Android, Windows and
 * Linux, opened at run time. See vk_dyn.h.
 *
 * Windows: vulkan-1.dll. Linux: libvulkan.so.1, then libvulkan.so. When
 * neither opens, every call fails the way a loader with no driver does
 * (VK_ERROR_INITIALIZATION_FAILED, no instance) and xr_vk_loader_error()
 * says why, for the message the player sees (nv2a_vk_core.c, the presenter).
 *
 * Android: the system's loader, or a replacement driver opened through
 * libadrenotools (android_host.h has the variables; the activity sets them):
 *   RECOMP_ANDROID_DRIVER_DIR   a directory in the app's own internal storage
 *                               or its native library directory (the dynamic
 *                               linker refuses anything on shared storage)
 *   RECOMP_ANDROID_DRIVER_NAME  the driver's file name there, e.g.
 *                               libvulkan_freedreno.so (Mesa Turnip)
 *   RECOMP_ANDROID_LIB_DIR      the app's native library directory, where
 *                               libadrenotools' hook libraries are
 * libadrenotools (github.com/bylaws/libadrenotools, BSD-2-Clause) opens a
 * private copy of the system's libvulkan.so whose driver lookup ends at that
 * file; no root, nothing outside the app. Without the variables, or built
 * without libadrenotools, the loader is the system's.
 */
#include "vk_dyn.h"

#if defined(XR_VK_DYNAMIC)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if defined(__ANDROID__)
#include <SDL.h>
#include <SDL_system.h>
#if defined(XBOXRECOMP_HAVE_ADRENOTOOLS)
#include <adrenotools/driver.h>
#endif
#endif

/* Seen by the loader's users and, on Android, by SDL's dlsym. */
#if defined(_WIN32)
#define XR_VK_EXPORT
#else
#define XR_VK_EXPORT __attribute__((visibility("default")))
#endif

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static PFN_vkGetInstanceProcAddr s_gipa;
#if defined(__ANDROID__)
static char s_driver[256] = "system";
static int s_failed;
#endif
static char s_loader_err[256];

#if defined(__ANDROID__)
/* TU_DEBUG defaults, asked before the driver is opened; a TU_DEBUG the player
 * set (env.txt, even empty) is left as it is.
 *
 * The Turnip in the app (opened from the native library directory) is, since
 * BO-152, a prebuilt Mesa 26.2.0-devel build (whitebelyash's, as
 * UnleashedRecomp-Android ships it for its "A725 Performance" setting; see
 * docs/android.md). It runs with TU_DEBUG=sysmem,nobin on every Adreno, as
 * that app runs it: on the Retroid Pocket 5's Adreno 650 that pair ran with no
 * GPU hang where Mesa 26.2.4 hung every few minutes in races.
 *
 * BO-128: for a driver of the player's choosing on an Adreno 6xx, Turnip's
 * GMEM rendering with hardware binning wedges the GPU now and then
 * (RBBM_STATUS 0x00E59005, the vertex front end and the shader cores busy with
 * no memory traffic; the family of Mesa issues #5220 and #4923), so there the
 * old default stays: no binning pass and no UBWC (nobin,noubwc). */
static void turnip_defaults(int bundled)
{
    char model[64] = "";
    FILE *f;
    if (getenv("TU_DEBUG"))
        return;
    f = fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r");
    if (f) {
        if (!fgets(model, sizeof model, f))
            model[0] = 0;
        fclose(f);
    }
    model[strcspn(model, "\r\n")] = 0;
    if (bundled) {
        setenv("TU_DEBUG", "sysmem,nobin", 0);
        fprintf(stderr, "[vulkan] %s, the Turnip in the app: TU_DEBUG=sysmem,nobin\n",
                model[0] ? model : "GPU");
        return;
    }
    if (strncmp(model, "Adreno6", 7))
        return;
    setenv("TU_DEBUG", "nobin,noubwc", 0);
    fprintf(stderr, "[vulkan] %s: TU_DEBUG=nobin,noubwc (no binning pass, no UBWC)\n", model);
}

/* The driver in the app: the one in the native library directory. */
static int same_dir(const char *a, const char *b)
{
    size_t na, nb;
    if (!a || !b)
        return 0;
    na = strlen(a);
    nb = strlen(b);
    while (na > 1 && a[na - 1] == '/')
        na--;
    while (nb > 1 && b[nb - 1] == '/')
        nb--;
    return na == nb && !strncmp(a, b, na);
}

#endif /* __ANDROID__ */

#if defined(__ANDROID__)
static void open_loader(void)
{
    void *h = NULL;
    const char *dir = getenv("RECOMP_ANDROID_DRIVER_DIR");
    const char *name = getenv("RECOMP_ANDROID_DRIVER_NAME");
    const char *lib = getenv("RECOMP_ANDROID_LIB_DIR");

    if (dir && *dir && name && *name && strcmp(name, "system")) {
#if defined(XBOXRECOMP_HAVE_ADRENOTOOLS)
        char d[1024];
        turnip_defaults(same_dir(dir, lib));
        size_t n = strlen(dir);
        /* libadrenotools joins directory and name as they are. */
        snprintf(d, sizeof d, "%s%s", dir, n && dir[n - 1] == '/' ? "" : "/");
        if (lib && *lib)
            h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, lib,
                                           d, name, NULL, NULL);
        if (h) {
            snprintf(s_driver, sizeof s_driver, "%s", name);
            fprintf(stderr, "[vulkan] replacement driver %s%s opened through libadrenotools\n",
                    d, name);
        } else {
            /* A driver of the player's choosing that cannot be opened: the
             * one the app came with (RECOMP_ANDROID_DRIVER_FALLBACK_DIR and
             * _NAME, set by the activity for a chosen package), at this
             * start, and said on the screen. */
            const char *fdir = getenv("RECOMP_ANDROID_DRIVER_FALLBACK_DIR");
            const char *fname = getenv("RECOMP_ANDROID_DRIVER_FALLBACK_NAME");
            s_failed = 1;
            XR_ERROR( "[vulkan] replacement driver %s%s could not be opened through"
                    " libadrenotools (hooks in %s)\n",
                    d, name, lib && *lib ? lib : "(RECOMP_ANDROID_LIB_DIR not set)");
            if (fdir && *fdir && fname && *fname && lib && *lib) {
                char fd[1024];
                size_t fn = strlen(fdir);
                snprintf(fd, sizeof fd, "%s%s", fdir, fn && fdir[fn - 1] == '/' ? "" : "/");
                h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, lib,
                                               fd, fname, NULL, NULL);
                if (h) {
                    char msg[320];
                    s_failed = 0;
                    snprintf(s_driver, sizeof s_driver, "%s", fname);
                    snprintf(msg, sizeof msg, "The chosen graphics driver (%s) could not be"
                             " opened; the driver in the app is used.", name);
                    fprintf(stderr, "[vulkan] %s (%s%s)\n", msg, fd, fname);
                    SDL_AndroidShowToast(msg, 1, -1, 0, 0);
                }
            }
            if (!h)
                fprintf(stderr, "[vulkan] the system driver instead\n");
        }
#else
        s_failed = 1;
        fprintf(stderr, "[vulkan] RECOMP_ANDROID_DRIVER_NAME=%s: this build has no"
                " libadrenotools; the system driver instead\n", name);
#endif
    }
    if (!h)
        h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (h)
        s_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!s_gipa) {
        const char *e = dlerror();
        snprintf(s_loader_err, sizeof s_loader_err, "libvulkan.so could not be opened%s%s",
                 e ? ": " : "", e ? e : "");
        XR_ERROR( "[vulkan] no Vulkan loader: %s\n", e ? e : "(no reason given)");
    }
}

#elif defined(_WIN32)

static void open_loader(void)
{
    const char *ovr = getenv("RECOMP_VULKAN_LOADER");
    const char *name = ovr && *ovr ? ovr : "vulkan-1.dll";
    HMODULE h = LoadLibraryA(name);

    if (!h) {
        snprintf(s_loader_err, sizeof s_loader_err,
                 "%s could not be loaded (Windows error %lu): no Vulkan graphics driver"
                 " is installed", name, (unsigned long)GetLastError());
    } else {
        s_gipa = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(h, "vkGetInstanceProcAddr");
        if (!s_gipa)
            snprintf(s_loader_err, sizeof s_loader_err,
                     "vulkan-1.dll has no vkGetInstanceProcAddr: it is not a Vulkan loader");
    }
    if (!s_gipa)
        XR_ERROR( "[vulkan] no Vulkan loader: %s\n", s_loader_err);
}

#else /* Linux */

static void open_loader(void)
{
    /* The versioned name first: a bare libvulkan.so is the development
     * package's symlink, which most players do not have. RTLD_GLOBAL is not
     * asked for; SDL opens the same file by its own call and gets this copy.
     * RECOMP_VULKAN_LOADER=<file> opens that file instead (tests, a driver
     * under development). */
#if defined(__APPLE__)
    static const char *const names[] = { "libvulkan.1.dylib", "libvulkan.dylib",
                                         "/opt/homebrew/lib/libvulkan.1.dylib" };
#else
    static const char *const names[] = { "libvulkan.so.1", "libvulkan.so" };
#endif
    const char *ovr = getenv("RECOMP_VULKAN_LOADER");
    void *h = NULL;
    const char *e = NULL;
    size_t i;

    if (ovr && *ovr) {
        h = dlopen(ovr, RTLD_NOW | RTLD_LOCAL);
        if (!h)
            e = dlerror();
    } else {
        for (i = 0; !h && i < sizeof names / sizeof names[0]; i++) {
            h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
            if (!h && !e)
                e = dlerror();
        }
    }
    if (h)
        s_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (h && !s_gipa)
        e = "it has no vkGetInstanceProcAddr";
    if (!s_gipa) {
        snprintf(s_loader_err, sizeof s_loader_err,
                 "%s could not be opened: no Vulkan loader is installed%s%s",
                 ovr && *ovr ? ovr : names[0], e ? " (" : "", e ? e : "");
        if (e && strlen(s_loader_err) + 2 < sizeof s_loader_err)
            strcat(s_loader_err, ")");
        XR_ERROR( "[vulkan] no Vulkan loader: %s\n", s_loader_err);
    }
}

#endif

int xr_vk_loader_ok(void)
{
    pthread_once(&s_once, open_loader);
    return s_gipa != NULL;
}

const char *xr_vk_loader_error(void)
{
    pthread_once(&s_once, open_loader);
    return s_gipa ? "" : s_loader_err;
}

#if defined(__ANDROID__)
const char *xr_android_vk_driver(void)
{
    pthread_once(&s_once, open_loader);
    return s_driver;
}

int xr_android_vk_driver_failed(void)
{
    pthread_once(&s_once, open_loader);
    return s_failed;
}
#endif

XR_VK_EXPORT
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *name)
{
    pthread_once(&s_once, open_loader);
    return s_gipa ? s_gipa(instance, name) : NULL;
}

/* The four commands that exist before an instance does start as these: they
 * open the loader and go on to its own. vkCreateInstance then fills in every
 * other pointer from the new instance (one instance and one device in this
 * runtime, so the instance-level entry points serve the device too). A
 * second instance (the bug report's probe) fills them with the same loader
 * functions again. */
static VKAPI_ATTR VkResult VKAPI_CALL boot_vkEnumerateInstanceVersion(uint32_t *version)
{
    PFN_vkEnumerateInstanceVersion f = (PFN_vkEnumerateInstanceVersion)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (!f) {               /* a Vulkan 1.0 loader has none */
        *version = VK_API_VERSION_1_0;
        return s_gipa ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    }
    return f(version);
}

static VKAPI_ATTR VkResult VKAPI_CALL boot_vkEnumerateInstanceExtensionProperties(
    const char *layer, uint32_t *count, VkExtensionProperties *props)
{
    PFN_vkEnumerateInstanceExtensionProperties f = (PFN_vkEnumerateInstanceExtensionProperties)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!f) {
        *count = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return f(layer, count, props);
}

static VKAPI_ATTR VkResult VKAPI_CALL boot_vkEnumerateInstanceLayerProperties(
    uint32_t *count, VkLayerProperties *props)
{
    PFN_vkEnumerateInstanceLayerProperties f = (PFN_vkEnumerateInstanceLayerProperties)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    if (!f) {
        *count = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return f(count, props);
}

/* vkCreateDevice as the loader has it, and the one the runtime calls: after
 * it, any command the instance did not hand out (an old loader does not know
 * the Vulkan 1.3 names, but the driver's device does) is asked of the device. */
static PFN_vkCreateDevice s_real_create_device;

static VKAPI_ATTR VkResult VKAPI_CALL boot_vkCreateDevice(
    VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc,
    VkDevice *device)
{
    VkResult r = s_real_create_device ? s_real_create_device(phys, ci, alloc, device)
                                      : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS || !xr_vkGetDeviceProcAddr)
        return r;
#define XR_VK_FILL(name) \
    if (!xr_##name) \
        xr_##name = (PFN_##name)xr_vkGetDeviceProcAddr(*device, #name);
    XR_VK_FUNCS(XR_VK_FILL)
#undef XR_VK_FILL
    if (!xr_vkCmdBeginRenderingKHR) xr_vkCmdBeginRenderingKHR = xr_vkCmdBeginRendering;
    if (!xr_vkCmdEndRenderingKHR)   xr_vkCmdEndRenderingKHR = xr_vkCmdEndRendering;
    if (!xr_vkCmdBeginRendering)    xr_vkCmdBeginRendering = xr_vkCmdBeginRenderingKHR;
    if (!xr_vkCmdEndRendering)      xr_vkCmdEndRendering = xr_vkCmdEndRenderingKHR;
    return r;
}

const char *xr_vk_missing_core13(void)
{
    static const struct { const char *name; const void *const *ptr; } need[] = {
#define XR_VK_NEED(n) { #n, (const void *const *)&xr_##n },
        XR_VK_NEED(vkGetPhysicalDeviceFeatures2)
        XR_VK_NEED(vkGetPhysicalDeviceProperties2)
        XR_VK_NEED(vkCmdBeginRendering)
        XR_VK_NEED(vkCmdEndRendering)
        XR_VK_NEED(vkCmdPipelineBarrier2)
        XR_VK_NEED(vkGetSemaphoreCounterValue)
        XR_VK_NEED(vkWaitSemaphores)
        XR_VK_NEED(vkCmdSetCullMode)
        XR_VK_NEED(vkCmdSetFrontFace)
        XR_VK_NEED(vkCmdSetDepthTestEnable)
        XR_VK_NEED(vkCmdSetDepthWriteEnable)
        XR_VK_NEED(vkCmdSetDepthCompareOp)
        XR_VK_NEED(vkCmdSetStencilTestEnable)
        XR_VK_NEED(vkCmdSetStencilOp)
        XR_VK_NEED(vkCmdSetDepthBiasEnable)
#undef XR_VK_NEED
    };
    size_t i;

    for (i = 0; i < sizeof need / sizeof need[0]; i++)
        if (!*need[i].ptr)
            return need[i].name;
    return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL boot_vkCreateInstance(
    const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *instance)
{
    PFN_vkCreateInstance f = (PFN_vkCreateInstance)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r;

    if (!f)
        return VK_ERROR_INITIALIZATION_FAILED;
    r = f(ci, alloc, instance);
    if (r != VK_SUCCESS)
        return r;
#define XR_VK_LOAD(name) \
    if (strcmp(#name, "vkCreateInstance") && strcmp(#name, "vkEnumerateInstanceVersion") \
            && strcmp(#name, "vkEnumerateInstanceExtensionProperties") \
            && strcmp(#name, "vkEnumerateInstanceLayerProperties")) \
        xr_##name = (PFN_##name)s_gipa(*instance, #name);
    XR_VK_FUNCS(XR_VK_LOAD)
#undef XR_VK_LOAD
    s_real_create_device = (PFN_vkCreateDevice)s_gipa(*instance, "vkCreateDevice");
    xr_vkCreateDevice = boot_vkCreateDevice;
    /* The KHR names of what Vulkan 1.3 took in: the same commands. */
    if (!xr_vkCmdBeginRenderingKHR) xr_vkCmdBeginRenderingKHR = xr_vkCmdBeginRendering;
    if (!xr_vkCmdEndRenderingKHR)   xr_vkCmdEndRenderingKHR = xr_vkCmdEndRendering;
    if (!xr_vkCmdBeginRendering)    xr_vkCmdBeginRendering = xr_vkCmdBeginRenderingKHR;
    if (!xr_vkCmdEndRendering)      xr_vkCmdEndRendering = xr_vkCmdEndRenderingKHR;
    return r;
}

/* The pointers. Null until an instance exists, but for the four above. */
#define XR_VK_DEFINE(name) PFN_##name xr_##name;
XR_VK_FUNCS(XR_VK_DEFINE)
#undef XR_VK_DEFINE

__attribute__((constructor)) static void boot_pointers(void)
{
    xr_vkEnumerateInstanceVersion = boot_vkEnumerateInstanceVersion;
    xr_vkEnumerateInstanceExtensionProperties = boot_vkEnumerateInstanceExtensionProperties;
    xr_vkEnumerateInstanceLayerProperties = boot_vkEnumerateInstanceLayerProperties;
    xr_vkCreateInstance = boot_vkCreateInstance;
}

#else
typedef int xr_vk_dyn_unused;
#endif
