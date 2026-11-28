// The title's Direct3D 8 calls drawn with Vulkan 1.1, dynamic rendering and a timeline semaphore. The SDK's
// translated shaders are HLSL, which glslang turns into SPIR-V (vk_shader.cpp).
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dxgiformat.h>

#include "app/app.h"
#include "common/aspect.h"
#include "common/options.h"
#include "core/log.h"
#include "d3d/gpu_frame.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "host.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "vk_shader.h"

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr VkDeviceSize kUploadSize = 32ull << 20;  // per frame: vertices, indices, constants, texture staging
constexpr VkDeviceSize kUboRange = 3072;           // b0 and b1: 192 vertex constants, DrawConstants under it
constexpr VkDeviceSize kUboAlign = 256;
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kSetsPerPool = 1024;
static_assert(sizeof(DrawConstants) <= kUboRange);

const char* kClearHlsl = R"(
cbuffer B : register(b1) { float4 col; };
struct O { float4 p : SV_Position; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 psmain(O i) : SV_Target { return col; }
)";

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// Batched 2D touch overlay shaders (split for minimal VGPRs & 1-draw-call batching per run):
// 1. Ultra-lightweight textured quad shader (SVG button skins & Roboto glyphs)
const char* kOverlayTexHlsl = R"(
Texture2D img : register(t0);
SamplerState ls : register(s0);
struct I {
    float2 v0 : ATTR0;
    float2 v1 : ATTR1;
    float4 v2 : ATTR2;
};
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; };
O vs(I i) {
    O o;
    o.p = float4(i.v0, 0.0, 1.0);
    o.uv = i.v1;
    o.col = i.v2;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = img.SampleLevel(ls, i.uv, 0) * i.col;
    return float4(c.rgb * c.a, c.a);
}
)";

// 2. SDF rounded panel / border / clockwise progress ring shader (bound only when SDF quads exist)
const char* kOverlaySdfHlsl = R"(
struct I {
    float2 v0 : ATTR0;
    float2 v1 : ATTR1;
    float4 v2 : ATTR2;
    float4 v3 : ATTR3;
    float4 v4 : ATTR4;
    float v5 : ATTR5;
};
struct O {
    float4 p : SV_Position;
    float2 loc : TEXCOORD0;
    float4 tint : TEXCOORD1;
    float4 geom : TEXCOORD2;
    float4 bcol : TEXCOORD3;
    float mode : TEXCOORD4;
};
O vs(I i) {
    O o;
    o.p = float4(i.v0, 0.0, 1.0);
    o.loc = i.v1;
    o.tint = i.v2;
    o.geom = i.v3;
    o.bcol = i.v4;
    o.mode = i.v5;
    return o;
}
float4 psmain(O i) : SV_Target {
    if (i.mode > 1.5) {
        float2 p = (i.loc - 0.5) * i.geom.xy;
        float outerR = min(i.geom.x, i.geom.y) * 0.5 - 1.5;
        float innerR = max(outerR - max(i.geom.w, 2.0), 1.0);
        float dist = length(p);
        float ringMask = saturate(0.5 - (dist - outerR)) * saturate(0.5 - (innerR - dist));
        float ang = atan2(p.x, -p.y);
        float frac = (ang < 0.0) ? (ang * 0.15915494 + 1.0) : (ang * 0.15915494);
        float prog = saturate(i.geom.z);
        float fillMask = (prog >= 0.999) ? 1.0 : ((prog <= 0.001) ? 0.0 : saturate((prog - frac) * 90.0));
        float fa = i.tint.a * ringMask * fillMask;
        float ba = i.bcol.a * ringMask * (1.0 - fillMask);
        float outA = fa + ba;
        float3 outRgb = (outA > 1e-4) ? ((i.tint.rgb * fa + i.bcol.rgb * ba) / outA) : float3(0.0, 0.0, 0.0);
        return float4(outRgb * outA, outA);
    }
    float2 halfSz = max(i.geom.xy * 0.5, float2(1.0, 1.0));
    float2 p = (i.loc - 0.5) * i.geom.xy;
    float r = min(i.geom.z, min(halfSz.x, halfSz.y));
    float2 q = abs(p) - (halfSz - float2(r, r));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float fillMask = saturate(0.5 - d);
    float4 outCol = float4(i.tint.rgb, i.tint.a * fillMask);
    if (i.geom.w > 0.01) {
        float innerMask = saturate(0.5 - (d + i.geom.w));
        float borderMask = saturate(fillMask - innerMask);
        float fa = i.tint.a * innerMask;
        float ba = i.bcol.a * borderMask;
        float outA = ba + fa * (1.0 - ba);
        float3 outRgb = (outA > 1e-4) ? ((i.bcol.rgb * ba + i.tint.rgb * fa * (1.0 - ba)) / outA) : float3(0.0, 0.0, 0.0);
        outCol = float4(outRgb, outA);
    }
    return float4(outCol.rgb * outCol.a, outCol.a);
}
)";

struct OverlayTexVert {
    float x, y, u, v, r, g, b, a;
};
struct OverlaySdfVert {
    float x, y, lx, ly, r, g, b, a, gw, gh, rad, bw, br, bg, bb, ba, mode;
};
struct OverlayRun {
    bool sdf = false;
    uint32_t first = 0;
    uint32_t count = 0;
};

// Builds a specialized post-process / screen-filter pixel shader for only the active filter & features.
// Eliminates unused FXAA/HDR/filter branches (cutting VGPR pressure on mobile GPUs), moves gamma LUT
// evaluation to a single pass per pixel instead of per neighborhood tap, and replaces trig/fmod Noise,
// CRT scanlines, and aperture grille modulo with pure ALU arithmetic.
const std::string& FilterHlsl(uint32_t kind, uint32_t mode, bool use_gamma, bool use_hdr, bool use_fxaa) {
    static std::unordered_map<uint32_t, std::string> cache;
    const uint32_t key = (kind & 0x3u) | ((mode & 0x7u) << 2) | (use_gamma ? 0x20u : 0u) |
                         (use_hdr ? 0x40u : 0u) | (use_fxaa ? 0x80u : 0u);
    if (auto it = cache.find(key); it != cache.end()) return it->second;

    std::string s = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 crt; float4 look; float4 src; float4 post; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float3 Fetch(float2 uv) { return img.SampleLevel(ls, uv, 0).rgb; }
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float Noise(float2 p) {
    float3 q = frac(float3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}
float Corners(float2 uv, float k) { return pow(max(16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y), 1e-6), k); }
)";
    if (use_gamma) {
        s += R"(
float3 ApplyGamma(float3 c) {
    return float3(lut.SampleLevel(ps, float2(c.r, 0.5), 0).r,
                  lut.SampleLevel(ps, float2(c.g, 0.5), 0).g,
                  lut.SampleLevel(ps, float2(c.b, 0.5), 0).b);
}
)";
    } else {
        s += "float3 ApplyGamma(float3 c) { return c; }\n";
    }
    if (use_hdr) {
        s += R"(
float3 Tone(float3 c) {
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    float shadow = 1.0 - smoothstep(0.0, 0.8, l);
    c = lerp(l, c, 1.0 + shadow);
    float3 mapped = pow(max(c / (c + 1.0), 1e-6), 1.0 / 2.2);
    c = lerp(c, mapped, 0.8);
    return saturate(c * (1.0 - shadow * 0.5));
}
)";
    } else {
        s += "float3 Tone(float3 c) { return c; }\n";
    }
    if (use_fxaa && kind != 2u) {
        s += R"(
float3 Fxaa(float2 uv) {
    float2 px = src.xy;
    float3 m = Fetch(uv);
    float nw = Luma(Fetch(uv + float2(-1, -1) * px)), ne = Luma(Fetch(uv + float2(1, -1) * px));
    float sw = Luma(Fetch(uv + float2(-1,  1) * px)), se = Luma(Fetch(uv + float2(1,  1) * px));
    float lm = Luma(m);
    float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
    if (hi - lo < max(0.0312, hi * 0.125)) return m;
    float2 dir = float2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * px;
    float3 a = 0.5 * (Fetch(uv + dir * (1.0 / 3.0 - 0.5)) + Fetch(uv + dir * (2.0 / 3.0 - 0.5)));
    float3 b = a * 0.5 + 0.25 * (Fetch(uv - dir * 0.5) + Fetch(uv + dir * 0.5));
    float lb = Luma(b);
    return lb < lo || lb > hi ? a : b;
}
float3 Frame(float2 uv) { return Tone(ApplyGamma(Fxaa(uv))); }
)";
    } else {
        s += "float3 Frame(float2 uv) { return Tone(ApplyGamma(Fetch(uv))); }\n";
    }

    if (kind == 0u) {
        s += "float4 psmain(O i) : SV_Target { return float4(Frame(i.uv), 1.0); }\n";
    } else if (kind == 1u) {
        s += R"(
float4 psmain(O i) : SV_Target {
    float2 cc = i.uv * 2.0 - 1.0;
    cc *= 1.0 + crt.z * cc.yx * cc.yx;
    if (any(abs(cc) > 1.0)) return float4(0, 0, 0, 1);
    float2 uv = cc * 0.5 + 0.5;
    float3 c = Frame(uv);
    float d = abs(frac(uv.y * crt.x) - 0.5) * 2.0;
    float gap2 = d * d * (3.0 - 2.0 * d);
    float scan = 1.0 - crt.y * gap2;
    scan = lerp(scan, 1.0, saturate(Luma(c)) * 0.4);
    float gx = frac(floor(i.p.x) * 0.33333334 + 0.05);
    float3 grille = 1.0 - crt.w * float3(gx >= 0.34, gx < 0.34 || gx >= 0.67, gx < 0.67);
    return float4(saturate(c * scan * grille * Corners(uv, look.x) * look.y), 1.0);
}
)";
    } else if (kind == 2u) {
        s += R"(
float4 psmain(O i) : SV_Target {
    float3 c = Fetch(i.uv);
    float3 n = Fetch(i.uv - float2(0.0, src.y)), sv = Fetch(i.uv + float2(0.0, src.y));
    float3 w = Fetch(i.uv - float2(src.x, 0.0)), e = Fetch(i.uv + float2(src.x, 0.0));
    float3 lo = min(c, min(min(n, sv), min(w, e))), hi = max(c, max(max(n, sv), max(w, e)));
    float3 amp = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, 1e-4)));
    return float4(Tone(ApplyGamma(saturate(c + (4.0 * c - n - sv - w - e) * amp * 0.35))), 1.0);
}
)";
    } else {
        // pscolor specialized by mode (0: vibrant, 1: noir, 2: sepia, 3: night vision, 4: VHS)
        if (mode == 0u) {
            s += R"(
float4 psmain(O i) : SV_Target {
    float3 c = Frame(i.uv);
    c = lerp(Luma(c), c, 1.35);
    c = (c - 0.5) * 1.08 + 0.5;
    return float4(saturate(c), 1.0);
}
)";
        } else if (mode == 1u) {
            s += R"(
float4 psmain(O i) : SV_Target {
    float3 c = Frame(i.uv);
    float l = saturate((Luma(c) - 0.5) * 1.35 + 0.5);
    l += (Noise(i.p.xy + frac(src.z) * 97.0) - 0.5) * 0.05;
    l = saturate(l * Corners(i.uv, 0.35));
    return float4(l, l, l, 1.0);
}
)";
        } else if (mode == 2u) {
            s += R"(
float4 psmain(O i) : SV_Target {
    float3 c = Frame(i.uv);
    c = float3(dot(c, float3(0.393, 0.769, 0.189)), dot(c, float3(0.349, 0.686, 0.168)),
               dot(c, float3(0.272, 0.534, 0.131)));
    c *= Corners(i.uv, 0.2);
    return float4(saturate(c), 1.0);
}
)";
        } else if (mode == 3u) {
            s += R"(
float4 psmain(O i) : SV_Target {
    float3 c = Frame(i.uv);
    float l = pow(max(Luma(c), 1e-6), 0.7) * 1.5;
    l += (Noise(i.p.xy + frac(src.z) * 131.0) - 0.5) * 0.12;
    l *= 1.0 - 0.12 * step(0.5, frac(i.p.y * 0.5));
    return float4(saturate(float3(0.25, 1.0, 0.35) * l * Corners(i.uv, 0.6)), 1.0);
}
)";
        } else {
            s += R"(
float4 psmain(O i) : SV_Target {
    float2 uv = i.uv;
    float t = src.z;
    float row = floor(uv.y * 240.0);
    float band = saturate(1.0 - abs(frac(uv.y - t * 0.07) - 0.5) * 12.0);
    uv.x += (Noise(float2(row, floor(t * 30.0))) - 0.5) * (0.0015 + band * 0.006);
    float off = src.x * 2.5;
    float3 c;
    c.r = Fetch(uv + float2(off, 0.0)).r;
    c.g = Fetch(uv).g;
    c.b = Fetch(uv - float2(off, 0.0)).b;
    c = Tone(ApplyGamma(c));
    c = lerp(Luma(c), c, 0.8) * 0.95 + 0.03;
    c += (Noise(i.p.xy + frac(t) * 173.0) - 0.5) * 0.06;
    return float4(saturate(c), 1.0);
}
)";
        }
    }
    return cache.emplace(key, std::move(s)).first->second;
}

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

// ---- the console's values as Vulkan's

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0xF005: return VK_BLEND_OP_REVERSE_SUBTRACT;  // D3DBLENDOP_REVSUBTRACTSIGNED
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;  // includes 0x8006 ADD and 0xF006 ADDSIGNED
    }
}

bool IsSignedBlendOp(uint32_t v) { return v == 0xF005 || v == 0xF006; }

// D3DCMP_NEVER 0x200 to ALWAYS 0x207: VkCompareOp has the same order, and the shaders take the same numbers
uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }
VkCompareOp Cmp(uint32_t v) { return VkCompareOp(CmpIndex(v)); }

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// the input assembler's formats the SDK's vertex.cpp names
VkFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// ---- DXT on the CPU, for GPUs without BC textures (most phones)

inline uint32_t Rgb565Bgra(uint16_t c) {
    uint32_t r = (c >> 11) & 31u;
    uint32_t g = (c >> 5) & 63u;
    uint32_t b = c & 31u;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

inline uint32_t LerpRgbDiv3(uint32_t c0, uint32_t c1) {
    uint32_t b = (((c0 & 0xFFu) * 2u + (c1 & 0xFFu)) * 21846u) >> 16;
    uint32_t g = ((((c0 >> 8) & 0xFFu) * 2u + ((c1 >> 8) & 0xFFu)) * 21846u) >> 16;
    uint32_t r = ((((c0 >> 16) & 0xFFu) * 2u + ((c1 >> 16) & 0xFFu)) * 21846u) >> 16;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

inline uint32_t AvgRgbDiv2(uint32_t c0, uint32_t c1) {
    uint32_t b = ((c0 & 0xFFu) + (c1 & 0xFFu)) >> 1;
    uint32_t g = (((c0 >> 8) & 0xFFu) + ((c1 >> 8) & 0xFFu)) >> 1;
    uint32_t r = (((c0 >> 16) & 0xFFu) + ((c1 >> 16) & 0xFFu)) >> 1;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Compute 4-entry packed BGRA palette for a 4x4 BC1/2/3 colour block.
inline void DecodeColourPalette(const uint8_t* b, bool dxt1, uint32_t pal[4]) {
    uint16_t c0 = uint16_t(b[0] | (uint16_t(b[1]) << 8));
    uint16_t c1 = uint16_t(b[2] | (uint16_t(b[3]) << 8));
    pal[0] = Rgb565Bgra(c0);
    pal[1] = Rgb565Bgra(c1);
    if (c0 > c1 || !dxt1) {
        pal[2] = LerpRgbDiv3(pal[0], pal[1]);
        pal[3] = LerpRgbDiv3(pal[1], pal[0]);
    } else {
        pal[2] = AvgRgbDiv2(pal[0], pal[1]);
        pal[3] = 0;
    }
}

// Lightweight persistent worker pool for parallel CPU DXT decoding on mobile GPUs without BC support (e.g. Mali-G57).
class DxtWorkerPool {
public:
    ~DxtWorkerPool() { Shutdown(); }

    void RunRange(uint32_t total_rows, const std::function<void(uint32_t, uint32_t)>& fn) {
        EnsureStarted();
        if (workers_.empty() || total_rows < 32u) {
            fn(0, total_rows);
            return;
        }
        const uint32_t parts = uint32_t(workers_.size()) + 1u;
        const uint32_t chunk = (total_rows + parts - 1u) / parts;
        {
            std::lock_guard<std::mutex> lk(mu_);
            task_fn_ = &fn;
            task_total_ = total_rows;
            task_chunk_ = chunk;
            next_worker_idx_ = 1u;
            active_workers_ = uint32_t(workers_.size());
            ++generation_;
        }
        cv_start_.notify_all();
        // Calling thread executes slice 0 immediately.
        fn(0, std::min(chunk, total_rows));
        std::unique_lock<std::mutex> lk(mu_);
        cv_done_.wait(lk, [&] { return active_workers_ == 0u; });
        task_fn_ = nullptr;
    }

private:
    void EnsureStarted() {
        if (started_) return;
        started_ = true;
        unsigned hw = std::thread::hardware_concurrency();
        unsigned n = hw >= 6u ? 3u : (hw >= 4u ? 2u : (hw >= 2u ? 1u : 0u));
        workers_.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    void Shutdown() {
        if (!started_) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_start_.notify_all();
        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    void WorkerLoop() {
        uint64_t seen_gen = 0;
        for (;;) {
            const std::function<void(uint32_t, uint32_t)>* fn = nullptr;
            uint32_t begin = 0, end = 0;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_start_.wait(lk, [&] { return stop_ || generation_ != seen_gen; });
                if (stop_) return;
                seen_gen = generation_;
                fn = task_fn_;
                uint32_t idx = next_worker_idx_++;
                begin = idx * task_chunk_;
                end = std::min(begin + task_chunk_, task_total_);
            }
            if (fn && begin < end) (*fn)(begin, end);
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (--active_workers_ == 0u) cv_done_.notify_one();
            }
        }
    }

    bool started_ = false;
    bool stop_ = false;
    std::mutex mu_;
    std::condition_variable cv_start_;
    std::condition_variable cv_done_;
    std::vector<std::thread> workers_;
    const std::function<void(uint32_t, uint32_t)>* task_fn_ = nullptr;
    uint32_t task_total_ = 0;
    uint32_t task_chunk_ = 0;
    uint32_t next_worker_idx_ = 0;
    uint32_t active_workers_ = 0;
    uint64_t generation_ = 0;
};

void DecodeDxtRows(Host host, const uint8_t* in, size_t in_size, uint32_t row_pitch, uint32_t w, uint32_t h,
                   uint32_t s, uint32_t bw, uint32_t bh, uint32_t by_begin, uint32_t by_end, uint32_t* dst_slice) {
    uint32_t block = host == Host::BC1 ? 8 : 16;
    uint32_t pal[4];
    for (uint32_t by = by_begin; by < by_end; by++) {
        uint32_t max_y = std::min(4u, h - by * 4u);
        for (uint32_t bx = 0; bx < bw; bx++) {
            size_t at = (size_t(s) * bh + by) * row_pitch + size_t(bx) * block;
            if (at + block > in_size) {
                for (uint32_t y = 0; y < max_y; y++)
                    std::memset(dst_slice + (by * 4u + y) * w + bx * 4u, 0, std::min(4u, w - bx * 4u) * 4u);
                continue;
            }
            const uint8_t* b = &in[at];
            const uint8_t* cb = (host == Host::BC1) ? b : (b + 8);
            DecodeColourPalette(cb, host == Host::BC1, pal);
            uint32_t bits = uint32_t(cb[4]) | (uint32_t(cb[5]) << 8) | (uint32_t(cb[6]) << 16) |
                            (uint32_t(cb[7]) << 24);
            uint32_t max_x = std::min(4u, w - bx * 4u);
            uint32_t* block_dst = dst_slice + size_t(by * 4u) * w + bx * 4u;

            if (host == Host::BC1) {
                if (max_x == 4u && max_y == 4u) {
                    for (uint32_t y = 0; y < 4u; y++) {
                        uint32_t* row = block_dst + y * w;
                        row[0] = pal[bits & 3u];
                        row[1] = pal[(bits >> 2) & 3u];
                        row[2] = pal[(bits >> 4) & 3u];
                        row[3] = pal[(bits >> 6) & 3u];
                        bits >>= 8;
                    }
                } else {
                    for (uint32_t y = 0; y < max_y; y++) {
                        uint32_t* row = block_dst + y * w;
                        for (uint32_t x = 0; x < max_x; x++) row[x] = pal[(bits >> ((y * 4u + x) * 2u)) & 3u];
                    }
                }
            } else if (host == Host::BC2) {
                if (max_x == 4u && max_y == 4u) {
                    for (uint32_t y = 0; y < 4u; y++) {
                        uint32_t* row = block_dst + y * w;
                        uint16_t ab = uint16_t(b[y * 2u]) | (uint16_t(b[y * 2u + 1u]) << 8);
                        row[0] = (pal[bits & 3u] & 0x00FFFFFFu) | (uint32_t((ab & 15u) * 17u) << 24);
                        row[1] = (pal[(bits >> 2) & 3u] & 0x00FFFFFFu) | (uint32_t(((ab >> 4) & 15u) * 17u) << 24);
                        row[2] = (pal[(bits >> 4) & 3u] & 0x00FFFFFFu) | (uint32_t(((ab >> 8) & 15u) * 17u) << 24);
                        row[3] = (pal[(bits >> 6) & 3u] & 0x00FFFFFFu) | (uint32_t(((ab >> 12) & 15u) * 17u) << 24);
                        bits >>= 8;
                    }
                } else {
                    for (uint32_t y = 0; y < max_y; y++) {
                        uint32_t* row = block_dst + y * w;
                        for (uint32_t x = 0; x < max_x; x++) {
                            uint32_t i = y * 4u + x;
                            uint32_t a = ((b[i >> 1] >> ((i & 1u) * 4u)) & 15u) * 17u;
                            row[x] = (pal[(bits >> (i * 2u)) & 3u] & 0x00FFFFFFu) | (a << 24);
                        }
                    }
                }
            } else {
                uint32_t a[8] = {b[0], b[1]};
                if (a[0] > a[1]) {
                    for (uint32_t i = 1; i < 7u; i++) a[i + 1u] = (((7u - i) * a[0] + i * a[1]) * 9363u) >> 16;
                } else {
                    for (uint32_t i = 1; i < 5u; i++) a[i + 1u] = (((5u - i) * a[0] + i * a[1]) * 13108u) >> 16;
                    a[6] = 0;
                    a[7] = 255u;
                }
                uint64_t abits = uint64_t(b[2]) | (uint64_t(b[3]) << 8) | (uint64_t(b[4]) << 16) |
                                 (uint64_t(b[5]) << 24) | (uint64_t(b[6]) << 32) | (uint64_t(b[7]) << 40);
                if (max_x == 4u && max_y == 4u) {
                    for (uint32_t y = 0; y < 4u; y++) {
                        uint32_t* row = block_dst + y * w;
                        row[0] = (pal[bits & 3u] & 0x00FFFFFFu) | (a[abits & 7u] << 24);
                        row[1] = (pal[(bits >> 2) & 3u] & 0x00FFFFFFu) | (a[(abits >> 3) & 7u] << 24);
                        row[2] = (pal[(bits >> 4) & 3u] & 0x00FFFFFFu) | (a[(abits >> 6) & 7u] << 24);
                        row[3] = (pal[(bits >> 6) & 3u] & 0x00FFFFFFu) | (a[(abits >> 9) & 7u] << 24);
                        bits >>= 8;
                        abits >>= 12;
                    }
                } else {
                    for (uint32_t y = 0; y < max_y; y++) {
                        uint32_t* row = block_dst + y * w;
                        for (uint32_t x = 0; x < max_x; x++) {
                            uint32_t i = y * 4u + x;
                            row[x] = (pal[(bits >> (i * 2u)) & 3u] & 0x00FFFFFFu) |
                                     (a[(abits >> (i * 3u)) & 7u] << 24);
                        }
                    }
                }
            }
        }
    }
}

// a level of DXT1, 3 or 5 blocks (bw x bh per slice, row_pitch bytes a block row) into w x h BGRA texels
bool DecodeDxt(Host host, const std::vector<uint8_t>& in, uint32_t row_pitch, uint32_t w, uint32_t h, uint32_t z,
               std::vector<uint8_t>& out, DxtWorkerPool* pool) {
    uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    size_t out_bytes = size_t(w) * h * z * 4;
    out.resize(out_bytes);
    if ((w & 3u) || (h & 3u)) std::memset(out.data(), 0, out_bytes);
    auto* dst_all = reinterpret_cast<uint32_t*>(out.data());
    bool used_parallel = false;
    for (uint32_t s = 0; s < z; s++) {
        uint32_t* dst_slice = dst_all + size_t(s) * w * h;
        if (pool && bh >= 32u && bw >= 16u) {
            used_parallel = true;
            pool->RunRange(bh, [&](uint32_t by0, uint32_t by1) {
                DecodeDxtRows(host, in.data(), in.size(), row_pitch, w, h, s, bw, bh, by0, by1, dst_slice);
            });
        } else {
            DecodeDxtRows(host, in.data(), in.size(), row_pitch, w, h, s, bw, bh, 0, bh, dst_slice);
        }
    }
    return used_parallel;
}

// ---- device memory: 64 MB blocks per memory type, first fit inside them; big allocations get their own

class Allocator {
public:
    struct Alloc {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize off = 0, size = 0;
        uint8_t* map = nullptr;
        int block = -1;  // -1: memory of its own
    };

    void Init(VkPhysicalDevice pd, VkDevice dev) {
        dev_ = dev;
        vkGetPhysicalDeviceMemoryProperties(pd, &props_);
    }

    bool Get(const VkMemoryRequirements& r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice, bool linear, Alloc& out) {
        int type = Type(r.memoryTypeBits, want | nice);
        if (type < 0) type = Type(r.memoryTypeBits, want);
        if (type < 0) return false;
        bool host = props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        std::lock_guard lock(m_);
        if (r.size > kBlock / 4) {
            out = {};
            if (!Allocate(uint32_t(type), r.size, out.mem)) return false;
            out.size = r.size;
            if (host) vkMapMemory(dev_, out.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&out.map));
            return true;
        }
        for (size_t i = 0; i < blocks_.size(); i++) {
            Block& b = blocks_[i];
            if (b.type != uint32_t(type) || b.linear != linear) continue;
            if (Carve(b, r, out)) {
                out.block = int(i);
                return true;
            }
        }
        Block b;
        b.type = uint32_t(type);
        b.linear = linear;
        if (!Allocate(b.type, kBlock, b.mem)) return false;
        if (host) vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.map));
        b.free[0] = kBlock;
        blocks_.push_back(std::move(b));
        Carve(blocks_.back(), r, out);
        out.block = int(blocks_.size() - 1);
        return true;
    }

    void Free(Alloc& a) {
        if (!a.mem) return;
        std::lock_guard lock(m_);
        if (a.block < 0) {
            vkFreeMemory(dev_, a.mem, nullptr);
        } else {
            auto& free = blocks_[size_t(a.block)].free;
            auto it = free.emplace(a.off, a.size).first;
            if (auto next = std::next(it); next != free.end() && it->first + it->second == next->first) {
                it->second += next->second;
                free.erase(next);
            }
            if (it != free.begin()) {
                auto prev = std::prev(it);
                if (prev->first + prev->second == it->first) {
                    prev->second += it->second;
                    free.erase(it);
                }
            }
        }
        a = {};
    }

    bool HostCoherent(const Alloc& a) const {
        uint32_t type = a.block < 0 ? own_type_ : blocks_[size_t(a.block)].type;
        return props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

private:
    static constexpr VkDeviceSize kBlock = 64ull << 20;
    struct Block {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint32_t type = 0;
        bool linear = false;
        uint8_t* map = nullptr;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset to size
    };

    int Type(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < props_.memoryTypeCount; i++)
            if ((bits >> i & 1) && (props_.memoryTypes[i].propertyFlags & want) == want) return int(i);
        return -1;
    }

    bool Allocate(uint32_t type, VkDeviceSize size, VkDeviceMemory& mem) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = size;
        ai.memoryTypeIndex = type;
        own_type_ = type;
        if (vkAllocateMemory(dev_, &ai, nullptr, &mem) == VK_SUCCESS) return true;
        LOGE("vulkan: out of device memory (%llu bytes)", (unsigned long long)size);
        return false;
    }

    bool Carve(Block& b, const VkMemoryRequirements& r, Alloc& out) {
        for (auto it = b.free.begin(); it != b.free.end(); ++it) {
            VkDeviceSize o = it->first, s = it->second;
            VkDeviceSize at = (o + r.alignment - 1) / r.alignment * r.alignment;
            if (at + r.size > o + s) continue;
            b.free.erase(it);
            if (at > o) b.free[o] = at - o;
            if (at + r.size < o + s) b.free[at + r.size] = o + s - at - r.size;
            out.mem = b.mem;
            out.off = at;
            out.size = r.size;
            out.map = b.map ? b.map + at : nullptr;
            return true;
        }
        return false;
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props_{};
    std::vector<Block> blocks_;
    uint32_t own_type_ = 0;
    std::mutex m_;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;    // sampled: every level, a depth image's depth alone
    VkImageView attach = VK_NULL_HANDLE;  // as a render target: level 0, depth and stencil
    Allocator::Alloc mem;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 1, h = 1, d = 1, layers = 1, levels = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t cb_render_seq = 0;
    bool depth = false, stencil = false;
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
};

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    Allocator::Alloc mem;
};

