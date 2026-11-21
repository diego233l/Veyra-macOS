#pragma once
// Veyra's colour node (Windows engine::ColorSettings) evaluated on the CPU and
// baked into one 3D LUT per change. The GPU then does a single lookup per tap,
// so every per-pixel colour control the Veyra UI offers costs the same on an
// Intel Iris MacBook: curves, mixer, wheels, calibration, LUT files and all.
#include <QImage>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <array>
#include <vector>

namespace enh {

constexpr int kMixerBands = 8, kCurveCount = 5, kCurvePoints = 8, kZones = 4;
constexpr int kBakeSize = 33;   // 33^3 grid, stored as a (33*33) x 33 strip

struct CubeLut {
    int size = 0;
    float domainMin[3]{0, 0, 0}, domainMax[3]{1, 1, 1};
    std::vector<float> rgb;   // size^3 * 3, red fastest
    bool valid() const { return size >= 2 && rgb.size() == size_t(size) * size * size * 3; }
};
bool parseCube(const QString& path, CubeLut& out, QString* error = nullptr);

// Scalar catalogue: same names, ranges and groups as Windows' kColourParams.
struct ParamInfo { const char* name; const char* label; const char* group; float min, max, step, def; };
const std::vector<ParamInfo>& params();

QVariantMap defaultColour();                       // full state map (colourStateAt shape, minus index)
QVariantList curveSamples(const QVariantList& points);  // 129 samples of one curve
// Bake the enabled colour nodes, in order, into a strip image (RGBA8).
// luts resolves a node's "lutName" to a parsed .cube (may be null).
using LutResolver = const CubeLut* (*)(const QString& name, void* context);
QImage bake(const QVector<QVariantMap>& nodes, LutResolver resolve, void* context);
bool isNeutral(const QVariantMap& colour);

} // namespace enh
