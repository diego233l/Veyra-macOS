#version 440
// Veyra for macOS — the per-frame enhancement chain, one pass:
//   source (optionally bicubic-upscaled) -> baked colour LUT (all colour nodes)
//   -> FXAA -> clarity / dehaze -> sharpen (RCAS-style limited) -> output.
// The colour chain is baked on the CPU into a 33^3 LUT strip (see EnhanceColour.cpp),
// so every tap costs one texture read plus two LUT reads.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 srcTexel;      // 1 / native frame size
    float lutEnabled;   // 0 / 1
    float lutSize;      // N of the N^3 grid
    float fxaa;         // 0 / 1
    float fxaaQuality;  // 0..1, edge threshold tightness
    float sharpen;      // -1..1 (texture), negative softens
    float clarity;      // -1..1
    float dehaze;       // -1..1
    float sr;           // 0 / 1: bicubic sampling for upscaling
    float srSharpness;  // 0..1, extra sharpening when upscaling
    float bypass;       // 1 = untouched picture
};
layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D lut;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Catmull-Rom bicubic from 9 bilinear taps (the classic optimised form).
vec3 bicubic(vec2 uv) {
    vec2 texSize = 1.0 / srcTexel;
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;
    vec2 texPos0 = (texPos1 - 1.0) * srcTexel;
    vec2 texPos3 = (texPos1 + 2.0) * srcTexel;
    vec2 texPos12 = (texPos1 + offset12) * srcTexel;
    vec3 r = vec3(0.0);
    r += texture(source, vec2(texPos0.x, texPos0.y)).rgb * w0.x * w0.y;
    r += texture(source, vec2(texPos12.x, texPos0.y)).rgb * w12.x * w0.y;
    r += texture(source, vec2(texPos3.x, texPos0.y)).rgb * w3.x * w0.y;
    r += texture(source, vec2(texPos0.x, texPos12.y)).rgb * w0.x * w12.y;
    r += texture(source, vec2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y;
    r += texture(source, vec2(texPos3.x, texPos12.y)).rgb * w3.x * w12.y;
    r += texture(source, vec2(texPos0.x, texPos3.y)).rgb * w0.x * w3.y;
    r += texture(source, vec2(texPos12.x, texPos3.y)).rgb * w12.x * w3.y;
    r += texture(source, vec2(texPos3.x, texPos3.y)).rgb * w3.x * w3.y;
    return clamp(r, 0.0, 1.0);
}

vec3 applyLut(vec3 c) {
    if (lutEnabled < 0.5) return c;
    float n = lutSize;
    c = clamp(c, 0.0, 1.0) * (n - 1.0);
    float b0 = floor(c.b);
    float b1 = min(b0 + 1.0, n - 1.0);
    float f = c.b - b0;
    vec2 uv0 = vec2((c.r + 0.5 + b0 * n) / (n * n), (c.g + 0.5) / n);
    vec2 uv1 = vec2((c.r + 0.5 + b1 * n) / (n * n), (c.g + 0.5) / n);
    return mix(texture(lut, uv0).rgb, texture(lut, uv1).rgb, f);
}

vec3 graded(vec2 uv) {
    vec3 c = sr > 0.5 ? bicubic(uv) : texture(source, uv).rgb;
    return applyLut(c);
}

vec3 fxaaAt(vec2 uv, vec3 rgbM, float lN, float lS, float lW, float lE) {
    float lM = luma(rgbM);
    float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float threshold = mix(0.125, 0.063, fxaaQuality);
    if (lMax - lMin < max(0.0312, lMax * threshold)) return rgbM;
    vec2 dir = vec2(-(lN - lS), lE - lW);
    float dirReduce = max((lN + lS + lE + lW) * 0.03125, 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * srcTexel;
    vec3 rgbA = 0.5 * (graded(uv + dir * (1.0 / 3.0 - 0.5)) + graded(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (graded(uv - dir * 0.5) + graded(uv + dir * 0.5));
    float lB = luma(rgbB);
    return (lB < lMin || lB > lMax) ? rgbA : rgbB;
}

void main() {
    vec2 uv = qt_TexCoord0;
    vec4 raw = texture(source, uv);
    if (bypass > 0.5) { fragColor = vec4(raw.rgb, 1.0) * qt_Opacity; return; }

    vec3 c = graded(uv);
    bool spatial = fxaa > 0.5 || sharpen != 0.0 || sr > 0.5;
    if (spatial) {
        vec3 n = graded(uv + vec2(0.0, -srcTexel.y));
        vec3 s = graded(uv + vec2(0.0, srcTexel.y));
        vec3 w = graded(uv + vec2(-srcTexel.x, 0.0));
        vec3 e = graded(uv + vec2(srcTexel.x, 0.0));
        if (fxaa > 0.5) c = fxaaAt(uv, c, luma(n), luma(s), luma(w), luma(e));
        float amount = sharpen + (sr > 0.5 ? srSharpness : 0.0);
        if (amount > 0.0) {
            // RCAS-style: the unsharp cross, limited to the neighbourhood's range
            // so edges gain acutance without halos.
            vec3 mn = min(min(n, s), min(w, e));
            vec3 mx = max(max(n, s), max(w, e));
            vec3 sharp = c + (c * 4.0 - (n + s + w + e)) * amount * 0.5;
            c = clamp(sharp, min(mn, c), max(mx, c));
        } else if (amount < 0.0) {
            c = mix(c, (n + s + w + e + c) * 0.2, -amount);
        }
    }
    if (clarity != 0.0) {
        vec2 r = srcTexel * 6.0;
        float blur = 0.25 * (luma(graded(uv + vec2(r.x, r.y))) + luma(graded(uv + vec2(-r.x, r.y))) +
                             luma(graded(uv + vec2(r.x, -r.y))) + luma(graded(uv + vec2(-r.x, -r.y))));
        float l = luma(c);
        float mid = 1.0 - abs(l * 2.0 - 1.0);
        c += (l - blur) * clarity * 0.8 * mid;
    }
    if (dehaze != 0.0) {
        float k = dehaze * 0.12;
        c = (c - k) / max(1.0 - k, 0.2);
        float l = luma(c);
        c = l + (c - l) * (1.0 + dehaze * 0.15);
    }
    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0) * qt_Opacity;
}
