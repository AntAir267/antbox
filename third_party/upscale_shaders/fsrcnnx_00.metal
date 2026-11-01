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
    float4 res = float4(-0.15724922716617584228515625, -0.012089683674275875091552734375, 0.006148763932287693023681640625, -0.28528487682342529296875);
    int2 param = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2);
    int2 param_1 = pc.size;
    res += (float4(-0.0047900392673909664154052734375, 0.053744710981845855712890625, -2.4714463506825268268585205078125e-05, 0.0066653941757977008819580078125) * LUMA.read(uint2(edge(param, param_1, pc))).x);
    int2 param_2 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2, -1);
    int2 param_3 = pc.size;
    res += (float4(0.007314468733966350555419921875, -0.03090040385723114013671875, -0.01091813854873180389404296875, -0.00928403250873088836669921875) * LUMA.read(uint2(edge(param_2, param_3, pc))).x);
    int2 param_4 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2, 0);
    int2 param_5 = pc.size;
    res += (float4(0.059170089662075042724609375, 0.19749070703983306884765625, -0.01973575167357921600341796875, -0.054655484855175018310546875) * LUMA.read(uint2(edge(param_4, param_5, pc))).x);
    int2 param_6 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2, 1);
    int2 param_7 = pc.size;
    res += (float4(-0.001176438294351100921630859375, -0.02994510717689990997314453125, 0.02295873127877712249755859375, 0.00219088862650096416473388671875) * LUMA.read(uint2(edge(param_6, param_7, pc))).x);
    int2 param_8 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-2, 2);
    int2 param_9 = pc.size;
    res += (float4(0.0098101310431957244873046875, 0.00809954106807708740234375, -0.00304520200006663799285888671875, -0.01320355199277400970458984375) * LUMA.read(uint2(edge(param_8, param_9, pc))).x);
    int2 param_10 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1, -2);
    int2 param_11 = pc.size;
    res += (float4(-0.0168330334126949310302734375, -0.07437114417552947998046875, -0.02592616342008113861083984375, 0.02344804815948009490966796875) * LUMA.read(uint2(edge(param_10, param_11, pc))).x);
    int2 param_12 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1);
    int2 param_13 = pc.size;
    res += (float4(0.02399337850511074066162109375, 0.189654171466827392578125, 0.02077563293278217315673828125, -0.037033237516880035400390625) * LUMA.read(uint2(edge(param_12, param_13, pc))).x);
    int2 param_14 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1, 0);
    int2 param_15 = pc.size;
    res += (float4(0.009479950182139873504638671875, -0.065251119434833526611328125, -0.00042927931644953787326812744140625, -0.072621218860149383544921875) * LUMA.read(uint2(edge(param_14, param_15, pc))).x);
    int2 param_16 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1, 1);
    int2 param_17 = pc.size;
    res += (float4(0.029728479683399200439453125, -0.12101866304874420166015625, -0.02029293216764926910400390625, -0.0574462898075580596923828125) * LUMA.read(uint2(edge(param_16, param_17, pc))).x);
    int2 param_18 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(-1, 2);
    int2 param_19 = pc.size;
    res += (float4(-0.0318185277283191680908203125, 0.084077537059783935546875, 0.011045130901038646697998046875, 0.0415569432079792022705078125) * LUMA.read(uint2(edge(param_18, param_19, pc))).x);
    int2 param_20 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0, -2);
    int2 param_21 = pc.size;
    res += (float4(-0.0253141783177852630615234375, 0.116825617849826812744140625, 0.115972958505153656005859375, 0.0963164269924163818359375) * LUMA.read(uint2(edge(param_20, param_21, pc))).x);
    int2 param_22 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0, -1);
    int2 param_23 = pc.size;
    res += (float4(-0.110361583530902862548828125, -0.0276833958923816680908203125, -0.499959409236907958984375, 0.10538671910762786865234375) * LUMA.read(uint2(edge(param_22, param_23, pc))).x);
    int2 param_24 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0);
    int2 param_25 = pc.size;
    res += (float4(1.11004352569580078125, 0.06467644870281219482421875, 0.01540057174861431121826171875, 0.889158666133880615234375) * LUMA.read(uint2(edge(param_24, param_25, pc))).x);
    int2 param_26 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0, 1);
    int2 param_27 = pc.size;
    res += (float4(0.122933067381381988525390625, 0.17194688320159912109375, 0.573033809661865234375, -0.164554417133331298828125) * LUMA.read(uint2(edge(param_26, param_27, pc))).x);
    int2 param_28 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(0, 2);
    int2 param_29 = pc.size;
    res += (float4(-0.00904427282512187957763671875, -0.30239617824554443359375, -0.158949315547943115234375, 0.0418574027717113494873046875) * LUMA.read(uint2(edge(param_28, param_29, pc))).x);
    int2 param_30 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1, -2);
    int2 param_31 = pc.size;
    res += (float4(0.00319420360028743743896484375, -0.131092607975006103515625, 0.0075543406419456005096435546875, -0.001644934643991291522979736328125) * LUMA.read(uint2(edge(param_30, param_31, pc))).x);
    int2 param_32 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1, -1);
    int2 param_33 = pc.size;
    res += (float4(-0.099515028297901153564453125, -0.070192120969295501708984375, -0.01308958791196346282958984375, 0.13441701233386993408203125) * LUMA.read(uint2(edge(param_32, param_33, pc))).x);
    int2 param_34 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1, 0);
    int2 param_35 = pc.size;
    res += (float4(0.0060519003309309482574462890625, -0.1533465683460235595703125, 0.01141940057277679443359375, 0.02646839059889316558837890625) * LUMA.read(uint2(edge(param_34, param_35, pc))).x);
    int2 param_36 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1);
    int2 param_37 = pc.size;
    res += (float4(0.02440080232918262481689453125, 0.1881769001483917236328125, -0.02063511498272418975830078125, -0.0628309547901153564453125) * LUMA.read(uint2(edge(param_36, param_37, pc))).x);
    int2 param_38 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(1, 2);
    int2 param_39 = pc.size;
    res += (float4(0.007571312598884105682373046875, 0.05085943639278411865234375, 0.04304231703281402587890625, -0.01241887919604778289794921875) * LUMA.read(uint2(edge(param_38, param_39, pc))).x);
    int2 param_40 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2, -2);
    int2 param_41 = pc.size;
    res += (float4(-0.01668758690357208251953125, -0.004786551930010318756103515625, 0.000671912333928048610687255859375, 0.031680323183536529541015625) * LUMA.read(uint2(edge(param_40, param_41, pc))).x);
    int2 param_42 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2, -1);
    int2 param_43 = pc.size;
    res += (float4(-0.005846126936376094818115234375, 0.0990798473358154296875, -0.01777438260614871978759765625, -0.006612229160964488983154296875) * LUMA.read(uint2(edge(param_42, param_43, pc))).x);
    int2 param_44 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2, 0);
    int2 param_45 = pc.size;
    res += (float4(-0.09724019467830657958984375, -0.02254463732242584228515625, -0.00376935745589435100555419921875, 0.1953062713146209716796875) * LUMA.read(uint2(edge(param_44, param_45, pc))).x);
    int2 param_46 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2, 1);
    int2 param_47 = pc.size;
    res += (float4(-0.0216837190091609954833984375, -0.1824268400669097900390625, 0.00698162615299224853515625, 0.0283037684857845306396484375) * LUMA.read(uint2(edge(param_46, param_47, pc))).x);
    int2 param_48 = (int2(gl_GlobalInvocationID.xy) + pc.origin) + int2(2);
    int2 param_49 = pc.size;
    res += (float4(-0.002576799131929874420166015625, 0.045982711017131805419921875, -0.008021608926355838775634765625, 0.00841347873210906982421875) * LUMA.read(uint2(edge(param_48, param_49, pc))).x);
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

