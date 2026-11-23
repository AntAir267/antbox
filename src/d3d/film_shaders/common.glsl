// BO-136 film filters: the parameters every pass takes (the same block as
// the texture upscaler's passes, so both backends bind them alike). MIT.
#version 450
layout(push_constant) uniform UpParams {
    ivec2 origin;   // unused here (whole-image dispatches)
    ivec2 size;     // the input image's size
    ivec2 wrap;     // unused: films clamp to the edge
    int   mode;
    int   pad0;
} pc;

layout(local_size_x = 16, local_size_y = 16) in;

ivec2 edge(ivec2 p, ivec2 s)
{
    return clamp(p, ivec2(0), s - ivec2(1));
}
