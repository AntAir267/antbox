/*
 * nv2a_pcache.h - the on-disk pipeline-key list a GPU backend pre-warms from.
 *
 * A backend compiles a shader (or a pipeline) the first time a draw needs it,
 * on the NV2A ack thread, so every cold start stalls a frame on each one. This
 * file remembers what was compiled, as the backend's own keys (the vertex
 * program's slots, the combiner state, the pipeline's fixed state), so the
 * next start compiles them before the title draws with them. It stores keys,
 * not compiled code: the backend regenerates the source from a key, so a
 * changed generator never runs stale code, and a rebuild of the toolkit
 * keeps the list. The version string names what the keys mean (the backend's
 * key-format number and the sizes of the structures written as they are);
 * only a change there drops a file.
 *
 * The keys are the title's own data (a vertex program's slots are its
 * microcode, byte for byte as in its executable), so a list is made on the
 * player's machine and is not something to hand around.
 *
 * The approach is the one emulators converged on: Dolphin keeps a per-game
 * list of pipeline UIDs under a 'PUID' + GX_PIPELINE_UID_VERSION header and
 * compiles all of it at startup on its pre-compiler threads
 * (Source/Core/VideoCommon/ShaderCache.cpp, InitializeShaderCache and the
 * UID cache load; on Metal it replays only the UIDs, MTLUtil.mm turns the
 * binary caches off); xemu keeps its GL shader binaries under a header of
 * its version and the driver's, deletes a mismatch, and loads the list on a
 * background thread (hw/xbox/nv2a/pgraph/gl/shaders.c); UnleashedRecomp
 * ships a pipeline-state list it compiles during the intro logos and
 * compiles new ones on worker threads (UnleashedRecomp/gpu/video.cpp, the
 * pipeline pre-compilation and EnqueueGraphicsPipelineCompilation).
 *
 * File: a header (magic, format, the caller's version string), then records
 * { u32 type, u32 len, u64 fnv64(type, payload), payload[len] }, appended as
 * they are made. On open: a missing, foreign or wrong-version file is started
 * afresh (deleted); a record that fails its check ends the file there, and the
 * file is rewritten without the tail (a torn append from a killed process).
 * Appends are one fwrite + fflush each, so a killed process loses at most the
 * record being written. Identical records are written once.
 *
 * Threading: open, put and close on one thread; get/count only read what open
 * loaded and may run on any thread once open returned.
 */
#ifndef NV2A_PCACHE_H
#define NV2A_PCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nv2a_pcache nv2a_pcache;

/* Largest payload a record may have. */
#define NV2A_PCACHE_MAX_LEN 65536u

/* Opens <path>, creating it. `version` names the key format that may read
 * it; a file written under another is deleted and started afresh (one whose
 * version is this one plus "; toolkit <rev>", as written up to BO-93, is
 * read as it is). Returns NULL
 * when the file can neither be read nor created (the cache is then off);
 * `msg` (may be NULL) gets one line on what was found: loaded, new, version
 * mismatch, a cut tail. */
nv2a_pcache *nv2a_pcache_open(const char *path, const char *version,
                              char *msg, size_t msg_len);
/* Records loaded at open (not counting those put since). */
size_t nv2a_pcache_count(const nv2a_pcache *pc);
/* Record i of those loaded: 1, or 0 past the end. */
int nv2a_pcache_get(const nv2a_pcache *pc, size_t i, uint32_t *type,
                    const void **data, uint32_t *len);
/* Appends a record. 1 written, 0 already in the file, -1 error. */
int nv2a_pcache_put(nv2a_pcache *pc, uint32_t type, const void *data, uint32_t len);
void nv2a_pcache_close(nv2a_pcache *pc);

/* FNV-1a, 64-bit, continued from `h` (start with NV2A_FNV64_INIT). */
#define NV2A_FNV64_INIT 0xcbf29ce484222325ull
uint64_t nv2a_fnv64(const void *p, size_t n, uint64_t h);

