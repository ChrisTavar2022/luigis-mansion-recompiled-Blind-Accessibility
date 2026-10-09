// CRT post-process filter. An original implementation (not derived from any existing
// shader's code) of the usual CRT-simulation model: the game frame is averaged down
// to the scanline grid of a 480-line NTSC set, each line is drawn as an electron beam
// whose width and profile depend on its brightness, the result is seen through a
// phosphor mask (aperture grille / slot mask / shadow mask) in output pixels, and
// light scattered in the glass (halation) and across the faceplate (diffusion/bloom)
// comes from small blurred copies. Runs through Aurora's postfx hook, so the ImGui
// overlay is drawn afterwards and stays sharp.
//
// Pass chain (texture index in parentheses; 0 is the game frame):
//   crt_linearize (1)  game frame -> lines x (lines*aspect), linear light, RGBA16F
//   crt_hsoft     (2)  horizontal beam softness (gaussian in game pixels)
//   crt_blur_h/v  (3,4) 1/2 res blur   -> halation
//   crt_blur_h/v  (5,6) 1/4 res blur
//   crt_blur_h/v  (7,8) 1/8 res blur   -> diffusion / bloom
//   crt_final          beams + convergence + mask + glow + geometry -> swapchain
// Everything but the final pass works on <= 480-line textures, so the cost at 4K is
// the final pass only (2-4 bilinear taps per channel plus the mask math).
#include "recomp/crt.h"

#include "recomp/gx_bridge.h"

#include "port/settings.h"

