/* texup_vk_bench: one graphics.texture_upscale compute pass (BO-245) on
 * Vulkan, timed with timestamp queries, outside the game: the Vulkan
 * counterpart of texup_gpu_check's timing for the NNEDI3 pass.
 *
 *   texup_vk_bench <pass.spv> <group> <in.bgra> <w> <h> <out prefix> [reps] [wrap]
 *                  [recombine.spv]
 *
 * The pass gets the texture's BT.709 luma (RGBA16F, binding 0, as luma.comp
 * makes it) and writes 2w x 2h RGBA16F (binding 4), the push constants of
 * tools/shaders/upscale_wrap.py (origin 0, size w x h, wrap, mode 0); one
 * dispatch of ceil(w/group) x ceil(h/group) groups of group x group. Prints
 * the median and fastest GPU ms of <reps> dispatches (default 50) and ms per
 * 64K input texels; writes <prefix>_nnedi3_luma.f32 and _model.f32 (RGBA
 * float) for check_nnedi3.py. With recombine.spv: then recombine mode 1
 * (16 x 16 groups over 2w x 2h, the title's texture sampled at binding 0,
 * the 2x luma at 1), timed the same way, its result written as
 * <prefix>_nnedi3_x2.bgra (check_nnedi3.py --final). Loads libvulkan at run time; needs only the
 * Vulkan headers to build:
 *   cc -O2 -I<vulkan include> vk_bench.c -o texup_vk_bench -ldl -lm
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FN(n) static PFN_##n n;
#define FNS(X) X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkCreateImage) X(vkCreateImageView) \
    X(vkGetBufferMemoryRequirements) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkBindBufferMemory) X(vkBindImageMemory) X(vkMapMemory) X(vkCreateShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateComputePipelines) \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdWriteTimestamp) \
    X(vkCmdResetQueryPool) X(vkCreateQueryPool) X(vkGetQueryPoolResults) X(vkQueueSubmit) \
    X(vkQueueWaitIdle) X(vkResetCommandBuffer) X(vkCreateSampler)
FNS(FN)
static PFN_vkGetInstanceProcAddr gipa;

static VkDevice s_dev;
static VkPhysicalDevice s_pd;
static VkQueue s_q;
static VkCommandBuffer s_cb;

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "%s: %d\n", #x, (int)r_); exit(1); } } while (0)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(s_pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

static void *buffer(VkDeviceSize n, VkBuffer *b)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDeviceMemory m;
    void *p;
    bi.size = n;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CK(vkCreateBuffer(s_dev, &bi, NULL, b));
    vkGetBufferMemoryRequirements(s_dev, *b, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                  | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CK(vkAllocateMemory(s_dev, &ai, NULL, &m));
    CK(vkBindBufferMemory(s_dev, *b, m, 0));
    CK(vkMapMemory(s_dev, m, 0, n, 0, &p));
    return p;
}

static VkImageView image_fmt(uint32_t w, uint32_t h, VkImage *img, VkFormat fmt,
                             VkImageUsageFlags usage)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDeviceMemory m;
    VkImageView v;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CK(vkCreateImage(s_dev, &ii, NULL, img));
    vkGetImageMemoryRequirements(s_dev, *img, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CK(vkAllocateMemory(s_dev, &ai, NULL, &m));
    CK(vkBindImageMemory(s_dev, *img, m, 0));
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    CK(vkCreateImageView(s_dev, &vi, NULL, &v));
    return v;
}

static VkImageView image(uint32_t w, uint32_t h, VkImage *img)
{
    return image_fmt(w, h, img, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
}

static VkPipeline pipeline(const char *spv_path, VkPipelineLayout pl)
{
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkShaderModule sm;
    VkPipeline pipe;
    FILE *f = fopen(spv_path, "rb");
    long n;
    uint32_t *spv;
    if (!f) {
        fprintf(stderr, "cannot read %s\n", spv_path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    spv = (uint32_t *)malloc((size_t)n);
    if (fread(spv, 1, (size_t)n, f) != (size_t)n)
        exit(1);
    fclose(f);
    smi.codeSize = (size_t)n;
    smi.pCode = spv;
    CK(vkCreateShaderModule(s_dev, &smi, NULL, &sm));
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm;
    cpi.stage.pName = "main";
    cpi.layout = pl;
    CK(vkCreateComputePipelines(s_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    return pipe;
}

static void to_general(VkImage img)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                    | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static void all_barrier(void)
{
    VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &b, 0, NULL, 0, NULL);
}

static void begin(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CK(vkResetCommandBuffer(s_cb, 0));
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CK(vkBeginCommandBuffer(s_cb, &bi));
}

static void submit(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    CK(vkEndCommandBuffer(s_cb));
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    CK(vkQueueSubmit(s_q, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(s_q));
}

static uint16_t f2h(float f)
{
    uint32_t x;
    int e;
    memcpy(&x, &f, 4);
    e = (int)((x >> 23) & 0xFF) - 127 + 15;
    if (e <= 0)
        return (uint16_t)((x >> 16) & 0x8000);
    if (e >= 31)
        return (uint16_t)(((x >> 16) & 0x8000) | 0x7C00);
    /* round to nearest even */
    {
        uint32_t m = x & 0x7FFFFF, h = ((x >> 16) & 0x8000) | ((uint32_t)e << 10) | (m >> 13);
        uint32_t rest = m & 0x1FFF;
        if (rest > 0x1000 || (rest == 0x1000 && (h & 1)))
            h++;
        return (uint16_t)h;
    }
}

