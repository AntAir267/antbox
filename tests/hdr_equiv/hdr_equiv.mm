/*
 * hdr_equiv (BO-244): the same synthetic frame through the HDR output maths
 * of both presenters, on this Mac's GPU, in nits:
 *
 *   vk_old   the Vulkan flip pass's shader before BO-244 (old_flip_glsl.inc)
 *   vk       the Vulkan flip pass's shader now (video/hdr_shaders.h
 *            HDR_FLIP_GLSL), SPIR-V by glslangValidator, run by MoltenVK
 *   metal    the Metal composite (HDR_COMP_MSL fs_hdr_pq), the scene
 *            snapshot telling scene from HUD as in the game
 *   cpu      the shared functions compiled as C++ (HDR_TONE_FNS(HDR_CODE))
 *
 * The frame (256 x 8, half float, the title's encoding): row 0 a grey ramp
 * 0..1 (shadows to paper white), row 1 grey light 1..8 past paper white
 * (lamps, the sun), row 2 orange light (sparks, flames: r, 0.55 r, 0.15 r),
 * row 3 red light (brake lights), row 4 white-blue (headlights), row 5 a
 * colour ramp below 1, rows 6-7 HUD (a grey ramp and white text values,
 * drawn over the scene: the snapshot differs there). Outputs are float32
 * targets (no quantisation); the max relative difference of the brightest
 * channel's nits between each pair is printed per settings set, and a table
 * of sample pixels. Exit 1 when vk and metal differ by more than 0.5 %
 * (above 0.05 nits) or vk_old and vk by more than 0.1 %.
 *
 *   cmake -S tests/hdr_equiv -B /tmp/hdr_equiv && cmake --build /tmp/hdr_equiv
 *   /tmp/hdr_equiv/hdr_equiv            (needs glslangValidator and MoltenVK)
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

/* ---- the shared maths as C++ ---------------------------------------- */
struct float3 { float r, g, b; float3() : r(0), g(0), b(0) {} explicit float3(float v) : r(v), g(v), b(v) {}
                float3(float x, float y, float z) : r(x), g(y), b(z) {} };
struct float4 { float x, y, z, w; float4() : x(0), y(0), z(0), w(0) {} explicit float4(float v) : x(v), y(v), z(v), w(v) {}
                float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {} };
struct bool3 { bool r, g, b; };
static float3 operator+(float3 a, float3 b) { return float3(a.r + b.r, a.g + b.g, a.b + b.b); }
static float3 operator+(float a, float3 b) { return float3(a + b.r, a + b.g, a + b.b); }
static float3 operator+(float3 a, float b) { return float3(a.r + b, a.g + b, a.b + b); }
static float3 operator*(float3 a, float3 b) { return float3(a.r * b.r, a.g * b.g, a.b * b.b); }
static float3 operator*(float a, float3 b) { return float3(a * b.r, a * b.g, a * b.b); }
static float3 operator*(float3 a, float b) { return float3(a.r * b, a.g * b, a.b * b); }
static float3 operator/(float3 a, float3 b) { return float3(a.r / b.r, a.g / b.g, a.b / b.b); }
static float3 operator/(float3 a, float b) { return float3(a.r / b, a.g / b, a.b / b); }
static float3 &operator*=(float3 &a, float b) { a = a * b; return a; }
static float3 pow(float3 a, float3 e) { return float3(powf(a.r, e.r), powf(a.g, e.g), powf(a.b, e.b)); }
static float3 clamp(float3 a, float lo, float hi) { return float3(fminf(fmaxf(a.r, lo), hi), fminf(fmaxf(a.g, lo), hi), fminf(fmaxf(a.b, lo), hi)); }
static float clamp(float a, float lo, float hi) { return fminf(fmaxf(a, lo), hi); }
static float max(float a, float b) { return fmaxf(a, b); }
static float dot(float3 a, float3 b) { return a.r * b.r + a.g * b.g + a.b * b.b; }
static float mix(float a, float b, float t) { return a + (b - a) * t; }
static float smoothstep(float e0, float e1, float x) { float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); }
static bool3 hdr_gt(float3 a, float3 b) { return { a.r > b.r, a.g > b.g, a.b > b.b }; }
static float3 hdr_sel(float3 x, float3 y, bool3 c) { return float3(c.r ? y.r : x.r, c.g ? y.g : x.g, c.b ? y.b : x.b); }
#define HDR_GT(a, b) hdr_gt(a, b)
#define HDR_SEL(x, y, c) hdr_sel(x, y, c)
#include "video/hdr_shaders.h"
HDR_TONE_FNS(HDR_CODE)
#include "old_flip_glsl.inc"