#include <aurora/postfx.h>
#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace recomp::crt {
namespace {

constexpr const char* kShader = R"WGSL(
struct Crt {
    beam: vec4f,     // min sigma, max sigma, spot power, horizontal softness (game px)
    shape: vec4f,    // min shape, max shape, shape power, -
    mask: vec4f,     // type, triad size (px), strength, interlaced
    glow: vec4f,     // halation, diffusion, bloom, brightness
    conv_rg: vec4f,  // red x/y, green x/y (game px / lines)
    conv_b: vec4f,   // blue x/y, any convergence, -
    gamma: vec4f,    // gamma in, gamma out, curvature, corner radius
    dcopy: vec4f,    // display copy filter weights: line above, this line, line below; -
    dcopy2: vec4f,   // display copy gamma, EFB lines, -, -
};
@group(0) @binding(1) var<uniform> crt: Crt;
@group(0) @binding(3) var tex0: texture_2d<f32>;
@group(0) @binding(4) var tex1: texture_2d<f32>;
@group(0) @binding(5) var tex2: texture_2d<f32>;

// ---- pass: the display copy (GXCopyDisp) as the console's pixel engine does it: the
// vertical filter of GXSetCopyFilter over the line above / this line / the line below (in EFB
// lines, so it scales with the render resolution; edge lines repeat, as with the copy clamp),
// then the GXSetDispCopyGamma curve.
@fragment
fn dcopy_filter(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let dy = 1.0 / max(crt.dcopy2.y, 1.0);
    let above = textureSampleLevel(tex0, aurora_linear, in.uv - vec2f(0.0, dy), 0.0).rgb;
    let here = textureSampleLevel(tex0, aurora_linear, in.uv, 0.0).rgb;
    let below = textureSampleLevel(tex0, aurora_linear, in.uv + vec2f(0.0, dy), 0.0).rgb;
    var col = above * crt.dcopy.x + here * crt.dcopy.y + below * crt.dcopy.z;
    col = pow(clamp(col, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / crt.dcopy2.x));
    return vec4f(col, 1.0);
}

// ---- pass: average the game frame over each scanline band, decode to linear light
@fragment
fn crt_linearize(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let src = vec2f(textureDimensions(tex0));
    let dst = aurora_postfx.output_size;
    let band = src / dst;
    let n = vec2i(clamp(ceil(band * 0.5), vec2f(1.0), vec2f(4.0, 8.0)));
    let base = floor(in.position.xy) / dst;
    var acc = vec3f(0.0);
    for (var j = 0; j < n.y; j++) {
        for (var i = 0; i < n.x; i++) {
            let o = (vec2f(f32(i), f32(j)) + 0.5) / vec2f(n);
            let c = textureSampleLevel(tex0, aurora_linear, base + o / dst, 0.0).rgb;
            acc += pow(max(c, vec3f(1e-6)), vec3f(crt.gamma.x));
        }
    }
    return vec4f(acc / f32(n.x * n.y), 1.0);
}

// ---- pass: horizontal spot size of the beam (limited video bandwidth)
@fragment
fn crt_hsoft(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let p = vec2i(in.position.xy);
    let dims = vec2i(textureDimensions(tex0));
    let sigma = crt.beam.w;
    if (sigma < 0.05) {
        return textureLoad(tex0, p, 0);
    }
    let r = i32(clamp(ceil(sigma * 2.5), 1.0, 6.0));
    var acc = vec3f(0.0);
    var wsum = 0.0;
    for (var k = -r; k <= r; k++) {
        let w = exp(-0.5 * f32(k * k) / (sigma * sigma));
        acc += w * textureLoad(tex0, vec2i(clamp(p.x + k, 0, dims.x - 1), p.y), 0).rgb;
        wsum += w;
    }
    return vec4f(acc / wsum, 1.0);
}

// ---- passes: separable gaussian; steps are output texels, bilinear taps fill the gaps
fn blur(uv: vec2f, step: vec2f) -> vec3f {
    var acc = vec3f(0.0);
    var wsum = 0.0;
    for (var k = -4; k <= 4; k++) {
        let w = exp(-0.5 * f32(k * k) / (1.8 * 1.8));
        acc += w * textureSampleLevel(tex0, aurora_linear, uv + step * f32(k), 0.0).rgb;
        wsum += w;
    }
    return acc / wsum;
}

@fragment
fn crt_blur_h(in: AuroraPostFxVertex) -> @location(0) vec4f {
    return vec4f(blur(in.uv, vec2f(1.0 / aurora_postfx.output_size.x, 0.0)), 1.0);
}

@fragment
fn crt_blur_v(in: AuroraPostFxVertex) -> @location(0) vec4f {
    return vec4f(blur(in.uv, vec2f(0.0, 1.0 / aurora_postfx.output_size.y)), 1.0);
}

// ---- final pass
// Gamma(1 + x) for 0 <= x <= 1 (Abramowitz & Stegun 6.1.36).
fn gamma1p(x: f32) -> f32 {
    return 1.0 + x * (-0.577191652 + x * (0.988205891 + x * (-0.897056937 + x * (0.918206857 +
           x * (-0.756704078 + x * (0.482199394 + x * (-0.193527818 + x * 0.035868343)))))));
}

// Beam intensity at distance d (in line spacings) from a line of color c. The profile
// is exp(-0.5 |d/s|^p): s (width) and p (2 = gaussian, larger = flatter) grow with
// brightness; it is normalized to unit area so average brightness is preserved.
// fp is the output pixel height in line spacings; box-filtering the profile over the
// pixel (variance + fp^2/12) keeps thin scanlines from aliasing at low resolutions.
fn beam_weight(d: f32, c: f32, box: f32) -> f32 {
    let lc = log2(clamp(c, 1e-5, 1.0));
    let s = mix(crt.beam.x, crt.beam.y, exp2(lc * crt.beam.z));
    let p = mix(crt.shape.x, crt.shape.y, exp2(lc * crt.shape.z));
    let s2 = s * s + box;
    let se = sqrt(s2);
    let pe = mix(p, 2.0, box / s2);
    let inv = 1.0 / pe;
    let norm = 2.0 * se * exp2(inv) * gamma1p(inv);
    return exp(-0.5 * exp2(pe * log2(max(abs(d) / se, 1e-6)))) / norm;
}

// The scanline grid around uv: rows to visit and the distance to the first one.
struct Scan {
    x: f32,
    l0: f32,
    d0: f32,
    spacing: f32,
    field: f32,
    lines: f32,
    first: i32,
    last: i32,
};

fn scan_setup(uv: vec2f, off: vec2f) -> Scan {
    let dims = vec2f(textureDimensions(tex0));
    var s: Scan;
    s.lines = dims.y;
    let interlaced = crt.mask.w > 0.5;
    s.spacing = select(1.0, 2.0, interlaced);
    s.field = select(0.0, f32(aurora_postfx.frame & 1u), interlaced);
    let yf = (uv.y * s.lines - 0.5 + off.y - s.field) / s.spacing;
    s.l0 = floor(yf);
    s.d0 = yf - s.l0;
    s.x = uv.x + off.x / dims.x;
    // Wide beams reach past the two nearest lines.
    let wide = crt.beam.y > 0.5;
    s.first = select(0, -1, wide);
    s.last = select(1, 2, wide);
    return s;
}

fn scan_row(s: Scan, k: i32) -> vec4f {  // rgb = line color, a = 0 outside the picture
    let row = (s.l0 + f32(k)) * s.spacing + s.field;
    if (row < 0.0 || row >= s.lines) {
        return vec4f(0.0);
    }
    return vec4f(textureSampleLevel(tex0, aurora_linear, vec2f(s.x, (row + 0.5) / s.lines), 0.0).rgb, 1.0);
}

// All three guns at the same spot.
fn scan(uv: vec2f, box: f32) -> vec3f {
    let s = scan_setup(uv, vec2f(0.0));
    var acc = vec3f(0.0);
    for (var k = s.first; k <= s.last; k++) {
        let c = scan_row(s, k).rgb;
        let d = s.d0 - f32(k);
        acc += c * vec3f(beam_weight(d, c.r, box), beam_weight(d, c.g, box), beam_weight(d, c.b, box));
    }
    return acc;
}

// One gun (channel selected by `sel`) displaced by `off` (misconvergence).
fn scan1(uv: vec2f, off: vec2f, box: f32, sel: vec3f) -> f32 {
    let s = scan_setup(uv, off);
    var acc = 0.0;
    for (var k = s.first; k <= s.last; k++) {
        let c = dot(scan_row(s, k).rgb, sel);
        acc += c * beam_weight(s.d0 - f32(k), c, box);
    }
    return acc;
}

// Fraction of [t0, t1] (in mask periods) covered by [a, b) repeating every period.
fn coverage(t0: f32, t1: f32, a: f32, b: f32) -> f32 {
    let f1 = floor(t1) * (b - a) + clamp(fract(t1) - a, 0.0, b - a);
    let f0 = floor(t0) * (b - a) + clamp(fract(t0) - a, 0.0, b - a);
    return (f1 - f0) / (t1 - t0);
}

// RGB phosphor stripes of triad width t, box-filtered over the pixel; mean 1 per channel.
fn stripes(x: f32, t: f32) -> vec3f {
    let t0 = (x - 0.5) / t;
    let t1 = (x + 0.5) / t;
    let g = 0.03;
    let third = 1.0 / 3.0;
    return vec3f(coverage(t0, t1, g, third - g), coverage(t0, t1, third + g, 2.0 * third - g),
                 coverage(t0, t1, 2.0 * third + g, 1.0 - g)) / (third - 2.0 * g);
}

// Lit fraction of rows of pitch h with a dark gap (fraction of h); mean 1.
fn rows(y: f32, h: f32, gap: f32) -> f32 {
    return coverage((y - 0.5) / h, (y + 0.5) / h, gap * 0.5, 1.0 - gap * 0.5) / (1.0 - gap);
}

fn phosphor_mask(px: vec2f) -> vec3f {
    let t = max(crt.mask.y, 1.0);
    let kind = i32(crt.mask.x + 0.5);
    if (kind == 0) {  // aperture grille: continuous vertical stripes
        return stripes(px.x, t);
    }
    if (kind == 1) {  // slot mask: stripes cut into slots, alternate triads offset by half a slot
        let column = floor(px.x / t);
        let y = px.y + select(0.0, 0.5 * t, fract(column * 0.5) > 0.25);
        return stripes(px.x, t) * rows(y, t, 0.3);
    }
    if (kind == 2) {  // shadow mask: dot rows, every other row shifted by half a triad (delta)
        let h = 0.6 * t;
        let row = floor(px.y / h);
        let x = px.x + select(0.0, 0.5 * t, fract(row * 0.5) > 0.25);
        return stripes(x, t) * rows(px.y, h, 0.35);
    }
    return vec3f(1.0);
}

// Largest value phosphor_mask() returns (fully lit phosphor), so mask * color can be kept <= 1.
fn mask_peak() -> f32 {
    let stripe = 1.0 / (1.0 / 3.0 - 2.0 * 0.03);
    let kind = i32(crt.mask.x + 0.5);
    if (kind == 0) {
        return stripe;
    }
    if (kind == 1) {
        return stripe / 0.7;
    }
    if (kind == 2) {
        return stripe / 0.65;
    }
    return 2.0;
}

@fragment
fn crt_final(in: AuroraPostFxVertex) -> @location(0) vec4f {
    let vp = aurora_postfx.viewport;
    let uv0 = (in.position.xy - vp.xy) / vp.zw;
    let aspect = vp.z / vp.w;

    // Geometry: barrel distortion of a spherical-ish tube, scaled so the nearer edge
    // just touches the screen; rounded corners in pixels.
    var p = uv0 * 2.0 - 1.0;
    let k = crt.gamma.z;
    let pa = p * vec2f(aspect, 1.0);
    let r2 = dot(pa, pa) / (aspect * aspect + 1.0);
    let edge = min(aspect * aspect, 1.0) / (aspect * aspect + 1.0);
    p = p * (1.0 + k * r2) / (1.0 + k * edge);
    let uv = p * 0.5 + 0.5;
    let half_size = 0.5 * vp.zw;
    let radius = crt.gamma.w * vp.w;
    let q = abs(p) * half_size - (half_size - vec2f(radius));
    let dist = length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - radius;
    let inside = clamp(0.5 - dist, 0.0, 1.0);
    if (inside <= 0.0) {
        return vec4f(0.0, 0.0, 0.0, 1.0);
    }

    // Electron beams (per-channel when the guns are misconverged).
    let lines = f32(textureDimensions(tex0).y);
    let spacing = select(1.0, 2.0, crt.mask.w > 0.5);
    // Output pixel height in line spacings (curvature magnifies towards the edges).
    let fp = lines / (vp.w * spacing) * (1.0 + 3.0 * k * r2) / (1.0 + k * edge);
    let box = fp * fp / 12.0;
    var col: vec3f;
    if (crt.conv_b.z > 0.5) {
        col = vec3f(scan1(uv, crt.conv_rg.xy, box, vec3f(1.0, 0.0, 0.0)),
                    scan1(uv, crt.conv_rg.zw, box, vec3f(0.0, 1.0, 0.0)),
                    scan1(uv, crt.conv_b.xy, box, vec3f(0.0, 0.0, 1.0)));
    } else {
        col = scan(uv, box);
    }

    // Halation: electrons/light scattered inside the glass, before the mask.
    let halo = textureSampleLevel(tex1, aurora_linear, uv, 0.0).rgb;
    col = mix(col, halo, crt.glow.x) * crt.glow.w;

    // Phosphor mask, brightness-compensated (mean 1 per channel). A phosphor cannot
    // emit more than full drive, so for bright channels the mask contrast is reduced
    // until the brightest phosphor just reaches 1: highlights keep their brightness and
    // hue, and the mask fades out in them like a real tube's glow fills the gaps.
    let peak = mask_peak();
    // A third of the strength is kept regardless, trading some highlight clipping for a
    // visible mask on bright content.
    let limit = clamp((1.0 / max(col, vec3f(1e-4)) - 1.0) / (peak - 1.0), vec3f(0.0), vec3f(1.0));
    let strength = max(min(vec3f(crt.mask.z), limit), vec3f(crt.mask.z * 0.35));
    var x = col * mix(vec3f(1.0), phosphor_mask(in.position.xy), strength);

    // Diffusion across the faceplate and bloom around bright areas.
    let wide = textureSampleLevel(tex2, aurora_linear, uv, 0.0).rgb * crt.glow.w;
    x = mix(x, wide, crt.glow.y) + crt.glow.z * wide * wide;

    x = clamp(x, vec3f(0.0), vec3f(1.0)) * inside;
    return vec4f(pow(x, vec3f(1.0 / crt.gamma.y)), 1.0);
}
)WGSL";