static float h2f(uint16_t h)
{
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, x;
    float f;
    if (e == 0) {
        f = ldexpf((float)m, -24);
        return s ? -f : f;
    }
    if (e == 31)
        x = s | 0x7F800000u | (m << 13);
    else
        x = s | ((e - 15 + 127) << 23) | (m << 13);
    memcpy(&f, &x, 4);
    return f;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void dump(const char *path, const uint16_t *hp, size_t texels)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f)
        return;
    for (i = 0; i < texels * 4; i++) {
        float v = h2f(hp[i]);
        fwrite(&v, 4, 1, f);
    }
    fclose(f);
}

/* recombine.comp mode 1 after the NNEDI3 pass: the title's texture
 * (B8G8R8A8, sampled at binding 0), the 2x luma (1), RGBA8 out (4). */
static void recombine_stage(const char *spv, const char *prefix, const uint32_t *px, uint32_t w,
                            uint32_t h, uint32_t wrap, VkImageView y2, uint32_t reps,
                            double ms_per_tick, VkQueryPool qp, VkBuffer rb_b, uint8_t *rb);

/* reps timed dispatches of pipe (after three warm-up ones): ms sorted. */
static void timed(VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet ds, VkQueryPool qp,
                  const int32_t *pc, uint32_t gx, uint32_t gy, uint32_t reps, double ms_per_tick,
                  double *ms)
{
    uint32_t i;
    for (i = 0; i < reps + 3; i++) {
        uint64_t ts[2];
        begin();
        vkCmdResetQueryPool(s_cb, qp, 0, 2);
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        vkCmdPushConstants(s_cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc);
        vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        vkCmdDispatch(s_cb, gx, gy, 1);
        vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        submit();
        CK(vkGetQueryPoolResults(s_dev, qp, 0, 2, sizeof ts, ts, 8,
                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        if (i >= 3)    /* the first three warm the clocks and caches */
            ms[i - 3] = (double)(ts[1] - ts[0]) * ms_per_tick / 1e6;
    }
    qsort(ms, reps, sizeof *ms, cmp_d);
}

static void recombine_stage(const char *spv, const char *prefix, const uint32_t *px, uint32_t w,
                            uint32_t h, uint32_t wrap, VkImageView y2, uint32_t reps,
                            double ms_per_tick, VkQueryPool qp, VkBuffer rb_b, uint8_t *rb)
{
    VkDescriptorSetLayoutBinding lb[3];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout dsl;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorPool dp;
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSet ds;
    VkDescriptorImageInfo dii[3];
    VkWriteDescriptorSet wr[3];
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSampler smp;
    VkImage src_img, out_img;
    VkImageView src_v, out_v;
    VkBuffer up_b;
    uint8_t *up;
    VkBufferImageCopy bc;
    int32_t pc[8];
    double *ms = (double *)calloc(reps, sizeof *ms);
    char path[1200];
    FILE *f;
    uint32_t i;
    static const uint32_t bind[3] = { 0, 1, 4 };

    memset(lb, 0, sizeof lb);
    for (i = 0; i < 3; i++) {
        lb[i].binding = bind[i];
        lb[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                 : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    dli.bindingCount = 3;
    dli.pBindings = lb;
    CK(vkCreateDescriptorSetLayout(s_dev, &dli, NULL, &dsl));
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    CK(vkCreatePipelineLayout(s_dev, &pli, NULL, &pl));
    pipe = pipeline(spv, pl);
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CK(vkCreateSampler(s_dev, &sci, NULL, &smp));
    src_v = image_fmt(w, h, &src_img, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    out_v = image_fmt(w * 2, h * 2, &out_img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT);
    up = (uint8_t *)buffer((VkDeviceSize)w * h * 4, &up_b);
    memcpy(up, px, (size_t)w * h * 4);
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = w;
    bc.imageExtent.height = h;
    bc.imageExtent.depth = 1;
    begin();
    to_general(src_img);
    to_general(out_img);
    vkCmdCopyBufferToImage(s_cb, up_b, src_img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    all_barrier();
    submit();
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    CK(vkCreateDescriptorPool(s_dev, &dpi, NULL, &dp));
    dsa.descriptorPool = dp;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &dsl;
    CK(vkAllocateDescriptorSets(s_dev, &dsa, &ds));
    memset(dii, 0, sizeof dii);
    memset(wr, 0, sizeof wr);
    dii[0].sampler = smp;
    dii[0].imageView = src_v;
    dii[1].imageView = y2;
    dii[2].imageView = out_v;
    for (i = 0; i < 3; i++) {
        dii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = ds;
        wr[i].dstBinding = bind[i];
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = lb[i].descriptorType;
        wr[i].pImageInfo = &dii[i];
    }
    vkUpdateDescriptorSets(s_dev, 3, wr, 0, NULL);
    memset(pc, 0, sizeof pc);
    pc[2] = (int32_t)w;
    pc[3] = (int32_t)h;
    pc[4] = (wrap & 1) != 0;
    pc[5] = (wrap & 2) != 0;
    pc[6] = 1;
    timed(pipe, pl, ds, qp, pc, (w * 2 + 15) / 16, (h * 2 + 15) / 16, reps, ms_per_tick, ms);
    printf("%s %ux%u -> %ux%u: median %.4f ms, fastest %.4f ms; per 64K input texels median %.4f ms\n",
           spv, w, h, w * 2, h * 2, ms[reps / 2], ms[0], ms[reps / 2] / (w * h) * 65536.0);
    bc.imageExtent.width = w * 2;
    bc.imageExtent.height = h * 2;
    begin();
    all_barrier();
    vkCmdCopyImageToBuffer(s_cb, out_img, VK_IMAGE_LAYOUT_GENERAL, rb_b, 1, &bc);
    submit();
    for (i = 0; i < w * h * 4; i++) {      /* RGBA to the .bgra files' order */
        uint8_t t = rb[i * 4];
        rb[i * 4] = rb[i * 4 + 2];
        rb[i * 4 + 2] = t;
    }
    snprintf(path, sizeof path, "%s_nnedi3_x2.bgra", prefix);
    if ((f = fopen(path, "wb")) != NULL) {
        fwrite(rb, 4, (size_t)w * h * 4, f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstance inst;
    VkPhysicalDevice pds[8];
    VkPhysicalDeviceProperties pp;
    VkQueueFamilyProperties qf[16];
    uint32_t npd = 8, nqf = 16, qi = 0, w, h, group, reps, wrap, i;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    void *lib;
    FILE *f;
    uint32_t *px;
    VkDescriptorSetLayoutBinding lb[2];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout dsl;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorPool dp;
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSet ds;
    VkDescriptorImageInfo dii[2];
    VkWriteDescriptorSet wr[2];
    VkCommandPoolCreateInfo cpo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool cp;
    VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    VkQueryPool qp;
    VkImage in_img, out_img;
    VkImageView in_v, out_v;
    VkBuffer up_b, rb_b;
    uint16_t *up, *rb;
    VkBufferImageCopy bc;
    int32_t pc[8];
    double *ms;
    char path[1200];

    if (argc < 7) {
        fprintf(stderr, "usage: %s <pass.spv> <group> <in.bgra> <w> <h> <out prefix> [reps] [wrap]\n",
                argv[0]);
        return 2;
    }
    group = (uint32_t)atoi(argv[2]);
    w = (uint32_t)atoi(argv[4]);
    h = (uint32_t)atoi(argv[5]);
    reps = argc > 7 ? (uint32_t)atoi(argv[7]) : 50;
    wrap = argc > 8 ? (uint32_t)atoi(argv[8]) : 3;
    lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib)
        lib = dlopen("libvulkan.dylib", RTLD_NOW);
    if (!lib || !(gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr"))) {
        fprintf(stderr, "no libvulkan\n");
        return 1;
    }
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    CK(vkCreateInstance(&ici, NULL, &inst));
#define LOAD(n) n = (PFN_##n)gipa(inst, #n);
    FNS(LOAD)
    CK(vkEnumeratePhysicalDevices(inst, &npd, pds));
    s_pd = pds[0];
    for (i = 0; i < npd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            s_pd = pds[i];
            break;
        }
    }
    vkGetPhysicalDeviceProperties(s_pd, &pp);
    vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &nqf, qf);
    for (i = 0; i < nqf; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT && qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            qi = i;
            break;
        }
    dq.queueFamilyIndex = qi;
    dq.queueCount = 1;
    dq.pQueuePriorities = &prio;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dq;
    CK(vkCreateDevice(s_pd, &dci, NULL, &s_dev));
    vkGetDeviceQueue(s_dev, qi, 0, &s_q);

    memset(lb, 0, sizeof lb);
    lb[0].binding = 0;
    lb[1].binding = 4;
    for (i = 0; i < 2; i++) {
        lb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    dli.bindingCount = 2;
    dli.pBindings = lb;
    CK(vkCreateDescriptorSetLayout(s_dev, &dli, NULL, &dsl));
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    CK(vkCreatePipelineLayout(s_dev, &pli, NULL, &pl));
    pipe = pipeline(argv[1], pl);

    in_v = image(w, h, &in_img);
    out_v = image(w * 2, h * 2, &out_img);
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    CK(vkCreateDescriptorPool(s_dev, &dpi, NULL, &dp));
    dsa.descriptorPool = dp;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &dsl;
    CK(vkAllocateDescriptorSets(s_dev, &dsa, &ds));
    memset(dii, 0, sizeof dii);
    memset(wr, 0, sizeof wr);
    dii[0].imageView = in_v;
    dii[1].imageView = out_v;
    for (i = 0; i < 2; i++) {
        dii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = ds;
        wr[i].dstBinding = i ? 4 : 0;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[i].pImageInfo = &dii[i];
    }
    vkUpdateDescriptorSets(s_dev, 2, wr, 0, NULL);
    cpo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpo.queueFamilyIndex = qi;
    CK(vkCreateCommandPool(s_dev, &cpo, NULL, &cp));
    cba.commandPool = cp;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    CK(vkAllocateCommandBuffers(s_dev, &cba, &s_cb));
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2;
    CK(vkCreateQueryPool(s_dev, &qpi, NULL, &qp));

    /* The luma, as luma.comp makes it (BT.709 of the gamma-coded RGB). */
    px = (uint32_t *)malloc((size_t)w * h * 4);
    f = fopen(argv[3], "rb");
    if (!f || fread(px, 4, (size_t)w * h, f) != (size_t)w * h) {
        fprintf(stderr, "cannot read %s\n", argv[3]);
        return 1;
    }
    fclose(f);
    up = (uint16_t *)buffer((VkDeviceSize)w * h * 8, &up_b);
    rb = (uint16_t *)buffer((VkDeviceSize)w * h * 32, &rb_b);
    for (i = 0; i < w * h; i++) {
        float b = (float)(px[i] & 0xFF) / 255.0f, g = (float)(px[i] >> 8 & 0xFF) / 255.0f;
        float r = (float)(px[i] >> 16 & 0xFF) / 255.0f;
        up[i * 4 + 0] = f2h(0.2126f * r + 0.7152f * g + 0.0722f * b);
        up[i * 4 + 1] = up[i * 4 + 2] = 0;
        up[i * 4 + 3] = f2h(1.0f);
    }
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = w;
    bc.imageExtent.height = h;
    bc.imageExtent.depth = 1;
    begin();
    to_general(in_img);
    to_general(out_img);
    vkCmdCopyBufferToImage(s_cb, up_b, in_img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    all_barrier();
    submit();

    memset(pc, 0, sizeof pc);
    pc[2] = (int32_t)w;
    pc[3] = (int32_t)h;
    pc[4] = (wrap & 1) != 0;
    pc[5] = (wrap & 2) != 0;
    ms = (double *)calloc(reps, sizeof *ms);
    timed(pipe, pl, ds, qp, pc, (w + group - 1) / group, (h + group - 1) / group, reps,
          pp.limits.timestampPeriod, ms);
    printf("%s %ux%u group %u on %s: median %.4f ms, fastest %.4f ms; per 64K input texels"
           " median %.4f, fastest %.4f ms\n", argv[1], w, h, group, pp.deviceName, ms[reps / 2],
           ms[0], ms[reps / 2] / (w * h) * 65536.0, ms[0] / (w * h) * 65536.0);

    begin();
    all_barrier();
    vkCmdCopyImageToBuffer(s_cb, in_img, VK_IMAGE_LAYOUT_GENERAL, rb_b, 1, &bc);
    submit();
    snprintf(path, sizeof path, "%s_nnedi3_luma.f32", argv[6]);
    dump(path, rb, (size_t)w * h);
    bc.imageExtent.width = w * 2;
    bc.imageExtent.height = h * 2;
    begin();
    vkCmdCopyImageToBuffer(s_cb, out_img, VK_IMAGE_LAYOUT_GENERAL, rb_b, 1, &bc);
    submit();
    snprintf(path, sizeof path, "%s_nnedi3_model.f32", argv[6]);
    dump(path, rb, (size_t)w * h * 4);
    if (argc > 9)
        recombine_stage(argv[9], argv[6], px, w, h, wrap, out_v, reps, pp.limits.timestampPeriod,
                        qp, rb_b, (uint8_t *)rb);
    return 0;
}