// Persistent DEVICE_LOCAL static geometry / object cache entry (+ cached 3D AABB for O(1) frustum culling)
struct StaticBuf {
    Buffer gpu;
    uint32_t bytes = 0;
    uint64_t epoch = 0;
    uint64_t fast_sig = 0;
    uint64_t last_frame = 0;
    uint32_t stable_frames = 0;
    bool promoted = false;
    bool has_aabb = false;
    uint32_t aabb_stride = 0;
    uint32_t aabb_offset = 0;
    uint32_t aabb_vcount = 0;
    float min_p[3] = {};
    float max_p[3] = {};
};

// a console surface the renderer draws into
struct Target {
    Image img;
    Image msaa;                   // optional MSAA 2x/4x companion image resolved directly into img
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    Image fb;                     // copy read by draws that sample the target they render to
};

struct Tex {
    Image img;
    TextureDesc d;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    bool decode = false;  // DXT decoded on the CPU
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t fast_sig = 0;
    uint64_t epoch = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;
    uint32_t stable_frames = 0;
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;  // Topology, strip at bit 4
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t strides[16];
    uint32_t buffers;
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc;
    uint32_t sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t samples;
    uint32_t has_resolve;
};

struct BlitOptions {
    bool gamma = false;
    bool alpha = false;
    uint32_t taps = 1;
    float mul = 1;
    bool over = false;  // premultiplied over the target, which keeps its alpha
};

struct DrawTextures {
    TexInfo info[4];
    TextureDesc desc[4] = {};
    bool valid[4] = {};
    VkImageView view[4] = {};
    VkFormat fmt[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool rt[4] = {};
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};
    bool blur[4] = {};
    uint32_t gw[4] = {};  // a render target's guest width, 0 for any other texture
};

struct DrawShaders {
    bool prog = false;
    bool fixed_transform = false;
    FixedKey fk;
    uint64_t vkey = 0, vh = 0, pkey = 0, ph = 0;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
};

struct GuestExtent {
    uint32_t w, h;
};

// ---- what the console's state asks of a draw

// the game's 2D layer leaves oPos.w at 0: screen space, drawn with w = 1 as XYZRHW is
std::string VertexProgram(const VertexShader& vs, uint16_t regs, uint64_t decode) {
    const std::string clamp = "    p.w = magclamp(p.w);\n";
    const std::string screen_space = "    p.w = p.w == 0 ? 1 : magclamp(p.w);\n";
    std::string src = VertexProgramHlsl(vs.program, regs, decode);
    size_t at = src.find(clamp);
    if (at != std::string::npos) src.replace(at, clamp.size(), screen_space);
    return src;
}

// a point list writes its size: Vulkan leaves it undefined otherwise; empty when the source has no main to wrap
std::string PointsHlsl(const std::string& src) {
    size_t at = src.find("VOut main(");
    if (at == std::string::npos) return {};
    size_t p = at + 10, sp = src.find(' ', p);
    std::string type = src.substr(p, sp - p);
    // the module's entry point is named main: the program's own main goes by another name
    std::string out = src;
    out.replace(at, 10, "VOut vs_body(");
    return out + "\nVOut main_points(" + type + " i, out float psize : PSIZE) { psize = 1; return vs_body(i); }\n";
}

bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f, kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 64;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    return false;
}

constexpr uint32_t kFmtD16 = 0x2C;
constexpr uint32_t kFmtLinD16 = 0x30;
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

constexpr uint32_t kSigWholeMax = 4096;
constexpr uint32_t kSigSamples = 32;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// Ultra-lightweight 4-point (128-byte) signature for unchanged static guest textures.
uint64_t FastMemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= 256u) return Hash(p, total, sig);
    constexpr uint32_t kFastSample = 32u;
    const uint32_t span = total - kFastSample;
    sig = Hash(p, kFastSample, sig);
    sig = Hash(p + (span / 3u), kFastSample, sig);
    sig = Hash(p + ((span * 2u) / 3u), kFastSample, sig);
    sig = Hash(p + span, kFastSample, sig);
    return sig;
}

// Fast 8-point content fingerprint for intra-frame static vertex/index buffer deduplication.
uint64_t FastBufSig(const uint8_t* p, uint32_t total) {
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= 256u) return Hash(p, total, sig);
    constexpr uint32_t kSample = 32u;
    const uint32_t span = total - kSample;
    for (uint32_t i = 0; i < 8u; ++i) {
        sig = Hash(p + (uint64_t(span) * i / 7u), kSample, sig);
    }
    return sig;
}

bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4, pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    return false;
}

bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    return px;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B, kPaletteCommon = 0, kPaletteData = 4, kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

DrawShaders ClassifyShaders(const State& s, const Assembled& a) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kPointsSalt = 0x9017;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    if (a.topology == Topology::Points) vkey ^= kPointsSalt;
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgram(*s.vs, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to Vulkan's RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    constexpr uint32_t kCullCw = 0x900;
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
}

constexpr float kAlphaTestAlways = 7.0f;

// FOG off leaves the constants as fog disabled: the shaders' fog factor is then 1, the colour as drawn
void FillFogConstants(const State& s, DrawConstants& c) {
    if (!s.rs[kRsFogEnable] || !game::Opt().fog) return;
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    if (s.ps) {
        for (int i = 0; i < 8; i++) {
            Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
            Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
        }
        Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
        Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    }
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;
constexpr float kBlurTapTexels = 7.0f / 6;

void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, the host half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    c.texsize[i][2] = -0.5f / (float(d.width) * k);
    c.texsize[i][3] = -0.5f / (float(d.height) * k);
    if (stages.wide[i] > 0) {
        bool blur = stages.blur[i];
        float fw = blur ? kBlurTapTexels : kGlowBoxTexels;
        float per_texel = blur ? 1.0f : 0.5f;
        float n = std::ceil(fw * stages.wide[i] * per_texel);
        c.spread[i][0] = fw / (n * float(d.width) * k);
        c.spread[i][1] = fw / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        if (stages.valid[i]) FillTexelConstants(i, stages.desc[i], stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void ComputeFixedMatrices(const State& s, GuestExtent size, float wv[16], float full[16]) {
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w, sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1, oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    MatMul(wvp, vpm, full);
}

// Conservative 8-corner local-space AABB clip-space frustum test (~40ns for any mesh size!).
bool IsAabbOutsideFrustum(const float min_p[3], const float max_p[3], const float full[16]) {
    uint32_t all_out = 0x3Fu;
    for (int i = 0; i < 8; ++i) {
        const float px = (i & 1) ? max_p[0] : min_p[0];
        const float py = (i & 2) ? max_p[1] : min_p[1];
        const float pz = (i & 4) ? max_p[2] : min_p[2];
        const float cx = px * full[0] + py * full[4] + pz * full[8] + full[12];
        const float cy = px * full[1] + py * full[5] + pz * full[9] + full[13];
        const float cz = px * full[2] + py * full[6] + pz * full[10] + full[14];
        const float cw = px * full[3] + py * full[7] + pz * full[11] + full[15];
        uint32_t out = 0u;
        if (cw <= 1e-6f) {
            out = (1u << 4);
        } else {
            const float wx = cw * 1.05f;
            const float wy = cw * 1.05f;
            if (cx < -wx) out |= (1u << 0);
            if (cx >  wx) out |= (1u << 1);
            if (cy < -wy) out |= (1u << 2);
            if (cy >  wy) out |= (1u << 3);
            if (cz < -cw * 0.02f) out |= (1u << 4);
            if (cz >  cw * 1.02f) out |= (1u << 5);
        }
        all_out &= out;
        if (all_out == 0u) return false;
    }
    return all_out != 0u;
}

// Conservative clip-space frustum cull for fixed-function 3D draws (front-of-camera culling).
// Uses direct vertex test for tiny draws (<=256 verts) and cached 3D AABB object cache for larger meshes (up to 16384 verts!).
bool IsFixedDrawOutsideFrustum(const Assembled& a, const float full[16],
                               std::unordered_map<uint64_t, StaticBuf>& static_bufs) {
    if (!a.vertices.empty() || a.vertex_count < 3u || a.vertex_count > 16384u) return false;
    if (!(a.regs & 1u) || a.elements[0].format != DXGI_FORMAT_R32G32B32_FLOAT) return false;
    const VertexElement& e = a.elements[0];
    const VertexBuffer& vb = a.buffers[e.buffer];
    if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;

    if (a.vertex_count <= 256u) {
        uint32_t all_out = 0x3Fu;
        for (uint32_t v = 0; v < a.vertex_count; ++v) {
            float p[3];
            std::memcpy(p, vb.src + size_t(v) * vb.stride + e.offset, sizeof(p));
            const float cx = p[0] * full[0] + p[1] * full[4] + p[2] * full[8] + full[12];
            const float cy = p[0] * full[1] + p[1] * full[5] + p[2] * full[9] + full[13];
            const float cz = p[0] * full[2] + p[1] * full[6] + p[2] * full[10] + full[14];
            const float cw = p[0] * full[3] + p[1] * full[7] + p[2] * full[11] + full[15];
            uint32_t out = 0u;
            if (cw <= 1e-6f) {
                out = (1u << 4);
            } else {
                const float wx = cw * 1.05f;
                const float wy = cw * 1.05f;
                if (cx < -wx) out |= (1u << 0);
                if (cx >  wx) out |= (1u << 1);
                if (cy < -wy) out |= (1u << 2);
                if (cy >  wy) out |= (1u << 3);
                if (cz < -cw * 0.02f) out |= (1u << 4);
                if (cz >  cw * 1.02f) out |= (1u << 5);
            }
            all_out &= out;
            if (all_out == 0u) return false;
        }
        return all_out != 0u;
    }

    // Large static mesh (257..16384 verts): reuse or compute cached 3D AABB in static_bufs_!
    if (!kernel::g_base) return false;
    const uintptr_t p_addr = reinterpret_cast<uintptr_t>(vb.src);
    const uintptr_t base = reinterpret_cast<uintptr_t>(kernel::g_base);
    if (p_addr < base || (p_addr - base) >= 0x100000000ull) return false;
    const uint32_t guest_va = uint32_t(p_addr - base);
    const uint64_t key = (uint64_t(guest_va) << 32) | uint64_t(vb.bytes);
    const uint64_t ep = DirtyEpoch(guest_va);
    StaticBuf& sb = static_bufs[key];
    if (!sb.has_aabb || sb.epoch != ep || sb.aabb_stride != vb.stride ||
        sb.aabb_offset != e.offset || sb.aabb_vcount != a.vertex_count) {
        float mn[3] = {1e30f, 1e30f, 1e30f};
        float mx[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t v = 0; v < a.vertex_count; ++v) {
            float pt[3];
            std::memcpy(pt, vb.src + size_t(v) * vb.stride + e.offset, sizeof(pt));
            for (int k = 0; k < 3; ++k) {
                if (pt[k] < mn[k]) mn[k] = pt[k];
                if (pt[k] > mx[k]) mx[k] = pt[k];
            }
        }
        if (!std::isfinite(mn[0]) || !std::isfinite(mx[0])) return false;
        std::memcpy(sb.min_p, mn, sizeof(mn));
        std::memcpy(sb.max_p, mx, sizeof(mx));
        sb.epoch = ep;
        sb.bytes = vb.bytes;
        sb.aabb_stride = vb.stride;
        sb.aabb_offset = e.offset;
        sb.aabb_vcount = a.vertex_count;
        sb.has_aabb = true;
    }
    return IsAabbOutsideFrustum(sb.min_p, sb.max_p, full);
}

struct VsMeta {
    uint16_t max_vsc_regs = 192;
    int16_t mvp_cbase = -1;  // >= 0 when direct screen-space 4x4 DP4 MVP on v0 + rcp(w) + c[59] detected
};

VsMeta AnalyzeVertexProgram(const std::vector<uint32_t>& prog) {
    VsMeta m{};
    if (prog.size() < 4u) {
        m.max_vsc_regs = 1;
        return m;
    }
    auto fld = [](const uint32_t* t, int sub, int bit, int len) -> uint32_t {
        return (t[sub] >> bit) & ((1u << len) - 1u);
    };
    auto is_ident_swizzle = [](uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3) -> bool {
        return s0 == 0u && s1 == 1u && s2 == 2u && s3 == 3u;
    };

    uint16_t max_c = 1;
    bool has_relative = false;
    bool reads_c58 = false;
    int dp4_reg = -1;
    int dp4_c[4] = {-1, -1, -1, -1};
    bool dp4_clobbered = false;
    int rcp_reg = -1;
    bool mad_opos_ok = false;

    for (size_t n = 0; n + 4u <= prog.size(); n += 4u) {
        const uint32_t* t = &prog[n];
        const uint32_t mac = fld(t, 1, 21, 4);
        const uint32_t ilu = fld(t, 1, 25, 3);
        const uint32_t muxA = fld(t, 2, 26, 2);
        const uint32_t muxB = fld(t, 2, 11, 2);
        const uint32_t muxC = fld(t, 3, 28, 2);
        const bool uses_c = (muxA == 3u || muxB == 3u || muxC == 3u);
        const bool rel = fld(t, 3, 1, 1) != 0u;
        const uint32_t cr = fld(t, 1, 13, 8);
        if (uses_c) {
            if (rel) {
                has_relative = true;
                max_c = 192;
            } else {
                const uint16_t need = uint16_t(std::min<uint32_t>(cr + 1u, 192u));
                if (need > max_c) max_c = need;
                if (cr == 58u) reads_c58 = true;
            }
        }
        if (mac == 13u) {
            has_relative = true;
            max_c = 192;
        }

        if (!has_relative) {
            const uint32_t r = fld(t, 3, 20, 4);
            const uint32_t mac_mask = (ilu && r == 1u) ? 0u : fld(t, 3, 24, 4);
            const uint32_t ilu_mask = fld(t, 3, 16, 4);
            const uint32_t ir = mac ? 1u : r;

            // Detect DP4 (7) or DPH (6) of v0 with c[cr] into a single lane of temp reg < 12
            bool matched_dp4 = false;
            if ((mac == 7u || mac == 6u) && !rel && r < 12u &&
                (mac_mask == 8u || mac_mask == 4u || mac_mask == 2u || mac_mask == 1u)) {
                const uint32_t negA = fld(t, 1, 8, 1);
                const uint32_t negB = fld(t, 2, 25, 1);
                const bool idA = !negA && is_ident_swizzle(fld(t, 1, 6, 2), fld(t, 1, 4, 2), fld(t, 1, 2, 2), fld(t, 1, 0, 2));
                const bool idB = !negB && is_ident_swizzle(fld(t, 2, 23, 2), fld(t, 2, 21, 2), fld(t, 2, 19, 2), fld(t, 2, 17, 2));
                const uint32_t vin = fld(t, 1, 9, 4);
                const bool v0_and_c = ((muxA == 2u && vin == 0u && muxB == 3u) ||
                                       (muxB == 2u && vin == 0u && muxA == 3u));
                if (idA && idB && v0_and_c && cr < 192u) {
                    const int lane = (mac_mask == 8u) ? 0 : (mac_mask == 4u) ? 1 : (mac_mask == 2u) ? 2 : 3;
                    if (dp4_reg < 0) dp4_reg = int(r);
                    if (int(r) == dp4_reg && !dp4_clobbered && dp4_c[lane] < 0) {
                        dp4_c[lane] = int(cr);
                        matched_dp4 = true;
                    }
                }
            }

            const bool dp4_done = (dp4_reg >= 0 && dp4_c[0] >= 0 && dp4_c[1] >= 0 && dp4_c[2] >= 0 && dp4_c[3] >= 0);

            // Detect RCP(2) or RCC(3) on dp4_reg.w -> rcp_reg
            if (dp4_done && !dp4_clobbered && (ilu == 2u || ilu == 3u) && muxC == 1u && ilu_mask != 0u && ir < 12u) {
                const uint32_t regC = (fld(t, 2, 0, 2) << 2) | fld(t, 3, 30, 2);
                const uint32_t swC0 = fld(t, 2, 8, 2);
                const uint32_t negC = fld(t, 2, 10, 1);
                if (!negC && int(regC) == dp4_reg && swC0 == 3u) {
                    rcp_reg = int(ir);
                }
            }

            // Detect MAD oPos.xyz, dp4_reg, rcp_reg, c[59]
            if (dp4_done && !dp4_clobbered && rcp_reg >= 0 && mac == 4u && !rel && cr == 59u) {
                const uint32_t omask = fld(t, 3, 12, 4);
                const bool out_en = fld(t, 3, 11, 1) != 0u;
                const bool use_ilu = fld(t, 3, 2, 1) == 1u;
                const uint32_t o = fld(t, 3, 3, 8) & 0xFu;
                const bool writes_opos_xyz = out_en && !use_ilu && o == 0u && ((omask & 0xEu) == 0xEu);
                if (writes_opos_xyz && muxA == 1u && muxB == 1u && muxC == 3u) {
                    const uint32_t regA = fld(t, 2, 28, 4);
                    const uint32_t regB = fld(t, 2, 13, 4);
                    if ((int(regA) == dp4_reg && int(regB) == rcp_reg) ||
                        (int(regB) == dp4_reg && int(regA) == rcp_reg)) {
                        mad_opos_ok = true;
                    }
                }
            }

            // If dp4_reg is overwritten by anything else before MAD oPos, disqualify
            if (!matched_dp4 && !mad_opos_ok && dp4_reg >= 0) {
                if ((mac && mac != 13u && mac_mask && int(r) == dp4_reg) ||
                    (ilu && ilu_mask && int(ir) == dp4_reg)) {
                    dp4_clobbered = true;
                }
            }
        }

        if (fld(t, 3, 0, 1)) break;
    }

    m.max_vsc_regs = max_c;
    if (!has_relative && !reads_c58 && mad_opos_ok && !dp4_clobbered &&
        dp4_c[0] >= 0 && dp4_c[1] == dp4_c[0] + 1 && dp4_c[2] == dp4_c[0] + 2 && dp4_c[3] == dp4_c[0] + 3 &&
        dp4_c[3] < 192) {
        m.mvp_cbase = int16_t(dp4_c[0]);
    }
    return m;
}

bool IsProgDrawOutsideFrustum(const State& s, const Assembled& a, GuestExtent size, int16_t cbase,
                              std::unordered_map<uint64_t, StaticBuf>& static_bufs) {
    if (cbase < 0 || cbase + 3 >= 192 || !a.vertices.empty() || a.vertex_count < 3u || a.vertex_count > 16384u)
        return false;
    if (!(a.regs & 1u) || a.elements[0].format != DXGI_FORMAT_R32G32B32_FLOAT) return false;
    const VertexElement& e = a.elements[0];
    const VertexBuffer& vb = a.buffers[e.buffer];
    if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
    if (size.w == 0u || size.h == 0u || s.zscale <= 0.0f) return false;

    const float* r0 = s.vsc[size_t(cbase + 0)].data();
    const float* r1 = s.vsc[size_t(cbase + 1)].data();
    const float* r2 = s.vsc[size_t(cbase + 2)].data();
    const float* r3 = s.vsc[size_t(cbase + 3)].data();
    const float* c59 = s.vsc[59].data();
    if (!std::isfinite(c59[0]) || !std::isfinite(c59[1]) || !std::isfinite(c59[2]) || !std::isfinite(r3[3]))
        return false;

    const float lo_x = -0.12f * float(size.w);
    const float hi_x =  1.12f * float(size.w);
    const float lo_y = -0.12f * float(size.h);
    const float hi_y =  1.12f * float(size.h);
    const float lo_z = -0.05f * s.zscale;
    const float hi_z =  1.05f * s.zscale;

    auto test_point = [&](float px, float py, float pz) -> uint32_t {
        const float cw = px * r3[0] + py * r3[1] + pz * r3[2] + r3[3];
        if (cw <= 1e-6f) return (1u << 4);
        const float inv_w = 1.0f / cw;
        const float sx = (px * r0[0] + py * r0[1] + pz * r0[2] + r0[3]) * inv_w + c59[0];
        const float sy = (px * r1[0] + py * r1[1] + pz * r1[2] + r1[3]) * inv_w + c59[1];
        const float sz = (px * r2[0] + py * r2[1] + pz * r2[2] + r2[3]) * inv_w + c59[2];
        uint32_t out = 0u;
        if (sx < lo_x) out |= (1u << 0);
        if (sx > hi_x) out |= (1u << 1);
        if (sy < lo_y) out |= (1u << 2);
        if (sy > hi_y) out |= (1u << 3);
        if (sz < lo_z) out |= (1u << 4);
        if (sz > hi_z) out |= (1u << 5);
        return out;
    };

    if (a.vertex_count <= 256u) {
        uint32_t all_out = 0x3Fu;
        for (uint32_t v = 0; v < a.vertex_count; ++v) {
            float p[3];
            std::memcpy(p, vb.src + size_t(v) * vb.stride + e.offset, sizeof(p));
            all_out &= test_point(p[0], p[1], p[2]);
            if (all_out == 0u) return false;
        }
        return all_out != 0u;
    }

    if (!kernel::g_base) return false;
    const uintptr_t p_addr = reinterpret_cast<uintptr_t>(vb.src);
    const uintptr_t base = reinterpret_cast<uintptr_t>(kernel::g_base);
    if (p_addr < base || (p_addr - base) >= 0x100000000ull) return false;
    const uint32_t guest_va = uint32_t(p_addr - base);
    const uint64_t key = (uint64_t(guest_va) << 32) | uint64_t(vb.bytes);
    const uint64_t ep = DirtyEpoch(guest_va);
    StaticBuf& sb = static_bufs[key];
    if (!sb.has_aabb || sb.epoch != ep || sb.aabb_stride != vb.stride ||
        sb.aabb_offset != e.offset || sb.aabb_vcount != a.vertex_count) {
        float mn[3] = {1e30f, 1e30f, 1e30f};
        float mx[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t v = 0; v < a.vertex_count; ++v) {
            float pt[3];
            std::memcpy(pt, vb.src + size_t(v) * vb.stride + e.offset, sizeof(pt));
            for (int k = 0; k < 3; ++k) {
                if (pt[k] < mn[k]) mn[k] = pt[k];
                if (pt[k] > mx[k]) mx[k] = pt[k];
            }
        }
        if (!std::isfinite(mn[0]) || !std::isfinite(mx[0])) return false;
        std::memcpy(sb.min_p, mn, sizeof(mn));
        std::memcpy(sb.max_p, mx, sizeof(mx));
        sb.epoch = ep;
        sb.bytes = vb.bytes;
        sb.aabb_stride = vb.stride;
        sb.aabb_offset = e.offset;
        sb.aabb_vcount = a.vertex_count;
        sb.has_aabb = true;
    }
    uint32_t all_out = 0x3Fu;
    for (int i = 0; i < 8; ++i) {
        const float px = (i & 1) ? sb.max_p[0] : sb.min_p[0];
        const float py = (i & 2) ? sb.max_p[1] : sb.min_p[1];
        const float pz = (i & 4) ? sb.max_p[2] : sb.min_p[2];
        all_out &= test_point(px, py, pz);
        if (all_out == 0u) return false;
    }
    return all_out != 0u;
}

void FillTransformConstants(const State& s, const float wv[16], const float full[16], DrawConstants& c) {
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

void LightToView(const std::array<uint32_t, 26>& l, const float* vm, float (&o)[7][4]) {
    float f[26];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++)
        if ((fk.types >> (i * 2)) & 3) LightToView(s.lights[size_t(i)], vm, c.lt[i]);
}

struct GuestRect {
    int32_t left, top, right, bottom;
};
GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * 16;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)),
            int32_t(kernel::R32(r + 12))};
}
void ReadGuestPoint(uint32_t points, uint32_t i, int32_t& x, int32_t& y) {
    x = int32_t(kernel::R32(points + i * 8));
    y = int32_t(kernel::R32(points + i * 8 + 4));
}
uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }

uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// ---- layouts: the stages and accesses an image's layout stands for

void LayoutMasks(VkImageLayout l, VkPipelineStageFlags& st, VkAccessFlags& ac) {
    switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            ac = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            ac = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            st = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
            ac = VK_ACCESS_SHADER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            ac = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            st = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            ac = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            st = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            ac = 0;
            break;
        default:
            st = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            ac = 0;
            break;
    }
}

bool HasStencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override = default;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override { Submit(true, false); }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }
    void WaitGpuFrame(uint64_t n);

private:
    struct Alloc {
        VkBuffer buf;
        VkDeviceSize off;
        uint8_t* cpu;
    };
    struct Frame {
        struct CachedBuf {
            Alloc alloc{};
            uint64_t epoch = 0;
            uint64_t sig = 0;
        };
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        Buffer upload;
        VkDeviceSize used = 0;
        Alloc dummy_vc{};
        std::array<std::array<float, 4>, 192> last_vsc{};
        size_t last_vsc_bytes = 0;
        Alloc last_vsc_alloc{};
        bool has_last_vsc = false;
        DrawConstants last_dc{};
        Alloc last_dc_alloc{};
        bool has_last_dc = false;
        std::unordered_map<uint64_t, CachedBuf> buf_cache;
        uint64_t value = 0;  // the timeline's value once the GPU is past this frame
        std::vector<Image> images;
        std::vector<Buffer> buffers;
        std::vector<VkDescriptorPool> pools;
        size_t pool_at = 0;  // the descriptor pool sets come from
        size_t total_sets = 0;
        std::unordered_map<uint64_t, VkDescriptorSet> sets;
        std::unordered_map<VkImageView, std::vector<uint64_t>> view_keys;
        VkSemaphore acquire = VK_NULL_HANDLE;
    };
    struct Late {
        Buffer rb[kFrames];
        Image down;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0, next = 0;
    };

    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    VkCommandBuffer Cb() { return CurrentFrame().cb; }

    // Init, in order
    bool CreateInstance();
    bool CreateSurface();
    bool PickDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateLayouts();
    bool CreateFrames();
    bool CreateFixedImages();

    // memory and objects
    bool CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, uint32_t levels,
                     VkImageUsageFlags usage, VkImageViewType vt, bool cube = false,
                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void Destroy(Image& im);
    void Destroy(Buffer& b);
    void Retire(Image& im) {
        if (rend_color_ == &im || rend_depth_ == &im || rend_resolve_ == &im) EndRender();
        if (im) CurrentFrame().images.push_back(im);
        im = {};
    }
    void Retire(Buffer& b) {
        if (b.buf) CurrentFrame().buffers.push_back(b);
        b = {};
    }
    Alloc Upload(VkDeviceSize size, VkDeviceSize align = 16);
    Alloc UploadCached(const uint8_t* src, uint32_t bytes, VkDeviceSize align, bool allow_cache);
    Alloc Constants(const void* data, size_t bytes);
    Alloc ConstantsTrimmed(const void* data, size_t bytes);

    // command recording
    void Begin();
    void Submit(bool wait, bool present);
    void WaitValue(uint64_t v);
    uint64_t Completed();
    VkFence SubmitFence(uint64_t value);
    void PollFences();
    void ResetFrame(Frame& f);
    void Transition(Image& im, VkImageLayout to);
    void BeginRender(Image* color, Image* depth, Image* resolve = nullptr);
    void BeginRenderClear(Image* color, Image* depth, bool clear_c, const float cc[4], bool clear_z, float z,
                          bool clear_s, uint32_t stencil, Image* resolve = nullptr);
    void EndRender();
    // without dynamic rendering: render passes that only load and store, and framebuffers per pair of views
    VkRenderPass Pass(VkFormat color, VkFormat depth, bool clear_c = false, bool clear_ds = false,
                      VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT, bool has_resolve = false,
                      bool dont_care_c = false);
    VkFramebuffer Framebuffer(VkFormat color_fmt, VkFormat depth_fmt, VkImageView color, VkImageView depth,
                              uint32_t w, uint32_t h, VkImageView resolve = VK_NULL_HANDLE,
                              VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void DropFramebuffers(VkImageView v);
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    void BindPipeline(VkPipeline pipe);
    void SetBlend(const float c[4]);
    void SetStencil(uint32_t ref);
    VkDescriptorSet Set(const VkImageView views[4], const VkSampler samplers[4]);
    void BindSet(VkDescriptorSet set, VkDeviceSize vc, VkDeviceSize dc);

    // shaders and pipelines
    VkShaderModule Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash);
    VkPipeline FullScreenPipeline(const char* hlsl, const char* ps_entry, VkFormat color, VkFormat depth, bool over,
                                  uint8_t wmask = 0xF);
    VkPipeline OverlayPipeline(bool sdf, VkFormat color);
    VkPipeline EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps);
    VkSampler Sampler(const VkSamplerCreateInfo& ci);
    VkSampler StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp);
    VkSampleCountFlagBits DesiredMsaaSamples() const;

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    void UpgradeTargetSigned(Target& t);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    void UploadTarget(Target& t, const Surface& s);
    bool PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    void Blit(Image& dst, const VkRect2D& to, Image& src, const float uvrect[4], const BlitOptions& o);
    Image* Feedback(Target& t);
    bool DepthCopy(const State& s);
    bool Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps = 1);
    Image* CopySource(Target& t, Image& down);
    bool ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

    // draws
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh, bool signed_src);
    bool BindStageTextures(const State& s, DrawTextures& stages);
    VkImageView BindTexture(const State& s, int stage, DrawTextures& stages);
    bool GlowComposite(const State& s, const DrawTextures& stages) const;
    VkImageView BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                  DrawTextures& stages);
    VkImageView BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                 DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours);
    void BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    void ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                     float z, uint32_t stencil);

    // present
    bool AcquireImage(uint32_t& index);
    void DrawFrame(Target* t, uint32_t index);
    void PrepareUi(float w, float h);
    void RenderUi();
    void UploadGammaRamp();
    void UploadSkin();
    void ReleaseOldTextures();
    void ApplySettings();
    void SavePipelineCache();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    uint32_t surface_epoch_ = 0;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceFeatures feats_{};
    uint32_t api_ = VK_API_VERSION_1_1;
    uint32_t qfam_ = 0;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::vector<const char*> dev_exts_;
    bool bc_ = false;
    VkSampleCountFlags msaa_caps_ = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits msaa_samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkFormat depth_fmt_ = VK_FORMAT_D24_UNORM_S8_UINT;
    bool depth_linear_ = false;
    bool r32f_linear_ = false;
    Allocator alloc_;
    PFN_vkCmdBeginRenderingKHR begin_rendering_ = nullptr;
    PFN_vkCmdEndRenderingKHR end_rendering_ = nullptr;
    PFN_vkWaitSemaphoresKHR wait_semaphores_ = nullptr;
    PFN_vkGetSemaphoreCounterValueKHR counter_value_ = nullptr;
    bool dynamic_ = true;  // dynamic rendering; Vulkan 1.1 drivers (older Adrenos) only have render passes
    bool timeline_ok_ = true;  // without it a fence per submit stands in for the timeline's values
    struct Pending {
        uint64_t value;
        VkFence fence;
    };
    std::mutex fence_lock_;  // the game thread waits on frames too
    std::vector<Pending> pending_;
    std::vector<VkFence> spare_fences_;
    uint64_t fenced_ = 0;  // the highest value whose fence signalled
    std::unordered_map<uint64_t, VkRenderPass> passes_;
    struct Fb {
        VkImageView color, depth, resolve;
        VkFramebuffer fb;
    };
    std::unordered_map<uint64_t, Fb> framebuffers_;

    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    VkFormat swap_fmt_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swap_extent_{};
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    std::vector<VkSemaphore> swap_done_;
    bool vsync_ = true;
    bool swap_stale_ = false;
    bool surface_lost_ = false;  // the driver said the window is gone: the surface is made again too
    uint32_t surface_failures_ = 0;  // presents in a row without a surface, for the log
    uint32_t present_index_ = 0;  // the image the frame's submit signals the present of

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    size_t cache_saved_ = 0;
    std::atomic<bool> cache_saving_{false};
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> value_{0};
    Frame frames_[kFrames];
    uint64_t frame_ = 0;

    // the rendering under way: its attachments and cached CB state
    bool rendering_ = false;
    Image* rend_color_ = nullptr;
    Image* rend_depth_ = nullptr;
    Image* rend_resolve_ = nullptr;
    uint64_t render_seq_ = 1;
    VkPipeline cmd_pipe_ = VK_NULL_HANDLE;
    VkDescriptorSet cmd_set_ = VK_NULL_HANDLE;
    VkDeviceSize cmd_vc_off_ = 0, cmd_dc_off_ = 0;
    uint32_t cmd_vb_count_ = 0;
    VkBuffer cmd_vbufs_[16] = {};
    VkDeviceSize cmd_voffs_[16] = {};
    VkBuffer cmd_ib_ = VK_NULL_HANDLE;
    VkDeviceSize cmd_ib_off_ = 0;
    VkIndexType cmd_ib_type_ = VK_INDEX_TYPE_UINT16;
    VkImageView last_set_views_[4] = {};
    VkSampler last_set_samplers_[4] = {};
    VkDescriptorSet last_set_ = VK_NULL_HANDLE;
    bool has_vp_ = false, has_sc_ = false, has_blend_ = false, has_stencil_ = false;
    VkViewport cmd_vp_{};
    VkRect2D cmd_sc_{};
    float cmd_blend_[4] = {};
    uint32_t cmd_stencil_ = 0;
    std::vector<uint8_t> upload_px_;
    std::vector<uint8_t> upload_decoded_;
    DxtWorkerPool dxt_pool_;
    uint64_t perf_vb_hits_ = 0;
    uint64_t perf_static_buf_hits_ = 0;
    uint64_t perf_vsc_hits_ = 0;
    uint64_t perf_dc_hits_ = 0;
    uint64_t perf_pso_hits_ = 0;
    uint64_t perf_fast_tex_hits_ = 0;
    uint64_t perf_clear_coalesced_ = 0;
    uint64_t perf_frustum_culled_ = 0;
    uint64_t perf_scissor_culled_ = 0;
    uint64_t perf_dxt_parallel_ = 0;

    // Cached batched 2D touch overlay pipelines & pre-pass vertex buffers
    VkPipeline overlay_tex_pipe_ = VK_NULL_HANDLE;
    VkPipeline overlay_sdf_pipe_ = VK_NULL_HANDLE;
    VkFormat overlay_pipe_fmt_ = VK_FORMAT_UNDEFINED;
    std::vector<touch::Quad> ui_quads_;
    std::vector<OverlayTexVert> ui_tex_verts_;
    std::vector<OverlaySdfVert> ui_sdf_verts_;
    std::vector<OverlayRun> ui_runs_;
    Alloc ui_tex_alloc_{};
    Alloc ui_sdf_alloc_{};

    uint64_t mru_vkey_ = 0;
    uint64_t mru_vh_ = 0;
    VkShaderModule mru_vs_ = VK_NULL_HANDLE;
    uint64_t mru_pkey_ = 0;
    uint64_t mru_ph_ = 0;
    VkShaderModule mru_ps_ = VK_NULL_HANDLE;
    uint64_t mru_pso_hash_ = 0;
    VkPipeline mru_pso_pipe_ = VK_NULL_HANDLE;

    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;
    std::unordered_map<uint64_t, VsMeta> vs_meta_;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, VkSampler> stage_samplers_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint64_t, StaticBuf> static_bufs_;
    VkDeviceSize static_buf_bytes_ = 0;
    Image lut_;
    Image dummy_[3];  // 2D, 3D, cube
    VkSampler linear_ = VK_NULL_HANDLE, point_ = VK_NULL_HANDLE;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    Image skin_;  // the touch controls' button skins (touch.cpp), made once
    bool skin_tried_ = false;
    bool gamma_ = false;
    uint64_t present_pipe_key_ = 0;
    VkPipeline present_pipe_ = VK_NULL_HANDLE;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1.0f;
    Assembled asm_;
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    std::atomic<uint64_t> presents_seen_{0};
    std::atomic<uint64_t> present_value_[kFrames + 1] = {};
    uint64_t presents_ = 0;
    uint32_t frame_draws_ = 0;
    uint64_t frame_verts_ = 0;
    uint32_t frame_culled_ = 0;
    uint32_t frame_buf_lookups_ = 0;
    uint32_t frame_buf_hits_ = 0;
    double fps_ = 0;
    float ema_frame_ms_ = 16.67f;
    std::chrono::steady_clock::time_point last_present_t_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

#define VK_CHECK(x)                                                    \
    do {                                                               \
        VkResult _r = (x);                                             \
        if (_r != VK_SUCCESS) {                                        \
            LOGE("vulkan: %s failed (%d)", #x, int(_r));               \
            return false;                                              \
        }                                                              \
    } while (0)

// ---- Init

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    vkshader::Init(app::ExeDir() / "shadercache");
    if (!CreateInstance() || !CreateSurface() || !PickDevice() || !CreateDevice()) return false;
    alloc_.Init(pd_, dev_);
    if (!CreateLayouts() || !CreateFrames() || !CreateSwapchain() || !CreateFixedImages()) return false;
    Begin();
    scale_ = app::RenderScale(guest_h_);
    LOGI("vulkan: %ux%u guest, %ux%u screen, render scale %.3f", width, height, swap_extent_.width,
         swap_extent_.height, scale_);
    return true;
}

bool VulkanBackend::CreateInstance() {
    uint32_t n = 0;
    const char* const* sdl = SDL_Vulkan_GetInstanceExtensions(&n);
    std::vector<const char*> exts(sdl, sdl + n);
    uint32_t loader = VK_API_VERSION_1_1;
    vkEnumerateInstanceVersion(&loader);
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = app::kGame.log;
    ai.pEngineName = "XboxXBE-Recomp";
    ai.apiVersion = std::min<uint32_t>(loader, VK_API_VERSION_1_3);
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    return true;
}

