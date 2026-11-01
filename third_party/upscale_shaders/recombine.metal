#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

// Implementation of signed integer mod accurate to SPIR-V specification
template<typename Tx, typename Ty>
inline Tx spvSMod(Tx x, Ty y)
{
    Tx remainder = x - y * (x / y);
    return select(Tx(remainder + y), remainder, remainder == 0 || (x >= 0) == (y >= 0));
}

struct UpParams
{
    int2 origin;
    int2 size;
    int2 wrap;
    int mode;
    int pad0;
};

constant uint3 gl_WorkGroupSize [[maybe_unused]] = uint3(16u, 16u, 1u);

constant spvUnsafeArray<float, 4> _199 = spvUnsafeArray<float, 4>({ -0.0625, 0.5625, 0.5625, -0.0625 });

static inline __attribute__((always_inline))
int2 edge(thread const int2& p, thread const int2& s, constant UpParams& pc)
{
    int2 c = clamp(p, int2(0), s - int2(1));
    int2 w = spvSMod(spvSMod(p, s) + s, s);
    int _49;
    if (pc.wrap.x != 0)
    {
        _49 = w.x;
    }
    else
    {
        _49 = c.x;
    }
    int _62;
    if (pc.wrap.y != 0)
    {
        _62 = w.y;
    }
    else
    {
        _62 = c.y;
    }
    return int2(_49, _62);
}

static inline __attribute__((always_inline))
float4 src(thread const int2& p, constant UpParams& pc, texture2d<float> source, sampler sourceSmplr)
{
    int2 param = p;
    int2 param_1 = pc.size;
    return source.read(uint2(edge(param, param_1, pc)), 0);
}

kernel void main0(constant UpParams& pc [[buffer(0)]], texture2d<float> source [[texture(0)]], texture2d<float> ymodel [[texture(1)]], texture2d<float, access::write> destination [[texture(4)]], sampler sourceSmplr [[sampler(0)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    int2 q = int2(gl_GlobalInvocationID.xy) + pc.origin;
    if (any(q >= (pc.size * int2(2))))
    {
        return;
    }
    float ym;
    if (pc.mode == 0)
    {
        float4 s = ymodel.read(uint2((q >> int2(1))));
        int i = ((q.x & 1) * 2) + (q.y & 1);
        float _141;
        if (i == 0)
        {
            _141 = s.x;
        }
        else
        {
            float _149;
            if (i == 1)
            {
                _149 = s.y;
            }
            else
            {
                float _157;
                if (i == 2)
                {
                    _157 = s.z;
                }
                else
                {
                    _157 = s.w;
                }
                _149 = _157;
            }
            _141 = _149;
        }
        ym = _141;
    }
    else
    {
        int2 s2 = pc.size * int2(2);
        ym = 0.0;
        for (int v = 0; v < 4; v++)
        {
            float row = 0.0;
            for (int u = 0; u < 4; u++)
            {
                int2 param = q + int2(u - 2, v - 2);
                int2 param_1 = s2;
                row += (_199[u] * ymodel.read(uint2(edge(param, param_1, pc))).x);
            }
            ym += (_199[v] * row);
        }
    }
    float2 f = ((float2(q) + float2(0.5)) * 0.5) - float2(0.5);
    int2 i0 = int2(floor(f));
    float2 t = f - float2(i0);
    int2 param_2 = i0;
    int2 param_3 = i0 + int2(1, 0);
    int2 param_4 = i0 + int2(0, 1);
    int2 param_5 = i0 + int2(1);
    float3 rgb = mix(mix(src(param_2, pc, source, sourceSmplr).xyz, src(param_3, pc, source, sourceSmplr).xyz, float3(t.x)), mix(src(param_4, pc, source, sourceSmplr).xyz, src(param_5, pc, source, sourceSmplr).xyz, float3(t.x)), float3(t.y));
    float yb = dot(rgb, float3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875));
    rgb = fast::clamp(rgb + float3(fast::clamp(ym, 0.0, 1.0) - yb), float3(0.0), float3(1.0));
    float a = source.read(uint2((q >> int2(1))), 0).w;
    destination.write(float4(rgb, a), uint2(q));
}

