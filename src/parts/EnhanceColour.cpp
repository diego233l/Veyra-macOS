#include "EnhanceColour.h"

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace enh {
namespace {

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float smoothstep(float a, float b, float x) {
    if (a == b) return x < a ? 0.f : 1.f;
    const float t = clamp01((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}
float luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float toDisplay(float c) {
    c = std::max(c, 0.f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

void rgbToHsl(const float c[3], float& h, float& s, float& l) {
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
    l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    if (d < 1e-6f) { h = 0; s = 0; return; }
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == c[0]) h = (c[1] - c[2]) / d + (c[1] < c[2] ? 6 : 0);
    else if (mx == c[1]) h = (c[2] - c[0]) / d + 2;
    else h = (c[0] - c[1]) / d + 4;
    h *= 60.f;
}
float hue2rgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}
void hslToRgb(float h, float s, float l, float c[3]) {
    h = std::fmod(std::fmod(h, 360.f) + 360.f, 360.f) / 360.f;
    if (s <= 0) { c[0] = c[1] = c[2] = l; return; }
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    c[0] = hue2rgb(p, q, h + 1.f / 3);
    c[1] = hue2rgb(p, q, h);
    c[2] = hue2rgb(p, q, h - 1.f / 3);
}

// Hue bands of the HSL mixer: red, orange, yellow, green, aqua, blue, purple, magenta.
const float kBandHue[kMixerBands] = {0, 30, 60, 120, 180, 240, 270, 300};
void bandWeights(float hue, float w[kMixerBands]) {
    for (int i = 0; i < kMixerBands; ++i) w[i] = 0;
    for (int i = 0; i < kMixerBands; ++i) {
        const float a = kBandHue[i], b = i + 1 < kMixerBands ? kBandHue[i + 1] : 360.f;
        if (hue >= a && hue < b) {
            const float t = (hue - a) / (b - a);
            w[i] = 1 - t;
            w[(i + 1) % kMixerBands] = t;
            return;
        }
    }
}

// Monotone cubic (Fritsch-Carlson) through the curve's points.
struct Curve {
    std::vector<float> x, y, m;
    bool identity = true;
    explicit Curve(const QVariantList& points) {
        for (const auto& p : points) {
            const auto map = p.toMap();
            x.push_back(clamp01(float(map.value("x").toDouble())));
            y.push_back(clamp01(float(map.value("y").toDouble())));
        }
        if (x.size() < 2) { x = {0, 1}; y = {0, 1}; }
        identity = x.size() == 2 && x[0] == 0 && y[0] == 0 && x[1] == 1 && y[1] == 1;
        const size_t n = x.size();
        std::vector<float> d(n - 1);
        for (size_t i = 0; i + 1 < n; ++i) d[i] = x[i + 1] > x[i] ? (y[i + 1] - y[i]) / (x[i + 1] - x[i]) : 0;
        m.assign(n, 0);
        m[0] = d[0];
        m[n - 1] = d[n - 2];
        for (size_t i = 1; i + 1 < n; ++i) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) * 0.5f;
        for (size_t i = 0; i + 1 < n; ++i) {
            if (d[i] == 0) { m[i] = m[i + 1] = 0; continue; }
            const float a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
            if (s > 9) { const float t = 3 / std::sqrt(s); m[i] = t * a * d[i]; m[i + 1] = t * b * d[i]; }
        }
    }
    float operator()(float v) const {
        if (identity) return v;
        v = clamp01(v);
        if (v <= x.front()) return y.front();
        if (v >= x.back()) return y.back();
        size_t i = 0;
        while (i + 2 < x.size() && v > x[i + 1]) ++i;
        const float h = x[i + 1] - x[i];
        if (h <= 0) return y[i];
        const float t = (v - x[i]) / h, t2 = t * t, t3 = t2 * t;
        return clamp01((2 * t3 - 3 * t2 + 1) * y[i] + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * y[i + 1] +
                       (t3 - t2) * h * m[i + 1]);
    }
};

struct Colour {
    float exposure, contrast, highlights, shadows, whites, blacks, temperature, tint, vibrance, saturation;
    float paramHighlights, paramLights, paramDarks, paramShadows, splitHighlights, splitMidtones, splitShadows;
    float gradingBlending, gradingBalance, calibrationShadowTint, lutStrength;
    float mixerHue[kMixerBands], mixerSaturation[kMixerBands], mixerLuminance[kMixerBands], blackWhiteMix[kMixerBands];
    float calibrationHue[3], calibrationSaturation[3];
    float wheelHue[kZones], wheelSat[kZones], wheelLum[kZones];
    bool blackWhite;
    unsigned bypass;
    int lutInputSpace;
    std::vector<Curve> curves;
    const CubeLut* lut = nullptr;

    explicit Colour(const QVariantMap& m) {
        auto f = [&](const char* k, float d = 0) { return float(m.value(QLatin1String(k), d).toDouble()); };
        exposure = f("exposure"); contrast = f("contrast"); highlights = f("highlights"); shadows = f("shadows");
        whites = f("whites"); blacks = f("blacks"); temperature = f("temperature"); tint = f("tint");
        vibrance = f("vibrance"); saturation = f("saturation");
        paramHighlights = f("paramHighlights"); paramLights = f("paramLights"); paramDarks = f("paramDarks");
        paramShadows = f("paramShadows"); splitHighlights = f("splitHighlights"); splitMidtones = f("splitMidtones");
        splitShadows = f("splitShadows"); gradingBlending = f("gradingBlending", 50); gradingBalance = f("gradingBalance");
        calibrationShadowTint = f("calibrationShadowTint"); lutStrength = f("lutStrength", 100);
        auto arr = [&](const char* k, float* out, int n) {
            const auto list = m.value(QLatin1String(k)).toList();
            for (int i = 0; i < n; ++i) out[i] = i < list.size() ? float(list[i].toDouble()) : 0.f;
        };
        arr("mixerHue", mixerHue, kMixerBands); arr("mixerSaturation", mixerSaturation, kMixerBands);
        arr("mixerLuminance", mixerLuminance, kMixerBands); arr("blackWhiteMix", blackWhiteMix, kMixerBands);
        arr("calibrationHue", calibrationHue, 3); arr("calibrationSaturation", calibrationSaturation, 3);
        const auto wheels = m.value("grading").toList();
        for (int i = 0; i < kZones; ++i) {
            const auto w = wheels.value(i).toMap();
            wheelHue[i] = float(w.value("hue").toDouble());
            wheelSat[i] = float(w.value("saturation").toDouble());
            wheelLum[i] = float(w.value("luminance").toDouble());
        }
        blackWhite = m.value("blackWhite").toBool();
        bypass = unsigned(m.value("groupBypassMask").toInt());
        lutInputSpace = m.value("lutInputSpace").toInt();
        for (const auto& c : m.value("curves").toList()) curves.emplace_back(c.toList());
        while (curves.size() < size_t(kCurveCount)) curves.emplace_back(QVariantList{});
    }
    bool on(int group) const { return !(bypass & (1u << group)); }

    void hslBands(float c[3], const float* hueShift, const float* satShift, const float* lumShift,
                  const float* centres, int count) const {
        float h, s, l;
        rgbToHsl(c, h, s, l);
        if (s < 1e-4f) return;
        float w[kMixerBands]{};
        if (count == kMixerBands) bandWeights(h, w);
        else for (int i = 0; i < count; ++i) {
            float d = std::fabs(h - centres[i]);
            d = std::min(d, 360 - d);
            w[i] = std::max(0.f, 1 - d / 120.f);
        }
        float dh = 0, ds = 0, dl = 0;
        for (int i = 0; i < count; ++i) {
            dh += w[i] * hueShift[i];
            ds += w[i] * satShift[i];
            if (lumShift) dl += w[i] * lumShift[i];
        }
        h += dh / 100.f * 30.f;
        s = clamp01(s * (1 + ds / 100.f));
        l = clamp01(l + dl / 100.f * 0.25f * s);
        hslToRgb(h, s, l, c);
    }

    void apply(float c[3]) const {
        // 5 · calibration (camera-calibration style, first in Lightroom's order)
        if (on(5)) {
            static const float primaries[3] = {0, 120, 240};
            if (calibrationHue[0] || calibrationHue[1] || calibrationHue[2] || calibrationSaturation[0] ||
                calibrationSaturation[1] || calibrationSaturation[2])
                hslBands(c, calibrationHue, calibrationSaturation, nullptr, primaries, 3);
            if (calibrationShadowTint) {
                const float w = 1 - smoothstep(0.f, 0.5f, luma(c));
                const float t = calibrationShadowTint / 100.f * 0.08f * w;
                c[0] += t; c[2] += t; c[1] -= t;
            }
        }
        // White balance and exposure in linear light.
        float lin[3] = {toLinear(clamp01(c[0])), toLinear(clamp01(c[1])), toLinear(clamp01(c[2]))};
        if (on(1)) {
            lin[0] *= 1 + 0.25f * temperature / 100.f;
            lin[2] *= 1 - 0.25f * temperature / 100.f;
            lin[1] *= 1 - 0.2f * tint / 100.f;
        }
        if (on(0)) {
            const float k = std::exp2(exposure);
            for (float& v : lin) v *= k;
        }
        for (int i = 0; i < 3; ++i) c[i] = clamp01(toDisplay(lin[i]));
        // 0 · tone
        if (on(0)) {
            if (contrast) {
                const float k = 1 + contrast / 100.f;
                for (int i = 0; i < 3; ++i) c[i] = clamp01(0.5f + (c[i] - 0.5f) * k);
            }
            const float l = luma(c);
            const auto lift = [&](float amount, float weight) {
                const float d = amount / 100.f * 0.3f * weight;
                for (int i = 0; i < 3; ++i) c[i] = clamp01(d > 0 ? c[i] + d * (1 - c[i]) : c[i] + d * c[i]);
            };
            if (highlights) lift(highlights, smoothstep(0.45f, 1.f, l));
            if (shadows) lift(shadows, 1 - smoothstep(0.f, 0.55f, l));
            if (whites) lift(whites, smoothstep(0.7f, 1.f, l));
            if (blacks) lift(blacks, 1 - smoothstep(0.f, 0.3f, l));
        }
        // 2 · parametric + point curves
        if (on(2)) {
            const float b1 = 0.25f + splitShadows / 100.f * 0.15f, b2 = 0.5f + splitMidtones / 100.f * 0.15f,
                        b3 = 0.75f + splitHighlights / 100.f * 0.15f;
            const float amount[4] = {paramShadows, paramDarks, paramLights, paramHighlights};
            const float lo[4] = {0, b1, b2, b3}, hi[4] = {b1, b2, b3, 1};
            if (paramShadows || paramDarks || paramLights || paramHighlights)
                for (int ch = 0; ch < 3; ++ch) {
                    float v = c[ch], add = 0;
                    for (int r = 0; r < 4; ++r) {
                        if (!amount[r]) continue;
                        const float w = (hi[r] - lo[r]), centre = (lo[r] + hi[r]) * 0.5f;
                        const float d = std::fabs(v - centre) / w;
                        if (d < 1) add += amount[r] / 100.f * 0.2f * 0.5f * (1 + std::cos(d * 3.14159265f));
                    }
                    c[ch] = clamp01(v + add);
                }
            for (int ch = 0; ch < 3; ++ch) c[ch] = curves[0](c[ch]);
            for (int ch = 0; ch < 3; ++ch) c[ch] = curves[1 + ch](c[ch]);
        }
        // 3 · HSL mixer and black & white
        if (on(3)) {
            bool any = false;
            for (int i = 0; i < kMixerBands; ++i) any |= mixerHue[i] || mixerSaturation[i] || mixerLuminance[i];
            if (any && !blackWhite) hslBands(c, mixerHue, mixerSaturation, mixerLuminance, kBandHue, kMixerBands);
            if (blackWhite) {
                float h, s, l;
                rgbToHsl(c, h, s, l);
                float w[kMixerBands];
                bandWeights(h, w);
                float shift = 0;
                for (int i = 0; i < kMixerBands; ++i) shift += w[i] * blackWhiteMix[i];
                const float g = clamp01(luma(c) + shift / 100.f * 0.3f * s);
                c[0] = c[1] = c[2] = g;
            }
        }
        // 1 · vibrance and saturation
        if (on(1) && !blackWhite && (vibrance || saturation)) {
            const float l = luma(c);
            const float sat = std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
            const float k = (1 + saturation / 100.f) * (1 + vibrance / 100.f * (1 - clamp01(sat * 1.5f)));
            for (int i = 0; i < 3; ++i) c[i] = clamp01(l + (c[i] - l) * k);
        }
        // 4 · colour grading wheels
        if (on(4)) {
            const float l = luma(c);
            const float mid = 0.5f + gradingBalance / 100.f * 0.25f;
            const float soft = 0.5f + gradingBlending / 100.f;   // blending widens the zones
            float w[kZones];
            w[0] = std::pow(clamp01(1 - l / std::max(mid, 0.05f)), 1.f / soft);
            w[2] = std::pow(clamp01((l - mid) / std::max(1 - mid, 0.05f)), 1.f / soft);
            w[1] = clamp01(1 - w[0] - w[2]);
            w[3] = 1;
            for (int z = 0; z < kZones; ++z) {
                if (!wheelSat[z] && !wheelLum[z]) continue;
                float tint[3];
                hslToRgb(wheelHue[z], 1.f, 0.5f, tint);
                const float s = wheelSat[z] / 100.f * 0.2f * w[z], lum = wheelLum[z] / 100.f * 0.25f * w[z];
                for (int i = 0; i < 3; ++i) c[i] = clamp01(c[i] + (tint[i] - 0.5f) * s + lum);
            }
        }
        // 6 · LUT file
        if (on(6) && lut && lut->valid() && lutStrength > 0) {
            float in[3];
            for (int i = 0; i < 3; ++i) {
                const float linv = toLinear(clamp01(c[i]));
                if (lutInputSpace == 0)   // Cineon log
                    in[i] = clamp01((std::log10(linv * (1 - 0.0108f) + 0.0108f) * 300.f + 685.f) / 1023.f);
                else if (lutInputSpace == 2) {   // PQ, SDR white at 100 nits
                    const float y = std::pow(linv * 0.01f, 0.1593017578125f);
                    in[i] = std::pow((0.8359375f + 18.8515625f * y) / (1 + 18.6875f * y), 78.84375f);
                } else in[i] = c[i];
            }
            float out[3];
            sampleCube(*lut, in, out);
            const float k = clamp01(lutStrength / 100.f);
            for (int i = 0; i < 3; ++i) c[i] = clamp01(c[i] + (out[i] - c[i]) * k);
        }
    }

    static void sampleCube(const CubeLut& l, const float in[3], float out[3]) {
        const int n = l.size;
        float p[3];
        int i0[3], i1[3];
        float f[3];
        for (int a = 0; a < 3; ++a) {
            const float range = std::max(l.domainMax[a] - l.domainMin[a], 1e-5f);
            p[a] = clamp01((in[a] - l.domainMin[a]) / range) * (n - 1);
            i0[a] = std::min(int(p[a]), n - 1);
            i1[a] = std::min(i0[a] + 1, n - 1);
            f[a] = p[a] - i0[a];
        }
        auto at = [&](int r, int g, int b, int ch) { return l.rgb[(size_t(b) * n * n + size_t(g) * n + r) * 3 + ch]; };
        for (int ch = 0; ch < 3; ++ch) {
            const float c00 = at(i0[0], i0[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i0[2], ch) * f[0];
            const float c10 = at(i0[0], i1[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i0[2], ch) * f[0];
            const float c01 = at(i0[0], i0[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i1[2], ch) * f[0];
            const float c11 = at(i0[0], i1[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i1[2], ch) * f[0];
            const float c0 = c00 * (1 - f[1]) + c10 * f[1], c1 = c01 * (1 - f[1]) + c11 * f[1];
            out[ch] = c0 * (1 - f[2]) + c1 * f[2];
        }
    }
};

} // namespace

const std::vector<ParamInfo>& params() {
    static const std::vector<ParamInfo> list = {
        {"exposure", "曝光（EV）", "亮", -5, 5, 0.05f, 0},
        {"contrast", "对比度", "亮", -100, 100, 1, 0},
        {"highlights", "高光", "亮", -100, 100, 1, 0},
        {"shadows", "阴影", "亮", -100, 100, 1, 0},
        {"whites", "白色", "亮", -100, 100, 1, 0},
        {"blacks", "黑色", "亮", -100, 100, 1, 0},
        {"texture", "纹理", "效果", -100, 100, 1, 0},
        {"clarity", "清晰度", "效果", -100, 100, 1, 0},
        {"dehaze", "去朦胧", "效果", -100, 100, 1, 0},
        {"temperature", "色温（相对）", "颜色", -100, 100, 1, 0},
        {"tint", "色调", "颜色", -100, 100, 1, 0},
        {"vibrance", "自然饱和度", "颜色", -100, 100, 1, 0},
        {"saturation", "饱和度", "颜色", -100, 100, 1, 0},
        {"paramHighlights", "参数化高光", "曲线", -100, 100, 1, 0},
        {"paramLights", "参数化亮调", "曲线", -100, 100, 1, 0},
        {"paramDarks", "参数化暗调", "曲线", -100, 100, 1, 0},
        {"paramShadows", "参数化阴影", "曲线", -100, 100, 1, 0},
        {"splitHighlights", "分离高光", "曲线", -100, 100, 1, 0},
        {"splitMidtones", "分离中间调", "曲线", -100, 100, 1, 0},
        {"splitShadows", "分离阴影", "曲线", -100, 100, 1, 0},
        {"gradingBlending", "混合", "颜色分级", 0, 100, 1, 50},
        {"gradingBalance", "平衡", "颜色分级", -100, 100, 1, 0},
        {"calibrationShadowTint", "阴影色调", "校准", -100, 100, 1, 0},
        {"lutStrength", "LUT 强度", "LUT", 0, 100, 1, 100},
    };
    return list;
}

QVariantMap defaultColour() {
    QVariantMap m;
    for (const auto& p : params()) m.insert(QString::fromUtf8(p.name), double(p.def));
    auto zeros = [](int n) { QVariantList l; for (int i = 0; i < n; ++i) l << 0.0; return l; };
    m["mixerHue"] = zeros(kMixerBands);
    m["mixerSaturation"] = zeros(kMixerBands);
    m["mixerLuminance"] = zeros(kMixerBands);
    m["blackWhiteMix"] = zeros(kMixerBands);
    m["calibrationHue"] = zeros(3);
    m["calibrationSaturation"] = zeros(3);
    QVariantList curves, wheels;
    for (int i = 0; i < kCurveCount; ++i)
        curves << QVariant(QVariantList{QVariantMap{{"x", 0.0}, {"y", 0.0}}, QVariantMap{{"x", 1.0}, {"y", 1.0}}});
    for (int i = 0; i < kZones; ++i) wheels << QVariantMap{{"hue", 0.0}, {"saturation", 0.0}, {"luminance", 0.0}};
    m["curves"] = curves;
    m["grading"] = wheels;
    m["blackWhite"] = false;
    m["groupBypassMask"] = 0;
    m["lutName"] = QString();
    m["lutInputSpace"] = 0;
    m["enabled"] = false;
    return m;
}

QVariantList curveSamples(const QVariantList& points) {
    const Curve curve(points);
    QVariantList out;
    for (int i = 0; i <= 128; ++i) out << double(curve(float(i) / 128.f));
    return out;
}

bool isNeutral(const QVariantMap& colour) {
    QVariantMap a = colour, b = defaultColour();
    for (const char* k : {"enabled", "index", "curveSamples", "texture", "clarity", "dehaze"}) {
        a.remove(QLatin1String(k));
        b.remove(QLatin1String(k));
    }
    return a == b;
}

QImage bake(const QVector<QVariantMap>& nodes, LutResolver resolve, void* context) {
    std::vector<Colour> grades;
    for (const auto& n : nodes) {
        grades.emplace_back(n);
        const QString lutName = n.value("lutName").toString();
        if (!lutName.isEmpty() && resolve) grades.back().lut = resolve(lutName, context);
    }
    const int N = kBakeSize;
    QImage img(N * N, N, QImage::Format_RGBA8888);
    for (int b = 0; b < N; ++b)
        for (int g = 0; g < N; ++g) {
            uchar* line = img.scanLine(g);
            for (int r = 0; r < N; ++r) {
                float c[3] = {float(r) / (N - 1), float(g) / (N - 1), float(b) / (N - 1)};
                for (const auto& grade : grades) grade.apply(c);
                uchar* px = line + (b * N + r) * 4;
                for (int i = 0; i < 3; ++i) px[i] = uchar(std::lround(clamp01(c[i]) * 255.f));
                px[3] = 255;
            }
        }
    return img;
}

bool parseCube(const QString& path, CubeLut& out, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    CubeLut lut;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        const QString key = parts.value(0).toUpper();
        if (key == QLatin1String("LUT_3D_SIZE")) {
            lut.size = parts.value(1).toInt();
            if (lut.size < 2 || lut.size > 128) { if (error) *error = QStringLiteral("bad LUT_3D_SIZE"); return false; }
            lut.rgb.reserve(size_t(lut.size) * lut.size * lut.size * 3);
        } else if (key == QLatin1String("DOMAIN_MIN") && parts.size() >= 4) {
            for (int i = 0; i < 3; ++i) lut.domainMin[i] = parts[i + 1].toFloat();
        } else if (key == QLatin1String("DOMAIN_MAX") && parts.size() >= 4) {
            for (int i = 0; i < 3; ++i) lut.domainMax[i] = parts[i + 1].toFloat();
        } else if (key == QLatin1String("LUT_1D_SIZE")) {
            if (error) *error = QStringLiteral("1D LUTs are not supported");
            return false;
        } else if (parts.size() == 3 && (key[0].isDigit() || key[0] == '-' || key[0] == '.')) {
            bool ok[3];
            for (int i = 0; i < 3; ++i) lut.rgb.push_back(parts[i].toFloat(&ok[i]));
            if (!ok[0] || !ok[1] || !ok[2]) { if (error) *error = QStringLiteral("bad data line"); return false; }
        }
    }
    if (!lut.valid()) { if (error) *error = QStringLiteral("incomplete LUT"); return false; }
    out = std::move(lut);
    return true;
}

} // namespace enh
