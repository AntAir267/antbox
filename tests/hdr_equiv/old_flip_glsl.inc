/* The Vulkan flip pass's shader as it was before BO-244 (toolkit 4b531c78,
 * d3d/nv2a_pb_vulkan.c), kept so hdr_equiv shows the shared header gives
 * the same values. */
static const char s_old_flip_glsl[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    /* a: x the peak in paper whites, y paper white / 10,000 nits, z the
     * highlight gain, w 1 = the sRGB curve; b: x the shadows' power, y the
     * shoulder's softness, z the black level / 10,000 nits. */
    "layout(push_constant) uniform PC { vec4 a; vec4 b; } pc;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec3 c = max(texelFetch(src, ivec2(gl_FragCoord.xy), 0).rgb, vec3(0.0));\n"
    "    vec3 l = pc.a.w < 0.5 ? pow(c, vec3(2.2))\n"
    "             : mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));\n"
    "    float y = dot(l, vec3(0.2126, 0.7152, 0.0722));\n"
    "    if (y > 0.0 && y < 0.18 && pc.b.x != 1.0) l *= 0.18 * pow(y / 0.18, pc.b.x) / y;\n"
    "    float mx = max(l.r, max(l.g, l.b)), m = max(pc.a.x, 1.0);\n"
    "    if (mx > 1.0) {\n"
    "        float x = 1.0 + pc.a.z * (mx - 1.0);\n"
    "        float v = 1.0;\n"
    "        if (m > 1.0001) {\n"
    "            float k = m - 1.0, e = 1.0 + k * (1.0 - exp(-(x - 1.0) / k));\n"
    /* BO-195 P2: past 2.5x a blend towards a slower rational curve (the
     * same slope at 1, the same peak), so a 4-6x core keeps a gradient. */
    "            float r = 1.0 + k * (x - 1.0) / ((x - 1.0) + k);\n"
    "            v = mix(e, r, pc.b.y * clamp(k - 0.5, 0.0, 1.0) * smoothstep(2.5, 4.0, x));\n"
    "        }\n"
    "        l *= v / mx;\n"
    "    }\n"
    /* BO-195 P7: black level, nits' = black + (1 - black / peak) x nits */
    "    l *= pc.a.y;\n"
    "    l = pc.b.z + (1.0 - pc.b.z / (pc.a.x * pc.a.y)) * l;\n"
    "    l = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956) * l;\n"
    "    vec3 q = pow(clamp(l, 0.0, 1.0), vec3(0.1593017578125));\n"
    "    o = vec4(pow((0.8359375 + 18.8515625 * q) / (1.0 + 18.6875 * q), vec3(78.84375)), 1.0);\n"
    "}\n";