struct Uniforms {
    float beam[4];
    float shape[4];
    float mask[4];
    float glow[4];
    float convRg[4];
    float convB[4];
    float gamma[4];
    float dcopy[4];
    float dcopy2[4];
};

int lineCount(port::CrtScanlines s) { return s == port::CrtScanlines::DoubleStrike240 ? 240 : 480; }

int sAppliedKey = -1;  // -1 = chain disabled

// lines < 0: no CRT passes. filter: the display copy filter pass runs first (alone when the
// CRT filter is off), and every CRT pass input moves up by one.
void setChain(int lines, bool filter) {
    const int key = lines < 0 && !filter ? -1 : (std::max(lines, 0) * 2 + (filter ? 1 : 0));
    if (key == sAppliedKey) return;
    sAppliedKey = key;
    if (key < 0) {
        aurora_postfx_set_chain(nullptr, nullptr, 0);
        return;
    }
    const auto rel = [](AuroraPostFxSizeMode m, float v) { return AuroraPostFxSize{m, v}; };
    const AuroraPostFxSize same = rel(AURORA_POSTFX_SIZE_INPUT, 1.0f), half = rel(AURORA_POSTFX_SIZE_INPUT, 0.5f);
    std::vector<AuroraPostFxPass> passes;
    const uint32_t o = filter ? 1 : 0;
    if (filter) passes.push_back({"dcopy_filter", rel(AURORA_POSTFX_SIZE_SOURCE, 1.0f), rel(AURORA_POSTFX_SIZE_SOURCE, 1.0f), false, 1, {0}});
    if (lines >= 0) {
        passes.push_back({"crt_linearize", rel(AURORA_POSTFX_SIZE_ASPECT, 1.0f), rel(AURORA_POSTFX_SIZE_ABSOLUTE, float(lines)), true, 1, {o}});
        passes.push_back({"crt_hsoft", same, same, true, 1, {o + 1}});
        for (uint32_t i = 0; i < 3; i++) {
            passes.push_back({"crt_blur_h", half, half, true, 1, {o + 2 + 2 * i}});
            passes.push_back({"crt_blur_v", same, same, true, 1, {o + 3 + 2 * i}});
        }
        passes.push_back({"crt_final", {}, {}, false, 3, {o + 2, o + 4, o + 8}});
    }
    aurora_postfx_set_chain(kShader, passes.data(), uint32_t(passes.size()));
}

