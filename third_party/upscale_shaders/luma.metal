#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct UpParams
{
    int2 origin;
    int2 size;
    int2 wrap;
    int mode;
    int pad0;
};

constant uint3 gl_WorkGroupSize [[maybe_unused]] = uint3(16u, 16u, 1u);

kernel void main0(constant UpParams& pc [[buffer(0)]], texture2d<float> source [[texture(0)]], texture2d<float, access::write> destination [[texture(4)]], sampler sourceSmplr [[sampler(0)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    int2 p = int2(gl_GlobalInvocationID.xy) + pc.origin;
    if (any(p >= pc.size))
    {
        return;
    }
    float3 c = source.read(uint2(p), 0).xyz;
    destination.write(float4(dot(c, float3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875)), 0.0, 0.0, 1.0), uint2(p));
}

