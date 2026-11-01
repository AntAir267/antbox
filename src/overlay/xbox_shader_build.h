/*
 * xbox_shader_build.h - a shader pre-build, as the settings overlay sees it.
 *
 * A host that can build its shaders ahead of time (it plays the title's
 * scenes in a child process, nv2a_pcache.h) gives the overlay these calls.
 * The overlay then has an action for it on the Graphics page, with the
 * progress. Nothing starts by itself and nothing is offered at the first
 * start (the installer's option is the place for that); closing the overlay
 * stops a build (the title is paused while the overlay is open, and a build
 * beside a running title would cost it frames). No SDL here: a host's C sources include it.
 */
#ifndef XBOX_SHADER_BUILD_H
#define XBOX_SHADER_BUILD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XBOX_SHADERS_IDLE = 0,      /* never built here */
    XBOX_SHADERS_RUNNING,
    XBOX_SHADERS_DONE,          /* built (now or by an earlier run) */
    XBOX_SHADERS_FAILED         /* the last build stopped; `text` says why */
} xbox_shader_build_state;

typedef struct xbox_shader_build_ops {
    int  (*start)(void);                /* 1 started */
    void (*cancel)(void);               /* asks a running build to stop */
    /* The state; while running, how far (0..1) and what it is doing. */
    int  (*poll)(float *fraction, char *text, unsigned n);
    const char *cost;                   /* what it does and costs, a sentence or two */
} xbox_shader_build_ops;

/* Any time; NULL takes it away. `ops` must stay valid. */
void xbox_OverlaySetShaderBuild(const xbox_shader_build_ops *ops);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_SHADER_BUILD_H */