// the epoch is taken only once the surface is made: until then each present tries again
bool VulkanBackend::CreateSurface() {
    uint32_t epoch = host::SurfaceEpoch();
    // Android has not handed the window back yet (resuming, or a surface change): wait for it without asking SDL,
    // which only fails, and log the outage once, not every present
    SDL_PropertiesID props = SDL_GetWindowProperties(host::Window());
    if (!SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)) {
        if (surface_failures_++ == 0) LOGI("vulkan: waiting for Android's window");
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(host::Window(), instance_, nullptr, &surface_)) {
        surface_ = VK_NULL_HANDLE;
        if (surface_failures_++ == 0) LOGE("vulkan: no surface: %s", SDL_GetError());
        return false;
    }
    if (surface_failures_) LOGI("vulkan: surface made after %u tries", surface_failures_ + 1);
    surface_failures_ = 0;
    surface_epoch_ = epoch;
    surface_lost_ = false;
    return true;
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
}

bool VulkanBackend::PickDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    // rejections are reported, not warned: release logs drop warnings and these are what a bug report needs
    if (pds.empty()) LOGE("vulkan: the driver lists no GPU");
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        LOGR("GPU: %s, Vulkan %u.%u.%u, driver %08X", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
        if (p.apiVersion < VK_API_VERSION_1_1) {
            LOGE("vulkan: %s is below Vulkan 1.1", p.deviceName);
            continue;
        }
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ext(en);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, ext.data());
        uint32_t api = std::min<uint32_t>(p.apiVersion, VK_API_VERSION_1_3);
        bool swapchain = HasExtension(ext, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        bool dynamic = api >= VK_API_VERSION_1_3 || HasExtension(ext, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        if (!swapchain) {
            LOGE("vulkan: %s lacks swapchain", p.deviceName);
            continue;
        }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            pd_ = pd;
            props_ = p;
            api_ = api;
            host::SetGpuInfo(p.deviceName, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
            qfam_ = q;
            dev_exts_ = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            auto want = [&](const char* name) {
                if (HasExtension(ext, name)) dev_exts_.push_back(name);
            };
            if (api < VK_API_VERSION_1_3) {
                want(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
                want(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
                want(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
            }
            dynamic_ = dynamic;
            // the 1.1 drivers' timeline extension lost the device on the first submit (Mali-G76): fences there
            timeline_ok_ = api >= VK_API_VERSION_1_2;
            if (!dynamic_) LOGR("vulkan: no dynamic rendering, using render passes");
            if (!timeline_ok_) LOGR("vulkan: Vulkan 1.1, using fences for timeline semaphores");
            return true;
        }
        LOGE("vulkan: %s has no graphics queue that can present", p.deviceName);
    }
    LOGE("vulkan: no GPU with Vulkan 1.1 that can present");
    return false;
}

bool VulkanBackend::CreateDevice() {
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR ts{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    // the chain holds only what is used: a 1.1 driver may not know the rest
    auto chain = [&]() -> void* {
        ts.pNext = nullptr;
        dr.pNext = timeline_ok_ ? &ts : nullptr;
        return dynamic_ ? (void*)&dr : timeline_ok_ ? (void*)&ts : nullptr;
    };
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain()};
    vkGetPhysicalDeviceFeatures2(pd_, &f2);
    if (timeline_ok_ && !ts.timelineSemaphore) {
        LOGR("vulkan: timeline semaphores not supported, using fences");
        timeline_ok_ = false;
    }
    if (dynamic_ && !dr.dynamicRendering) {
        LOGR("vulkan: dynamic rendering listed but not supported, using render passes");
        dynamic_ = false;
    }
    feats_ = {};
    feats_.samplerAnisotropy = f2.features.samplerAnisotropy;
    feats_.fillModeNonSolid = f2.features.fillModeNonSolid;
    feats_.textureCompressionBC = f2.features.textureCompressionBC;
    bc_ = feats_.textureCompressionBC;
    VkPhysicalDeviceFeatures2 on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain()};
    on.features = feats_;
    dr.dynamicRendering = VK_TRUE;
    ts.timelineSemaphore = VK_TRUE;
    float prio = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qfam_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &on};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = uint32_t(dev_exts_.size());
    ci.ppEnabledExtensionNames = dev_exts_.data();
    VK_CHECK(vkCreateDevice(pd_, &ci, nullptr, &dev_));
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);
    bool core13 = api_ >= VK_API_VERSION_1_3, core12 = api_ >= VK_API_VERSION_1_2;
    begin_rendering_ = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(dev_, core13 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR"));
    end_rendering_ = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(dev_, core13 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR"));
    wait_semaphores_ = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
        vkGetDeviceProcAddr(dev_, core12 ? "vkWaitSemaphores" : "vkWaitSemaphoresKHR"));
    counter_value_ = reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(
        vkGetDeviceProcAddr(dev_, core12 ? "vkGetSemaphoreCounterValue" : "vkGetSemaphoreCounterValueKHR"));
    if ((dynamic_ && (!begin_rendering_ || !end_rendering_)) || (timeline_ok_ && (!wait_semaphores_ || !counter_value_))) {
        LOGE("vulkan: device functions missing");
        return false;
    }
    // the depth buffer: 24 bit with stencil as on the console where the GPU has it
    auto usable = [&](VkFormat f) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd_, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return (fp.optimalTilingFeatures & need) == need;
    };
    depth_fmt_ = usable(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd_, depth_fmt_, &fp);
    depth_linear_ = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    vkGetPhysicalDeviceFormatProperties(pd_, VK_FORMAT_R32_SFLOAT, &fp);
    r32f_linear_ = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    msaa_caps_ = props_.limits.framebufferColorSampleCounts & props_.limits.framebufferDepthSampleCounts;
    msaa_samples_ = DesiredMsaaSamples();
    LOGI("vulkan: depth %s, BC textures %s, MSAA caps 0x%x (active %ux)",
         depth_fmt_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
         bc_ ? "on the GPU" : "decoded on the CPU", uint32_t(msaa_caps_), uint32_t(msaa_samples_));
    return true;
}

VkSampleCountFlagBits VulkanBackend::DesiredMsaaSamples() const {
    const uint32_t aa = game::Opt().anti_aliasing;
    if (aa == 3 && (msaa_caps_ & VK_SAMPLE_COUNT_4_BIT)) return VK_SAMPLE_COUNT_4_BIT;
    if ((aa == 2 || aa == 3) && (msaa_caps_ & VK_SAMPLE_COUNT_2_BIT)) return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

bool VulkanBackend::CreateSwapchain() {
    if (!surface_) return false;
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd_, surface_, &caps);
    if (cr != VK_SUCCESS) {
        if (cr == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
        LOGE("vulkan: surface capabilities failed (%d)", int(cr));
        return false;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    // the window's own size, landscape; Android's compositor turns the picture with the screen
    int w, h;
    host::WindowPixels(w, h);
    if (w <= 0 || h <= 0) {
        w = int(caps.currentExtent.width);
        h = int(caps.currentExtent.height);
    }
    if (w <= 0 || h <= 0) return false;
    vsync_ = app::Get().vsync;
    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                mode = m;
                break;
            }
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = {uint32_t(w), uint32_t(h)};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap_;
    // retiring the old chain gives its images back to the window: the GPU must be done drawing them
    if (swap_) vkQueueWaitIdle(queue_);
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VkResult sr = vkCreateSwapchainKHR(dev_, &ci, nullptr, &fresh);
    if (sr != VK_SUCCESS) {
        if (sr == VK_ERROR_SURFACE_LOST_KHR || sr == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) surface_lost_ = true;
        LOGE("vulkan: swap chain %dx%d failed (%d)", w, h, int(sr));
        return false;
    }
    DestroySwapchain();
    swap_ = fresh;
    swap_fmt_ = chosen.format;
    swap_extent_ = ci.imageExtent;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev_, swap_, &count, nullptr);
    swap_images_.resize(count);
    vkGetSwapchainImagesKHR(dev_, swap_, &count, swap_images_.data());
    for (VkImage im : swap_images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_fmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev_, &vi, nullptr, &v);
        swap_views_.push_back(v);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(dev_, &si, nullptr, &s);
        swap_done_.push_back(s);
    }
    swap_stale_ = false;
    LOGI("vulkan: swap chain %ux%u, %u images, %s", w, h, count, vsync_ ? "vsync" : "no vsync");
    return true;
}

// the old chain's views and semaphores; the chain itself when it is not being replaced
void VulkanBackend::DestroySwapchain() {
    if (!swap_) return;
    vkQueueWaitIdle(queue_);
    for (VkImageView v : swap_views_) {
        DropFramebuffers(v);
        vkDestroyImageView(dev_, v, nullptr);
    }
    for (VkSemaphore s : swap_done_) vkDestroySemaphore(dev_, s, nullptr);
    swap_views_.clear();
    swap_done_.clear();
    swap_images_.clear();
    vkDestroySwapchainKHR(dev_, swap_, nullptr);
    swap_ = VK_NULL_HANDLE;
}

bool VulkanBackend::CreateLayouts() {
    VkDescriptorSetLayoutBinding b[vkshader::kBindings] = {};
    for (uint32_t i = 0; i < vkshader::kBindings; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                              : i < vkshader::kBindingSamplers ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                               : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = vkshader::kBindings;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_));
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &set_layout_;
    VK_CHECK(vkCreatePipelineLayout(dev_, &pi, nullptr, &layout_));
    // pipelines from earlier runs: building them is what stutters on a phone
    std::vector<char> blob;
    std::ifstream f(app::ExeDir() / "shadercache" / "pipelines.bin", std::ios::binary);
    if (f) blob.assign(std::istreambuf_iterator<char>(f), {});
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.data();
    if (vkCreatePipelineCache(dev_, &ci, nullptr, &cache_) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(dev_, &ci, nullptr, &cache_));
    }
    cache_saved_ = blob.size();
    return true;
}

bool VulkanBackend::CreateFrames() {
    VkSemaphoreTypeCreateInfoKHR ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    if (timeline_ok_) VK_CHECK(vkCreateSemaphore(dev_, &si, nullptr, &timeline_));
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = qfam_;
        VK_CHECK(vkCreateCommandPool(dev_, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev_, &ai, &f.cb));
        if (!CreateBuffer(f.upload, kUploadSize,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true))
            return false;
        VkSemaphoreCreateInfo bi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev_, &bi, nullptr, &f.acquire));
    }
    return true;
}

bool VulkanBackend::CreateFixedImages() {
    constexpr VkImageUsageFlags kSampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(lut_, kBgra, 256, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D) ||
        !CreateImage(dummy_[0], kBgra, 1, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D) ||
        !CreateImage(dummy_[1], kBgra, 1, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_3D) ||
        !CreateImage(dummy_[2], kBgra, 1, 1, 1, 6, 1, kSampled, VK_IMAGE_VIEW_TYPE_CUBE, true))
        return false;
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    linear_ = Sampler(sc);
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    point_ = Sampler(sc);
    // the first frame's commands: the fixed images cleared to black, the ramp the identity
    Begin();
    VkClearColorValue black{};
    for (Image* im : {&lut_, &dummy_[0], &dummy_[1], &dummy_[2]}) {
        Transition(*im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->layers};
        vkCmdClearColorImage(Cb(), im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
        Transition(*im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (int i = 0; i < 768; i++) ramp_[i] = uint8_t(i & 255);
    UploadGammaRamp();
    Submit(true, false);
    // Submit(true) began the next recording; Init begins once more
    vkEndCommandBuffer(Cb());
    vkResetCommandPool(dev_, CurrentFrame().pool, 0);
    return true;
}

// ---- memory and objects

bool VulkanBackend::CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers,
                                uint32_t levels, VkImageUsageFlags usage, VkImageViewType vt, bool cube,
                                VkSampleCountFlagBits samples) {
    im = {};
    im.fmt = fmt;
    im.w = w;
    im.h = h;
    im.d = d;
    im.layers = layers;
    im.levels = levels;
    im.samples = samples;
    im.depth = fmt == depth_fmt_;
    im.stencil = im.depth && HasStencil(fmt);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev_, im.image, &r);
    if (!alloc_.Get(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, im.mem) ||
        vkBindImageMemory(dev_, im.image, im.mem.mem, im.mem.off) != VK_SUCCESS) {
        Destroy(im);
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = vt;
    vi.format = fmt;
    vi.subresourceRange = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                           levels, 0, layers};
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && samples == VK_SAMPLE_COUNT_1_BIT)
        vkCreateImageView(dev_, &vi, nullptr, &im.view);
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (im.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                                                           : VK_IMAGE_ASPECT_COLOR_BIT),
                               0, 1, 0, 1};
        vkCreateImageView(dev_, &vi, nullptr, &im.attach);
    }
    return true;
}

