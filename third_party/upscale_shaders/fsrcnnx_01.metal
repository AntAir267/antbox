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
float4 hook(constant UpParams& pc, texture2d<float> LUMA, thread uint3& gl_GlobalInvocationID)
{
    float4 res = float4(0.05414475500583648681640625, 0.008830674923956394195556640625, -0.011238957755267620086669921875, -0.012786095030605792999267578125);
    int2 param = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2);
    int2 param_1 = pc.size;
    res += (float4(0.01426600106060504913330078125, 0.0137931071221828460693359375, 0.006118810735642910003662109375, -0.010413422249257564544677734375) * LUMA.read(uint2(edge(param, param_1, pc))).x);
    int2 param_2 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2, -1);
    int2 param_3 = pc.size;
    res += (float4(0.014729280956089496612548828125, -0.0289912857115268707275390625, 0.02667694352567195892333984375, 0.0933856964111328125) * LUMA.read(uint2(edge(param_2, param_3, pc))).x);
    int2 param_4 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2, 0);
    int2 param_5 = pc.size;
    res += (float4(-0.17343382537364959716796875, 0.111631669104099273681640625, -0.19731573760509490966796875, -0.0581855811178684234619140625) * LUMA.read(uint2(edge(param_4, param_5, pc))).x);
    int2 param_6 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2, 1);
    int2 param_7 = pc.size;
    res += (float4(0.034750722348690032958984375, -0.03415666520595550537109375, 0.0061667622067034244537353515625, 0.007525888271629810333251953125) * LUMA.read(uint2(edge(param_6, param_7, pc))).x);
    int2 param_8 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2, 2);
    int2 param_9 = pc.size;
    res += (float4(0.006988436914980411529541015625, -0.01942502148449420928955078125, 0.008083012886345386505126953125, -0.0036874092184007167816162109375) * LUMA.read(uint2(edge(param_8, param_9, pc))).x);
    int2 param_10 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1, -2);
    int2 param_11 = pc.size;
    res += (float4(0.02337642014026641845703125, 0.034474499523639678955078125, 0.01621459424495697021484375, 0.097952999174594879150390625) * LUMA.read(uint2(edge(param_10, param_11, pc))).x);
    int2 param_12 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1);
    int2 param_13 = pc.size;
    res += (float4(0.12807969748973846435546875, -0.101833917200565338134765625, -0.01329771988093852996826171875, -0.001947462209500372409820556640625) * LUMA.read(uint2(edge(param_12, param_13, pc))).x);
    int2 param_14 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1, 0);
    int2 param_15 = pc.size;
    res += (float4(0.4286882579326629638671875, 0.122267775237560272216796875, 0.704669415950775146484375, 0.094547569751739501953125) * LUMA.read(uint2(edge(param_14, param_15, pc))).x);
    int2 param_16 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1, 1);
    int2 param_17 = pc.size;
    res += (float4(0.110744178295135498046875, -0.013443307019770145416259765625, -0.01749009080231189727783203125, -0.16864454746246337890625) * LUMA.read(uint2(edge(param_16, param_17, pc))).x);
    int2 param_18 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1, 2);
    int2 param_19 = pc.size;
    res += (float4(0.032147862017154693603515625, 0.006535784341394901275634765625, 0.03008059971034526824951171875, 0.042011328041553497314453125) * LUMA.read(uint2(edge(param_18, param_19, pc))).x);
    int2 param_20 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0, -2);
    int2 param_21 = pc.size;
    res += (float4(-0.124034158885478973388671875, 0.095030330121517181396484375, -0.012964845634996891021728515625, -0.2681856453418731689453125) * LUMA.read(uint2(edge(param_20, param_21, pc))).x);
    int2 param_22 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0, -1);
    int2 param_23 = pc.size;
    res += (float4(0.484696090221405029296875, 0.0351924635469913482666015625, 0.022304333746433258056640625, -0.12736307084560394287109375) * LUMA.read(uint2(edge(param_22, param_23, pc))).x);
    int2 param_24 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0);
    int2 param_25 = pc.size;
    res += (float4(-1.93795073032379150390625, -0.24444420635700225830078125, 0.0291962660849094390869140625, -0.383557856082916259765625) * LUMA.read(uint2(edge(param_24, param_25, pc))).x);
    int2 param_26 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0, 1);
    int2 param_27 = pc.size;
    res += (float4(0.63962781429290771484375, -0.07659383118152618408203125, -0.0552659817039966583251953125, 0.439354598522186279296875) * LUMA.read(uint2(edge(param_26, param_27, pc))).x);
    int2 param_28 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0, 2);
    int2 param_29 = pc.size;
    res += (float4(-0.19697280228137969970703125, -0.0607173256576061248779296875, 0.01311135478317737579345703125, 0.054201781749725341796875) * LUMA.read(uint2(edge(param_28, param_29, pc))).x);
    int2 param_30 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1, -2);
    int2 param_31 = pc.size;
    res += (float4(0.009169600903987884521484375, -0.0031533432193100452423095703125, -0.036877758800983428955078125, -0.0459998287260532379150390625) * LUMA.read(uint2(edge(param_30, param_31, pc))).x);
    int2 param_32 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1, -1);
    int2 param_33 = pc.size;
    res += (float4(0.109699249267578125, 0.2597902715206146240234375, 0.03048696927726268768310546875, -0.01952007226645946502685546875) * LUMA.read(uint2(edge(param_32, param_33, pc))).x);
    int2 param_34 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1, 0);
    int2 param_35 = pc.size;
    res += (float4(0.2889648377895355224609375, -0.427559196949005126953125, -0.741415679454803466796875, 0.26954424381256103515625) * LUMA.read(uint2(edge(param_34, param_35, pc))).x);
    int2 param_36 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1);
    int2 param_37 = pc.size;
    res += (float4(0.0892018377780914306640625, -0.02291375584900379180908203125, 0.0244414471089839935302734375, -0.1926898956298828125) * LUMA.read(uint2(edge(param_36, param_37, pc))).x);
    int2 param_38 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1, 2);
    int2 param_39 = pc.size;
    res += (float4(0.05763585865497589111328125, 0.00278469733893871307373046875, -0.00368615053594112396240234375, -0.02535471133887767791748046875) * LUMA.read(uint2(edge(param_38, param_39, pc))).x);
    int2 param_40 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2, -2);
    int2 param_41 = pc.size;
    res += (float4(0.01596240699291229248046875, 0.0319602824747562408447265625, 0.0019470085389912128448486328125, 0.008978049270808696746826171875) * LUMA.read(uint2(edge(param_40, param_41, pc))).x);
    int2 param_42 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2, -1);
    int2 param_43 = pc.size;
    res += (float4(0.0552792511880397796630859375, 0.05430543422698974609375, 0.01340628229081630706787109375, 0.0545728243887424468994140625) * LUMA.read(uint2(edge(param_42, param_43, pc))).x);
    int2 param_44 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2, 0);
    int2 param_45 = pc.size;
    res += (float4(-0.117009222507476806640625, 0.1963327825069427490234375, 0.150389015674591064453125, 0.18918283283710479736328125) * LUMA.read(uint2(edge(param_44, param_45, pc))).x);
    int2 param_46 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2, 1);
    int2 param_47 = pc.size;
    res += (float4(-0.00844217836856842041015625, 0.129701793193817138671875, -0.03306008875370025634765625, -0.094206370413303375244140625) * LUMA.read(uint2(edge(param_46, param_47, pc))).x);
    int2 param_48 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2);
    int2 param_49 = pc.size;
    res += (float4(0.01184404082596302032470703125, -0.033787585794925689697265625, 0.005506346933543682098388671875, 0.0254479162395000457763671875) * LUMA.read(uint2(edge(param_48, param_49, pc))).x);
    return res;
}

kernel void main0(constant UpParams& pc [[buffer(0)]], texture2d<float> LUMA [[texture(0)]], texture2d<float, access::write> destination [[texture(4)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    int2 p = int2(gl_GlobalInvocationID.xy) + pc.origin;
    if (any(p >= pc.size))
    {
        return;
    }
    destination.write(hook(pc, LUMA, gl_GlobalInvocationID), uint2(p));
}