/* ---- the frame -------------------------------------------------------- */
enum { W = 256, H = 8, HUD_ROW = 6 };
static void make_frame(std::vector<float> &f, std::vector<float> &snap)
{
    f.assign(W * H * 4, 1.0f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float t = x / (float)(W - 1), r = 0, g = 0, b = 0;
            switch (y) {
            case 0: r = g = b = t; break;
            case 1: r = g = b = 1.0f + 7.0f * t; break;
            case 2: r = 0.5f + 5.5f * t; g = 0.55f * r; b = 0.15f * r; break;
            case 3: r = 0.4f + 4.6f * t; g = 0.08f * r; b = 0.05f * r; break;
            case 4: b = 0.6f + 5.4f * t; r = 0.85f * b; g = 0.92f * b; break;
            case 5: r = t; g = 1.0f - t; b = 0.5f * t; break;
            case 6: r = g = b = t; break;
            default: r = g = b = (x & 8) ? 1.0f : 0.02f; break;
            }
            float *p = &f[(y * W + x) * 4];
            p[0] = r; p[1] = g; p[2] = b; p[3] = 1;
        }
    /* fp16 as the game's targets */
    for (auto &v : f) v = (float)(__fp16)v;
    snap = f;
    for (int y = HUD_ROW; y < H; y++)       /* the HUD drew over these: the snapshot had scene */
        for (int x = 0; x < W; x++)
            snap[(y * W + x) * 4 + 0] = (float)(__fp16)(snap[(y * W + x) * 4 + 0] + 0.37f);
}

struct Settings { const char *name; float pk, pw, hw, sh, hl, srgb, sf, bl; };
static void consts(const Settings &s, float a[4], float b[4], float h[4])
{
    a[0] = s.pk / s.pw; a[1] = s.pw / 10000.0f; a[2] = s.hl; a[3] = s.srgb;
    b[0] = s.sh; b[1] = s.sf; b[2] = s.bl / 10000.0f; b[3] = 0;
    h[0] = s.hw / 10000.0f; h[1] = s.pk / 10000.0f; h[2] = 1; h[3] = 0;
}

static float pq_nits(float e)
{
    float p = powf(fmaxf(e, 0.0f), 1.0f / 78.84375f);
    return 10000.0f * powf(fmaxf(p - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * p), 1.0f / 0.1593017578125f);
}

