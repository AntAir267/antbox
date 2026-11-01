/* A stand-in Vulkan loader for tests/vk_dyn: FAKE_VK_MODE decides which
 * Vulkan 1.3 device commands its device hands out.
 *   full   everything (a Vulkan 1.3 driver)
 *   khr    the dynamic-rendering extension's names, not the core ones
 *   old11  none of the 1.3 names (a pre-1.3 loader or driver)
 * The commands themselves do nothing: the test only looks at which pointers
 * the runtime ends up with. */
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#ifndef FAKE_EXPORT
#define FAKE_EXPORT
#endif

static int mode_is(const char *m)
{
    const char *e = getenv("FAKE_VK_MODE");
    return e && !strcmp(e, m);
}

static void VKAPI_CALL dummy_cmd(void) {}

static VkResult VKAPI_CALL fake_enum_version(uint32_t *v)
{
    *v = mode_is("old11") ? VK_API_VERSION_1_1 : VK_API_VERSION_1_3;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_instance(const VkInstanceCreateInfo *ci,
                                                const VkAllocationCallbacks *a, VkInstance *out)
{
    (void)ci; (void)a;
    *out = (VkInstance)(uintptr_t)0x1000;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_device(VkPhysicalDevice p, const VkDeviceCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkDevice *out)
{
    (void)p; (void)ci; (void)a;
    *out = (VkDevice)(uintptr_t)0x2000;
    return VK_SUCCESS;
}

static int is_core13(const char *n)
{
    static const char *const names[] = {
        "vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdPipelineBarrier2",
        "vkCmdSetCullMode", "vkCmdSetFrontFace", "vkCmdSetDepthTestEnable",
        "vkCmdSetDepthWriteEnable", "vkCmdSetDepthCompareOp", "vkCmdSetStencilTestEnable",
        "vkCmdSetStencilOp", "vkCmdSetDepthBiasEnable",
    };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(n, names[i]))
            return 1;
    return 0;
}

/* A device-level command by name. */
static PFN_vkVoidFunction VKAPI_CALL fake_gdpa(VkDevice d, const char *n)
{
    (void)d;
    if (is_core13(n))
        return mode_is("full") ? (PFN_vkVoidFunction)dummy_cmd : NULL;
    if (mode_is("khr") && (!strcmp(n, "vkCmdBeginRenderingKHR") || !strcmp(n, "vkCmdEndRenderingKHR")))
        return (PFN_vkVoidFunction)dummy_cmd;
    if (!strcmp(n, "vkCmdBeginRenderingKHR") || !strcmp(n, "vkCmdEndRenderingKHR"))
        return NULL;
    return (PFN_vkVoidFunction)dummy_cmd;
}

FAKE_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i,
                                                                           const char *n)
{
    if (!i) {
        if (!strcmp(n, "vkEnumerateInstanceVersion")) return (PFN_vkVoidFunction)fake_enum_version;
        if (!strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction)fake_create_instance;
        return (PFN_vkVoidFunction)dummy_cmd;
    }
    if (!strcmp(n, "vkCreateDevice")) return (PFN_vkVoidFunction)fake_create_device;
    if (!strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)fake_gdpa;
    /* What an instance-level lookup of a device command gives: the loader's
     * trampoline for the names it knows, nothing for what it does not (every
     * Vulkan 1.3 name, in an old one). */
    if (is_core13(n) || !strcmp(n, "vkCmdBeginRenderingKHR") || !strcmp(n, "vkCmdEndRenderingKHR"))
        return NULL;
    return (PFN_vkVoidFunction)dummy_cmd;
}