/* The directory pipeline caches go in, created if missing: `dir` when it is
 * set (a leading "~/" is $HOME), else the platform's per-user cache directory
 * (macOS ~/Library/Caches/<app>, elsewhere $XDG_CACHE_HOME or ~/.cache/<app>,
 * Windows %LOCALAPPDATA%\<app>). "none", "off", "0" or "" turn it off.
 * Returns 1 with the path in `out`, 0 when off or not creatable. */
int nv2a_pcache_dir(const char *dir, const char *app, char *out, size_t n);
/* The <app> the backends pass: the executable's name, or what a port set
 * before the backend started (its own name, when the executable has another). */
const char *nv2a_pcache_app(void);
void nv2a_pcache_set_app(const char *app);

/* ---- what the running backend's cache is doing ------------------------------
 *
 * One process-wide record the backend with the key list keeps up to date,
 * for whoever shows it (a settings page, a pre-build's progress line). Read
 * from any thread: the numbers only grow, a torn read is one count off. */
enum { NV2A_PC_VS = 0, NV2A_PC_FS = 1, NV2A_PC_PSO = 2 };
typedef struct nv2a_pcache_stat {
    int      active;          /* a backend is up and has a key list on disk */
    int      warming;         /* its pre-warm thread is running */
    int      waiting;         /* its render thread is waiting for the start-up
                               * pre-warm right now (no frame comes meanwhile:
                               * the window says so, overlay.h) */
    uint32_t listed[3];       /* keys in the list: vertex, fragment, pipeline */
    uint32_t warmed[3];       /* built by the pre-warm thread */
    uint32_t ack[3];          /* built by the render thread: the list lacked them */
    uint32_t ack_max_us;      /* the longest of those */
    uint64_t ack_ns;          /* and their sum */
    uint32_t hold_done, hold_total;   /* BO-208: the build the title is held for ("Preparing graphics N/M") */
    uint32_t warm_ms;         /* how long the first pre-warm took (0: not done yet) */
    int      cold;            /* that build is a first one (manifest keys new, or the driver's cache empty) */
    int      hdr_round;       /* BO-110: the pre-warm running builds the HDR
                               * variants after a switch to HDR (the window
                               * says so while it runs) */
    char     dir[512];        /* the cache directory, "" when off */
} nv2a_pcache_stat;
void nv2a_pcache_stat_get(nv2a_pcache_stat *out);
/* The backend's side. */
void nv2a_pcache_stat_open(const char *dir, const uint32_t listed[3]);
void nv2a_pcache_stat_warming(int on);   /* off also ends an HDR round */
void nv2a_pcache_stat_hdr(int on);
void nv2a_pcache_stat_waiting(int on);
void nv2a_pcache_stat_hold(uint32_t done, uint32_t total, int cold);
void nv2a_pcache_stat_warm_ms(uint32_t ms);
void nv2a_pcache_stat_built(int prewarm, int kind, uint64_t ns);

/* Asks the running backend to read its key list again and build what it
 * does not have yet, on its pre-warm thread: for after another process (a
 * pre-build) has appended to it. 0 when no backend with a key list is up. */
int  nv2a_pcache_rescan(void);
void nv2a_pcache_set_rescan(void (*fn)(void));

/* Appends a key to the running backend's list without building it (a
 * rescan then does, on the pre-warm thread): type NV2A_PC_VS with a vertex
 * program's slots, NV2A_PC_FS with an NV2ACombinerState. 0 when no backend
 * with a key list is up. Any thread. */
int  nv2a_pcache_seed(uint32_t type, const void *data, uint32_t len);
void nv2a_pcache_set_seed(void (*fn)(uint32_t type, const void *data, uint32_t len));