bool sliderF(const char* label, float& v, float lo, float hi, const char* fmt = "%.2f") {
    return ImGui::SliderFloat(label, &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
}

template <typename E>
bool comboE(const char* label, E& value, const char* const* names, int count) {
    int i = int(value);
    const bool changed = ImGui::Combo(label, &i, names, count);
    if (changed) value = E(i);
    return changed;
}

}  // namespace

void applyPreset(port::CrtSettings& s, port::CrtPreset preset) {
    using namespace port;
    const bool enabled = s.enabled;
    const CrtScanlines scanlines = s.scanlines;
    s = CrtSettings{};  // defaults = aperture grille
    s.enabled = enabled;
    s.scanlines = scanlines;
    s.preset = preset;
    switch (preset) {
    case CrtPreset::ApertureGrille:
    case CrtPreset::Custom:
        break;
    case CrtPreset::SlotMask:
        s.beamMinSigma = 0.22f;
        s.beamMaxSigma = 0.40f;
        s.beamMaxShape = 3.5f;
        s.mask = CrtMask::SlotMask;
        s.maskSize = 4.0f;
        s.maskStrength = 0.7f;
        s.halation = 0.05f;
        s.diffusion = 0.07f;
        s.curvature = 0.03f;
        s.cornerRadius = 0.03f;
        s.brightness = 1.3f;
        break;
    case CrtPreset::ShadowMask:
        s.beamMinSigma = 0.26f;
        s.beamMaxSigma = 0.44f;
        s.beamMaxShape = 2.6f;
        s.horizontalSoftness = 0.6f;
        s.mask = CrtMask::ShadowMask;
        s.maskSize = 4.0f;
        s.maskStrength = 0.6f;
        s.halation = 0.06f;
        s.diffusion = 0.08f;
        s.bloom = 0.2f;
        s.curvature = 0.04f;
        s.cornerRadius = 0.04f;
        s.brightness = 1.3f;
        break;
    case CrtPreset::ConsumerTv:
        s.beamMinSigma = 0.30f;
        s.beamMaxSigma = 0.48f;
        s.beamMinShape = 2.0f;
        s.beamMaxShape = 2.4f;
        s.horizontalSoftness = 0.9f;
        s.mask = CrtMask::ShadowMask;
        s.maskSize = 5.0f;
        s.maskStrength = 0.45f;
        s.halation = 0.10f;
        s.diffusion = 0.12f;
        s.bloom = 0.25f;
        s.convergence[0] = 0.35f;
        s.convergence[1] = 0.10f;
        s.convergence[4] = -0.35f;
        s.convergence[5] = -0.05f;
        s.curvature = 0.08f;
        s.cornerRadius = 0.06f;
        s.brightness = 1.25f;
        break;
    }
}