bool VulkanBackend::CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    b = {};
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    if (vkCreateBuffer(dev_, &ci, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev_, b.buf, &r);
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryPropertyFlags nice = host && (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
    if (!alloc_.Get(r, want, nice, true, b.mem) || vkBindBufferMemory(dev_, b.buf, b.mem.mem, b.mem.off) != VK_SUCCESS) {
        Destroy(b);
        return false;
    }
    return true;
}

void VulkanBackend::Destroy(Image& im) {
    if (im.view) {
        if (last_set_) {
            for (int i = 0; i < 4; ++i) {
                if (last_set_views_[i] == im.view) {
                    last_set_ = VK_NULL_HANDLE;
                    break;
                }
            }
        }
        for (Frame& fr : frames_) {
            auto vit = fr.view_keys.find(im.view);
            if (vit != fr.view_keys.end()) {
                for (uint64_t key : vit->second) fr.sets.erase(key);
                fr.view_keys.erase(vit);
            }
        }
        vkDestroyImageView(dev_, im.view, nullptr);
    }
    DropFramebuffers(im.attach);
    if (im.attach) vkDestroyImageView(dev_, im.attach, nullptr);
    if (im.image) vkDestroyImage(dev_, im.image, nullptr);
    alloc_.Free(im.mem);
    im = {};
}

void VulkanBackend::Destroy(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    alloc_.Free(b.mem);
    b = {};
}

// bytes in this frame's ring; a big upload gets a buffer of its own, released with the frame. A full ring flushes:
// callers upload before they begin rendering and bind
VulkanBackend::Alloc VulkanBackend::Upload(VkDeviceSize size, VkDeviceSize align) {
    if (size > kUploadSize / 4) {
        Buffer b;
        if (!CreateBuffer(b, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true))
            return {VK_NULL_HANDLE, 0, nullptr};
        Alloc a{b.buf, 0, b.mem.map};
        CurrentFrame().buffers.push_back(b);
        return a;
    }
    Frame* f = &CurrentFrame();
    VkDeviceSize o = (f->used + align - 1) / align * align;
    if (o + size > kUploadSize) {
        Submit(true, false);
        f = &CurrentFrame();
        o = 0;
    }
    f->used = o + size;
    return {f->upload.buf, o, f->upload.mem.map + o};
}

VulkanBackend::Alloc VulkanBackend::UploadCached(const uint8_t* src, uint32_t bytes, VkDeviceSize align,
                                                 bool allow_cache) {
    const uint32_t alloc_bytes = std::max<uint32_t>(bytes, 4u);
    if (allow_cache && src && bytes >= 256u && bytes <= (kUploadSize / 8) && kernel::g_base) {
        const uintptr_t p = reinterpret_cast<uintptr_t>(src);
        const uintptr_t base = reinterpret_cast<uintptr_t>(kernel::g_base);
        if (p >= base && (p - base) < 0x100000000ull) {
            const uint32_t guest_va = uint32_t(p - base);
            const uint64_t key = (uint64_t(guest_va) << 32) | uint64_t(bytes);
            const uint64_t ep = DirtyEpoch(guest_va);
            const uint64_t sig = FastBufSig(src, bytes);

            // 1. Persistent DEVICE_LOCAL Static Object / Geometry Cache (zero per-frame memcpy, fast VRAM reads!)
            ++frame_buf_lookups_;
            constexpr VkDeviceSize kStaticBufBudget = 64ull << 20;
            StaticBuf& sb = static_bufs_[key];
            if (sb.promoted && sb.gpu.buf && sb.bytes == bytes && sb.epoch == ep &&
                (sb.last_frame == presents_ + 1 || sb.fast_sig == sig)) {
                sb.last_frame = presents_ + 1;
                ++perf_static_buf_hits_;
                ++frame_buf_hits_;
                return {sb.gpu.buf, 0, nullptr};
            }
            if (sb.promoted && (sb.bytes != bytes || sb.epoch != ep || sb.fast_sig != sig)) {
                if (sb.gpu.buf) {
                    static_buf_bytes_ -= std::min<VkDeviceSize>(static_buf_bytes_, sb.bytes);
                    Retire(sb.gpu);
                }
                sb.promoted = false;
                sb.stable_frames = 0;
                sb.has_aabb = false;
            }
            if (!sb.promoted) {
                if (sb.bytes == bytes && sb.epoch == ep && sb.fast_sig == sig) {
                    if (sb.last_frame != presents_ + 1) ++sb.stable_frames;
                } else {
                    sb.bytes = bytes;
                    sb.epoch = ep;
                    sb.fast_sig = sig;
                    sb.stable_frames = 0;
                    sb.has_aabb = false;
                }
                sb.last_frame = presents_ + 1;
                if (sb.stable_frames >= 2u && bytes <= (4u << 20) && static_buf_bytes_ + bytes <= kStaticBufBudget) {
                    Buffer dev_buf;
                    if (CreateBuffer(dev_buf, alloc_bytes,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     false)) {
                        Alloc stg = Upload(alloc_bytes, align);
                        if (stg.cpu) {
                            std::memcpy(stg.cpu, src, bytes);
                            EndRender();
                            VkBufferCopy cp{stg.off, 0, alloc_bytes};
                            vkCmdCopyBuffer(Cb(), stg.buf, dev_buf.buf, 1, &cp);
                            VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                            bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            bb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
                            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            bb.buffer = dev_buf.buf;
                            bb.offset = 0;
                            bb.size = alloc_bytes;
                            vkCmdPipelineBarrier(Cb(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
                            sb.gpu = dev_buf;
                            sb.promoted = true;
                            static_buf_bytes_ += bytes;
                            ++perf_static_buf_hits_;
                            return {sb.gpu.buf, 0, nullptr};
                        }
                        Destroy(dev_buf);
                    }
                }
            }

            // 2. Intra-frame upload ring deduplication cache
            Frame& f = CurrentFrame();
            auto it = f.buf_cache.find(key);
            if (it != f.buf_cache.end() && it->second.epoch == ep && it->second.sig == sig) {
                ++perf_vb_hits_;
                ++frame_buf_hits_;
                return it->second.alloc;
            }
            Alloc a = Upload(alloc_bytes, align);
            if (a.cpu) {
                std::memcpy(a.cpu, src, bytes);
                // Re-fetch CurrentFrame() in case Upload() flushed & reset the ring.
                CurrentFrame().buf_cache[key] = {a, ep, sig};
            }
            return a;
        }
    }
    Alloc a = Upload(alloc_bytes, align);
    if (a.cpu && src && bytes) std::memcpy(a.cpu, src, bytes);
    return a;
}

VulkanBackend::Alloc VulkanBackend::Constants(const void* data, size_t bytes) {
    Alloc a = Upload(kUboRange, kUboAlign);
    std::memcpy(a.cpu, data, std::min<size_t>(bytes, kUboRange));
    return a;
}

VulkanBackend::Alloc VulkanBackend::ConstantsTrimmed(const void* data, size_t bytes) {
    const size_t copy_bytes = std::min<size_t>(bytes, kUboRange);
    const VkDeviceSize step = std::max<VkDeviceSize>(
        ((VkDeviceSize(copy_bytes) + kUboAlign - 1u) / kUboAlign) * kUboAlign, kUboAlign);
    Frame* f = &CurrentFrame();
    VkDeviceSize o = (f->used + kUboAlign - 1u) / kUboAlign * kUboAlign;
    // Ensure descriptor binding 0 [o, o + kUboRange] stays strictly within kUploadSize
    if (o + kUboRange > kUploadSize) {
        Submit(true, false);
        f = &CurrentFrame();
        o = 0;
    }
    f->used = o + step;
    Alloc a{f->upload.buf, o, f->upload.mem.map + o};
    if (copy_bytes) std::memcpy(a.cpu, data, copy_bytes);
    return a;
}

// ---- command recording

void VulkanBackend::Begin() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(Cb(), &bi);
    rendering_ = false;
    rend_color_ = rend_depth_ = nullptr;
    ++render_seq_;
    Frame& f = CurrentFrame();
    f.dummy_vc = {};
    f.has_last_vsc = false;
    f.has_last_dc = false;
    f.buf_cache.clear();
    cmd_pipe_ = VK_NULL_HANDLE;
    cmd_set_ = VK_NULL_HANDLE;
    cmd_vb_count_ = 0;
    cmd_ib_ = VK_NULL_HANDLE;
    last_set_ = VK_NULL_HANDLE;
    has_vp_ = has_sc_ = has_blend_ = has_stencil_ = false;
}

uint64_t VulkanBackend::Completed() {
    if (!timeline_ok_) {
        std::lock_guard lock(fence_lock_);
        PollFences();
        return fenced_;
    }
    uint64_t v = 0;
    counter_value_(dev_, timeline_, &v);
    return v;
}

// the signalled fences, oldest first, back to the spares; called with fence_lock_ held
void VulkanBackend::PollFences() {
    size_t done = 0;
    while (done < pending_.size() && vkGetFenceStatus(dev_, pending_[done].fence) == VK_SUCCESS) {
        fenced_ = pending_[done].value;
        vkResetFences(dev_, 1, &pending_[done].fence);
        spare_fences_.push_back(pending_[done].fence);
        done++;
    }
    pending_.erase(pending_.begin(), pending_.begin() + done);
}

// the fence the submit of this value signals; called with fence_lock_ held
VkFence VulkanBackend::SubmitFence(uint64_t value) {
    VkFence f = VK_NULL_HANDLE;
    if (!spare_fences_.empty()) {
        f = spare_fences_.back();
        spare_fences_.pop_back();
    } else {
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev_, &ci, nullptr, &f) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    pending_.push_back({value, f});
    return f;
}

void VulkanBackend::WaitValue(uint64_t v) {
    if (!v || Completed() >= v) return;
    if (!timeline_ok_) {
        // the lock is held through the wait so no fence is reset under it; the GPU needs no thread to get there
        std::lock_guard lock(fence_lock_);
        for (const Pending& p : pending_)
            if (p.value >= v) {
                vkWaitForFences(dev_, 1, &p.fence, VK_TRUE, UINT64_MAX);
                break;
            }
        PollFences();
        return;
    }
    VkSemaphoreWaitInfoKHR wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &v;
    wait_semaphores_(dev_, &wi, UINT64_MAX);
}

// the frame's garbage freed and its pools reset, once the GPU is past it
void VulkanBackend::ResetFrame(Frame& f) {
    WaitValue(f.value);
    for (Image& im : f.images) Destroy(im);
    for (Buffer& b : f.buffers) Destroy(b);
    f.images.clear();
    f.buffers.clear();
    constexpr size_t kMaxCachedFrameSets = 2048;
    if (f.total_sets >= kMaxCachedFrameSets) {
        for (VkDescriptorPool p : f.pools) vkResetDescriptorPool(dev_, p, 0);
        f.pool_at = 0;
        f.sets.clear();
        f.view_keys.clear();
        f.total_sets = 0;
    }
    f.used = 0;
    f.dummy_vc = {};
    f.has_last_vsc = false;
    f.has_last_dc = false;
    f.buf_cache.clear();
}

// present: the frame's last commands, waiting on the acquired image and signalling its present semaphore
void VulkanBackend::Submit(bool wait, bool present) {
    EndRender();
    Frame& f = CurrentFrame();
    vkEndCommandBuffer(f.cb);
    uint64_t signal = ++value_;
    // without the timeline only the present semaphore is signalled, and a fence tells when the GPU is past
    VkSemaphore signals[2] = {timeline_, VK_NULL_HANDLE};
    uint64_t values[2] = {signal, 0};
    uint32_t first = timeline_ok_ ? 0 : 1;
    VkTimelineSemaphoreSubmitInfoKHR ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR};
    ti.signalSemaphoreValueCount = present ? 2 : 1;
    ti.pSignalSemaphoreValues = values;
    uint64_t wait_value = 0;
    ti.waitSemaphoreValueCount = present ? 1 : 0;
    ti.pWaitSemaphoreValues = &wait_value;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, timeline_ok_ ? &ti : nullptr};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (present) {
        signals[1] = swap_done_[present_index_];
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquire;
        si.pWaitDstStageMask = &stage;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cb;
    si.signalSemaphoreCount = (present ? 2 : 1) - first;
    si.pSignalSemaphores = signals + first;
    VkResult r;
    if (timeline_ok_) {
        r = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    } else {
        std::lock_guard lock(fence_lock_);
        r = vkQueueSubmit(queue_, 1, &si, SubmitFence(signal));
    }
    if (r != VK_SUCCESS)
        LOGE("vulkan: submit %llu%s failed (%d)", (unsigned long long)signal, present ? " (present)" : "", int(r));
    f.value = signal;
    if (wait) {
        ResetFrame(f);
        vkResetCommandPool(dev_, f.pool, 0);
        Begin();
    }
}

void VulkanBackend::Transition(Image& im, VkImageLayout to) {
    if (im.layout == to) return;
    EndRender();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkPipelineStageFlags src, dst;
    LayoutMasks(im.layout, src, b.srcAccessMask);
    LayoutMasks(to, dst, b.dstAccessMask);
    if (im.layout == VK_IMAGE_LAYOUT_UNDEFINED) src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    b.oldLayout = im.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    VkImageAspectFlags aspect = im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (im.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                                         : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange = {aspect, 0, im.levels, 0, im.layers};
    vkCmdPipelineBarrier(Cb(), src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    im.layout = to;
    im.cb_render_seq = 0;
}

// a rendering over the whole of the attachments, which keep what they hold; the one under way carries on when it
// has the same ones
void VulkanBackend::BeginRender(Image* color, Image* depth, Image* resolve) {
    if (rendering_ && color == rend_color_ && depth == rend_depth_ && resolve == rend_resolve_) return;
    EndRender();
    if (color) Transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (resolve) Transition(*resolve, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth) Transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    // Emit attachment write->read/write barrier only if any attachment already rendered in this CB without transition.
    if ((color && color->cb_render_seq == render_seq_) ||
        (resolve && resolve->cb_render_seq == render_seq_) ||
        (depth && depth->cb_render_seq == render_seq_)) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        vkCmdPipelineBarrier(Cb(), st, st, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    if (color) color->cb_render_seq = render_seq_;
    if (resolve) resolve->cb_render_seq = render_seq_;
    if (depth) depth->cb_render_seq = render_seq_;
    rendering_ = true;
    rend_color_ = color;
    rend_depth_ = depth;
    rend_resolve_ = resolve;
    Image* any = color ? color : depth;
    const VkSampleCountFlagBits samples = any ? any->samples : VK_SAMPLE_COUNT_1_BIT;
    const bool has_resolve = (color != nullptr) && (resolve != nullptr) && (samples > VK_SAMPLE_COUNT_1_BIT);
    if (!dynamic_) {
        VkFormat cf = color ? color->fmt : VK_FORMAT_UNDEFINED, df = depth ? depth->fmt : VK_FORMAT_UNDEFINED;
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = Pass(cf, df, false, false, samples, has_resolve);
        bi.framebuffer = Framebuffer(cf, df, color ? color->attach : VK_NULL_HANDLE,
                                     depth ? depth->attach : VK_NULL_HANDLE, any->w, any->h,
                                     has_resolve ? resolve->attach : VK_NULL_HANDLE, samples);
        bi.renderArea = {{0, 0}, {any->w, any->h}};
        vkCmdBeginRenderPass(Cb(), &bi, VK_SUBPASS_CONTENTS_INLINE);
        return;
    }
    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingAttachmentInfoKHR da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    ri.layerCount = 1;
    if (color) {
        ca.imageView = color->attach;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (has_resolve) {
            ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            ca.resolveImageView = resolve->attach;
            ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (depth) {
        da.imageView = depth->attach;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        if (depth->stencil) ri.pStencilAttachment = &da;
    }
    begin_rendering_(Cb(), &ri);
}

void VulkanBackend::BeginRenderClear(Image* color, Image* depth, bool clear_c, const float cc[4], bool clear_z, float z,
                                     bool clear_s, uint32_t stencil, Image* resolve) {
    EndRender();
    if (color) Transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (resolve) Transition(*resolve, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth) Transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    if ((color && color->cb_render_seq == render_seq_) ||
        (resolve && resolve->cb_render_seq == render_seq_) ||
        (depth && depth->cb_render_seq == render_seq_)) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        vkCmdPipelineBarrier(Cb(), st, st, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    if (color) color->cb_render_seq = render_seq_;
    if (resolve) resolve->cb_render_seq = render_seq_;
    if (depth) depth->cb_render_seq = render_seq_;
    rendering_ = true;
    rend_color_ = color;
    rend_depth_ = depth;
    rend_resolve_ = resolve;
    Image* any = color ? color : depth;
    const VkSampleCountFlagBits samples = any ? any->samples : VK_SAMPLE_COUNT_1_BIT;
    const bool has_resolve = (color != nullptr) && (resolve != nullptr) && (samples > VK_SAMPLE_COUNT_1_BIT);
    if (!dynamic_) {
        VkFormat cf = color ? color->fmt : VK_FORMAT_UNDEFINED;
        VkFormat df = depth ? depth->fmt : VK_FORMAT_UNDEFINED;
        const bool clear_ds = depth && clear_z && (!depth->stencil || clear_s);
        VkClearValue cvs[3] = {};
        uint32_t cv_count = 0;
        if (color) {
            if (clear_c) std::memcpy(cvs[cv_count].color.float32, cc, sizeof(float) * 4);
            ++cv_count;
            if (has_resolve) ++cv_count;
        }
        if (depth) {
            cvs[cv_count].depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
            ++cv_count;
        }
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = Pass(cf, df, clear_c, clear_ds, samples, has_resolve);
        bi.framebuffer = Framebuffer(cf, df, color ? color->attach : VK_NULL_HANDLE,
                                     depth ? depth->attach : VK_NULL_HANDLE, any->w, any->h,
                                     has_resolve ? resolve->attach : VK_NULL_HANDLE, samples);
        bi.renderArea = {{0, 0}, {any->w, any->h}};
        bi.clearValueCount = cv_count;
        bi.pClearValues = cvs;
        vkCmdBeginRenderPass(Cb(), &bi, VK_SUBPASS_CONTENTS_INLINE);
        if (depth && !clear_ds && (clear_z || (clear_s && depth->stencil))) {
            VkClearAttachment ca{};
            ca.aspectMask = (clear_z ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                            (clear_s && depth->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            ca.clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
            VkClearRect cr{{{0, 0}, {any->w, any->h}}, 0, 1};
            vkCmdClearAttachments(Cb(), 1, &ca, 1, &cr);
        }
        return;
    }
    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingAttachmentInfoKHR da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingAttachmentInfoKHR sa{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    ri.layerCount = 1;
    if (color) {
        ca.imageView = color->attach;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = clear_c ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (clear_c) std::memcpy(ca.clearValue.color.float32, cc, sizeof(float) * 4);
        if (has_resolve) {
            ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            ca.resolveImageView = resolve->attach;
            ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (depth) {
        da.imageView = depth->attach;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = clear_z ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        ri.pDepthAttachment = &da;
        if (depth->stencil) {
            sa = da;
            sa.loadOp = clear_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            ri.pStencilAttachment = &sa;
        }
    }
    begin_rendering_(Cb(), &ri);
}

void VulkanBackend::EndRender() {
    if (!rendering_) return;
    if (dynamic_)
        end_rendering_(Cb());
    else
        vkCmdEndRenderPass(Cb());
    rendering_ = false;
    rend_color_ = rend_depth_ = rend_resolve_ = nullptr;
    cmd_pipe_ = VK_NULL_HANDLE;
    cmd_set_ = VK_NULL_HANDLE;
    cmd_vb_count_ = 0;
    cmd_ib_ = VK_NULL_HANDLE;
    has_vp_ = has_sc_ = has_blend_ = has_stencil_ = false;
}

// the layouts stay the attachment ones throughout, as with dynamic rendering: barriers outside do the transitions.
// pipelines only need a compatible pass, which load ops are not part of
VkRenderPass VulkanBackend::Pass(VkFormat color, VkFormat depth, bool clear_c, bool clear_ds,
                                 VkSampleCountFlagBits samples, bool has_resolve, bool dont_care_c) {
    const bool resolve = has_resolve && (color != VK_FORMAT_UNDEFINED) && (samples > VK_SAMPLE_COUNT_1_BIT);
    const uint64_t c_op = dont_care_c ? 2ull : (clear_c ? 1ull : 0ull);
    const uint64_t k[6] = {uint64_t(color), uint64_t(depth), c_op, uint64_t(clear_ds),
                           uint64_t(samples), uint64_t(resolve)};
    uint64_t key = Hash(k, sizeof(k));
    if (auto it = passes_.find(key); it != passes_.end()) return it->second;
    VkAttachmentDescription at[3] = {};
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference rr{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint32_t n = 0;
    if (color != VK_FORMAT_UNDEFINED) {
        const VkAttachmentLoadOp color_load = dont_care_c ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                          : (clear_c ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
        at[n] = {0, color, samples, color_load,
                 VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        cr.attachment = n++;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &cr;
        if (resolve) {
            at[n] = {0, color, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                     VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            rr.attachment = n++;
            sp.pResolveAttachments = &rr;
        }
    }
    if (depth != VK_FORMAT_UNDEFINED) {
        at[n] = {0, depth, samples, clear_ds ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                 VK_ATTACHMENT_STORE_OP_STORE,
                 clear_ds ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        dr.attachment = n++;
        sp.pDepthStencilAttachment = &dr;
    }
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n;
    ci.pAttachments = at;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev_, &ci, nullptr, &rp) != VK_SUCCESS) LOGE("vulkan: render pass failed");
    passes_[key] = rp;
    return rp;
}

VkFramebuffer VulkanBackend::Framebuffer(VkFormat color_fmt, VkFormat depth_fmt, VkImageView color, VkImageView depth,
                                         uint32_t w, uint32_t h, VkImageView resolve,
                                         VkSampleCountFlagBits samples) {
    const bool has_resolve = (color != VK_NULL_HANDLE) && (resolve != VK_NULL_HANDLE) && (samples > VK_SAMPLE_COUNT_1_BIT);
    const uint64_t k[6] = {uint64_t(color), uint64_t(depth), uint64_t(has_resolve ? resolve : VK_NULL_HANDLE),
                           uint64_t(samples), w, h};
    uint64_t key = Hash(k, sizeof(k));
    if (auto it = framebuffers_.find(key); it != framebuffers_.end()) return it->second.fb;
    VkImageView views[3];
    uint32_t n = 0;
    if (color) {
        views[n++] = color;
        if (has_resolve) views[n++] = resolve;
    }
    if (depth) views[n++] = depth;
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = Pass(color_fmt, depth_fmt, false, false, samples, has_resolve);
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev_, &ci, nullptr, &fb) != VK_SUCCESS) LOGE("vulkan: framebuffer %ux%u failed", w, h);
    framebuffers_[key] = {color, depth, has_resolve ? resolve : VK_NULL_HANDLE, fb};
    return fb;
}

// a view's framebuffers go with it, once the GPU is past them
void VulkanBackend::DropFramebuffers(VkImageView v) {
    if (!v || framebuffers_.empty()) return;
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        if (it->second.color == v || it->second.depth == v || it->second.resolve == v) {
            vkDestroyFramebuffer(dev_, it->second.fb, nullptr);
            it = framebuffers_.erase(it);
        } else {
            ++it;
        }
    }
}

// in the D3D convention, y down from the top: a negative height turns Vulkan's clip space the same way up
void VulkanBackend::SetViewport(float x, float y, float w, float h) {
    VkViewport vp{x, y + h, w, -h, 0, 1};
    if (has_vp_ && std::memcmp(&cmd_vp_, &vp, sizeof(vp)) == 0) return;
    cmd_vp_ = vp;
    has_vp_ = true;
    vkCmdSetViewport(Cb(), 0, 1, &vp);
}

void VulkanBackend::SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    VkRect2D r{{std::max(x0, 0), std::max(y0, 0)}, {uint32_t(std::max(x1 - std::max(x0, 0), 0)), uint32_t(std::max(y1 - std::max(y0, 0), 0))}};
    if (has_sc_ && std::memcmp(&cmd_sc_, &r, sizeof(r)) == 0) return;
    cmd_sc_ = r;
    has_sc_ = true;
    vkCmdSetScissor(Cb(), 0, 1, &r);
}

void VulkanBackend::BindPipeline(VkPipeline pipe) {
    if (pipe == cmd_pipe_) return;
    cmd_pipe_ = pipe;
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
}

void VulkanBackend::SetBlend(const float c[4]) {
    if (has_blend_ && std::memcmp(cmd_blend_, c, sizeof(cmd_blend_)) == 0) return;
    std::memcpy(cmd_blend_, c, sizeof(cmd_blend_));
    has_blend_ = true;
    vkCmdSetBlendConstants(Cb(), c);
}

void VulkanBackend::SetStencil(uint32_t ref) {
    if (has_stencil_ && cmd_stencil_ == ref) return;
    cmd_stencil_ = ref;
    has_stencil_ = true;
    vkCmdSetStencilReference(Cb(), VK_STENCIL_FACE_FRONT_AND_BACK, ref);
}

// the draw's descriptor set: the ring at b0 and b1 (offsets bound with it), the views and samplers; cached per frame
// and invalidated when a bound VkImageView is destroyed
VkDescriptorSet VulkanBackend::Set(const VkImageView views[4], const VkSampler samplers[4]) {
    if (last_set_ &&
        std::memcmp(last_set_views_, views, sizeof(VkImageView) * 4) == 0 &&
        std::memcmp(last_set_samplers_, samplers, sizeof(VkSampler) * 4) == 0) {
        return last_set_;
    }
    Frame& f = CurrentFrame();
    uint64_t key = Hash(views, sizeof(VkImageView) * 4, Hash(samplers, sizeof(VkSampler) * 4));
    if (auto it = f.sets.find(key); it != f.sets.end()) {
        std::memcpy(last_set_views_, views, sizeof(VkImageView) * 4);
        std::memcpy(last_set_samplers_, samplers, sizeof(VkSampler) * 4);
        last_set_ = it->second;
        return it->second;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
        if (f.pool_at == f.pools.size()) {
            VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetsPerPool * 2},
                                             {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool * 4},
                                             {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerPool * 4}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = kSetsPerPool;
            pi.poolSizeCount = 3;
            pi.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(dev_, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            f.pools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pools[f.pool_at];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        if (vkAllocateDescriptorSets(dev_, &ai, &set) == VK_SUCCESS) break;
        f.pool_at++;
    }
    VkDescriptorBufferInfo bi{f.upload.buf, 0, kUboRange};
    VkDescriptorImageInfo ii[4], si[4];
    VkWriteDescriptorSet w[vkshader::kBindings];
    for (uint32_t i = 0; i < vkshader::kBindings; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        if (i < 2) {
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[i].pBufferInfo = &bi;
        } else if (i < vkshader::kBindingSamplers) {
            uint32_t s = i - vkshader::kBindingTextures;
            ii[s] = {VK_NULL_HANDLE, views[s] ? views[s] : dummy_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[i].pImageInfo = &ii[s];
        } else {
            uint32_t s = i - vkshader::kBindingSamplers;
            si[s] = {samplers[s] ? samplers[s] : point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            w[i].pImageInfo = &si[s];
        }
    }
    vkUpdateDescriptorSets(dev_, vkshader::kBindings, w, 0, nullptr);
    f.sets[key] = set;
    ++f.total_sets;
    for (int i = 0; i < 4; i++) {
        if (views[i]) f.view_keys[views[i]].push_back(key);
    }
    std::memcpy(last_set_views_, views, sizeof(VkImageView) * 4);
    std::memcpy(last_set_samplers_, samplers, sizeof(VkSampler) * 4);
    last_set_ = set;
    return set;
}

void VulkanBackend::BindSet(VkDescriptorSet set, VkDeviceSize vc, VkDeviceSize dc) {
    if (set == cmd_set_ && vc == cmd_vc_off_ && dc == cmd_dc_off_) return;
    cmd_set_ = set;
    cmd_vc_off_ = vc;
    cmd_dc_off_ = dc;
    uint32_t offs[2] = {uint32_t(vc), uint32_t(dc)};
    vkCmdBindDescriptorSets(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 2, offs);
}

// ---- shaders and pipelines

VkShaderModule VulkanBackend::Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash) {
    uint64_t seed = Hash(entry, std::strlen(entry), stage == vkshader::Stage::Vertex ? 1 : 2);
    hash = Hash(src.data(), src.size(), seed);
    if (auto it = modules_.find(hash); it != modules_.end()) return it->second;
    std::vector<uint32_t> spirv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkshader::Compile(src, entry, stage, hash, spirv)) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spirv.size() * 4;
        ci.pCode = spirv.data();
        if (vkCreateShaderModule(dev_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    }
    modules_[hash] = m;
    return m;
}

// a full screen pass: the triangle from SV_VertexID, solid, no culling; depth set: writes depth only
VkPipeline VulkanBackend::FullScreenPipeline(const char* hlsl, const char* ps_entry, VkFormat color, VkFormat depth,
                                             bool over, uint8_t wmask) {
    uint64_t key = Hash(hlsl, std::strlen(hlsl),
                        uint64_t(color) << 32 | uint64_t(depth) << 5 | uint64_t(wmask & 0xF) << 1 | (over ? 1 : 0));
    key = Hash(ps_entry, std::strlen(ps_entry), key);
    if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
    uint64_t hv, hp;
    VkShaderModule vs = Module(hlsl, "vs", vkshader::Stage::Vertex, hv);
    VkShaderModule ps = Module(hlsl, ps_entry, vkshader::Stage::Pixel, hp);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vs && ps) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = ps;
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = ds.depthWriteEnable = depth != VK_FORMAT_UNDEFINED;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = wmask & 0xF;
        if (over) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = color != VK_FORMAT_UNDEFINED ? 1 : 0;
        cb.pAttachments = &ba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfoKHR rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        rinfo.colorAttachmentCount = cb.attachmentCount;
        rinfo.pColorAttachmentFormats = &color;
        rinfo.depthAttachmentFormat = depth;
        rinfo.stencilAttachmentFormat = HasStencil(depth) ? depth : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic_ ? &rinfo : nullptr};
        if (!dynamic_) pi.renderPass = Pass(color, depth);
        pi.stageCount = 2;
        pi.pStages = st;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = layout_;
        if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    }
    pipelines_[key] = pipe;
    return pipe;
}

VkPipeline VulkanBackend::OverlayPipeline(bool sdf, VkFormat color) {
    if (overlay_pipe_fmt_ == color) {
        VkPipeline cached = sdf ? overlay_sdf_pipe_ : overlay_tex_pipe_;
        if (cached != VK_NULL_HANDLE) return cached;
    } else {
        overlay_tex_pipe_ = VK_NULL_HANDLE;
        overlay_sdf_pipe_ = VK_NULL_HANDLE;
        overlay_pipe_fmt_ = color;
    }
    const char* hlsl = sdf ? kOverlaySdfHlsl : kOverlayTexHlsl;
    uint64_t key = Hash(hlsl, std::strlen(hlsl), (uint64_t(color) << 32) | (sdf ? 0x201u : 0x101u));
    if (auto it = pipelines_.find(key); it != pipelines_.end()) {
        if (sdf) overlay_sdf_pipe_ = it->second;
        else overlay_tex_pipe_ = it->second;
        return it->second;
    }
    uint64_t hv, hp;
    VkShaderModule vs = Module(hlsl, "vs", vkshader::Stage::Vertex, hv);
    VkShaderModule ps = Module(hlsl, "psmain", vkshader::Stage::Pixel, hp);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vs && ps) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = ps;
        st[1].pName = "main";

        VkVertexInputBindingDescription vb{};
        vb.binding = 0;
        vb.stride = sdf ? sizeof(OverlaySdfVert) : sizeof(OverlayTexVert);
        vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription va[6]{};
        uint32_t na = 0;
        if (!sdf) {
            va[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayTexVert, x)};
            va[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayTexVert, u)};
            va[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OverlayTexVert, r)};
            na = 3;
        } else {
            va[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlaySdfVert, x)};
            va[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlaySdfVert, lx)};
            va[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OverlaySdfVert, r)};
            va[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OverlaySdfVert, gw)};
            va[4] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OverlaySdfVert, br)};
            va[5] = {5, 0, VK_FORMAT_R32_SFLOAT, offsetof(OverlaySdfVert, mode)};
            na = 6;
        }
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = na;
        vi.pVertexAttributeDescriptions = va;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xF;
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfoKHR rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        rinfo.colorAttachmentCount = 1;
        rinfo.pColorAttachmentFormats = &color;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic_ ? &rinfo : nullptr};
        if (!dynamic_) pi.renderPass = Pass(color, VK_FORMAT_UNDEFINED);
        pi.stageCount = 2;
        pi.pStages = st;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = layout_;
        if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    }
    pipelines_[key] = pipe;
    if (sdf) overlay_sdf_pipe_ = pipe;
    else overlay_tex_pipe_ = pipe;
    return pipe;
}

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps) {
    uint64_t h = Hash(&k, sizeof(k));
    if (mru_pso_pipe_ != VK_NULL_HANDLE && mru_pso_hash_ == h) {
        ++perf_pso_hits_;
        return mru_pso_pipe_;
    }
    if (auto it = pipelines_.find(h); it != pipelines_.end()) {
        mru_pso_hash_ = h;
        mru_pso_pipe_ = it->second;
        return it->second;
    }
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = "main";
    // the attributes at their register's location, as vk_shader.cpp numbers the inputs
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t na = 0;
    for (uint32_t i = 0; i < k.buffers; i++) vb[i] = {i, k.strides[i], VK_VERTEX_INPUT_RATE_VERTEX};
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        va[na++] = {r, (e >> 8) & 0xFF, VertexFormat(e & 0xFF), e >> 16};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = k.buffers;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    Topology topo = Topology(k.topo & 0xF);
    bool strip = k.topo >> 4;
    ia.topology = topo == Topology::Points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : topo == Topology::Lines ? (strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                                            : (strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    constexpr uint32_t kFillWireframe = 0x1B01;
    rs.polygonMode = k.fill == kFillWireframe && feats_.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zbias != 0 || k.zslope != 0;
    rs.depthBiasConstantFactor = float(k.zbias);
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    const VkSampleCountFlagBits samples = VkSampleCountFlagBits(k.samples ? k.samples : VK_SAMPLE_COUNT_1_BIT);
    ms.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen;
    ds.depthWriteEnable = k.zwrite;
    ds.depthCompareOp = Cmp(k.zfunc);
    ds.stencilTestEnable = k.sten;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), Cmp(k.sfunc), k.smask, k.swmask, 0};
    ds.front = ds.back = so;
    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = k.blend;
    ba.srcColorBlendFactor = Blend(k.src, false);
    ba.dstColorBlendFactor = Blend(k.dst, false);
    ba.colorBlendOp = BlendOp(k.op);
    ba.srcAlphaBlendFactor = Blend(k.src, true);
    ba.dstAlphaBlendFactor = Blend(k.dst, true);
    ba.alphaBlendOp = BlendOp(k.op);
    ba.colorWriteMask = k.wmask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &ba;
    VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;
    VkFormat color = VkFormat(k.rtfmt);
    VkPipelineRenderingCreateInfoKHR rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rinfo.colorAttachmentCount = cb.attachmentCount;
    rinfo.pColorAttachmentFormats = &color;
    rinfo.depthAttachmentFormat = k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    rinfo.stencilAttachmentFormat = k.has_ds && HasStencil(depth_fmt_) ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic_ ? &rinfo : nullptr};
    if (!dynamic_) pi.renderPass = Pass(k.has_rt ? color : VK_FORMAT_UNDEFINED,
                                        k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED, false, false,
                                        samples, k.has_resolve != 0);
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipe) != VK_SUCCESS) {
        LOGE("vulkan: pipeline creation failed");
        pipe = VK_NULL_HANDLE;
    }
    pipelines_[h] = pipe;
    return pipe;
}

