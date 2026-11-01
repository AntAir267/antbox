/* The run-time Vulkan loader (src/d3d/vk_dyn.c), BO-231. One mode per process
 * (the loader is opened once): see CMakeLists.txt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_dyn.h"

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

/* No loader: nothing crashes, the reason is there for the message. */
static void mode_missing(void)
{
    uint32_t ver = 77, n = 0;
    VkInstance inst = VK_NULL_HANDLE;
    VkInstanceCreateInfo ci;

#ifdef _WIN32
    _putenv_s("RECOMP_VULKAN_LOADER", "no-such-vulkan-loader.dll");
#else
    setenv("RECOMP_VULKAN_LOADER", "/no/such/libvulkan.so.1", 1);
#endif
    CHECK(!xr_vk_loader_ok(), "a loader that does not exist opened");
    CHECK(strstr(xr_vk_loader_error(), "no-such") || strstr(xr_vk_loader_error(), "/no/such"),
          "the reason does not name the file: '%s'", xr_vk_loader_error());
    CHECK(vkEnumerateInstanceVersion(&ver) != VK_SUCCESS, "version of no loader");
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS && n == 0,
          "extensions of no loader");
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    CHECK(vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS && !inst, "an instance of no loader");
    CHECK(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") == NULL, "gipa of no loader");
    printf("missing: '%s'\n", xr_vk_loader_error());
}

/* The fake loader: which 1.3 names the runtime ends up with. */
static void mode_fake(const char *mode)
{
    uint32_t ver = 0;
    VkInstance inst = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkInstanceCreateInfo ci;
    VkDeviceCreateInfo dci;
    const char *missing;

    CHECK(xr_vk_loader_ok(), "the fake loader did not open: %s", xr_vk_loader_error());
    CHECK(vkEnumerateInstanceVersion(&ver) == VK_SUCCESS, "version");
    CHECK(!strcmp(mode, "old11") ? ver == VK_API_VERSION_1_1 : ver == VK_API_VERSION_1_3,
          "loader version %u.%u", VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver));
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    CHECK(vkCreateInstance(&ci, NULL, &inst) == VK_SUCCESS && inst, "instance");
    /* The instance's lookup gave the common names, not the 1.3 ones. */
    CHECK(xr_vkQueueSubmit != NULL, "a 1.0 name is missing");
    CHECK(xr_vkCmdBeginRendering == NULL, "an instance lookup produced vkCmdBeginRendering");
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    CHECK(vkCreateDevice(VK_NULL_HANDLE, &dci, NULL, &dev) == VK_SUCCESS && dev, "device");
    missing = xr_vk_missing_core13();
    if (!strcmp(mode, "full")) {
        CHECK(!missing, "full driver: '%s' missing", missing ? missing : "");
        CHECK(xr_vkCmdBeginRendering && xr_vkCmdBeginRenderingKHR && xr_vkCmdEndRendering
              && xr_vkCmdPipelineBarrier2, "device lookups left a hole");
    } else if (!strcmp(mode, "khr")) {
        /* dynamic rendering by its extension names: the two names are one */
        CHECK(xr_vkCmdBeginRendering && xr_vkCmdBeginRendering == xr_vkCmdBeginRenderingKHR
              && xr_vkCmdEndRendering == xr_vkCmdEndRenderingKHR, "KHR names not used");
        CHECK(missing && !strcmp(missing, "vkCmdPipelineBarrier2"), "missing: '%s'",
              missing ? missing : "(none)");
    } else {
        CHECK(missing && !strcmp(missing, "vkCmdBeginRendering"), "missing: '%s'",
              missing ? missing : "(none)");
    }
    printf("%s: first missing 1.3 command: %s\n", mode, missing ? missing : "(none)");
}

/* The machine's own loader, when it has one. */
static int mode_system(void)
{
    uint32_t ver = 0;

    if (!xr_vk_loader_ok()) {
        printf("skip: no Vulkan loader here (%s)\n", xr_vk_loader_error());
        return 77;
    }
    CHECK(vkEnumerateInstanceVersion(&ver) == VK_SUCCESS && ver >= VK_API_VERSION_1_0,
          "version %u", ver);
    CHECK(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") != NULL, "gipa");
    printf("system loader %u.%u.%u\n", VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver),
           VK_API_VERSION_PATCH(ver));
    return 0;
}

int main(int argc, char **argv)
{
    const char *m = argc > 1 ? argv[1] : "system";

    if (!strcmp(m, "missing"))
        mode_missing();
    else if (!strcmp(m, "system")) {
        int r = mode_system();
        if (r)
            return r;
    } else
        mode_fake(m);
    if (bad)
        printf("vk_dyn %s: %d FAILED\n", m, bad);
    else
        printf("vk_dyn %s: ok\n", m);
    return bad ? 1 : 0;
}
