/**
 * The shared Vulkan device (see nv2a_vk_core.h): instance, physical and
 * logical device, the one graphics queue and its lock, and the small
 * allocation and barrier helpers the backend and the presenter both use.
 *
 * Device choice follows xemu's Vulkan renderer (hw/xbox/nv2a/pgraph/vk/
 * instance.c: a graphics queue family that can present, the portability
 * extensions MoltenVK needs, D24S8 or D32S8 by format support) with this
 * renderer's own needs: Vulkan 1.3 core features instead of extensions, and
 * D32S8 preferred, since the Metal and D3D11 backends compare the same
 * normalised float depth the CPU rasteriser keeps.
 */
#include "nv2a_vk_core.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_env.h"
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

static struct nvk_core s_core;
static int             s_state;          /* 0 not tried, 1 up, -1 failed */
static char            s_err[256];
static pthread_mutex_t s_init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_queue_lock = PTHREAD_MUTEX_INITIALIZER;

const char *nvk_result_str(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    default: return "VkResult(?)";
    }
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name))
            return 1;
    return 0;
}

/* fmt takes two strings: a, and r's name (none for VK_SUCCESS: a reason
 * that is not a failed call). */
static int fail(const char *fmt, const char *a, VkResult r)
{
    snprintf(s_err, sizeof s_err, fmt, a ? a : "", r == VK_SUCCESS ? "" : nvk_result_str(r));
    return 0;
}

static int make_instance(const char *const *inst_exts, unsigned n)
{
    const char *exts[32];
    uint32_t ne = 0, navail = 0, i;
    VkExtensionProperties *avail = NULL;
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    const char *layers[1];
    uint32_t nl = 0;
    VkResult r;
    uint32_t ver = 0;

#if defined(__APPLE__)
    /* MoltenVK compiles with Metal fast math on unless told otherwise; the
     * Metal backend's rule is fast math off, so float results match the CPU
     * path's IEEE arithmetic (a far-plane quad at z = zmax lands an ulp past
     * the clip volume otherwise: the smoke test's far quad). Before the
     * instance: MoltenVK reads its configuration then. */
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);
#endif
#if defined(XR_VK_DYNAMIC)
    /* BO-231: the loader is opened here, not by the system before main. */
    if (!xr_vk_loader_ok()) {
        snprintf(s_err, sizeof s_err, "%s", xr_vk_loader_error());
        return 0;
    }