VkSampler VulkanBackend::Sampler(const VkSamplerCreateInfo& ci) {
    uint64_t key = Hash(&ci.magFilter, sizeof(VkSamplerCreateInfo) - offsetof(VkSamplerCreateInfo, magFilter));
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(dev_, &ci, nullptr, &s);
    samplers_[key] = s;
    return s;
}

// fixed border colours only: the console's nearest
VkBorderColor Border(uint32_t argb) {
    if ((argb >> 24) < 128) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    uint32_t sum = ((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255);
    return sum >= 384 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}

VkSampler VulkanBackend::StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp) {
    const auto& t = s.tss[size_t(i)];
    const uint32_t user_aniso = app::Get().aniso;
    const uint32_t shadow_func = stages.info[i].shadow ? CmpIndex(s.rs[kRsShadowFunc]) : 0u;
    const bool filterable = stages.fmt[i] != VK_FORMAT_R32_SFLOAT || r32f_linear_;
    const uint32_t flags = (clamp ? 1u : 0u) | (filterable ? 2u : 0u) | (stages.rt[i] ? 4u : 0u) |
                           (stages.info[i].shadow ? 8u : 0u) | (stages.info[i].rect ? 16u : 0u) |
                           (uint32_t(stages.info[i].kind & 3u) << 5) | (shadow_func << 8);
    const uint32_t sk[12] = {
        t[kTssMagFilter], t[kTssMinFilter], t[kTssMipFilter], t[kTssMaxAnisotropy],
        t[kTssAddressU], t[kTssAddressV], t[kTssAddressW], t[kTssMipLodBias],
        t[kTssMaxMipLevel], t[kTssBorderColor], stages.levels[i], (user_aniso << 16) | flags,
    };
    const uint64_t skey = Hash(sk, sizeof(sk));
    if (auto it = stage_samplers_.find(skey); it != stage_samplers_.end()) return it->second;

    VkSamplerCreateInfo sd{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    sd.minFilter = lin(min) && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.magFilter = lin(mag) && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mipmapMode = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

    // Accurate Anisotropic Filtering:
    // Only apply AF to filterable mipped 2D world textures (never UI rects, RTs, cubemaps, or shadow maps).
    // When user forces AF (2x..16x), upgrade mipmapMode to LINEAR (true trilinear AF) to eliminate road mip seams!
    const bool mipped = mip && stages.levels[i] > 1;
    const bool eligible_af = (stages.info[i].kind == kTex2D) && !stages.rt[i] &&
                             !stages.info[i].shadow && !stages.info[i].rect && filterable && mipped;
    uint32_t an = 1;
    if (eligible_af && user_aniso > 1 && lin(min)) {
        an = user_aniso;
        sd.minFilter = VK_FILTER_LINEAR;
        sd.magFilter = VK_FILTER_LINEAR;
        sd.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    } else if (eligible_af && user_aniso > 0 && (min == kTexfAnisotropic || mag == kTexfAnisotropic)) {
        an = std::max<uint32_t>(t[kTssMaxAnisotropy], 1);
    }
    if (an > 1 && feats_.samplerAnisotropy) {
        sd.anisotropyEnable = VK_TRUE;
        sd.maxAnisotropy = std::min(float(an), props_.limits.maxSamplerAnisotropy);
    } else {
        sd.anisotropyEnable = VK_FALSE;
        sd.maxAnisotropy = 1.0f;
    }

    uint32_t u = t[kTssAddressU], v = t[kTssAddressV];
    sd.addressModeU = Address(clamp && u == kTaddressWrap ? kTaddressClamp : u);
    sd.addressModeV = Address(clamp && v == kTaddressWrap ? kTaddressClamp : v);
    sd.addressModeW = Address(t[kTssAddressW]);
    const float limit = props_.limits.maxSamplerLodBias;
    float bias = mipped ? std::clamp(Bits(t[kTssMipLodBias]), -limit, limit) : 0.0f;
    if (sd.anisotropyEnable) bias = std::clamp(bias, -0.5f, 0.5f);
    sd.mipLodBias = bias;
    sd.minLod = std::min(float(t[kTssMaxMipLevel]), float(stages.levels[i] - 1));
    sd.maxLod = mip ? VK_LOD_CLAMP_NONE : sd.minLod;
    sd.borderColor = Border(t[kTssBorderColor]);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, Vulkan r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        bool linear = (lin(min) || lin(mag)) && depth_linear_;
        sd.minFilter = sd.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sd.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sd.compareEnable = VK_TRUE;
        sd.compareOp = VkCompareOp(swap[shadow_func]);
        sd.anisotropyEnable = VK_FALSE;
        sd.maxAnisotropy = 1.0f;
        sd.mipLodBias = 0.0f;
    }
    VkSampler res = Sampler(sd);
    if (stage_samplers_.size() < 4096) stage_samplers_[skey] = res;
    return res;
}

// ---- targets

Target* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(TargetMapKey(data, depth));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    return &it->second;
}

void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (draw_rt_ == &it->second) draw_rt_ = nullptr;
    if (draw_ds_ == &it->second) draw_ds_ = nullptr;
    Retire(it->second.img);
    Retire(it->second.msaa);
    Retire(it->second.fb);
    targets_.erase(it);
}

Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    uint64_t key = TargetMapKey(data, depth);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) ReleaseTarget(it);
    Target t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    t.w = std::max(1u, uint32_t(std::lround(gw * scale_)));
    t.h = std::max(1u, uint32_t(std::lround(gh * scale_)));
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!CreateImage(t.img, depth ? depth_fmt_ : kBgra, t.w, t.h, 1, 1, 1, usage, VK_IMAGE_VIEW_TYPE_2D)) {
        LOGE("vulkan: no %ux%u target", t.w, t.h);
        return nullptr;
    }
    // a new target starts cleared
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (depth) {
        VkClearDepthStencilValue one{1, 0};
        VkImageSubresourceRange r{VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (t.img.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &r);
    } else {
        VkClearColorValue black{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
    }
    // Companion hardware MSAA 2x/4x attachment for full-size 3D scene color & depth targets
    if (msaa_samples_ > VK_SAMPLE_COUNT_1_BIT && gw >= guest_w_ && gh >= guest_h_) {
        VkImageUsageFlags ms_usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                           : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (CreateImage(t.msaa, depth ? depth_fmt_ : kBgra, t.w, t.h, 1, 1, 1, ms_usage,
                        VK_IMAGE_VIEW_TYPE_2D, false, msaa_samples_)) {
            const float zero[4] = {0, 0, 0, 0};
            if (depth) {
                BeginRenderClear(nullptr, &t.msaa, false, zero, true, 1.0f, t.msaa.stencil, 0u, nullptr);
            } else {
                BeginRenderClear(&t.msaa, nullptr, true, zero, false, 1.0f, false, 0u, &t.img);
            }
            EndRender();
        }
    }
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    Target& ref = targets_[key];
    ref = std::move(t);
    return &ref;
}

// Xbox signed blend ops (D3DBLENDOP_REVSUBTRACTSIGNED / ADDSIGNED) accumulate signed values across passes
// (e.g. volumetric headlight front/back face cancellation). UNORM targets clamp negative intermediates to 0,
// so promote the colour target to RGBA16F on first signed blend while preserving its current pixels.
void VulkanBackend::UpgradeTargetSigned(Target& t) {
    constexpr VkFormat kSignedFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (t.depth || t.img.fmt == kSignedFmt) return;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(pd_, kSignedFmt, &fp);
    constexpr VkFormatFeatureFlags kNeed =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & kNeed) != kNeed) return;

    Image fresh;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!CreateImage(fresh, kSignedFmt, t.w, t.h, 1, 1, 1, usage, VK_IMAGE_VIEW_TYPE_2D)) return;
    if (t.drawn) {
        float uv[4] = {0, 0, 1, 1};
        Blit(fresh, {{0, 0}, {t.w, t.h}}, t.img, uv, {.alpha = true});
    } else {
        Transition(fresh, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue zero{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(Cb(), fresh.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &r);
    }
    EndRender();
    Retire(t.img);
    Retire(t.msaa);
    Retire(t.fb);
    t.img = std::move(fresh);
    if (msaa_samples_ > VK_SAMPLE_COUNT_1_BIT && t.gw >= guest_w_ && t.gh >= guest_h_) {
        if (CreateImage(t.msaa, kSignedFmt, t.w, t.h, 1, 1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                        VK_IMAGE_VIEW_TYPE_2D, false, msaa_samples_)) {
            const float zero[4] = {0, 0, 0, 0};
            BeginRenderClear(&t.msaa, nullptr, true, zero, false, 1.0f, false, 0u, nullptr);
            EndRender();
        }
    }
    LOGR("[OK] vulkan: upgraded target 0x%08X (%ux%u) to RGBA16F for signed blend ops", t.data, t.gw, t.gh);
}

// BGRA pixels into a new w x h image, left readable
bool VulkanBackend::PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    if (!CreateImage(im, kBgra, w, h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_VIEW_TYPE_2D))
        return false;
    Alloc a = Upload(VkDeviceSize(w) * h * 4, 16);
    if (!a.cpu) return false;
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * w * 4, &px[size_t(y) * pitch], w * 4);
    Transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// CPU pixels of a colour surface into its target, scaled; after a Sync only the changed ones, over the picture
void VulkanBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!DescribeSurface(s, d) && !Describe(s.header, d)) return;
    if (Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    Image tex;
    if (!PixelImage(tex, px, pitch, d.width, d.height)) return;
    float uv[4] = {0, 0, 1, 1};
    Blit(t.img, {{0, 0}, {t.w, t.h}}, tex, uv, {.alpha = true, .over = over});
    Retire(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// src over the rect of dst through the blit shader: its constants first, then the rendering
void VulkanBackend::Blit(Image& dst, const VkRect2D& to, Image& src, const float uvrect[4], const BlitOptions& o) {
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), o.mul};
    Alloc cb = Constants(c, sizeof(c));
    VkPipeline pipe = FullScreenPipeline(kBlitHlsl, "psmain", dst.fmt, VK_FORMAT_UNDEFINED, o.over);
    if (!pipe) return;
    Transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageView views[4] = {src.view, lut_.view, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkSampler samplers[4] = {linear_, point_, point_, point_};
    VkDescriptorSet set = Set(views, samplers);
    BeginRender(&dst, nullptr);
    BindPipeline(pipe);
    BindSet(set, cb.off, cb.off);
    SetViewport(float(to.offset.x), float(to.offset.y), float(to.extent.width), float(to.extent.height));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    vkCmdDraw(Cb(), 3, 1, 0, 0);
}

// a copy of the target to read while drawing to it
Image* VulkanBackend::Feedback(Target& t) {
    if (!t.fb && !CreateImage(t.fb, t.img.fmt, t.w, t.h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_VIEW_TYPE_2D))
        return nullptr;
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(t.fb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageAspectFlags aspect = t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCopy c{{aspect, 0, 0, 1}, {0, 0, 0}, {aspect, 0, 0, 1}, {0, 0, 0}, {t.w, t.h, 1}};
    vkCmdCopyImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(t.fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &t.fb;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool VulkanBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1)
        return false;
    auto di = targets_.find(TargetMapKey(mem::VirtualFromPhysical(s.rt.data), true));
    auto si = targets_.find(TargetMapKey(d.data, true));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    Alloc cb = Constants(nullptr, 0);
    VkPipeline pipe = FullScreenPipeline(kDepthHlsl, "psmain", VK_FORMAT_UNDEFINED, depth_fmt_, false);
    if (!pipe) return false;
    Transition(src.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageView views[4] = {src.img.view, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkSampler samplers[4] = {linear_, point_, point_, point_};
    VkDescriptorSet set = Set(views, samplers);
    BeginRender(nullptr, &dst.img);
    BindPipeline(pipe);
    BindSet(set, cb.off, cb.off);
    SetViewport(0, 0, float(dst.w), float(dst.h));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    vkCmdDraw(Cb(), 3, 1, 0, 0);
    dst.drawn = true;
    return true;
}

// src filtered into a new w x h BGRA image, left as a copy source
bool VulkanBackend::Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps) {
    if (!down || down.w != w || down.h != h) {
        Retire(down);
        if (!CreateImage(down, kBgra, w, h, 1, 1, 1,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_2D))
            return false;
    }
    float uv[4] = {0, 0, 1, 1};
    Blit(down, {{0, 0}, {w, h}}, src, uv, {.alpha = true, .taps = taps});
    Transition(down, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return true;
}

// the target as a guest sized BGRA copy source: itself, or shrunk into down
Image* VulkanBackend::CopySource(Target& t, Image& down) {
    if (t.depth) return nullptr;
    if (t.w != t.gw || t.h != t.gh || t.img.fmt != kBgra) {
        uint32_t taps = uint32_t(std::ceil(float(t.w) / t.gw - 1e-3f));
        return Shrink(t.img, t.gw, t.gh, down, std::max(taps, 1u)) ? &down : nullptr;
    }
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return &t.img;
}

// a copy source to the CPU, rows packed; waits for the GPU
bool VulkanBackend::ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    Buffer rb;
    if (!CreateBuffer(rb, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(Cb(), im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    Submit(true, false);
    out.assign(rb.mem.map, rb.mem.map + size_t(w) * h * 4);
    Destroy(rb);
    return true;
}

// ---- draws

void VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    if (mru_vs_ != VK_NULL_HANDLE && mru_vkey_ == sh.vkey) {
        sh.vh = mru_vh_;
        sh.vs = mru_vs_;
        return;
    }
    if (auto it = shader_src_.find(sh.vkey); it != shader_src_.end()) {
        sh.vh = it->second;
        sh.vs = modules_[sh.vh];
        mru_vkey_ = sh.vkey;
        mru_vh_ = sh.vh;
        mru_vs_ = sh.vs;
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    const char* entry = "main";
    if (a.topology == Topology::Points) {
        std::string points = PointsHlsl(src);
        if (!points.empty()) {
            src = std::move(points);
            entry = "main_points";
        }
    }
    sh.vs = Module(src, entry, vkshader::Stage::Vertex, sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

constexpr uint64_t kSignedPixelSalt = 0x5164E5B1E4D00001ull;

void VulkanBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh, bool signed_src) {
    uint64_t key = sh.pkey ^ kPixelKeySalt ^ (signed_src ? kSignedPixelSalt : 0ull);
    if (mru_ps_ != VK_NULL_HANDLE && mru_pkey_ == key) {
        sh.ph = mru_ph_;
        sh.ps = mru_ps_;
        return;
    }
    if (auto it = shader_src_.find(key); it != shader_src_.end()) {
        sh.ph = it->second;
        sh.ps = modules_[sh.ph];
        mru_pkey_ = key;
        mru_ph_ = sh.ph;
        mru_ps_ = sh.ps;
        return;
    }
    std::string hlsl = PixelHlsl(pk);
    if (signed_src) {
        // Xbox NV2A D3DBLENDOP_REVSUBTRACTSIGNED (0xF005) and ADDSIGNED (0xF006) reinterpret
        // each 8-bit unsigned fragment output channel as a 2's complement signed byte
        // (0xFF -> -1/255, 0x00 -> 0, 0x7F -> +127/255, 0x80 -> -128/255). With an RGBA16F
        // target, negative outputs pass unclamped into Vulkan blending so MC3D's headlight
        // cones add +1/255 on back faces (0xF005: dst - (-1/255)) and cancel -1/255 on front
        // faces (0xF006: dst + (-1/255)).
        constexpr char kRet[] = "return outc;";
        size_t pos = hlsl.rfind(kRet);
        if (pos != std::string::npos) {
            hlsl.replace(pos, sizeof(kRet) - 1,
                         "{ float4 _u = floor(saturate(outc) * 255.0 + 0.5); "
                         "outc = (_u - step(float4(128.0, 128.0, 128.0, 128.0), _u) * 256.0) / 255.0; } return outc;");
        }
    }
    sh.ps = Module(hlsl, "main", vkshader::Stage::Pixel, sh.ph);
    if (signed_src && sh.ps) {
        static bool logged_signed = false;
        if (!logged_signed) {
            logged_signed = true;
            LOGR("[OK] vulkan: compiled 2s-complement signed-blend pixel shader (hash=%016llX)",
                 (unsigned long long)sh.ph);
        }
    }
    shader_src_[key] = sh.ph;
}

constexpr float kUpscaledMin = 1.01f;

VkImageView VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                             DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return VK_NULL_HANDLE;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    stages.gw[stage] = t.depth ? 0 : t.gw;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    Target* rt = draw_rt_;
    float k = float(t.w) / (t.gw * stages.k[stage]);
    bool blur = rt && t.gw < guest_w_ && rt->gw < guest_w_;
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (blur || t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        stages.blur[stage] = blur;
        info.flags |= kTexWide;
    }
    stages.fmt[stage] = t.img.fmt;
    if (feedback) {
        Image* fb = Feedback(t);
        return fb ? fb->view : VK_NULL_HANDLE;
    }
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    Transition(t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t.img.view;
}

bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    Retire(tx.img);
    tx = Tex{};
    tx.d = d;
    bool bc = fi.compressed;
    tx.decode = bc && !bc_;
    switch (fi.host) {
        case Host::BC1: tx.fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
        case Host::BC2: tx.fmt = VK_FORMAT_BC2_UNORM_BLOCK; break;
        case Host::BC3: tx.fmt = VK_FORMAT_BC3_UNORM_BLOCK; break;
        case Host::R32F: tx.fmt = VK_FORMAT_R32_SFLOAT; break;
        default: tx.fmt = kBgra; break;
    }
    if (tx.decode) tx.fmt = kBgra;
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    VkImageViewType vt = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    if (!CreateImage(tx.img, tx.fmt, w, h, d.cube ? 1 : d.depth, d.cube ? 6 : 1, tx.levels,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, vt, d.cube)) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                  const uint32_t* colours) {
    Transition(tx.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t>& px = upload_px_;
    std::vector<uint8_t>& decoded = upload_decoded_;
    bool bc = fi.compressed && !tx.decode;
    uint32_t block = fi.host == Host::BC1 ? 8 : 16;
    for (uint32_t f = 0; f < faces; f++)
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            uint32_t mw = std::max(tx.img.w >> l, 1u), mh = std::max(tx.img.h >> l, 1u), md = std::max(tx.img.d >> l, 1u);
            const std::vector<uint8_t>* src = &px;
            if (tx.decode) {
                if (DecodeDxt(fi.host, px, pitch, mw, mh, md, decoded, &dxt_pool_)) ++perf_dxt_parallel_;
                pitch = mw * 4;
                src = &decoded;
            }
            // rows as Vulkan reads them packed: block rows for DXT, texel rows else
            uint32_t row = bc ? std::max(1u, (mw + 3) / 4) * block : mw * 4;
            uint32_t rows = bc ? std::max(1u, (mh + 3) / 4) : mh;
            size_t src_rows = pitch ? src->size() / pitch / md : 0;
            const VkDeviceSize total_bytes = VkDeviceSize(row) * rows * md;
            Alloc a = Upload(total_bytes, 16);
            if (!a.cpu) continue;
            if (pitch == row && src_rows >= rows && size_t(total_bytes) <= src->size()) {
                // Single sequential streaming copy saturates ARM64 write-combining buffers.
                std::memcpy(a.cpu, src->data(), size_t(total_bytes));
            } else {
                for (uint32_t z = 0; z < md; z++)
                    for (uint32_t y = 0; y < rows; y++) {
                        uint8_t* dst = a.cpu + (size_t(z) * rows + y) * row;
                        if (y < src_rows) std::memcpy(dst, &(*src)[(size_t(z) * src_rows + y) * pitch], std::min(pitch, row));
                        else std::memset(dst, 0, row);
                    }
            }
            VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1}, {0, 0, 0}, {mw, mh, md}};
            vkCmdCopyBufferToImage(Cb(), a.buf, tx.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
    Transition(tx.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

VkImageView VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                            DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return VK_NULL_HANDLE;
    Tex& t = textures_[key];
    if (t.frame != presents_ + 1 || !same || t.palette != palette.hash) {
        const bool pal_changed = (t.palette != palette.hash);
        t.frame = presents_ + 1;
        t.palette = palette.hash;
        const uint64_t ep = DirtyEpoch(d.data);
        const uint32_t total = TotalSize(d);
        const uint32_t rehash_mask = (fi.compressed && t.stable_frames >= 8u) ? 255u : 31u;
        bool need_full = !same || pal_changed || ep != t.epoch || t.stable_frames < 2u ||
                         (((presents_ + (d.data >> 6)) & rehash_mask) == 0u);
        if (!need_full) {
            const uint64_t fsig = FastMemSig(d.data, total);
            if (fsig == t.fast_sig) {
                ++t.stable_frames;
                ++perf_fast_tex_hits_;
            } else {
                need_full = true;
            }
        }
        if (need_full) {
            const uint64_t sig = MemSig(d.data, total) ^ ep ^ palette.hash;
            t.epoch = ep;
            t.fast_sig = FastMemSig(d.data, total);
            if (sig != t.sig || !same) {
                t.sig = sig;
                t.stable_frames = 0;
                UploadTexture(t, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)),
                              palette.va ? palette.colours : nullptr);
            } else {
                ++t.stable_frames;
            }
        }
    }
    Transition(t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    stages.fmt[stage] = t.fmt;
    stages.levels[stage] = t.levels;
    return t.img.view;
}

VkImageView VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.valid[stage] = false;
    stages.rt[stage] = false;
    stages.gw[stage] = 0;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.fmt[stage] = kBgra;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return VK_NULL_HANDLE;
    stages.desc[stage] = d;
    stages.valid[stage] = true;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    auto tit = targets_.find(TargetMapKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, true, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

bool VulkanBackend::BindStageTextures(const State& s, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) stages.view[i] = BindTexture(s, i, stages);
    // a stage without a texture reads a black one of its kind
    for (int i = 0; i < 4; i++) {
        if (stages.view[i]) continue;
        uint8_t kind = stages.info[i].kind;
        stages.view[i] = dummy_[kind == kTex3D ? 1 : kind == kTexCube ? 2 : 0].view;
        if (stages.info[i].shadow) stages.info[i].shadow = 0;
    }
    return true;
}

constexpr float kDepth24Max = 16777215.0f;

void VulkanBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (draw_ds_ ? float(draw_ds_->w) / draw_ds_->gw : 1.0f);
    }
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = sh.vh;
    k.ps = sh.ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++)
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    k.buffers = a.buffer_count;
    for (uint32_t i = 0; i < a.buffer_count; i++) k.strides[i] = a.buffers[i].stride;
    k.topo = uint32_t(a.topology) | (a.strip ? 0x10 : 0);
    k.has_rt = draw_rt_ != nullptr;
    k.has_ds = draw_ds_ != nullptr;
    k.rtfmt = draw_rt_ ? uint32_t(draw_rt_->img.fmt) : 0;
    const bool use_msaa = (msaa_samples_ > VK_SAMPLE_COUNT_1_BIT) && (draw_rt_ || draw_ds_) &&
                          (!draw_rt_ || bool(draw_rt_->msaa)) && (!draw_ds_ || bool(draw_ds_->msaa));
    k.samples = uint32_t(use_msaa ? msaa_samples_ : VK_SAMPLE_COUNT_1_BIT);
    k.has_resolve = (use_msaa && draw_rt_ != nullptr) ? 1u : 0u;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// the glow's last step: a render target at most half the frame's width added (blend ONE onto the frame) over a
// full size target. BLOOM off skips it, the glow passes before it then go unseen. A composite done any other way
// (one pass mixing both, alpha blended) is left alone, so the frame never goes black for it
bool VulkanBackend::GlowComposite(const State& s, const DrawTextures& stages) const {
    constexpr uint32_t kBlendOne = 1;
    if (!draw_rt_ || draw_rt_->gw < guest_w_ || !SimpleState(s, kMBlendEnable) || SimpleState(s, kMDestBlend) != kBlendOne)
        return false;
    for (int i = 0; i < 4; i++) {
        if (!stages.gw[i] || stages.gw[i] * 2 > draw_rt_->gw) continue;
        // each kind once in the log, to tell what it took
        static std::set<uint64_t> seen;
        uint64_t kind = uint64_t(stages.gw[i]) << 32 | SimpleState(s, kMSrcBlend) << 16 | draw_rt_->gw;
        if (seen.size() < 16 && seen.insert(kind).second)
            LOGR("vulkan: bloom off skips %u wide onto %u wide, src blend %u", stages.gw[i], draw_rt_->gw,
                 SimpleState(s, kMSrcBlend));
        return true;
    }
    return false;
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (DepthCopy(s)) return;
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    if (s.vs && (s.vs->program_type & 0xE) == 0x4) {
        a.pretransformed = true;
    }

    // the targets, CPU writes to them first
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    const bool signed_blend =
        rt && SimpleState(s, kMBlendEnable) && IsSignedBlendOp(SimpleState(s, kMBlendOp));
    if (signed_blend) UpgradeTargetSigned(*rt);
    const bool signed_src = signed_blend && rt->img.fmt == VK_FORMAT_R16G16B16A16_SFLOAT;
    draw_rt_ = rt;
    draw_ds_ = ds;

    // Early zero-area scissor & no-op write culling (before shader/texture/UBO/VB work!)
    Target& bt_pre = draw_rt_ ? *draw_rt_ : *draw_ds_;
    GuestExtent size;
    SurfaceSize(draw_rt_ ? s.rt : s.zs, size.w, size.h);
    {
        const float W = float(bt_pre.w), H = float(bt_pre.h);
        const float sc = W / size.w;
        int32_t x0 = int32_t(s.viewport.x * sc), y0 = int32_t(s.viewport.y * sc);
        int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc), y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
        if (s.scissor_count) {
            x0 = std::max(x0, int32_t(s.scissors[0] * sc));
            y0 = std::max(y0, int32_t(s.scissors[1] * sc));
            x1 = std::min(x1, int32_t(s.scissors[2] * sc));
            y1 = std::min(y1, int32_t(s.scissors[3] * sc));
        }
        x0 = std::clamp(x0, 0, int32_t(W));
        y0 = std::clamp(y0, 0, int32_t(H));
        x1 = std::clamp(x1, 0, int32_t(W));
        y1 = std::clamp(y1, 0, int32_t(H));
        if (x1 <= x0 || y1 <= y0) {
            ++perf_scissor_culled_;
            ++frame_culled_;
            return;
        }
    }

    DrawShaders sh = ClassifyShaders(s, a);
    float fixed_wv[16], fixed_full[16];
    if (sh.fixed_transform) {
        ComputeFixedMatrices(s, size, fixed_wv, fixed_full);
        if (!a.pretransformed && s.rs[kRsVertexBlend] == 0 && a.topology != Topology::Points && !s.scissor_count &&
            IsFixedDrawOutsideFrustum(a, fixed_full, static_bufs_)) {
            ++perf_frustum_culled_;
            ++frame_culled_;
            return;
        }
    }
    VsMeta vs_meta{};
    if (sh.prog) {
        EnsureProgramHash(s);
        auto mit = vs_meta_.find(s.vs->hash);
        if (mit == vs_meta_.end()) {
            vs_meta = AnalyzeVertexProgram(s.vs->program);
            vs_meta_.emplace(s.vs->hash, vs_meta);
        } else {
            vs_meta = mit->second;
        }
        if (vs_meta.mvp_cbase >= 0 && !a.pretransformed && s.rs[kRsVertexBlend] == 0 &&
            a.topology != Topology::Points && !s.scissor_count &&
            IsProgDrawOutsideFrustum(s, a, size, vs_meta.mvp_cbase, static_bufs_)) {
            ++perf_frustum_culled_;
            ++frame_culled_;
            return;
        }
    }
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    if (!BindStageTextures(s, stages) || (!draw_rt_ && !draw_ds_)) return;
    if (!game::Opt().bloom && GlowComposite(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh, signed_src);
    if (!sh.vs || !sh.ps) return;

    PsoKey k;
    BuildPipelineKey(s, a, sh, k);
    VkPipeline pipe = EnsurePipeline(k, sh.vs, sh.ps);
    if (!pipe) return;

    // everything uploaded before the rendering: a full ring flushes
    VkDeviceSize need = 2 * kUboRange + 1024;
    for (uint32_t i = 0; i < a.buffer_count; i++) need += a.buffers[i].bytes + 16;
    need += VkDeviceSize(a.index_count) * 4 + 16;
    if (need < kUploadSize / 4 && CurrentFrame().used + need > kUploadSize) Submit(true, false);
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, c);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, fixed_wv, fixed_full, c);
        FillLightConstants(s, sh.fk, c);
    }
    Alloc vc{};
    if (sh.prog) {
        const size_t vsc_bytes = std::clamp<size_t>(size_t(vs_meta.max_vsc_regs) * 16u, 16u, sizeof(s.vsc));
        Frame& cur = CurrentFrame();
        if (cur.has_last_vsc && cur.last_vsc_bytes >= vsc_bytes &&
            std::memcmp(cur.last_vsc.data(), s.vsc.data(), vsc_bytes) == 0) {
            vc = cur.last_vsc_alloc;
            ++perf_vsc_hits_;
        } else {
            vc = ConstantsTrimmed(s.vsc.data(), vsc_bytes);
            Frame& after = CurrentFrame();
            std::memcpy(after.last_vsc.data(), s.vsc.data(), vsc_bytes);
            after.last_vsc_bytes = vsc_bytes;
            after.last_vsc_alloc = vc;
            after.has_last_vsc = true;
        }
    } else {
        Frame& cur = CurrentFrame();
        if (!cur.dummy_vc.cpu) {
            static constexpr float kZeroVsc[4] = {};
            cur.dummy_vc = Constants(kZeroVsc, sizeof(kZeroVsc));
        }
        vc = cur.dummy_vc;
    }
    Alloc dc{};
    {
        Frame& cur = CurrentFrame();
        if (cur.has_last_dc && std::memcmp(&cur.last_dc, &c, sizeof(c)) == 0) {
            dc = cur.last_dc_alloc;
            ++perf_dc_hits_;
        } else {
            dc = Constants(&c, sizeof(c));
            Frame& after = CurrentFrame();
            after.last_dc = c;
            after.last_dc_alloc = dc;
            after.has_last_dc = true;
        }
    }
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16];
    const bool raw_guest_verts = a.vertices.empty();
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        const bool can_cache = raw_guest_verts && (b.src != a.gathered[i].data());
        Alloc v = UploadCached(b.src, b.bytes, 16, can_cache);
        vbufs[i] = v.buf;
        voffs[i] = v.off;
    }
    Alloc ib{};
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        const bool can_cache_ib = a.index16 && a.indices.empty();
        ib = UploadCached(static_cast<const uint8_t*>(a.index_src), ibytes, 4, can_cache_ib);
    }
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    VkSampler samplers[4];
    for (int i = 0; i < 4; i++) samplers[i] = s.textures[size_t(i)] ? StageSampler(s, i, stages, clamp) : point_;
    VkDescriptorSet set = Set(stages.view, samplers);

    // the rendering (bind companion MSAA 2x/4x attachments + automatic hardware resolve when active)
    const bool use_msaa = (msaa_samples_ > VK_SAMPLE_COUNT_1_BIT) && (draw_rt_ || draw_ds_) &&
                          (!draw_rt_ || bool(draw_rt_->msaa)) && (!draw_ds_ || bool(draw_ds_->msaa));
    Image* rt_att = draw_rt_ ? (use_msaa ? &draw_rt_->msaa : &draw_rt_->img) : nullptr;
    Image* ds_att = draw_ds_ ? (use_msaa ? &draw_ds_->msaa : &draw_ds_->img) : nullptr;
    Image* rt_res = (use_msaa && draw_rt_) ? &draw_rt_->img : nullptr;
    BeginRender(rt_att, ds_att, rt_res);
    if (draw_rt_) draw_rt_->drawn = true;
    if (draw_ds_) draw_ds_->drawn = true;
    BindPipeline(pipe);
    BindSet(set, vc.off, dc.off);
    Target& bt = draw_rt_ ? *draw_rt_ : *draw_ds_;
    float W = float(bt.w), H = float(bt.h);
    SetViewport(0, 0, W, H);
    float sc = W / size.w;
    int32_t x0 = int32_t(s.viewport.x * sc), y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc), y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    SetScissor(std::clamp(x0, 0, int32_t(W)), std::clamp(y0, 0, int32_t(H)), std::clamp(x1, 0, int32_t(W)),
               std::clamp(y1, 0, int32_t(H)));
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    SetBlend(bf);
    SetStencil(SimpleState(s, kMStencilRef) & 0xFF);
    if (a.buffer_count) {
        bool same_vb = (cmd_vb_count_ == a.buffer_count) &&
                       std::memcmp(cmd_vbufs_, vbufs, sizeof(VkBuffer) * a.buffer_count) == 0 &&
                       std::memcmp(cmd_voffs_, voffs, sizeof(VkDeviceSize) * a.buffer_count) == 0;
        if (!same_vb) {
            cmd_vb_count_ = a.buffer_count;
            std::memcpy(cmd_vbufs_, vbufs, sizeof(VkBuffer) * a.buffer_count);
            std::memcpy(cmd_voffs_, voffs, sizeof(VkDeviceSize) * a.buffer_count);
            vkCmdBindVertexBuffers(Cb(), 0, a.buffer_count, vbufs, voffs);
        }
    }
    if (a.index_count) {
        const VkIndexType itype = a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        if (cmd_ib_ != ib.buf || cmd_ib_off_ != ib.off || cmd_ib_type_ != itype) {
            cmd_ib_ = ib.buf;
            cmd_ib_off_ = ib.off;
            cmd_ib_type_ = itype;
            vkCmdBindIndexBuffer(Cb(), ib.buf, ib.off, itype);
        }
        vkCmdDrawIndexed(Cb(), a.index_count, 1, 0, a.base_vertex, 0);
        ++frame_draws_;
        frame_verts_ += a.index_count;
    } else {
        vkCmdDraw(Cb(), a.draw_count, 1, 0, 0);
        ++frame_draws_;
        frame_verts_ += a.draw_count;
    }
}

VkRect2D ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    int32_t l = std::clamp(int32_t(x0 * sc), 0, int32_t(t.w)), tp = std::clamp(int32_t(y0 * sc), 0, int32_t(t.h));
    int32_t r = std::clamp(int32_t(x1 * sc), 0, int32_t(t.w)), b = std::clamp(int32_t(y1 * sc), 0, int32_t(t.h));
    return {{l, tp}, {uint32_t(std::max(r - l, 0)), uint32_t(std::max(b - tp, 0))}};
}

