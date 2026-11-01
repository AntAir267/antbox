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
float4 hook(constant UpParams& pc, texture2d<float> MODEL1, thread uint3& gl_GlobalInvocationID, texture2d<float> MODEL2, texture2d<float> FEATURE1)
{
    float4 res = float4(0.024564854800701141357421875, -0.446778476238250732421875, 0.01975269429385662078857421875, -0.011000041849911212921142578125);
    int2 param = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0);
    int2 param_1 = pc.size;
    res += (float4x4(float4(0.030266530811786651611328125, -0.926222145557403564453125, -0.116113476455211639404296875, -0.050690047442913055419921875), float4(0.2716045379638671875, -0.04858715832233428955078125, 0.0044713355600833892822265625, -0.4274623394012451171875), float4(0.0749531090259552001953125, -0.3700785338878631591796875, 0.0350039415061473846435546875, -0.05407865345478057861328125), float4(-0.060739003121852874755859375, -0.801990032196044921875, 0.0923245251178741455078125, 0.1258827745914459228515625)) * MODEL1.read(uint2(edge(param, param_1, pc))));
    int2 param_2 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0);
    int2 param_3 = pc.size;
    res += (float4x4(float4(-0.06491352617740631103515625, 0.081523679196834564208984375, 0.006733429618179798126220703125, 0.127742588520050048828125), float4(-0.0051357815973460674285888671875, -0.1485908329486846923828125, 0.0074226572178304195404052734375, 0.005062350071966648101806640625), float4(0.0588018335402011871337890625, -0.069255292415618896484375, 0.12887252867221832275390625, -0.09893862903118133544921875), float4(0.042793683707714080810546875, 0.0967708528041839599609375, -0.045563213527202606201171875, -0.0711275041103363037109375)) * MODEL2.read(uint2(edge(param_2, param_3, pc))));
    int2 param_4 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0);
    int2 param_5 = pc.size;
    res += FEATURE1.read(uint2(edge(param_4, param_5, pc)));
    res = fast::max(res, float4(0.0)) + (float4(0.99271869659423828125, 0.0570580027997493743896484375, 1.322675228118896484375, 1.00694668292999267578125) * fast::min(res, float4(0.0)));
    return res;
}

kernel void main0(constant UpParams& pc [[buffer(0)]], texture2d<float> MODEL1 [[texture(0)]], texture2d<float> MODEL2 [[texture(1)]], texture2d<float> FEATURE1 [[texture(2)]], texture2d<float, access::write> destination [[texture(4)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    int2 p = int2(gl_GlobalInvocationID.xy) + pc.origin;
    if (any(p >= pc.size))
    {
        return;
    }
    destination.write(hook(pc, MODEL1, gl_GlobalInvocationID, MODEL2, FEATURE1), uint2(p));
}