/* ---- Metal -------------------------------------------------------------- */
static id<MTLDevice> s_dev;
static id<MTLRenderPipelineState> s_pso;
static bool metal_init(void)
{
    s_dev = MTLCreateSystemDefaultDevice();
    if (!s_dev) return false;
    std::string src = "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct VO { float4 p [[position]]; float2 t; };\n"
        "vertex VO vs_blit(uint i [[vertex_id]]) { VO o; o.t = float2((i << 1) & 2, i & 2);"
        " o.p = float4(o.t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
    src += HDR_COMP_MSL;
    NSError *e = nil;
    MTLCompileOptions *o = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    o.fastMathEnabled = NO;     /* as fb_present_metal.m */
#pragma clang diagnostic pop
    id<MTLLibrary> lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:o error:&e];
    if (!lib) { fprintf(stderr, "MSL: %s\n", [[e description] UTF8String]); return false; }
    for (NSString *fn in @[ @"fs_hdr_edr", @"fs_hdr_ovl", @"fs_hdr_pat" ])
        if (![lib newFunctionWithName:fn]) { fprintf(stderr, "MSL: no %s\n", [fn UTF8String]); return false; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs_blit"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs_hdr_pq"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    s_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!s_pso) { fprintf(stderr, "MSL pipeline: %s\n", [[e description] UTF8String]); return false; }
    return true;
}
static id<MTLTexture> metal_tex(const std::vector<float> &px)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                  width:W height:H mipmapped:NO];
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    std::vector<__fp16> h(px.size());
    for (size_t i = 0; i < px.size(); i++) h[i] = (__fp16)px[i];
    [t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:h.data() bytesPerRow:W * 8];
    return t;
}
static bool metal_run(const std::vector<float> &f, const std::vector<float> &snap, const Settings &s,
                      std::vector<float> &out)
{
    @autoreleasepool {
        id<MTLTexture> tf = metal_tex(f), ts = metal_tex(snap);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                      width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> dst = [s_dev newTextureWithDescriptor:td];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;   /* as the presenter's s_smp[1] */
        id<MTLSamplerState> smp = [s_dev newSamplerStateWithDescriptor:sd];
        float hq[12];
        consts(s, hq, hq + 4, hq + 8);
        id<MTLCommandQueue> q = [s_dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:s_pso];
        [e setFragmentTexture:tf atIndex:0];
        [e setFragmentTexture:ts atIndex:1];
        [e setFragmentSamplerState:smp atIndex:0];
        [e setFragmentBytes:hq length:sizeof hq atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        out.resize(W * H * 4);
        [dst getBytes:out.data() bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    }
    return true;
}

/* ---- Vulkan (MoltenVK) ---------------------------------------------- */
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #x, r_); return false; } } while (0)
static VkInstance vi;
static VkPhysicalDevice vpd;
static VkDevice vd;
static VkQueue vq;
static uint32_t vqf;
static VkCommandPool vcp;
static bool vk_init(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_1;
    const char *iext[] = { VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME };
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ii.pApplicationInfo = &ai;
    ii.enabledExtensionCount = 1;
    ii.ppEnabledExtensionNames = iext;
    VKC(vkCreateInstance(&ii, NULL, &vi));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(vi, &n, &vpd);
    if (!n) return false;
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(vpd, &pp);
    fprintf(stderr, "Vulkan: %s (MoltenVK)\n", pp.deviceName);
    uint32_t qn = 8;
    VkQueueFamilyProperties qp[8];
    vkGetPhysicalDeviceQueueFamilyProperties(vpd, &qn, qp);
    for (vqf = 0; vqf < qn && !(qp[vqf].queueFlags & VK_QUEUE_GRAPHICS_BIT); vqf++) {}
    float pr = 1;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = vqf; qi.queueCount = 1; qi.pQueuePriorities = &pr;
    const char *dext[] = { "VK_KHR_portability_subset" };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = dext;
    VKC(vkCreateDevice(vpd, &di, NULL, &vd));
    vkGetDeviceQueue(vd, vqf, 0, &vq);
    VkCommandPoolCreateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    ci.queueFamilyIndex = vqf;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKC(vkCreateCommandPool(vd, &ci, NULL, &vcp));
    return true;
}
static uint32_t vk_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vpd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}
static std::vector<uint32_t> spirv(const char *src, const char *tag)
{
    char in[256], out[256], cmd[700];
    snprintf(in, sizeof in, "%s/hdr_equiv_%s.frag", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", tag);
    snprintf(out, sizeof out, "%s.spv", in);
    FILE *f = fopen(in, "w"); fputs(src, f); fclose(f);
    snprintf(cmd, sizeof cmd, "glslangValidator -V -S frag '%s' -o '%s' > /dev/null 2>&1", in, out);
    std::vector<uint32_t> w;
    if (system(cmd) != 0) { fprintf(stderr, "glslang failed on %s\n", tag); return w; }
    f = fopen(out, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    w.resize(n / 4); fread(w.data(), 4, w.size(), f); fclose(f);
    return w;
}
static const char *s_vs =
    "#version 450\nvoid main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);"
    " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static bool vk_run(const char *fs_src, const char *tag, const std::vector<float> &f, const Settings &s,
                   std::vector<float> &out)
{
    std::vector<uint32_t> fsw = spirv(fs_src, tag), vsw;
    {
        char in[256], o[256], cmd[700];
        snprintf(in, sizeof in, "%s/hdr_equiv_vs.vert", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        snprintf(o, sizeof o, "%s.spv", in);
        FILE *fp = fopen(in, "w"); fputs(s_vs, fp); fclose(fp);
        snprintf(cmd, sizeof cmd, "glslangValidator -V '%s' -o '%s' > /dev/null 2>&1", in, o);
        if (system(cmd) != 0) return false;
        fp = fopen(o, "rb"); fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        vsw.resize(n / 4); fread(vsw.data(), 4, vsw.size(), fp); fclose(fp);
    }
    if (fsw.empty()) return false;
    /* the source image: half float, as the game's view target */
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ic.extent = { W, H, 1 }; ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL; ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage src, dst;
    VKC(vkCreateImage(vd, &ic, NULL, &src));
    ic.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VKC(vkCreateImage(vd, &ic, NULL, &dst));
    VkDeviceMemory ms, md, mb;
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    vkGetImageMemoryRequirements(vd, src, &mr);
    ma.allocationSize = mr.size; ma.memoryTypeIndex = vk_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(vd, &ma, NULL, &ms)); vkBindImageMemory(vd, src, ms, 0);
    vkGetImageMemoryRequirements(vd, dst, &mr);
    ma.allocationSize = mr.size; ma.memoryTypeIndex = vk_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(vd, &ma, NULL, &md)); vkBindImageMemory(vd, dst, md, 0);
    /* one host buffer for the upload and the readback */
    VkBuffer buf;
    VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bc.size = W * H * 16; bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VKC(vkCreateBuffer(vd, &bc, NULL, &buf));
    vkGetBufferMemoryRequirements(vd, buf, &mr);
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = vk_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKC(vkAllocateMemory(vd, &ma, NULL, &mb)); vkBindBufferMemory(vd, buf, mb, 0);
    void *map;
    vkMapMemory(vd, mb, 0, VK_WHOLE_SIZE, 0, &map);
    for (size_t i = 0; i < f.size(); i++) ((__fp16 *)map)[i] = (__fp16)f[i];
    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView sv, dv;
    vc.image = src; vc.format = VK_FORMAT_R16G16B16A16_SFLOAT; VKC(vkCreateImageView(vd, &vc, NULL, &sv));
    vc.image = dst; vc.format = VK_FORMAT_R32G32B32A32_SFLOAT; VKC(vkCreateImageView(vd, &vc, NULL, &dv));
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSampler smp; VKC(vkCreateSampler(vd, &sci, NULL, &smp));
    VkAttachmentDescription ad = {};
    ad.format = VK_FORMAT_R32G32B32A32_SFLOAT; ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; ad.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = {}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpc.attachmentCount = 1; rpc.pAttachments = &ad; rpc.subpassCount = 1; rpc.pSubpasses = &sp;
    VkRenderPass rp; VKC(vkCreateRenderPass(vd, &rpc, NULL, &rp));
    VkFramebufferCreateInfo fc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fc.renderPass = rp; fc.attachmentCount = 1; fc.pAttachments = &dv; fc.width = W; fc.height = H; fc.layers = 1;
    VkFramebuffer fb; VKC(vkCreateFramebuffer(vd, &fc, NULL, &fb));
    VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo lc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lc.bindingCount = 1; lc.pBindings = &lb;
    VkDescriptorSetLayout dsl; VKC(vkCreateDescriptorSetLayout(vd, &lc, NULL, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plc.setLayoutCount = 1; plc.pSetLayouts = &dsl; plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pcr;
    VkPipelineLayout pl; VKC(vkCreatePipelineLayout(vd, &plc, NULL, &pl));
    VkShaderModuleCreateInfo smc = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vsm, fsm;
    smc.codeSize = vsw.size() * 4; smc.pCode = vsw.data(); VKC(vkCreateShaderModule(vd, &smc, NULL, &vsm));
    smc.codeSize = fsw.size() * 4; smc.pCode = fsw.data(); VKC(vkCreateShaderModule(vd, &smc, NULL, &fsm));
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vsm; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.lineWidth = 1; rs.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo msi = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {}; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &msi; gp.pColorBlendState = &cbs;
    gp.layout = pl; gp.renderPass = rp;
    VkPipeline pso; VKC(vkCreateGraphicsPipelines(vd, VK_NULL_HANDLE, 1, &gp, NULL, &pso));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpc.maxSets = 1; dpc.poolSizeCount = 1; dpc.pPoolSizes = &ps;
    VkDescriptorPool dp; VKC(vkCreateDescriptorPool(vd, &dpc, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
    VkDescriptorSet ds; VKC(vkAllocateDescriptorSets(vd, &dai, &ds));
    VkDescriptorImageInfo dii = { smp, sv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ds; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(vd, 1, &wr, 0, NULL);

    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = vcp; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb; VKC(vkAllocateCommandBuffers(vd, &cai, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = src; ib.subresourceRange = vc.subresourceRange;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkBufferImageCopy bic = {}; bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; bic.imageExtent = { W, H, 1 };
    vkCmdCopyBufferToImage(cb, buf, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea = sc;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
    float pc[12];
    consts(s, pc, pc + 4, pc + 8);            /* a, b: exactly nv2a_pb_vulkan.c's pc[0..7] */
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VKC(vkQueueSubmit(vq, 1, &si, VK_NULL_HANDLE));
    vkQueueWaitIdle(vq);
    out.assign((float *)map, (float *)map + W * H * 4);
    /* leaked on purpose: a one-shot test */
    return true;
}

/* ---- the CPU path (the shared functions as C++) ---------------------------- */
static void cpu_run(const std::vector<float> &f, const Settings &s, std::vector<float> &out)
{
    float a_[4], b_[4], h_[4];
    consts(s, a_, b_, h_);
    float4 a(a_[0], a_[1], a_[2], a_[3]), b(b_[0], b_[1], b_[2], b_[3]);
    out.resize(W * H * 4);
    for (int i = 0; i < W * H; i++) {
        float3 c(fmaxf(f[i * 4], 0), fmaxf(f[i * 4 + 1], 0), fmaxf(f[i * 4 + 2], 0));
        float3 o = hdr_pq(hdr_tone(c, a, b) * a.y, a.x * a.y, b);
        out[i * 4] = o.r; out[i * 4 + 1] = o.g; out[i * 4 + 2] = o.b; out[i * 4 + 3] = 1;
    }
}

static float maxnits(const std::vector<float> &o, int i)
{
    return fmaxf(pq_nits(o[i * 4]), fmaxf(pq_nits(o[i * 4 + 1]), pq_nits(o[i * 4 + 2])));
}
static float cmp(const std::vector<float> &x, const std::vector<float> &y, float *absmax, int rows = H)
{
    float m = 0, am = 0;
    for (int i = 0; i < W * rows; i++)
        for (int ch = 0; ch < 3; ch++) {
            float p = pq_nits(x[i * 4 + ch]), q = pq_nits(y[i * 4 + ch]);
            float d = fabsf(p - q);
            am = fmaxf(am, d);
            if (fmaxf(p, q) > 0.05f && d > 0.05f)
                m = fmaxf(m, d / fmaxf(p, q));
        }
    if (absmax) *absmax = am;
    return m;
}

int main(void)
{
    @autoreleasepool {
        std::vector<float> f, snap;
        make_frame(f, snap);
        if (!metal_init()) return 2;
        if (!vk_init()) { fprintf(stderr, "no Vulkan (MoltenVK)\n"); return 2; }
        const Settings sets[] = {
            /* name, peak, paper, HUD, shadows, highlights, srgb, softness, black */
            { "PG32UCDP defaults (EDID 507.6, paper 200)", 507.6f, 200, 200, 1.0f, 1.0f, 0, 0.35f, 0 },
            { "Deck OLED-like (peak 1000, paper 200)",     1000,   200, 200, 1.0f, 1.0f, 0, 0.35f, 0 },
            { "tuned (peak 800, paper 250, sh 1.1, hl 1.5, black 0.5)", 800, 250, 250, 1.1f, 1.5f, 0, 0.35f, 0.5f },
            { "sRGB decode, HUD 150",                      600,    180, 150, 1.0f, 1.0f, 1, 0.35f, 0 },
        };
        int bad = 0;
        for (const Settings &s : sets) {
            std::vector<float> vk_old, vk, mt, cpu;
            if (!vk_run(s_old_flip_glsl, "old", f, s, vk_old) || !vk_run(HDR_FLIP_GLSL, "new", f, s, vk)
                    || !metal_run(f, snap, s, mt))
                return 2;
            cpu_run(f, s, cpu);
            float a1, a2, a3;
            /* With the HUD's white apart from paper white the Vulkan backend
             * scales the HUD in its combiners (not run here): the HUD rows
             * are left out of the Metal comparison then. */
            const int rows = s.hw == s.pw ? H : HUD_ROW;
            float e_old = cmp(vk_old, vk, &a1), e_mt = cmp(vk, mt, &a2, rows), e_cpu = cmp(vk, cpu, &a3);
            printf("\n== %s\n", s.name);
            printf("max rel. difference (abs nits): vk_old vs vk %.5f (%.3f), vk vs metal %.5f (%.3f)%s,"
                   " vk vs cpu %.5f (%.3f)\n", e_old, a1, e_mt, a2, rows < H ? " (scene rows)" : "", e_cpu, a3);
            printf("%-34s %9s %9s %9s %9s\n", "pixel (encoded r,g,b)", "vk_old", "vk", "metal", "cpu");
            const int srows[] = { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 4, 6, 7 };
            const int cols[] = { 13, 64, 128, 255, 0, 40, 128, 255, 128, 255, 255, 255, 255, 8 };
            for (unsigned k = 0; k < sizeof srows / sizeof srows[0]; k++) {
                int i = srows[k] * W + cols[k];
                char lab[64];
                snprintf(lab, sizeof lab, "row %d (%.3f, %.3f, %.3f)%s", srows[k], f[i * 4], f[i * 4 + 1], f[i * 4 + 2],
                         srows[k] >= HUD_ROW ? " HUD" : "");
                printf("%-34s %9.2f %9.2f %9.2f %9.2f\n", lab, maxnits(vk_old, i), maxnits(vk, i), maxnits(mt, i),
                       maxnits(cpu, i));
            }
            if (e_old > 0.001f || e_mt > 0.005f)
                bad = 1;
        }
        printf("\n%s\n", bad ? "FAIL" : "OK: Vulkan (before and after) and Metal give the same nits");
        return bad;
    }
}
