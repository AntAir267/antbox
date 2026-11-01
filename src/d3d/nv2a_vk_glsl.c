/**
 * GLSL -> SPIR-V with glslang, and the SPIR-V disk cache (see nv2a_vk_glsl.h).
 *
 * The glslang call sequence follows xemu's pgraph_vk_compile_glsl_to_spv
 * (hw/xbox/nv2a/pgraph/vk/glsl.c; credited in NOTICE): preprocess, parse, link with
 * SPV and Vulkan rules, generate. Unlike xemu, a failure is returned (and
 * logged by the caller with the source) rather than asserted, and the
 * resource limits are glslang's defaults.
 *
 * Cache file <dir>/vk_spirv.bin in nv2a_pcache's record format (nv2a_pcache.h,
 * the Metal pipeline cache's): version "vk spirv 1; glslang <ver>", records of
 * type 0 holding the 64-bit key (FNV-1a of the stage and the source) and the
 * words, appended as they are compiled.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "nv2a_vk_glsl.h"
#include "nv2a_vk_core.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#if __has_include(<glslang/build_info.h>)
#include <glslang/build_info.h>
#else
#define GLSLANG_VERSION_MAJOR 0   /* glslang before 14 ships no build_info.h */
#define GLSLANG_VERSION_MINOR 0
#define GLSLANG_VERSION_PATCH 0
#endif

#include "recomp_env.h"
#include "nv2a_pcache.h"
#include "nv2a_backend_common.h"
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

#define LOGP "[vulkan] "

typedef struct {
    uint64_t  key;
    uint32_t  n;
    uint32_t *words;
} SpvEntry;

/* The device's demote feature is on (nv2a_vk_core.c), or no device yet. */
static int spv_demote(void)
{
    const struct nvk_core *c = nvk_core_get();
    return !c || c->demote;
}

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static SpvEntry *s_tab;          /* open addressing, s_cap a power of two */
static size_t    s_cap, s_count;
static int       s_loaded, s_glslang_up;
static uint64_t  s_hits, s_compiles;

/* render.shader_cache, as the Metal pipeline cache reads it. */
int nvk_cache_dir(char *out, size_t n)
{
    return nv2a_pcache_dir(nv2a_host_opts()->shader_cache, nv2a_pcache_app(), out, n);
}

static SpvEntry *slot(uint64_t key)
{
    size_t i = (size_t)key & (s_cap - 1);
    while (s_tab[i].words && s_tab[i].key != key)
        i = (i + 1) & (s_cap - 1);
    return &s_tab[i];
}

static int grow(void)
{
    SpvEntry *old = s_tab;
    size_t oc = s_cap, i;
    size_t nc = oc ? oc * 2 : 1024;
    SpvEntry *t = (SpvEntry *)calloc(nc, sizeof *t);
    if (!t)
        return 0;
    s_tab = t;
    s_cap = nc;
    for (i = 0; i < oc; i++)
        if (old[i].words)
            *slot(old[i].key) = old[i];
    free(old);
    return 1;
}

static void insert(uint64_t key, uint32_t *words, uint32_t n)
{
    SpvEntry *e;
    if ((s_count + 1) * 2 > s_cap && !grow()) {
        free(words);
        return;
    }
    e = slot(key);
    if (e->words) {
        free(words);
        return;
    }
    e->key = key;
    e->n = n;
    e->words = words;
    s_count++;
}

/* The SPIR-V records: type 0, payload the 64-bit key then the words. */
static nv2a_pcache *s_file;

/* s_lock held. Opens the file (again: `reload` closes it first, to take in
 * what another process appended) and puts its records in the table; one
 * already there is kept. */
static void load(int reload)
{
    char dir[1024], path[1100], ver[128], msg[256];
    size_t i, n;

    s_loaded = 1;
    if (reload) {
        if (!s_file)
            return;
        nv2a_pcache_close(s_file);
        s_file = NULL;
    }
    if (!nvk_cache_dir(dir, sizeof dir)
            || snprintf(path, sizeof path, "%s/vk_spirv.bin", dir) >= (int)sizeof path)
        return;
    snprintf(ver, sizeof ver, "vk spirv %s; glslang %d.%d.%d", spv_demote() ? "1" : "1 (1.5)",
             GLSLANG_VERSION_MAJOR,
             GLSLANG_VERSION_MINOR, GLSLANG_VERSION_PATCH);
    s_file = nv2a_pcache_open(path, ver, msg, sizeof msg);
    if (!s_file)
        return;
    n = nv2a_pcache_count(s_file);
    for (i = 0; i < n; i++) {
        const void *d;
        uint32_t type, len, *w;
        uint64_t key;
        if (!nv2a_pcache_get(s_file, i, &type, &d, &len) || type != 0 || len < 12 || len % 4)
            continue;
        memcpy(&key, d, 8);
        w = (uint32_t *)malloc(len - 8);
        if (!w)
            break;
        memcpy(w, (const uint8_t *)d + 8, len - 8);
        insert(key, w, (len - 8) / 4);
    }
    if (!reload)
        fprintf(stderr, LOGP "SPIR-V cache %s: %s\n", path, msg);
}