void update() {
    static bool sEnvChecked = false;
    if (!sEnvChecked) {
        // Test override (not saved unless the menu changes something): LM_CRT=<preset 0-3>[,<scanlines 0-2>]
        sEnvChecked = true;
        if (const char* env = std::getenv("LM_CRT")) {
            auto& s = port::settings().crt;
            s.enabled = true;
            applyPreset(s, port::CrtPreset(std::clamp(std::atoi(env), 0, 3)));
            if (const char* comma = std::strchr(env, ','))
                s.scanlines = port::CrtScanlines(std::clamp(std::atoi(comma + 1), 0, 2));
        }
    }
    const auto& s = port::settings().crt;
    // Display copy filter (GXSetCopyFilter vertical filter + GXSetDispCopyGamma), skipped
    // when it would not change the picture.
    const auto& dc = recomp::gx::displayCopy();
    const float above = float(dc.vfilter[0] + dc.vfilter[1]) / 64.0f;
    const float here = float(dc.vfilter[2] + dc.vfilter[3] + dc.vfilter[4]) / 64.0f;
    const float below = float(dc.vfilter[5] + dc.vfilter[6]) / 64.0f;
    const float gamma = dc.gamma == 1 ? 1.7f : dc.gamma == 2 ? 2.2f : 1.0f;
    const bool filter = port::settings().rendering.displayCopyFilter &&
                        (above != 0.0f || below != 0.0f || here != 1.0f || gamma != 1.0f);
    const float dcopy[4] = {above, here, below, 0.0f};
    const float dcopy2[4] = {gamma, float(dc.efbHeight), 0.0f, 0.0f};
    if (!s.enabled) {
        if (filter) {
            Uniforms u{};
            std::memcpy(u.dcopy, dcopy, sizeof(dcopy));
            std::memcpy(u.dcopy2, dcopy2, sizeof(dcopy2));
            aurora_postfx_set_uniforms(&u, sizeof(u));
        }
        setChain(-1, filter);
        return;
    }
    const bool conv = std::any_of(std::begin(s.convergence), std::end(s.convergence), [](float v) { return v != 0.0f; });
    Uniforms u{
        {s.beamMinSigma, std::max(s.beamMaxSigma, s.beamMinSigma), s.beamSpotPower, s.horizontalSoftness},
        {s.beamMinShape, std::max(s.beamMaxShape, s.beamMinShape), s.beamShapePower, 0.0f},
        {float(int(s.mask)), s.maskSize, s.mask == port::CrtMask::None ? 0.0f : s.maskStrength,
         s.scanlines == port::CrtScanlines::Interlaced480i ? 1.0f : 0.0f},
        {s.halation, s.diffusion, s.bloom, s.brightness},
        {s.convergence[0], s.convergence[1], s.convergence[2], s.convergence[3]},
        {s.convergence[4], s.convergence[5], conv ? 1.0f : 0.0f, 0.0f},
        {s.gammaIn, s.gammaOut, s.curvature, s.cornerRadius},
    };
    std::memcpy(u.dcopy, dcopy, sizeof(dcopy));
    std::memcpy(u.dcopy2, dcopy2, sizeof(dcopy2));
    aurora_postfx_set_uniforms(&u, sizeof(u));
    setChain(lineCount(s.scanlines), filter);
}