#endif
    if (vkEnumerateInstanceVersion(&ver) != VK_SUCCESS || ver < VK_API_VERSION_1_3) {
        snprintf(s_err, sizeof s_err, "the Vulkan loader is version %u.%u, Vulkan 1.3 is needed",
                 VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver));
        return 0;
    }
    vkEnumerateInstanceExtensionProperties(NULL, &navail, NULL);
    avail = (VkExtensionProperties *)calloc(navail ? navail : 1, sizeof *avail);
    if (!avail)
        return fail("%s%s", "out of memory ", VK_ERROR_OUT_OF_HOST_MEMORY);
    vkEnumerateInstanceExtensionProperties(NULL, &navail, avail);
    for (i = 0; i < n && ne < 28; i++)
        exts[ne++] = inst_exts[i];
    memset(&ci, 0, sizeof ci);
    /* MoltenVK is a portability implementation: the loader lists it only
     * when asked to (VK_KHR_portability_enumeration). */
    if (has_ext(avail, navail, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        exts[ne++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    /* BO-110: HDR colour spaces of a window's surface (HDR10 ST 2084 under
     * gamescope's WSI layer, a Wayland compositor with colour management). */
    if (n && has_ext(avail, navail, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
        exts[ne++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
        s_core.colorspace_ext = 1;
    }
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
    /* BO-207: whether a surface takes VK_KHR_present_wait2 (its swapchain
     * flags need the surface's word for it). */
    if (n && has_ext(avail, navail, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
        exts[ne++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        s_core.surface_caps2 = 1;
    }
#endif
    if (recomp_env(RENV_VK_VALIDATION)) {
        uint32_t nlp = 0;
        VkLayerProperties *lp;
        vkEnumerateInstanceLayerProperties(&nlp, NULL);
        lp = (VkLayerProperties *)calloc(nlp ? nlp : 1, sizeof *lp);
        if (lp) {
            vkEnumerateInstanceLayerProperties(&nlp, lp);
            for (i = 0; i < nlp; i++)
                if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation"))
                    layers[nl++] = "VK_LAYER_KHRONOS_validation";
            free(lp);
        }
        fprintf(stderr, "[vulkan] validation layer %s\n", nl ? "on" : "not installed");
        s_core.validation = nl > 0;
    }
    free(avail);
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "xboxrecomp";
    app.pEngineName = "xboxrecomp nv2a";
    app.apiVersion = VK_API_VERSION_1_3;
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = ne;
    ci.ppEnabledExtensionNames = exts;
    ci.enabledLayerCount = nl;
    ci.ppEnabledLayerNames = layers;
    r = vkCreateInstance(&ci, NULL, &s_core.inst);
    if (r != VK_SUCCESS)
        return fail("vkCreateInstance failed%s: %s", "", r);
    return 1;
}

/* A score for a device that has what this renderer needs, else -1. */
static int device_score(VkPhysicalDevice pd, VkSurfaceKHR surf, uint32_t *qfam)
{
    VkPhysicalDeviceProperties p;
    VkQueueFamilyProperties qf[16];
    uint32_t nq = 16, i;

    vkGetPhysicalDeviceProperties(pd, &p);
    if (p.apiVersion < VK_API_VERSION_1_3)
        return -1;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (i = 0; i < nq; i++) {
        VkBool32 present = VK_TRUE;
        if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;
        if (surf)
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &present);
        if (!present)
            continue;
        *qfam = i;
        return p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
             : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
    }
    return -1;
}

static int make_device(void)
{
    VkPhysicalDevice pds[8];
    uint32_t npd = 8, i, best_q = 0, navail = 0, ne = 0;
    int best = -1;
    VkExtensionProperties *avail;
    const char *exts[16];
    VkPhysicalDevicePresentIdFeaturesKHR pid, have_pid;
    VkPhysicalDevicePresentWaitFeaturesKHR pw, have_pw;
    int want_pw = 0;
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
    VkPhysicalDevicePresentId2FeaturesKHR pid2, have_pid2;
    VkPhysicalDevicePresentWait2FeaturesKHR pw2, have_pw2;
    int want_pw2 = 0;
#endif
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceFeatures2 have2;
    VkPhysicalDeviceVulkan12Features have12;
    VkPhysicalDeviceVulkan13Features have13;
    VkDeviceCreateInfo dci;
    VkFormatProperties fp;
    VkResult r;

    r = vkEnumeratePhysicalDevices(s_core.inst, &npd, pds);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !npd)
        return fail("no Vulkan device%s (%s)", "", r);
    for (i = 0; i < npd; i++) {
        uint32_t q = 0;
        int sc = device_score(pds[i], s_core.surface, &q);
        if (sc > best) {
            best = sc;
            s_core.phys = pds[i];
            best_q = q;
        }
    }
    if (best < 0)
        return fail("no Vulkan 1.3 device with a graphics queue%s%s",
                    s_core.surface ? " that presents to the window" : "", VK_SUCCESS);
    s_core.qfam = best_q;
    vkGetPhysicalDeviceProperties(s_core.phys, &s_core.props);
    vkGetPhysicalDeviceMemoryProperties(s_core.phys, &s_core.mem);
    snprintf(s_core.name, sizeof s_core.name, "%s", s_core.props.deviceName);

    vkEnumerateDeviceExtensionProperties(s_core.phys, NULL, &navail, NULL);
    avail = (VkExtensionProperties *)calloc(navail ? navail : 1, sizeof *avail);
    if (!avail)
        return fail("%s%s", "out of memory ", VK_ERROR_OUT_OF_HOST_MEMORY);
    vkEnumerateDeviceExtensionProperties(s_core.phys, NULL, &navail, avail);
    if (s_core.surface)
        exts[ne++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    /* A portability implementation's subset must be enabled when listed. */
    if (has_ext(avail, navail, "VK_KHR_portability_subset")) {
        exts[ne++] = "VK_KHR_portability_subset";
        s_core.portability = 1;
    }
    /* When each frame reached the display (vkWaitForPresentKHR): the
     * presenter's present-to-present statistics. */
    /* BO-110: the HDR mastering metadata of a swapchain (vkSetHdrMetadataEXT). */
    if (s_core.surface && has_ext(avail, navail, VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
        exts[ne++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
        s_core.hdr_metadata = 1;
    }
    /* BO-210: the window system's refresh cycle (gamescope's WSI layer has
     * it), for Smooth's guard and the vblank lock. */
    if (s_core.surface && has_ext(avail, navail, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) {
        exts[ne++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
        s_core.display_timing = 1;
    }
    want_pw = s_core.surface && has_ext(avail, navail, VK_KHR_PRESENT_ID_EXTENSION_NAME)
              && has_ext(avail, navail, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
    /* BO-207: AMD's Windows driver (RX 9060 XT, 2026) has only the second
     * pair, present_id2 + present_wait2: the same per-frame display times. */
    want_pw2 = !want_pw && s_core.surface && s_core.surface_caps2
               && has_ext(avail, navail, VK_KHR_PRESENT_ID_2_EXTENSION_NAME)
               && has_ext(avail, navail, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
#endif
    free(avail);

    memset(&have13, 0, sizeof have13);
    have13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    memset(&have12, 0, sizeof have12);
    have12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    have12.pNext = &have13;
    memset(&have_pw, 0, sizeof have_pw);
    have_pw.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    memset(&have_pid, 0, sizeof have_pid);
    have_pid.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    have_pid.pNext = &have_pw;
    have13.pNext = want_pw ? &have_pid : NULL;
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
    memset(&have_pw2, 0, sizeof have_pw2);
    have_pw2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR;
    memset(&have_pid2, 0, sizeof have_pid2);
    have_pid2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
    have_pid2.pNext = &have_pw2;
    if (want_pw2)
        have13.pNext = &have_pid2;
#endif
    memset(&have2, 0, sizeof have2);
    have2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    have2.pNext = &have12;
    vkGetPhysicalDeviceFeatures2(s_core.phys, &have2);
    if (!have13.dynamicRendering || !have13.synchronization2 || !have12.timelineSemaphore)
        return fail("%s lacks dynamic rendering, synchronization2 or timeline"
                    " semaphores%s", s_core.name, VK_SUCCESS);

    memset(&f13, 0, sizeof f13);
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    if (want_pw && have_pid.presentId && have_pw.presentWait) {
        memset(&pw, 0, sizeof pw);
        pw.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
        pw.presentWait = VK_TRUE;
        memset(&pid, 0, sizeof pid);
        pid.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
        pid.presentId = VK_TRUE;
        pid.pNext = &pw;
        f13.pNext = &pid;
        exts[ne++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
        exts[ne++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
        s_core.present_wait = 1;
    }
#if defined(_WIN32) && defined(VK_KHR_present_wait2)
    if (want_pw2 && have_pid2.presentId2 && have_pw2.presentWait2) {
        memset(&pw2, 0, sizeof pw2);
        pw2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR;
        pw2.presentWait2 = VK_TRUE;
        memset(&pid2, 0, sizeof pid2);
        pid2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
        pid2.presentId2 = VK_TRUE;
        pid2.pNext = &pw2;
        f13.pNext = &pid2;
        exts[ne++] = VK_KHR_PRESENT_ID_2_EXTENSION_NAME;
        exts[ne++] = VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME;
        s_core.present_wait = 1;
        s_core.present_wait2 = 1;
        fprintf(stderr, "[vulkan] %s: VK_KHR_present_wait2 (no VK_KHR_present_wait): the"
                " presenter times each frame on the display with it\n", s_core.name);
    }
#endif
    if (s_core.surface && !s_core.present_wait)
        fprintf(stderr, "[vulkan] no VK_KHR_present_wait on %s (extensions %s, features %d/%d):"
                " the presenter cannot time each frame on the display\n", s_core.name,
                want_pw ? "offered" : "missing", want_pw ? have_pid.presentId : 0,
                want_pw ? have_pw.presentWait : 0);
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    /* glslang turns GLSL's discard into OpDemoteToHelperInvocation when it
     * writes SPIR-V 1.6, and a module that declares that capability may
     * only be created on a device with this feature switched on
     * (VUID-VkShaderModuleCreateInfo-pCode-08740). Vulkan 1.3 obliges a
     * driver to have it; it is asked for here, not assumed, and without it
     * the shaders are written as SPIR-V 1.5, where discard is OpKill
     * (nv2a_vk_glsl.c). */
    f13.shaderDemoteToHelperInvocation = have13.shaderDemoteToHelperInvocation;
    s_core.demote = have13.shaderDemoteToHelperInvocation ? 1 : 0;
    if (!s_core.demote)
        fprintf(stderr, "[vulkan] %s has no shaderDemoteToHelperInvocation: fragment shaders"
                " as SPIR-V 1.5 (discard is OpKill)\n", s_core.name);
    memset(&f12, 0, sizeof f12);
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = have12.hostQueryReset;
    memset(&f2, 0, sizeof f2);
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    /* Exact zpass counts, as Metal's counting visibility results. */
    f2.features.occlusionQueryPrecise = have2.features.occlusionQueryPrecise;
    /* Anisotropic filtering (RECOMP_GFX_ANISO): on where the device has it,
     * for the backend's samplers to ask for. */
    f2.features.samplerAnisotropy = have2.features.samplerAnisotropy;
    s_core.max_aniso = have2.features.samplerAnisotropy
                     ? s_core.props.limits.maxSamplerAnisotropy : 0.0f;
    if (!have12.hostQueryReset)
        fprintf(stderr, "[vulkan] %s: no hostQueryReset; occlusion reports are"
                " fixed (visible)\n", s_core.name);

    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = s_core.qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ne;
    dci.ppEnabledExtensionNames = exts;
    r = vkCreateDevice(s_core.phys, &dci, NULL, &s_core.dev);
    if (r != VK_SUCCESS)
        return fail("vkCreateDevice on %s failed: %s", s_core.name, r);
#if defined(XR_VK_DYNAMIC)
    {
        /* An old loader that does not know the Vulkan 1.3 names, and a driver
         * that does not hand them to the device either. */
        const char *missing = xr_vk_missing_core13();
        if (missing) {
            vkDestroyDevice(s_core.dev, NULL);
            s_core.dev = VK_NULL_HANDLE;
            snprintf(s_err, sizeof s_err, "%s has no %s: the Vulkan loader or the driver is older"
                     " than Vulkan 1.3", s_core.name, missing);
            return 0;
        }
    }
#endif
    vkGetDeviceQueue(s_core.dev, s_core.qfam, 0, &s_core.queue);

    vkGetPhysicalDeviceFormatProperties(s_core.phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    s_core.depth_format = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                        ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
    return 1;
}

const struct nvk_core *nvk_core_init(const char *const *inst_exts, unsigned n,
                                     VkSurfaceKHR (*make_surface)(VkInstance, void *),
                                     void *ctx, char *err, size_t errn)
{
    pthread_mutex_lock(&s_init_lock);
    if (!s_state) {
        s_state = -1;
        if (make_instance(inst_exts, n)) {
            if (make_surface) {
                s_core.surface = make_surface(s_core.inst, ctx);
                if (!s_core.surface)
                    fail("the window has no Vulkan surface%s%s", "", VK_SUCCESS);
            }
            if ((!make_surface || s_core.surface) && make_device()) {
                s_state = 1;
#if defined(__ANDROID__)
                {
                    /* For the app's driver page: what this start runs on. */
                    const char *info = getenv("RECOMP_ANDROID_DRIVER_INFO");
                    FILE *f = info && *info ? fopen(info, "w") : NULL;
                    if (f) {
                        uint32_t dv = s_core.props.driverVersion;
                        const char *drv = getenv("RECOMP_ANDROID_DRIVER_NAME");
                                                fprintf(f, "driver=%s\ndevice=%s\nvulkan=%u.%u.%u\n",
                                xr_android_vk_driver(), s_core.name,
                                VK_API_VERSION_MAJOR(s_core.props.apiVersion),
                                VK_API_VERSION_MINOR(s_core.props.apiVersion),
                                VK_API_VERSION_PATCH(s_core.props.apiVersion));
                        /* Mesa packs its version as a Vulkan one; Turnip's
                         * device name says it is Mesa's. */
                        if (strstr(s_core.name, "Turnip"))
                            fprintf(f, "mesa=%u.%u.%u\n", VK_API_VERSION_MAJOR(dv),
                                    VK_API_VERSION_MINOR(dv), VK_API_VERSION_PATCH(dv));
                        (void)drv;
                        fclose(f);
                    }
                }
#endif
                if (nvk_core_is_software()) {
#if defined(__linux__) && !defined(__ANDROID__)
                    const char *hint = "check that your user can use /dev/dri (render group)";
#else
                    const char *hint = "check the graphics driver";
#endif
                    XR_NOTICE( "[vulkan] WARNING: %s is a software renderer, not a GPU: "
                            "No GPU access, running on the CPU: %s\n", s_core.name, hint);
                }
                XR_NOTICE( "[vulkan] device up: %s (Vulkan %u.%u.%u, driver 0x%X%s),"
                        " queue family %u, depth %s%s\n", s_core.name,
                        VK_API_VERSION_MAJOR(s_core.props.apiVersion),
                        VK_API_VERSION_MINOR(s_core.props.apiVersion),
                        VK_API_VERSION_PATCH(s_core.props.apiVersion),
                        s_core.props.driverVersion,
                        s_core.portability ? ", portability subset" : "", s_core.qfam,
                        s_core.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8" : "D24S8",
                        s_core.surface ? ", window surface" : ", headless");
            }
        }
    }
    if (s_state < 0 && err)
        snprintf(err, errn, "%s", s_err);
    if (s_state < 0 && make_surface) {
        /* The window could not have Vulkan: a later headless init (the
         * backend's, with the SDL renderer window) may still find a device. */
        if (s_core.dev)
            vkDestroyDevice(s_core.dev, NULL);
        if (s_core.surface)
            vkDestroySurfaceKHR(s_core.inst, s_core.surface, NULL);
        if (s_core.inst)
            vkDestroyInstance(s_core.inst, NULL);
        memset(&s_core, 0, sizeof s_core);
        s_state = 0;
        pthread_mutex_unlock(&s_init_lock);
        return NULL;
    }
    pthread_mutex_unlock(&s_init_lock);
    return s_state > 0 ? &s_core : NULL;
}

int nvk_core_replace_surface(VkSurfaceKHR (*make_surface)(VkInstance, void *), void *ctx)
{
    if (s_state != 1 || !make_surface)
        return 0;
    if (s_core.surface)
        vkDestroySurfaceKHR(s_core.inst, s_core.surface, NULL);
    s_core.surface = make_surface(s_core.inst, ctx);
    return s_core.surface != VK_NULL_HANDLE;
}

/* Whether the device is a CPU implementation (llvmpipe, lavapipe,
 * SwiftShader): what the loader offers when the user's graphics device cannot
 * be opened (on Linux: no access to /dev/dri, the render group). The game
 * runs, slowly, so this is for a warning and never a stop. */
int nvk_core_is_software(void)
{
    const struct nvk_core *c = nvk_core_get();
    static const char *const names[] = { "llvmpipe", "lavapipe", "SwiftShader", "softpipe" };
    unsigned i;

    if (!c)
        return 0;
    if (c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        return 1;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strstr(c->name, names[i]))
            return 1;
    return 0;
}

const struct nvk_core *nvk_core_get(void)
{
    return __atomic_load_n(&s_state, __ATOMIC_ACQUIRE) > 0 ? &s_core : NULL;
}

/* One line for a settings page: the device and its Vulkan version, "" before
 * the device is up. */
const char *nvk_core_describe(void)
{
    static char line[320];
    if (__atomic_load_n(&s_state, __ATOMIC_ACQUIRE) <= 0)
        return "";
    if (!line[0])
        snprintf(line, sizeof line, "%s, Vulkan %u.%u.%u", s_core.name,
                 VK_API_VERSION_MAJOR(s_core.props.apiVersion),
                 VK_API_VERSION_MINOR(s_core.props.apiVersion),
                 VK_API_VERSION_PATCH(s_core.props.apiVersion));
    return line;
}

static void (*s_lost_fn)(const char *why);
static int s_lost;

void nvk_set_lost_handler(void (*fn)(const char *why))
{
    __atomic_store_n(&s_lost_fn, fn, __ATOMIC_RELEASE);
}

#define SUBMIT_KEEP 64
static struct { uint64_t n, ns; char what[160]; } s_sub[SUBMIT_KEEP];
static uint64_t s_sub_n;

uint64_t nvk_submit_note(const char *what)
{
    struct timespec ts;
    uint64_t n = ++s_sub_n;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_sub[n % SUBMIT_KEEP].n = n;
    s_sub[n % SUBMIT_KEEP].ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    snprintf(s_sub[n % SUBMIT_KEEP].what, sizeof s_sub[0].what, "%s", what ? what : "");
    return n;
}

/* The kept submits, oldest first, times relative to the last. Under the
 * queue lock when it can be had at once (a submit that failed is reported
 * from under it). */
static void submit_dump(void)
{
    int locked = pthread_mutex_trylock(&s_queue_lock) == 0;
    uint64_t last = s_sub_n, i, first = last > SUBMIT_KEEP ? last - SUBMIT_KEEP + 1 : 1;
    uint64_t t_last = last ? s_sub[last % SUBMIT_KEEP].ns : 0;
    fprintf(stderr, "[vulkan] the last %llu of %llu submits (the kernel's 'gpu fault ... ts T'"
            " is submit #T):\n", (unsigned long long)(last - first + 1),
            (unsigned long long)last);
    for (i = first; i <= last; i++)
        fprintf(stderr, "[vulkan]   #%llu %+.3f ms %s\n", (unsigned long long)s_sub[i % SUBMIT_KEEP].n,
                -(double)(t_last - s_sub[i % SUBMIT_KEEP].ns) / 1e6, s_sub[i % SUBMIT_KEEP].what);
    if (locked)
        pthread_mutex_unlock(&s_queue_lock);
}

void nvk_device_lost(const char *why)
{
    void (*fn)(const char *) = __atomic_load_n(&s_lost_fn, __ATOMIC_ACQUIRE);
    if (__atomic_exchange_n(&s_lost, 1, __ATOMIC_ACQ_REL))
        return;
    fprintf(stderr, "[vulkan] the graphics device is lost (%s): no picture can be drawn in this"
            " run any more\n", why ? why : "?");
    submit_dump();
    fflush(stderr);
    if (fn) {
        fn(why);
    } else {
        fprintf(stderr, "[vulkan] no window to say it in: ending the run (status 70)\n");
        exit(70);
    }
}

static double shut_ms(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) * 1e3 + (double)(t.tv_nsec - t0->tv_nsec) / 1e6;
}

void nvk_core_shutdown(void)
{
    struct timespec t0;
    VkResult r;

    /* A GPU wait still going on another thread gives up at its next slice
     * (the process ends with _exit on Android: the atexit hook never runs). */
    nvk_wait_cancel();
    pthread_mutex_lock(&s_init_lock);
    if (s_state != 1) {
        pthread_mutex_unlock(&s_init_lock);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* Taken for good: a submit or present that still comes waits here
     * rather than reach a destroyed device. The process ends next. */
    pthread_mutex_lock(&s_queue_lock);
    r = vkDeviceWaitIdle(s_core.dev);
    XR_NOTICE("[vulkan] teardown: vkDeviceWaitIdle %s (%.1f ms)\n", nvk_result_str(r),
            shut_ms(&t0));
    if (s_core.surface) {
        vkDestroySurfaceKHR(s_core.inst, s_core.surface, NULL);
        XR_NOTICE("[vulkan] teardown: surface destroyed (%.1f ms)\n", shut_ms(&t0));
    }
    vkDestroyDevice(s_core.dev, NULL);
    XR_NOTICE("[vulkan] teardown: device destroyed (%.1f ms)\n", shut_ms(&t0));
    vkDestroyInstance(s_core.inst, NULL);
    XR_NOTICE("[vulkan] teardown: instance destroyed (%.1f ms)\n", shut_ms(&t0));
    fflush(stderr);
    s_core.surface = VK_NULL_HANDLE;
    s_core.dev = VK_NULL_HANDLE;
    s_core.queue = VK_NULL_HANDLE;
    s_core.inst = VK_NULL_HANDLE;
    s_state = -1;
    pthread_mutex_unlock(&s_init_lock);
}

void nvk_queue_lock(void)   { pthread_mutex_lock(&s_queue_lock); }
void nvk_queue_unlock(void) { pthread_mutex_unlock(&s_queue_lock); }

int nvk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer)
{
    uint32_t i;
    int fallback = -1;
    for (i = 0; i < s_core.mem.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = s_core.mem.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & want) != want)
            continue;
        if ((f & prefer) == prefer)
            return (int)i;
        if (fallback < 0)
            fallback = (int)i;
    }
    return fallback;
}

int nvk_buffer_new(struct nvk_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer)
{
    VkBufferCreateInfo bi;
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai;
    int mt;

    memset(b, 0, sizeof *b);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s_core.dev, &bi, NULL, &b->buf) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(s_core.dev, b->buf, &mr);
    mt = nvk_mem_type(mr.memoryTypeBits, want, prefer);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (mt < 0 || vkAllocateMemory(s_core.dev, &ai, NULL, &b->mem) != VK_SUCCESS) {
        vkDestroyBuffer(s_core.dev, b->buf, NULL);
        b->buf = VK_NULL_HANDLE;
        return 0;
    }
    vkBindBufferMemory(s_core.dev, b->buf, b->mem, 0);
    b->size = size;
    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(s_core.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map);
    return 1;
}

void nvk_buffer_free(struct nvk_buffer *b)
{
    if (b->buf)
        vkDestroyBuffer(s_core.dev, b->buf, NULL);
    if (b->mem)
        vkFreeMemory(s_core.dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

static uint64_t s_img_bytes;   /* BO-240: nvk_image_bytes (render thread) */

static int image_new(struct nvk_image *im, VkFormat fmt, uint32_t w, uint32_t h,
                     uint32_t levels, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                     int cube)
{
    VkImageCreateInfo ii;
    VkImageViewCreateInfo vi;
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai;
    int mt;

    memset(im, 0, sizeof *im);
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = levels ? levels : 1;
    ii.arrayLayers = cube ? 6 : 1;
    ii.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_core.dev, &ii, NULL, &im->img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(s_core.dev, im->img, &mr);
    mt = nvk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mt < 0)
        mt = nvk_mem_type(mr.memoryTypeBits, 0, 0);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (mt < 0 || vkAllocateMemory(s_core.dev, &ai, NULL, &im->mem) != VK_SUCCESS) {
        vkDestroyImage(s_core.dev, im->img, NULL);
        im->img = VK_NULL_HANDLE;
        return 0;
    }
    vkBindImageMemory(s_core.dev, im->img, im->mem, 0);
    im->bytes = mr.size;
    s_img_bytes += mr.size;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = im->img;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = ii.mipLevels;
    vi.subresourceRange.layerCount = ii.arrayLayers;
    if (vkCreateImageView(s_core.dev, &vi, NULL, &im->view) != VK_SUCCESS) {
        nvk_image_free(im);
        return 0;
    }
    im->format = fmt;
    im->aspect = aspect;
    im->w = w;
    im->h = h;
    im->levels = ii.mipLevels;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

int nvk_image_new(struct nvk_image *im, VkFormat fmt, uint32_t w, uint32_t h,
                  uint32_t levels, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return image_new(im, fmt, w, h, levels, usage, aspect, 0);
}

int nvk_image_new_cube(struct nvk_image *im, VkFormat fmt, uint32_t side, uint32_t levels,
                       VkImageUsageFlags usage)
{
    return image_new(im, fmt, side, side, levels, usage, VK_IMAGE_ASPECT_COLOR_BIT, 1);
}

uint64_t nvk_image_bytes(void)
{
    return s_img_bytes;
}

void nvk_image_free(struct nvk_image *im)
{
    if (im->mem)
        s_img_bytes -= im->bytes;
    if (im->view)
        vkDestroyImageView(s_core.dev, im->view, NULL);
    if (im->img)
        vkDestroyImage(s_core.dev, im->img, NULL);
    if (im->mem)
        vkFreeMemory(s_core.dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static void layout_scope(VkImageLayout l, VkPipelineStageFlags2 *st, VkAccessFlags2 *ac)
{
    switch (l) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        *st = VK_PIPELINE_STAGE_2_NONE;
        *ac = 0;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *st = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        *ac = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *st = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        *ac = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
            | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *st = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        *ac = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *st = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        *ac = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *st = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        *ac = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        *st = VK_PIPELINE_STAGE_2_NONE;
        *ac = 0;
        break;
    default:   /* GENERAL: anything */
        *st = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        *ac = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        break;
    }
}

void nvk_image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                       VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier2 b;
    VkDependencyInfo di;

    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    layout_scope(from, &b.srcStageMask, &b.srcAccessMask);
    layout_scope(to, &b.dstStageMask, &b.dstAccessMask);
    if (to == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        b.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        b.dstAccessMask = 0;
    }
    if (from == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        /* Waited on at the acquire's semaphore, at the transfer stage. */
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = 0;
    }
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect ? aspect : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

/* A barrier even when the layout stays: before a transfer write or GENERAL
 * use, and before an attachment is drawn into again in a new pass. Two
 * render passes on the same colour or depth image are not ordered by
 * themselves; Turnip's sysmem mode (TU_DEBUG=sysmem) then lets the next
 * pass's load or clear race the last one's stores (the cube-map faces share
 * one depth buffer: car reflections that come and go on Adreno). */
void nvk_image_to(VkCommandBuffer cb, struct nvk_image *im, VkImageLayout to)
{
    if (im->layout == to && to != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            && to != VK_IMAGE_LAYOUT_GENERAL
            && to != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
            && to != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        return;
    nvk_image_barrier(cb, im->img, im->aspect, im->layout, to);
    im->layout = to;
}

/* ---- bounded GPU waits (nv2a_vk_core.h) ------------------------------------ */

#define WAIT_SLICE_NS 500000000ull       /* one vkWait* call; then the clock is asked */

static const char *const s_wait_site[NVK_WAIT_SITES] = {
    "serial", "frame", "upload", "present", "shot", "slot"
};
enum { FAULT_NONE, FAULT_RETRY, FAULT_TIMEOUT, FAULT_LOST };
static const char *const s_fault_name[] = { "none", "retry", "timeout", "lost" };

static pthread_once_t s_wait_once = PTHREAD_ONCE_INIT;
static uint64_t s_wait_limit_ns = 10000000000ull;
static int      s_wait_cancel;
static int      s_fault_site = -1, s_fault_kind;
static uint64_t s_fault_after = 300;
static uint64_t s_fault_n[NVK_WAIT_SITES];   /* waits made at each site */

static uint64_t wait_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void nvk_wait_cancel(void)
{
    __atomic_store_n(&s_wait_cancel, 1, __ATOMIC_RELEASE);
}

int nvk_core_lost(void)
{
    return __atomic_load_n(&s_lost, __ATOMIC_ACQUIRE);
}

static void wait_config(void)
{
    const char *e = recomp_env(RENV_VK_WAIT_LIMIT);
    if (e && *e && atol(e) > 0)
        s_wait_limit_ns = (uint64_t)atol(e) * 1000000ull;
    e = recomp_env(RENV_VK_WAIT_FAULT);
    if (e && *e) {
        const char *c = strchr(e, ':');
        int i;
        for (i = 0; c && i < NVK_WAIT_SITES; i++)
            if ((size_t)(c - e) == strlen(s_wait_site[i]) && !strncmp(e, s_wait_site[i], (size_t)(c - e)))
                s_fault_site = i;
        if (c && !strncmp(c + 1, "retry", 5))
            s_fault_kind = FAULT_RETRY;
        else if (c && !strncmp(c + 1, "timeout", 7))
            s_fault_kind = FAULT_TIMEOUT;
        else if (c && !strncmp(c + 1, "lost", 4))
            s_fault_kind = FAULT_LOST;
        c = c ? strchr(c + 1, ':') : NULL;
        if (c)
            s_fault_after = (uint64_t)atoll(c + 1);
        if (s_fault_site < 0 || !s_fault_kind) {
            s_fault_site = -1;
            fprintf(stderr, "[vulkan] vk_wait_fault=%s: not understood (site:kind[:n]; sites serial,"
                    " frame, upload, present, shot, slot; kinds retry, timeout, lost)\n", e);
        } else {
            fprintf(stderr, "[vulkan] vk_wait_fault (test hook): %s wait #%llu at site %s\n",
                    s_fault_name[s_fault_kind], (unsigned long long)s_fault_after + 1,
                    s_wait_site[s_fault_site]);
        }
    }
    /* The way out cancels a wait still going on another thread. */
    atexit(nvk_wait_cancel);
}

/* The fault for this wait at `site`, once (a fatal one ends the run). */
static int fault_take(int site)
{
    uint64_t n;
    if (s_fault_site != site)
        return FAULT_NONE;
    n = __atomic_fetch_add(&s_fault_n[site], 1, __ATOMIC_RELAXED);
    if (n != s_fault_after)
        return FAULT_NONE;
    fprintf(stderr, "[vulkan] vk_wait_fault: %s wait #%llu made to fail: %s\n", s_wait_site[site],
            (unsigned long long)n + 1, s_fault_name[s_fault_kind]);
    return s_fault_kind;
}

static void fault_sleep(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    nanosleep(&ts, NULL);
}

typedef VkResult (*wait_fn)(const void *ctx, uint64_t timeout_ns);

static int wait_bounded(int site, const char *what, wait_fn fn, const void *ctx, VkResult *res)
{
    static int said_retry, said_fail;
    uint64_t t0, el = 0;
    unsigned timeouts = 0;
    int fault, out;
    VkResult r;

    pthread_once(&s_wait_once, wait_config);
    if (site < 0 || site >= NVK_WAIT_SITES)
        site = NVK_WAIT_SERIAL;
    fault = fault_take(site);
    t0 = wait_now();
    for (;;) {
        if (fault == FAULT_LOST) {
            r = VK_ERROR_DEVICE_LOST;
        } else if (fault == FAULT_TIMEOUT || (fault == FAULT_RETRY && timeouts < 2)) {
            fault_sleep(WAIT_SLICE_NS);          /* as a slice that times out */
            r = VK_TIMEOUT;
        } else {
            r = fn(ctx, WAIT_SLICE_NS);
        }
        if (r != VK_TIMEOUT)
            break;
        timeouts++;
        el = wait_now() - t0;
        if (__atomic_load_n(&s_wait_cancel, __ATOMIC_ACQUIRE) || nvk_core_lost()) {
            fprintf(stderr, "[vulkan] GPU wait (%s, %s): cancelled after %.1f ms (%s)\n",
                    s_wait_site[site], what, (double)el / 1e6,
                    nvk_core_lost() ? "the device is lost" : "the run is ending");
            if (res)
                *res = r;
            return NVK_WAIT_CANCELLED;
        }
        if (el >= s_wait_limit_ns) {
            fprintf(stderr, "[vulkan] GPU wait (%s, %s): timed out for good, not done after %.1f ms"
                    " (%u timeouts, limit %.1f s)\n", s_wait_site[site], what, (double)el / 1e6,
                    timeouts, (double)s_wait_limit_ns / 1e9);
            if (res)
                *res = r;
            return NVK_WAIT_TIMEOUT;
        }
        if (timeouts == 1 && said_retry < 16)
            fprintf(stderr, "[vulkan] GPU wait (%s, %s): not done after %.1f ms, waiting on"
                    " (limit %.1f s)\n", s_wait_site[site], what, (double)el / 1e6,
                    (double)s_wait_limit_ns / 1e9);
    }
    if (res)
        *res = r;
    if (r == VK_SUCCESS) {
        if (timeouts && said_retry++ < 16)
            fprintf(stderr, "[vulkan] GPU wait (%s, %s): done after %u timeouts, %.1f ms\n",
                    s_wait_site[site], what, timeouts, (double)(wait_now() - t0) / 1e6);
        return NVK_WAIT_OK;
    }
    out = r == VK_ERROR_DEVICE_LOST ? NVK_WAIT_LOST : NVK_WAIT_ERROR;
    if (said_fail++ < 16)
        fprintf(stderr, "[vulkan] GPU wait (%s, %s): %s%s after %.1f ms\n", s_wait_site[site], what,
                out == NVK_WAIT_LOST ? "device lost, " : "failed, ", nvk_result_str(r),
                (double)(wait_now() - t0) / 1e6);
    return out;
}

static VkResult fence_fn(const void *ctx, uint64_t timeout_ns)
{
    return vkWaitForFences(s_core.dev, 1, (const VkFence *)ctx, VK_TRUE, timeout_ns);
}

int nvk_wait_fence(int site, const char *what, VkFence fence, VkResult *res)
{
    return wait_bounded(site, what, fence_fn, &fence, res);
}

struct timeline_ctx { VkSemaphore sem; uint64_t value; };

static VkResult timeline_fn(const void *ctx, uint64_t timeout_ns)
{
    const struct timeline_ctx *t = (const struct timeline_ctx *)ctx;
    VkSemaphoreWaitInfo wi;
    memset(&wi, 0, sizeof wi);
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &t->sem;
    wi.pValues = &t->value;
    return vkWaitSemaphores(s_core.dev, &wi, timeout_ns);
}

int nvk_wait_timeline(int site, const char *what, VkSemaphore sem, uint64_t value, VkResult *res)
{
    struct timeline_ctx t;
    t.sem = sem;
    t.value = value;
    return wait_bounded(site, what, timeline_fn, &t, res);
}

int nvk_wait_queue(int site, const char *what, VkFence fence, VkResult *res)
{
    VkResult r;
    pthread_mutex_lock(&s_queue_lock);
    r = vkResetFences(s_core.dev, 1, &fence);
    if (r == VK_SUCCESS)
        r = vkQueueSubmit(s_core.queue, 0, NULL, fence);
    pthread_mutex_unlock(&s_queue_lock);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] GPU wait (%s, %s): the queue's fence submit failed: %s\n",
                s_wait_site[site >= 0 && site < NVK_WAIT_SITES ? site : 0], what,
                nvk_result_str(r));
        if (res)
            *res = r;
        return r == VK_ERROR_DEVICE_LOST ? NVK_WAIT_LOST : NVK_WAIT_ERROR;
    }
    return nvk_wait_fence(site, what, fence, res);
}

int nvk_wait_fatal(int outcome, VkResult res, const char *what)
{
    char why[160];
    if (outcome == NVK_WAIT_OK || outcome == NVK_WAIT_CANCELLED)
        return 0;
    if (outcome == NVK_WAIT_TIMEOUT)
        snprintf(why, sizeof why, "%s: the GPU did not finish within %.0f s", what,
                 (double)s_wait_limit_ns / 1e9);
    else
        snprintf(why, sizeof why, "%s: %s", what, nvk_result_str(res));
    nvk_device_lost(why);
    return 1;
}