void nvk_glsl_reload(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_loaded)
        load(1);
    pthread_mutex_unlock(&s_lock);
}

static void store(uint64_t key, const uint32_t *w, uint32_t n)
{
    uint8_t *rec;
    uint32_t len = 8 + n * 4;
    if (!s_file || len > NV2A_PCACHE_MAX_LEN || !(rec = (uint8_t *)malloc(len)))
        return;
    memcpy(rec, &key, 8);
    memcpy(rec + 8, w, (size_t)n * 4);
    if (nv2a_pcache_put(s_file, 0, rec, len) < 0) {
        XR_ERROR( LOGP "SPIR-V cache: cannot append; off for this run\n");
        nv2a_pcache_close(s_file);
        s_file = NULL;
    }
    free(rec);
}

void nvk_glsl_stats(uint64_t *hits, uint64_t *compiles)
{
    *hits = s_hits;
    *compiles = s_compiles;
}

/* glslang, xemu's sequence. Returns malloc'd words or NULL with err. */
static uint32_t *compile(glslang_stage_t stage, const char *src, uint32_t *nout,
                         char *err, size_t errn)
{
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *prog;
    uint32_t *w = NULL;
    size_t n;

    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = stage;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV;
    /* SPIR-V 1.6 where the device has the demote feature on (discard is
     * OpDemoteToHelperInvocation there); 1.5 where it has not (OpKill). */
    in.target_language_version = spv_demote() ? GLSLANG_TARGET_SPV_1_6 : GLSLANG_TARGET_SPV_1_5;
    in.code = src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.force_default_version_and_profile = 0;
    in.forward_compatible = 0;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();

    sh = glslang_shader_create(&in);
    if (!sh) {
        snprintf(err, errn, "glslang_shader_create failed");
        return NULL;
    }
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        snprintf(err, errn, "%s%s", glslang_shader_get_info_log(sh),
                 glslang_shader_get_info_debug_log(sh));
        glslang_shader_delete(sh);
        return NULL;
    }
    prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        snprintf(err, errn, "link: %s%s", glslang_program_get_info_log(prog),
                 glslang_program_get_info_debug_log(prog));
        glslang_program_delete(prog);
        glslang_shader_delete(sh);
        return NULL;
    }
    glslang_program_SPIRV_generate(prog, stage);
    n = glslang_program_SPIRV_get_size(prog);
    if (n && (w = (uint32_t *)malloc(n * 4)) != NULL)
        glslang_program_SPIRV_get(prog, w);
    else
        snprintf(err, errn, "no SPIR-V: %s", glslang_program_SPIRV_get_messages(prog)
                 ? glslang_program_SPIRV_get_messages(prog) : "?");
    *nout = (uint32_t)n;
    glslang_program_delete(prog);
    glslang_shader_delete(sh);
    return w;
}

int nvk_glsl_spirv(enum nvk_stage stage, const char *src, const uint32_t **words,
                   size_t *n, int *compiled, char *err, size_t errn)
{
    uint64_t key;
    SpvEntry *e;
    uint32_t *w, nw = 0;

    key = nv2a_fnv64(&stage, sizeof stage, NV2A_FNV64_INIT);
    key = nv2a_fnv64(src, strlen(src), key);
    if (!key)
        key = 1;
    *compiled = 0;
    pthread_mutex_lock(&s_lock);
    if (!s_loaded)
        load(0);
    if (s_cap && (e = slot(key))->words) {
        *words = e->words;
        *n = e->n;
        s_hits++;
        pthread_mutex_unlock(&s_lock);
        return 1;
    }
    if (!s_glslang_up) {
        glslang_initialize_process();
        s_glslang_up = 1;
    }
    /* Compiled without the lock (glslang's shader and program objects are
     * per call), so one thread's compile does not hold up another's lookup:
     * the ack thread's draw against the pre-warm thread's key list. */
    pthread_mutex_unlock(&s_lock);
    w = compile(stage == NVK_STAGE_VERTEX ? GLSLANG_STAGE_VERTEX
                : stage == NVK_STAGE_COMPUTE ? GLSLANG_STAGE_COMPUTE : GLSLANG_STAGE_FRAGMENT,
                src, &nw, err, errn);
    if (!w)
        return 0;
    pthread_mutex_lock(&s_lock);
    s_compiles++;
    *compiled = 1;
    if (!(s_cap && slot(key)->words))   /* else another thread compiled it meanwhile */
        store(key, w, nw);
    insert(key, w, nw);                 /* frees w when the key is there */
    if (s_cap && (e = slot(key))->words) {
        *words = e->words;
        *n = e->n;
        pthread_mutex_unlock(&s_lock);
        return 1;
    }
    pthread_mutex_unlock(&s_lock);
    snprintf(err, errn, "SPIR-V cache: out of memory");
    return 0;
}