/* ---- vertex programs in an executable ---------------------------------------
 *
 * A title's vertex shaders are compiled into its executable as the XDK
 * assembler wrote them: u16 0x2078, u16 n, then n slots of 16 bytes, the
 * NV2A's microcode as SET_TRANSFORM_PROGRAM uploads it (a shader with
 * "#pragma screenspace" is uploaded as it is; D3D appends a viewport
 * epilogue to the others, which then do not match and cost nothing but an
 * unused entry). Calls fn for each one found in `image`: a slot's first
 * word is 0, FINAL (bit 0 of the fourth) is set on the last slot and only
 * there. Returns how many. A pre-build seeds a backend's key list with them
 * (type 0, the slots: the record both backends write for a vertex program),
 * so every vertex function is built before the title draws with it. */
size_t nv2a_pcache_scan_vsh(const void *image, size_t size,
                            void (*fn)(void *ctx, const uint32_t *slots, uint32_t n),
                            void *ctx);

/* ---- pixel-shader definitions in an executable -------------------------------
 *
 * A title's pixel shaders are D3DPIXELSHADERDEF blocks in its executable,
 * 60 words whose input, output, count and final-combiner words D3D writes
 * to the NV2A as they are. Calls fn with each combiner state they can lead
 * to (an NV2ACombinerState as the backends key a fragment shader, `len` its
 * size): one per way the definition's sampling stages can be bound (off, 2D,
 * cube), since a stage's mode follows the texture bound to it. Returns the
 * number of definitions found. A pre-build seeds the key list with them
 * (type NV2A_PC_FS), so a material the played scenes did not show has its
 * fragment shader built all the same. States the title sets through the
 * fixed-function texture stages are not in a definition and are not found.
 * (nv2a_pcache_psdef.c; needs d3d8_combiners_parse.c.) */
size_t nv2a_pcache_scan_psdef(const void *image, size_t size,
                              void (*fn)(void *ctx, const void *state, uint32_t len),
                              void *ctx);


/* ---- the shipped coverage manifest (BO-208) ----------------------------------
 *
 * A list of what to compile, made on the developer side by a sweep of the
 * title (every mode, track, car and camera) and shipped as data, so that a
 * first start can build everything before the title draws, instead of
 * learning it one hitch at a time. It holds no title code: a vertex program
 * is named by the 64-bit hash of its slots (record type NV2A_PC_MANIFEST_VS,
 * 8 bytes) and its body is found on the player's machine, in the executable
 * scan (nv2a_pcache_scan_vsh) of the player's own copy. Combiner states and
 * pipeline keys are register values and fixed state, the backend's own
 * records as in the key list.
 *
 * File: the key list's format, its version string the backend's key-format
 * version followed by "; manifest <revision>". A manifest of another key
 * format is ignored (logged). Applying it appends what the local key list
 * lacks, so the pre-warm builds it; a key already there is written once. */
#define NV2A_PC_MANIFEST_VS 16u

typedef struct nv2a_pcache_manifest_stat {
    int      found;           /* a manifest was read */
    char     revision[64];
    uint32_t records;         /* in the file */
    uint32_t vs_listed, vs_resolved;   /* vertex programs by hash, and found in the executable */
    uint32_t added;           /* records new to the local list */
} nv2a_pcache_manifest_stat;

/* The manifest for `backend` ("vulkan", "metal"), as bytes (compiled into the
 * program, so it is the same on every platform, an APK included; copied), and
 * where the player's executable is. Call before the backend starts.
 * RECOMP_PCACHE_MANIFEST=<file> reads that file instead; "none" is no manifest
 * (a sweep that records what the title needs, not what is known). */
void nv2a_pcache_set_manifest(const char *backend, const void *data, size_t size);
void nv2a_pcache_set_xbe(const char *xbe_path);
/* Appends `backend`'s manifest keys to `dst` (its open key list, of key-format
 * `version`). Returns the number of records added (also in st). */
uint32_t nv2a_pcache_manifest_apply(nv2a_pcache *dst, const char *backend,
                                    const char *version, nv2a_pcache_manifest_stat *st);
/* Lowers the calling thread's priority (a background pre-warm). */
void nv2a_pcache_lower_priority(void);
/* Online CPUs (at least 1). */
int  nv2a_pcache_cpu_count(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_PCACHE_H */