bool drawMenu() {
    using namespace port;
    auto& s = settings().crt;
    bool changed = ImGui::Checkbox("CRT filter", &s.enabled);
    if (!s.enabled) return changed;
    ImGui::SameLine();
    if (!aurora_postfx_active()) ImGui::TextDisabled("(shader failed to compile, see log)");

    static const char* kPresets[] = {"Aperture grille (Royale-like)", "Slot mask", "Shadow mask", "Consumer TV",
                                     "Custom"};
    int preset = int(s.preset);
    if (ImGui::Combo("CRT preset", &preset, kPresets, 5)) {
        if (CrtPreset(preset) != CrtPreset::Custom) applyPreset(s, CrtPreset(preset));
        s.preset = CrtPreset(preset);
        changed = true;
    }
    static const char* kLines[] = {"480 lines (progressive)", "240 lines (double strike)", "480i (interlaced fields)"};
    changed |= comboE("Scanlines", s.scanlines, kLines, 3);
    bool custom = false;

    if (ImGui::TreeNode("CRT advanced")) {
        ImGui::SeparatorText("Beam");
        custom |= sliderF("Min beam width", s.beamMinSigma, 0.05f, 0.6f);
        ImGui::SetItemTooltip("Scanline beam sigma for dark colors, as a fraction of the line spacing");
        custom |= sliderF("Max beam width", s.beamMaxSigma, 0.05f, 0.8f);
        ImGui::SetItemTooltip("Beam sigma for full brightness: bright lines bloom wider");
        custom |= sliderF("Beam width response", s.beamSpotPower, 0.1f, 4.0f);
        ImGui::SetItemTooltip("How quickly the beam widens with brightness (power curve)");
        custom |= sliderF("Min beam shape", s.beamMinShape, 2.0f, 8.0f);
        ImGui::SetItemTooltip("Beam profile exponent for dark colors: 2 = gaussian, higher = flatter top, sharper edge");
        custom |= sliderF("Max beam shape", s.beamMaxShape, 2.0f, 8.0f);
        custom |= sliderF("Beam shape response", s.beamShapePower, 0.1f, 4.0f);
        custom |= sliderF("Horizontal softness", s.horizontalSoftness, 0.0f, 2.0f, "%.2f px");
        ImGui::SetItemTooltip("Horizontal spot size (gaussian sigma) in game pixels");

        ImGui::SeparatorText("Phosphor mask");
        static const char* kMasks[] = {"Aperture grille", "Slot mask", "Shadow mask", "None"};
        custom |= comboE("Mask type", s.mask, kMasks, 4);
        custom |= sliderF("Mask size", s.maskSize, 1.0f, 12.0f, "%.1f px");
        ImGui::SetItemTooltip("Width of one RGB triad in output pixels (3 = one pixel per phosphor)");
        custom |= sliderF("Mask strength", s.maskStrength, 0.0f, 1.0f);

        ImGui::SeparatorText("Glow");
        custom |= sliderF("Halation", s.halation, 0.0f, 0.5f, "%.3f");
        ImGui::SetItemTooltip("Light scattered inside the tube glass (local glow, before the mask)");
        custom |= sliderF("Diffusion", s.diffusion, 0.0f, 0.5f, "%.3f");
        ImGui::SetItemTooltip("Wide haze from light diffused across the faceplate");
        custom |= sliderF("Bloom", s.bloom, 0.0f, 1.0f);
        custom |= sliderF("Brightness boost", s.brightness, 0.5f, 3.0f);

        ImGui::SeparatorText("Convergence (game pixels)");
        custom |= ImGui::SliderFloat2("Red offset", &s.convergence[0], -2.0f, 2.0f, "%.2f");
        custom |= ImGui::SliderFloat2("Green offset", &s.convergence[2], -2.0f, 2.0f, "%.2f");
        custom |= ImGui::SliderFloat2("Blue offset", &s.convergence[4], -2.0f, 2.0f, "%.2f");

        ImGui::SeparatorText("Gamma and geometry");
        custom |= sliderF("CRT gamma", s.gammaIn, 1.8f, 3.0f);
        ImGui::SetItemTooltip("Gamma of the emulated tube (decodes the game image to linear light)");
        custom |= sliderF("Display gamma", s.gammaOut, 1.8f, 3.0f);
        custom |= sliderF("Curvature", s.curvature, 0.0f, 0.3f, "%.3f");
        custom |= sliderF("Corner radius", s.cornerRadius, 0.0f, 0.2f, "%.3f");
        if (ImGui::Button("Reset to preset")) {
            applyPreset(s, s.preset == CrtPreset::Custom ? CrtPreset::ApertureGrille : s.preset);
            changed = true;
        }
        ImGui::TreePop();
    }
    if (custom) s.preset = CrtPreset::Custom;
    return changed || custom;
}

}  // namespace recomp::crt
