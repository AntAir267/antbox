/*
 * hostmem_probe: can this GPU read guest memory where it lies? The guest's
 * RAM is a memfd mapped several times (xbox_memory_layout.c); the native
 * renderer would import a mapping of it as a Vulkan buffer
 * (VK_EXT_external_memory_host) and bind vertex data at its offset instead of
 * copying it. This tries exactly that: a 64 MB memfd mapping, imported, a
 * vertex/index/storage buffer bound to it, and a GPU copy out of it checked.
 *   cc -O1 -o hostmem_probe hostmem_probe.c -lvulkan && ./hostmem_probe
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    printf("FAIL %s: %d\n", #x, r_); return 1; } } while (0)

int main(void)
{
    VkInstance inst;
    VkPhysicalDevice pd[8];
    uint32_t n = 8, i, q = 0;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    CHECK(vkEnumeratePhysicalDevices(inst, &n, pd));
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp };
        VkDevice dev;
        VkQueue queue;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        const char *ext[] = { "VK_EXT_external_memory_host" };
        VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        size_t size = 64u << 20;
        int fd;
        uint8_t *map;
        PFN_vkGetMemoryHostPointerPropertiesEXT getp;
        VkMemoryHostPointerPropertiesEXT mp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
        VkExternalMemoryBufferCreateInfo eb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb };
        VkBuffer buf;
        VkMemoryRequirements mr;
        VkImportMemoryHostPointerInfoEXT imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp };
        VkDeviceMemory mem;
        uint32_t t;

        vkGetPhysicalDeviceProperties(pd[i], &p);
        vkGetPhysicalDeviceProperties2(pd[i], &p2);
        printf("%s: minImportedHostPointerAlignment %llu\n", p.deviceName,
               (unsigned long long)hp.minImportedHostPointerAlignment);
        qi.queueFamilyIndex = 0;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = ext;
        if (vkCreateDevice(pd[i], &di, NULL, &dev) != VK_SUCCESS) {
            printf("  no VK_EXT_external_memory_host device\n");
            continue;
        }
        vkGetDeviceQueue(dev, 0, 0, &queue);
        fd = memfd_create("guest", 0);
        if (ftruncate(fd, (off_t)size)) return 1;
        map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        memset(map, 0x5A, size);
        getp = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev,
                    "vkGetMemoryHostPointerPropertiesEXT");
        printf("  memfd mapping: %s\n",
               getp(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, map, &mp)
               == VK_SUCCESS ? "properties ok" : "properties FAILED");
        eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                 | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        CHECK(vkCreateBuffer(dev, &bi, NULL, &buf));
        vkGetBufferMemoryRequirements(dev, buf, &mr);
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        imp.pHostPointer = map;
        ai.allocationSize = size;
        for (t = 0; t < 32; t++)
            if ((mp.memoryTypeBits & mr.memoryTypeBits) & (1u << t))
                break;
        ai.memoryTypeIndex = t;
        printf("  memory types: import %#x, buffer %#x -> %u\n", mp.memoryTypeBits,
               mr.memoryTypeBits, t);
        if (t == 32 || vkAllocateMemory(dev, &ai, NULL, &mem) != VK_SUCCESS) {
            printf("  import FAILED\n");
            continue;
        }
        CHECK(vkBindBufferMemory(dev, buf, mem, 0));
        printf("  import ok: a vertex/index/storage buffer over the memfd mapping\n");
        /* The GPU sees a write made through another mapping of the same memfd. */
        {
            uint8_t *alias = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            VkBufferCreateInfo ob = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            VkBuffer out;
            VkDeviceMemory om;
            VkMemoryRequirements orq;
            VkPhysicalDeviceMemoryProperties pm;
            VkMemoryAllocateInfo oa = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            VkCommandPool pool;
            VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            VkCommandBuffer cb;
            VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            VkBufferCopy cp = { 12345 * 16, 0, 16 };
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            uint8_t *o;
            alias[12345 * 16] = 0xA7;
            ob.size = 16;
            ob.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            CHECK(vkCreateBuffer(dev, &ob, NULL, &out));
            vkGetBufferMemoryRequirements(dev, out, &orq);
            vkGetPhysicalDeviceMemoryProperties(pd[i], &pm);
            for (t = 0; t < pm.memoryTypeCount; t++)
                if ((orq.memoryTypeBits & (1u << t)) && (pm.memoryTypes[t].propertyFlags
                        & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                    break;
            oa.allocationSize = orq.size;
            oa.memoryTypeIndex = t;
            CHECK(vkAllocateMemory(dev, &oa, NULL, &om));
            CHECK(vkBindBufferMemory(dev, out, om, 0));
            pci.queueFamilyIndex = 0;
            CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
            cai.commandPool = pool;
            cai.commandBufferCount = 1;
            CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
            CHECK(vkBeginCommandBuffer(cb, &cbi));
            vkCmdCopyBuffer(cb, buf, out, 1, &cp);
            CHECK(vkEndCommandBuffer(cb));
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
            CHECK(vkQueueWaitIdle(queue));
            CHECK(vkMapMemory(dev, om, 0, 16, 0, (void **)&o));
            printf("  GPU read through the import: %#x (want 0xa7): %s\n", o[0],
                   o[0] == 0xA7 ? "PASS" : "FAIL");
        }
    }
    return 0;
}
