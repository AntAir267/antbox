#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

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

static inline __attribute__((always_inline))
int2 edge(thread const int2& p, thread const int2& s, constant UpParams& pc)
{
    int2 c = clamp(p, int2(0), s - int2(1));
    int2 w = spvSMod(spvSMod(p, s) + s, s);
    int _48;
    if (pc.wrap.x != 0)
    {
        _48 = w.x;
    }
    else
    {
        _48 = c.x;
    }
    int _61;
    if (pc.wrap.y != 0)
    {
        _61 = w.y;
    }
    else
    {
        _61 = c.y;
    }
    return int2(_48, _61);
}

static inline __attribute__((always_inline))
float4 hook(constant UpParams& pc, texture2d<float> MODEL1, thread uint3& gl_GlobalInvocationID, texture2d<float> MODEL2, texture2d<float> FEATURE2)
{
    float4 res = float4(-0.0425243787467479705810546875, -0.3715015351772308349609375, -0.025622785091400146484375, -0.2774516046047210693359375);
    int2 param = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0);
    int2 param_1 = pc.size;
    res += (float4x4(float4(0.023811884224414825439453125, 0.02954806573688983917236328125, -0.0066418983042240142822265625, 0.1021223962306976318359375), float4(-0.0568209178745746612548828125, -0.43551003932952880859375, -0.270052254199981689453125, -0.2060186564922332763671875), float4(-0.06896133720874786376953125, -0.16896919906139373779296875, -0.03067485056817531585693359375, -0.24612522125244140625), float4(-0.0057375836186110973358154296875, -0.18923032283782958984375, -0.02858714945614337921142578125, -0.5032613277435302734375)) * MODEL1.read(uint2(edge(param, param_1, pc))));
    int2 param_2 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0);
    int2 param_3 = pc.size;
    res += (float4x4(float4(0.546321332454681396484375, 0.097280032932758331298828125, 0.03075607679784297943115234375, 0.067805893719196319580078125), float4(-0.035606302320957183837890625, -0.701386511325836181640625, 0.18904435634613037109375, -0.103665746748447418212890625), float4(-0.17458261549472808837890625, -0.2942218780517578125, -0.0485423319041728973388671875, -0.2983124554157257080078125), float4(-0.052443183958530426025390625, -0.3261034786701202392578125, 0.3217246532440185546875, 0.19580185413360595703125)) * MODEL2.read(uint2(edge(param_2, param_3, pc))));
    int2 param_4 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0);
    int2 param_5 = pc.size;
    res += FEATURE2.read(uint2(edge(param_4, param_5, pc)));
    res = fast::max(res, float4(0.0)) + (float4(0.139133989810943603515625, 0.096032835543155670166015625, 0.623534142971038818359375, 0.1177272796630859375) * fast::min(res, float4(0.0)));
    return res;
}

kernel void main0(constant UpParams& pc [[buffer(0)]], texture2d<float> MODEL1 [[texture(0)]], texture2d<float> MODEL2 [[texture(1)]], texture2d<float> FEATURE2 [[texture(2)]], texture2d<float, access::write> destination [[texture(4)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    int2 p = int2(gl_GlobalInvocationID.xy) + pc.origin;
    if (any(p >= pc.size))
    {
        return;
    }
    destination.write(hook(pc, MODEL1, gl_GlobalInvocationID, MODEL2, FEATURE2), uint2(p));
}