void VulkanBackend::ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                                uint32_t color, float z, uint32_t stencil) {
    std::vector<VkClearRect> rs;
    auto add = [&](const VkRect2D& r) {
        if (r.extent.width && r.extent.height) rs.push_back({r, 0, 1});
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        add(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    t.drawn = true;
    if (rs.empty()) return;
    VkClearAttachment ca{};
    if (t.depth) {
        ca.aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                        ((flags & kClearStencil) && t.img.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (!ca.aspectMask) return;
        ca.clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        BeginRender(nullptr, t.msaa ? &t.msaa : &t.img);
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        // Xbox D3DCLEAR_TARGET_* bits (0x10=R, 0x20=G, 0x40=B, 0x80=A) map 1:1 to Vulkan RGBA write mask bits.
        uint8_t wmask = uint8_t((flags >> 4) & 0xF);
        if (!wmask) return;
        if (wmask != 0xF) {
            float c[4];
            Color(color, c);
            Alloc cb = Constants(c, sizeof(c));
            VkPipeline pipe = FullScreenPipeline(kClearHlsl, "psmain", t.img.fmt, VK_FORMAT_UNDEFINED, false, wmask);
            if (!pipe) return;
            const VkImageView views[4] = {};
            const VkSampler samplers[4] = {};
            VkDescriptorSet set = Set(views, samplers);
            BeginRender(&t.img, nullptr);
            BindPipeline(pipe);
            BindSet(set, cb.off, cb.off);
            SetViewport(0, 0, float(t.w), float(t.h));
            for (const VkClearRect& r : rs) {
                SetScissor(r.rect.offset.x, r.rect.offset.y, r.rect.offset.x + int32_t(r.rect.extent.width),
                           r.rect.offset.y + int32_t(r.rect.extent.height));
                vkCmdDraw(Cb(), 3, 1, 0, 0);
            }
            return;
        }
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Color(color, ca.clearValue.color.float32);
        BeginRender(t.msaa ? &t.msaa : &t.img, nullptr, t.msaa ? &t.img : nullptr);
    }
    vkCmdClearAttachments(Cb(), 1, &ca, uint32_t(rs.size()), rs.data());
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                          float z, uint32_t stencil) {
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;

    // Fast tile-friendly full-screen clear path for Mali-G57: coalesces color + depth/stencil
    // into a single LOAD_OP_CLEAR render pass so the GPU skips full-screen DRAM tile reloads
    // and keeps the render pass open directly for the upcoming Draw() calls.
    const uint8_t wmask = uint8_t((flags >> 4) & 0xF);
    const bool clear_c = (rt != nullptr) && (wmask == 0xF);
    const bool clear_z = (ds != nullptr) && ((flags & kClearZ) != 0);
    const bool clear_s = (ds != nullptr) && ((flags & kClearStencil) != 0) && ds->img.stencil;
    const bool can_coalesce_color = (rt == nullptr) || (wmask == 0xF);
    const bool sizes_match = (!rt || !ds || (rt->w == ds->w && rt->h == ds->h));
    if (!count && !rects && can_coalesce_color && sizes_match && (clear_c || clear_z || clear_s)) {
        Target* ref_t = rt ? rt : ds;
        const auto& v = s.viewport;
        const VkRect2D full = ScaledRect(*ref_t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h));
        if (full.offset.x == 0 && full.offset.y == 0 &&
            full.extent.width == ref_t->w && full.extent.height == ref_t->h) {
            Target* bind_rt = rt;
            if (!bind_rt && ds && s.rt.header) {
                if (Target* cur_rt = FindTarget(s.rt, false);
                    cur_rt && cur_rt->w == ds->w && cur_rt->h == ds->h && DirtyEpoch(cur_rt->data) <= cur_rt->epoch) {
                    bind_rt = cur_rt;
                }
            }
            if (clear_c) {
                rt->epoch = DirtyEpoch(rt->data);
                rt->read.clear();
                rt->drawn = true;
            }
            if (clear_z || clear_s) ds->drawn = true;
            float cc[4] = {};
            if (clear_c) Color(color, cc);
            const bool use_msaa = (msaa_samples_ > VK_SAMPLE_COUNT_1_BIT) &&
                                  (!bind_rt || bool(bind_rt->msaa)) &&
                                  (!(clear_z || clear_s) || bool(ds->msaa));
            Image* c_att = bind_rt ? (use_msaa ? &bind_rt->msaa : &bind_rt->img) : nullptr;
            Image* d_att = (clear_z || clear_s) ? (use_msaa ? &ds->msaa : &ds->img) : nullptr;
            Image* c_res = (use_msaa && bind_rt) ? &bind_rt->img : nullptr;
            BeginRenderClear(c_att, d_att, clear_c, cc, clear_z, z, clear_s, stencil, c_res);
            ++perf_clear_coalesced_;
            return;
        }
    }

    for (Target* t : {rt, ds})
        if (t) ClearTarget(*t, s, count, rects, flags, color, z, stencil);
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points) {
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    src_t = FindTarget(src, false);
    if (!dst_t || !src_t || src_t == dst_t) return;
    Transition(src_t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(dst_t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    float dst_scale = float(dst_t->w) / dst_t->gw;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    std::vector<VkImageCopy> regions;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = 0, t = 0, r = std::min(src_t->w, dst_t->w), b = std::min(src_t->h, dst_t->h), dx = 0, dy = 0;
        if (listed) {
            GuestRect g = ReadGuestRect(src_rects, i);
            l = ScaleUnsigned(g.left, dst_scale);
            t = ScaleUnsigned(g.top, dst_scale);
            r = std::min(ScaleUnsigned(g.right, dst_scale), src_t->w);
            b = std::min(ScaleUnsigned(g.bottom, dst_scale), src_t->h);
            dx = l;
            dy = t;
            if (dst_points) {
                int32_t px, py;
                ReadGuestPoint(dst_points, i, px, py);
                dx = ScaleUnsigned(px, dst_scale);
                dy = ScaleUnsigned(py, dst_scale);
            }
        }
        if (r <= l || b <= t || dx >= dst_t->w || dy >= dst_t->h) continue;
        r = std::min(r, l + dst_t->w - dx);
        b = std::min(b, t + dst_t->h - dy);
        regions.push_back({{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {int32_t(l), int32_t(t), 0},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {int32_t(dx), int32_t(dy), 0},
                           {r - l, b - t, 1}});
    }
    if (!regions.empty()) {
        if (src_t->img.fmt == dst_t->img.fmt) {
            vkCmdCopyImage(Cb(), src_t->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t->img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
        } else {
            std::vector<VkImageBlit> blits;
            blits.reserve(regions.size());
            for (const VkImageCopy& r : regions) {
                VkImageBlit b{};
                b.srcSubresource = r.srcSubresource;
                b.srcOffsets[0] = r.srcOffset;
                b.srcOffsets[1] = {r.srcOffset.x + int32_t(r.extent.width), r.srcOffset.y + int32_t(r.extent.height), 1};
                b.dstSubresource = r.dstSubresource;
                b.dstOffsets[0] = r.dstOffset;
                b.dstOffsets[1] = {r.dstOffset.x + int32_t(r.extent.width), r.dstOffset.y + int32_t(r.extent.height), 1};
                blits.push_back(b);
            }
            vkCmdBlitImage(Cb(), src_t->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t->img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(blits.size()), blits.data(),
                           VK_FILTER_NEAREST);
        }
    }
    dst_t->drawn = true;
}

// ---- read-backs

void VulkanBackend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    Image down;
    Image* src = CopySource(t, down);
    std::vector<uint8_t> px;
    bool ok = src && ReadBack(*src, t.gw, t.gh, px);
    Destroy(down);  // ReadBack waited for the GPU
    if (!ok) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void VulkanBackend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = Completed(), now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        if (finished && recent && (best < 0 || l.fence[i] > l.fence[best])) best = i;
    }
    if (best < 0) return false;
    if (l.fence[best] > l.delivered) {
        const uint8_t* p = l.rb[best].mem.map;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.gw * 4, l.gw * 4);
        l.delivered = l.fence[best];
    }
    return true;
}

void VulkanBackend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        for (Buffer& b : l.rb) Retire(b);
        Retire(l.down);
        l = Late{};
        l.gw = t.gw;
        l.gh = t.gh;
    }
    uint32_t slot = l.next;
    if (l.fence[slot] > Completed()) return;
    Image* src = CopySource(t, l.down);
    if (!src) return;
    if (!l.rb[slot].buf && !CreateBuffer(l.rb[slot], VkDeviceSize(t.gw) * t.gh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
        return;
    EndRender();
    VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {t.gw, t.gh, 1}};
    vkCmdCopyImageToBuffer(Cb(), src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.rb[slot].buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    l.fence[slot] = value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

// ---- present

// the swap chain made again when Android gave a new window, the size or vsync changed; the surface too when the
// window is new, lost, or could not be made last time. false: nothing to show on, the frame is dropped
bool VulkanBackend::AcquireImage(uint32_t& index) {
    if (!host::Foreground()) return false;
    if (!surface_ || surface_lost_ || host::SurfaceEpoch() != surface_epoch_) {
        if (surface_) LOGI("vulkan: new surface (%s)", surface_lost_ ? "lost" : "new window");
        DestroySwapchain();
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (!CreateSurface()) return false;
        swap_stale_ = true;
    }
    int w, h;
    host::WindowPixels(w, h);
    if (!swap_ || swap_stale_ || uint32_t(w) != swap_extent_.width || uint32_t(h) != swap_extent_.height ||
        app::Get().vsync != vsync_) {
        if (!CreateSwapchain()) return false;
    }
    // a bounded wait: a window Android is taking away may never hand an image back
    constexpr uint64_t kAcquireTimeout = 500'000'000;
    for (int tries = 0; tries < 2; tries++) {
        VkResult r = vkAcquireNextImageKHR(dev_, swap_, kAcquireTimeout, CurrentFrame().acquire, VK_NULL_HANDLE, &index);
        // suboptimal is left alone: Android says so every frame on a turned screen, where only the compositor's
        // rotation differs, and making the chain again each frame freed images the GPU was still drawing (Mali)
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return true;
        if (r == VK_TIMEOUT || r == VK_NOT_READY) return false;
        if (r == VK_ERROR_SURFACE_LOST_KHR) {
            surface_lost_ = true;
            return false;
        }
        if (r != VK_ERROR_OUT_OF_DATE_KHR || !CreateSwapchain()) return false;
    }
    return false;
}

void VulkanBackend::DrawFrame(Target* t, uint32_t index) {
    EndRender();
    // Build & upload batched 2D touch overlay vertex buffers BEFORE swapchain barrier & render pass!
    PrepareUi(float(swap_extent_.width), float(swap_extent_.height));
    VkCommandBuffer cb = Cb();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = swap_images_[index];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    // the frame letterboxed to the game's aspect over black
    float aspect = game::Aspect();
    float w = float(swap_extent_.width), h = float(swap_extent_.height);
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    uint32_t filter = std::min(game::Opt().screen_filter, 8u);
    uint32_t crt = filter <= 2 ? filter : 0;
    bool fxaa = game::Opt().anti_aliasing == 1, ssaa = game::Opt().anti_aliasing == 4;
    bool post = game::Opt().hdr || fxaa;
    // opts.z: the taps a side over each screen pixel, 2 averaging SSAA's samples
    float c[24] = {0, 0, 1, 1, gamma_ ? 1.0f : 0.0f, 0, ssaa ? 2.0f : 1.0f, 1};
    c[20] = game::Opt().hdr ? 1.0f : 0.0f;
    c[21] = fxaa ? 1.0f : 0.0f;
    if (t) {
        // a texel of the frame, seconds that wrap before a float loses them, the colour look (4 vibrant on)
        using clock = std::chrono::steady_clock;
        float secs = std::fmod(std::chrono::duration<float>(clock::now().time_since_epoch()).count(), 1000.0f);
        float src[4] = {1.0f / float(t->img.w), 1.0f / float(t->img.h), secs, float(filter >= 4 ? filter - 4 : 0)};
        std::memcpy(c + 16, src, sizeof(src));
    }
    if (crt) {
        // light, then strong: the gaps' darkness, the curve, the mask's dimming, the corners' shade
        constexpr float kGap[] = {0.35f, 0.6f};
        constexpr float kCurve[] = {0.02f, 0.045f};
        constexpr float kMask[] = {0.15f, 0.3f};
        constexpr float kCorner[] = {0.1f, 0.2f};
        int k = int(crt) - 1;
        // the game's lines, as many screen pixels each as come nearest
        float per = std::max(1.0f, std::round(vh / float(guest_h_)));
        float lines = std::floor(vh / per);
        // on average a line keeps 1 - gap / 2 and the mask 1 - 2 mask / 3: that much taken back, most of it
        float kept = (1 - kGap[k] / 2) * (1 - 2 * kMask[k] / 3);
        float rest[8] = {lines, kGap[k], kCurve[k], kMask[k], kCorner[k], 1 + (1 / kept - 1) * 0.8f, 0, 0};
        std::memcpy(c + 8, rest, sizeof(rest));
    }
    Alloc cbuf = Constants(c, sizeof(c));
    VkPipeline pipe = VK_NULL_HANDLE;
    if (t) {
        const bool use_filter = (filter != 0u || post);
        const uint32_t kind = use_filter ? (crt ? 1u : (filter == 3 ? 2u : (filter ? 3u : 0u))) : 4u;
        const uint32_t mode = filter >= 4 ? (filter - 4) : 0u;
        const uint64_t fkey = uint64_t(swap_fmt_) << 16 | kind | (mode << 4) |
                              (gamma_ ? 0x100u : 0u) | (game::Opt().hdr ? 0x200u : 0u) | (fxaa ? 0x400u : 0u);
        if (present_pipe_ != VK_NULL_HANDLE && present_pipe_key_ == fkey) {
            pipe = present_pipe_;
        } else {
            if (use_filter) {
                const std::string& fsrc = FilterHlsl(kind, mode, gamma_, game::Opt().hdr, fxaa);
                pipe = FullScreenPipeline(fsrc.c_str(), "psmain", swap_fmt_, VK_FORMAT_UNDEFINED, false);
            } else {
                pipe = FullScreenPipeline(kBlitHlsl, "psmain", swap_fmt_, VK_FORMAT_UNDEFINED, false);
            }
            present_pipe_key_ = fkey;
            present_pipe_ = pipe;
        }
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (pipe) {
        Transition(t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const VkImageView views[4] = {t->img.view, lut_.view, VK_NULL_HANDLE, VK_NULL_HANDLE};
        const VkSampler samplers[4] = {linear_, point_, point_, point_};
        set = Set(views, samplers);
    }
    const bool full_cover = (pipe != VK_NULL_HANDLE) && (vw >= w - 1.0f) && (vh >= h - 1.0f);
    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    ca.imageView = swap_views_[index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = full_cover ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{0, 0, 0, 1}};
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, swap_extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (dynamic_) {
        begin_rendering_(cb, &ri);
    } else {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = Pass(swap_fmt_, VK_FORMAT_UNDEFINED, !full_cover, false, VK_SAMPLE_COUNT_1_BIT, false, full_cover);
        bi.framebuffer = Framebuffer(swap_fmt_, VK_FORMAT_UNDEFINED, swap_views_[index], VK_NULL_HANDLE,
                                     swap_extent_.width, swap_extent_.height);
        bi.renderArea = ri.renderArea;
        bi.clearValueCount = full_cover ? 0u : 1u;
        bi.pClearValues = full_cover ? nullptr : &ca.clearValue;
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    }
    if (pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        BindSet(set, cbuf.off, cbuf.off);
        SetViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
        SetScissor(0, 0, int32_t(w), int32_t(h));
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    RenderUi();
    if (dynamic_)
        end_rendering_(cb);
    else
        vkCmdEndRenderPass(cb);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

void VulkanBackend::PrepareUi(float w, float h) {
    if (app::kGame.ui) app::kGame.ui(fps_);
    ui_quads_.clear();
    ui_tex_verts_.clear();
    ui_sdf_verts_.clear();
    ui_runs_.clear();
    ui_tex_alloc_ = {};
    ui_sdf_alloc_ = {};

    touch::BuildOverlay(ui_quads_);
    if (ui_quads_.empty() || w <= 1.0f || h <= 1.0f) return;

    const float inv_w = 2.0f / w;
    const float inv_h = 2.0f / h;
    for (const touch::Quad& q : ui_quads_) {
        const float qw = q.x1 - q.x0;
        const float qh = q.y1 - q.y0;
        if (qw <= 0.5f || qh <= 0.5f) continue;

        const float nx0 = q.x0 * inv_w - 1.0f;
        const float nx1 = q.x1 * inv_w - 1.0f;
        const float ny0 = 1.0f - q.y0 * inv_h;
        const float ny1 = 1.0f - q.y1 * inv_h;
        const bool is_sdf = (q.mode != 0u);

        if (!is_sdf) {
            const uint32_t first = uint32_t(ui_tex_verts_.size());
            ui_tex_verts_.push_back({nx0, ny0, q.u0, q.v0, q.r, q.g, q.b, q.a});
            ui_tex_verts_.push_back({nx1, ny0, q.u1, q.v0, q.r, q.g, q.b, q.a});
            ui_tex_verts_.push_back({nx0, ny1, q.u0, q.v1, q.r, q.g, q.b, q.a});
            ui_tex_verts_.push_back({nx1, ny0, q.u1, q.v0, q.r, q.g, q.b, q.a});
            ui_tex_verts_.push_back({nx1, ny1, q.u1, q.v1, q.r, q.g, q.b, q.a});
            ui_tex_verts_.push_back({nx0, ny1, q.u0, q.v1, q.r, q.g, q.b, q.a});
            if (!ui_runs_.empty() && !ui_runs_.back().sdf) {
                ui_runs_.back().count += 6u;
            } else {
                ui_runs_.push_back({false, first, 6u});
            }
        } else {
            const uint32_t first = uint32_t(ui_sdf_verts_.size());
            const float fm = float(q.mode);
            ui_sdf_verts_.push_back({nx0, ny0, 0.0f, 0.0f, q.r, q.g, q.b, q.a, qw, qh, q.radius_px, q.border_px, q.br, q.bg, q.bb, q.ba, fm});
            ui_sdf_verts_.push_back({nx1, ny0, 1.0f, 0.0f, q.r, q.g, q.b, q.a, qw, qh, q.radius_px, q.border_px, q.br, q.bg, q.bb, q.ba, fm});
            ui_sdf_verts_.push_back({nx0, ny1, 0.0f, 1.0f, q.r, q.g, q.b, q.a, qw, qh, q.radius_px, q.border_px, q.br, q.bg, q.bb, q.ba, fm});
            ui_sdf_verts_.push_back({nx1, ny0, 1.0f, 0.0f, q.r, q.g, q.b, q.a, qw, qh, q.radius_px, q.border_px, q.br, q.bg, q.bb, q.ba, fm});
            ui_sdf_verts_.push_back({nx1, ny1, 1.0f, 1.0f, q.r, q.g, q.b, q.a, qw, qh, q.radius_px, q.border_px, q.br, q.bg, q.bb, q.ba, fm});
            ui_sdf_verts_.push_back({nx0, ny1, 0.0f, 1.0f, q.r, q.g, q.b, q.a, qw, qh, q.radius_px, q.border_px, q.br, q.bg, q.bb, q.ba, fm});
            if (!ui_runs_.empty() && ui_runs_.back().sdf) {
                ui_runs_.back().count += 6u;
            } else {
                ui_runs_.push_back({true, first, 6u});
            }
        }
    }

    const VkDeviceSize tex_bytes = ui_tex_verts_.size() * sizeof(OverlayTexVert);
    const VkDeviceSize sdf_bytes = ui_sdf_verts_.size() * sizeof(OverlaySdfVert);
    const VkDeviceSize need_ui = tex_bytes + sdf_bytes + kUboRange + 1024;
    if (need_ui < kUploadSize / 4 && CurrentFrame().used + need_ui > kUploadSize) {
        Submit(true, false);
    }
    if (tex_bytes > 0) {
        ui_tex_alloc_ = Upload(tex_bytes, 16);
        if (ui_tex_alloc_.cpu) std::memcpy(ui_tex_alloc_.cpu, ui_tex_verts_.data(), size_t(tex_bytes));
    }
    if (sdf_bytes > 0) {
        ui_sdf_alloc_ = Upload(sdf_bytes, 16);
        if (ui_sdf_alloc_.cpu) std::memcpy(ui_sdf_alloc_.cpu, ui_sdf_verts_.data(), size_t(sdf_bytes));
    }
}

void VulkanBackend::RenderUi() {
    if (ui_runs_.empty()) return;

    VkCommandBuffer cb = Cb();
    VkImageView atlas_view = skin_.view ? skin_.view : dummy_[0].view;
    const VkImageView views[4] = {atlas_view, dummy_[0].view, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkSampler samplers[4] = {linear_, point_, point_, point_};
    VkDescriptorSet set = Set(views, samplers);

    SetViewport(0, 0, float(swap_extent_.width), float(swap_extent_.height));
    SetScissor(0, 0, int32_t(swap_extent_.width), int32_t(swap_extent_.height));
    BindSet(set, 0, 0);

    VkBuffer bound_vb = VK_NULL_HANDLE;
    VkDeviceSize bound_off = 0;
    for (const OverlayRun& run : ui_runs_) {
        const Alloc& a = run.sdf ? ui_sdf_alloc_ : ui_tex_alloc_;
        if (!a.buf || run.count == 0u) continue;
        VkPipeline pipe = OverlayPipeline(run.sdf, swap_fmt_);
        if (!pipe) continue;
        BindPipeline(pipe);
        if (bound_vb != a.buf || bound_off != a.off) {
            bound_vb = a.buf;
            bound_off = a.off;
            vkCmdBindVertexBuffers(cb, 0, 1, &a.buf, &a.off);
            cmd_vb_count_ = 0;
        }
        vkCmdDraw(cb, run.count, 1, run.first, 0);
    }
}

void VulkanBackend::UploadGammaRamp() {
    ramp_dirty_ = false;
    Alloc a = Upload(256 * 4, 16);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[512 + i];
        p[1] = ramp_[256 + i];
        p[2] = ramp_[i];
        p[3] = 255;
        identity &= ramp_[i] == i && ramp_[256 + i] == i && ramp_[512 + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {256, 1, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, lut_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Rasterized SVG button skins + built-in crisp UI font uploaded directly as a native Vulkan texture
void VulkanBackend::UploadSkin() {
    skin_tried_ = true;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!touch::LoadSkin(rgba, w, h)) return;
    constexpr VkImageUsageFlags kSampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(skin_, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D)) return;
    Alloc a = Upload(rgba.size(), 16);
    if (!a.cpu) {
        Destroy(skin_);
        return;
    }
    std::memcpy(a.cpu, rgba.data(), rgba.size());
    Transition(skin_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, skin_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(skin_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    touch::SetSkinReady(true);
}

void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 2048;  // keep active open-world city textures warm to avoid re-decoding stalls
    constexpr uint64_t kIdleFrames = 1800;
    if (presents_ % kSweepEvery != 0) return;
    if (textures_.size() > kTexturesKept) {
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (it->second.frame + kIdleFrames < presents_) {
                Retire(it->second.img);
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (static_bufs_.size() > 512) {
        for (auto it = static_bufs_.begin(); it != static_bufs_.end();) {
            if (it->second.last_frame + kIdleFrames < presents_) {
                if (it->second.gpu.buf) {
                    static_buf_bytes_ -= std::min<VkDeviceSize>(static_buf_bytes_, it->second.bytes);
                    Retire(it->second.gpu);
                }
                it = static_bufs_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void VulkanBackend::ApplySettings() {
    const float want = app::RenderScale(guest_h_);
    const VkSampleCountFlagBits want_msaa = DesiredMsaaSamples();
    if (std::fabs(want - scale_) <= 1e-4f && want_msaa == msaa_samples_) return;
    Submit(true, false);
    for (auto& [key, t] : targets_) {
        Retire(t.img);
        Retire(t.msaa);
        Retire(t.fb);
    }
    targets_.clear();
    draw_rt_ = draw_ds_ = nullptr;
    scale_ = want;
    msaa_samples_ = want_msaa;
    LOGI("vulkan: render scale %.3f, MSAA %ux", scale_, uint32_t(msaa_samples_));
}

void VulkanBackend::SavePipelineCache() {
    if (cache_saving_.load(std::memory_order_relaxed)) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(dev_, cache_, &size, nullptr) != VK_SUCCESS || size == 0 || size == cache_saved_) return;
    std::vector<char> blob(size);
    if (vkGetPipelineCacheData(dev_, cache_, &size, blob.data()) != VK_SUCCESS) return;
    cache_saved_ = size;
    cache_saving_.store(true, std::memory_order_relaxed);
    const std::filesystem::path p = app::ExeDir() / "shadercache" / "pipelines.bin";
    std::thread([this, p, data = std::move(blob)]() mutable {
        std::filesystem::path tmp = p;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            if (f) f.write(data.data(), std::streamsize(data.size()));
        }
        std::error_code ec;
        std::filesystem::rename(tmp, p, ec);
        cache_saving_.store(false, std::memory_order_relaxed);
    }).detach();
}

void VulkanBackend::Present(const Surface& front) {
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (ramp_dirty_) UploadGammaRamp();
    if (!skin_tried_) UploadSkin();
    uint32_t index = 0;
    bool shown = AcquireImage(index);
    if (shown) {
        present_index_ = index;
        DrawFrame(t, index);
    }
    Submit(false, shown);
    present_value_[presents_ % (kFrames + 1)].store(value_);
    if (shown) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &swap_done_[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap_;
        pi.pImageIndices = &index;
        VkResult r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) swap_stale_ = true;
        if (r == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
    }
    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    Frame& next = CurrentFrame();
    const auto wait_t0 = std::chrono::steady_clock::now();
    ResetFrame(next);
    const auto wait_t1 = std::chrono::steady_clock::now();
    const float gpu_wait_ms = std::chrono::duration<float, std::milli>(wait_t1 - wait_t0).count();
    vkResetCommandPool(dev_, next.pool, 0);
    Begin();
    ReleaseOldTextures();
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    const float raw_frame_ms = std::chrono::duration<float, std::milli>(now - last_present_t_).count();
    last_present_t_ = now;
    if (raw_frame_ms > 0.2f && raw_frame_ms < 250.0f) {
        ema_frame_ms_ = ema_frame_ms_ * 0.85f + raw_frame_ms * 0.15f;
    }
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
    host::DisplayLogsFrame dlf{};
    dlf.fps = float(fps_);
    dlf.frame_ms = ema_frame_ms_;
    dlf.raw_frame_ms = raw_frame_ms;
    dlf.gpu_wait_ms = gpu_wait_ms;
    dlf.draws = frame_draws_;
    dlf.vertices = frame_verts_;
    dlf.culled = frame_culled_;
    dlf.tex_count = uint32_t(textures_.size());
    dlf.obj_hit_pct = frame_buf_lookups_ ? std::min<uint32_t>(100u, (frame_buf_hits_ * 100u) / frame_buf_lookups_) : 0u;
    dlf.render_w = t ? t->w : uint32_t(std::lround(float(guest_w_) * scale_));
    dlf.render_h = t ? t->h : uint32_t(std::lround(float(guest_h_) * scale_));
    host::UpdateDisplayLogs(dlf);
    frame_draws_ = 0;
    frame_verts_ = 0;
    frame_culled_ = 0;
    frame_buf_lookups_ = 0;
    frame_buf_hits_ = 0;
    if (presents_ == 180 || presents_ % 600 == 0) {
        SavePipelineCache();
        LOGR("[INFO] vkPerf: frame=%llu fps=%.1f msaa=%ux tex=%zu objBufs=%zu objHits=%llu vbHits=%llu vscHits=%llu dcHits=%llu psoHits=%llu fastTex=%llu clearCoalesce=%llu frustumCull=%llu scissorCull=%llu dxtPar=%llu",
             (unsigned long long)presents_, fps_, uint32_t(msaa_samples_),
             textures_.size(), static_bufs_.size(), (unsigned long long)perf_static_buf_hits_,
             (unsigned long long)perf_vb_hits_, (unsigned long long)perf_vsc_hits_,
             (unsigned long long)perf_dc_hits_, (unsigned long long)perf_pso_hits_,
             (unsigned long long)perf_fast_tex_hits_, (unsigned long long)perf_clear_coalesced_,
             (unsigned long long)perf_frustum_culled_, (unsigned long long)perf_scissor_culled_,
             (unsigned long long)perf_dxt_parallel_);
    }
    dynres::Frame();
    ApplySettings();
}

// the slot is not reused before the caller's wait: the title runs at most one present ahead
void VulkanBackend::WaitGpuFrame(uint64_t n) { WaitValue(present_value_[n % (kFrames + 1)].load()); }

VulkanBackend* g_vulkan = nullptr;

}  // namespace

std::unique_ptr<Backend> CreateBackend() {
    auto backend = std::make_unique<VulkanBackend>();
    g_vulkan = backend.get();
    return backend;
}

void WaitGpuFrame(uint64_t n) {
    if (g_vulkan) g_vulkan->WaitGpuFrame(n);
}

}  // namespace gpu
