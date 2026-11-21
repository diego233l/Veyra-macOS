// MacBridge · Enhance part: the effect chain, the colour page and the stage's shader.
#include "../MacBridge.h"
#include "EnhanceColour.h"
#include "veyra/Log.h"
#include "veyra/ui/UiLanguage.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QSet>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>

// Serves the baked colour LUT strip to VideoStage.qml (image://veyra-enhance/<revision>).
class EnhLutProvider : public QQuickImageProvider {
public:
    EnhLutProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    void set(const QImage& image) { QMutexLocker l(&lock_); image_ = image; }
    QImage requestImage(const QString&, QSize* size, const QSize&) override {
        QMutexLocker l(&lock_);
        if (size) *size = image_.size();
        return image_;
    }
private:
    QMutex lock_;
    QImage image_;
};

namespace {
QString zh(const char* source) { return veyra::ui::i18n::text(QString::fromUtf8(source)); }

struct TypeInfo { const char* id; const char* label; int maxInstances; bool repeatable, mustBeLast, justBeforeLast, changesResolution, experimental; };
const TypeInfo kTypes[] = {
    {"color", "调色", 6, true, false, false, false, false},
    {"sr", "超分辨率", 1, false, false, false, true, false},
    {"nr", "NR 画面增强", 4, true, false, false, false, true},
    {"protection", "NR 保护区域", 1, false, false, false, false, false},
    {"video-hdr", "RTX Video HDR", 1, false, false, true, false, false},
    {"frame-generation", "补帧", 1, false, true, false, false, false},
    {"aa", "抗锯齿", 2, true, false, false, false, false},
    {"beauty", "美颜", 2, true, false, false, false, false},
    {"exclusion", "全效果剔除", 1, false, false, true, false, false},
};
const TypeInfo* typeInfo(const QString& id) {
    for (const auto& t : kTypes) if (id == QLatin1String(t.id)) return &t;
    return nullptr;
}
bool macType(const QString& id) { return id == QLatin1String("color") || id == QLatin1String("aa") || id == QLatin1String("sr"); }

// Parsed .cube files, keyed by path and modification time.
struct LutCache { QHash<QString, enh::CubeLut> luts; QHash<QString, qint64> stamps; QString dir; };
LutCache& lutCache() { static LutCache c; return c; }
const enh::CubeLut* resolveLut(const QString& name, void*) {
    auto& c = lutCache();
    const QString path = c.dir + QLatin1Char('/') + name + QStringLiteral(".cube");
    const QFileInfo info(path);
    if (!info.exists()) return nullptr;
    const qint64 stamp = info.lastModified().toMSecsSinceEpoch();
    if (!c.luts.contains(path) || c.stamps.value(path) != stamp) {
        enh::CubeLut lut;
        QString error;
        if (!enh::parseCube(path, lut, &error)) {
            veyra::log::warn("mac-enhance", ("LUT " + path + ": " + error).toStdString());
            return nullptr;
        }
        c.luts.insert(path, std::move(lut));
        c.stamps.insert(path, stamp);
    }
    return &c.luts[path];
}

// "exposure", "mixerHue.3", "calibrationHue.1", "grading.2.hue"
bool setPath(QVariantMap& state, const QString& name, double value) {
    const QStringList parts = name.split(QLatin1Char('.'));
    if (parts.size() == 1) {
        for (const auto& p : enh::params())
            if (name == QLatin1String(p.name)) { state[name] = value; return true; }
        return false;
    }
    bool ok = false;
    const int index = parts[1].toInt(&ok);
    if (!ok || index < 0) return false;
    if (parts.size() == 2) {
        static const QStringList arrays{"mixerHue", "mixerSaturation", "mixerLuminance", "blackWhiteMix",
                                        "calibrationHue", "calibrationSaturation"};
        if (!arrays.contains(parts[0])) return false;
        QVariantList list = state.value(parts[0]).toList();
        if (index >= list.size()) return false;
        list[index] = value;
        state[parts[0]] = list;
        return true;
    }
    if (parts.size() == 3 && parts[0] == QLatin1String("grading")) {
        QVariantList wheels = state.value("grading").toList();
        if (index >= wheels.size()) return false;
        QVariantMap wheel = wheels[index].toMap();
        if (!wheel.contains(parts[2])) return false;
        wheel[parts[2]] = value;
        wheels[index] = wheel;
        state["grading"] = wheels;
        return true;
    }
    return false;
}
double getPath(const QVariantMap& state, const QString& name) {
    const QStringList parts = name.split(QLatin1Char('.'));
    QVariant v = state.value(parts[0]);
    for (int i = 1; i < parts.size(); ++i) {
        bool ok = false;
        const int index = parts[i].toInt(&ok);
        v = ok ? v.toList().value(index) : v.toMap().value(parts[i]);
    }
    return v.toDouble();
}
QString nvidiaReason() { return MacBridge::macText("Needs an NVIDIA RTX graphics card on Windows; not available on Mac"); }
QString amdIntelReason() { return MacBridge::macText("Needs AMD or Intel drivers on Windows; not available on Mac"); }
QString fgReason() { return MacBridge::macText("Frame generation needs NVIDIA, AMD or Intel runtimes on Windows; not available on Mac"); }
} // namespace

// ---------------------------------------------------------------------------
void MacBridge::initEnhance() {
    // The list page starts like Windows' default chain: a colour card and an SR
    // card, both off. The node editor starts empty (input wired to output).
    EnhDocument& list = enhDocs_[0];
    for (const char* type : {"color", "sr"}) {
        EnhNode node;
        node.type = QString::fromLatin1(type);
        node.id = list.nextId++;
        if (node.type == QLatin1String("color")) node.colour = enh::defaultColour();
        list.nodes << node;
    }
    m_chainValid = true;
    // HDR conversion (RTX Video HDR, HDR curves) is Windows/NVIDIA only: inert, well-formed values.
    const QString hdrReason = nvidiaReason();
    m_hdrDials = {{"shadowLift", 0.0}, {"brightness", 0.0}, {"highlight", 0}};
    m_hdrRecommendation = {{"available", false}, {"reason", hdrReason}, {"cllNits", 0.0}, {"fallNits", 0.0}};
    m_hdrDisplayInfo = {{"valid", false}, {"plausible", false}, {"peakNits", 0.0}, {"minNits", 0.0}, {"fullFrameNits", 0.0}};
    m_hdrSourceInfo = {{"valid", false}, {"doviProfile", 0}};
    m_videoHdrParams = {{"peakNits", 1000.0}, {"contrast", 125.0}, {"saturation", 75.0}, {"middleGray", 44.0}};
    m_hdrCurveParams = {{"midGrayNits", 0.0}, {"shadowLiftEv100", 0.0}};
    m_hdrAxes = {{"content", QString()}, {"environment", QString()}};
    lutCache().dir = enhLutDirectory();
    enhTimer_ = new QTimer(this);
    enhTimer_->setSingleShot(true);
    enhTimer_->setInterval(16);
    connect(enhTimer_, &QTimer::timeout, this, &MacBridge::pushEnhToStage);
}

void MacBridge::attachEnhance(QQuickWindow*) {
    enhProvider_ = new EnhLutProvider;
    if (qmlEngine()) qmlEngine()->addImageProvider(QStringLiteral("veyra-enhance"), enhProvider_);
    pushEnhToStage();
    emit effectCapabilitiesChanged();
    emit chainChanged();

    // Test hooks (harmless in normal use): show a still and force settings.
    const QString still = qEnvironmentVariable("VEYRA_TEST_STILL");
    if (!still.isEmpty()) {
        QTimer::singleShot(200, this, [this, still] {
            const QImage img(still);
            showStill(QUrl::fromLocalFile(still));
            setSourceOpen(QStringLiteral("image"), QFileInfo(still).fileName(), img.width(), img.height(), 0);
            const QString page = qEnvironmentVariable("VEYRA_TEST_PAGE");
            if (!page.isEmpty()) emit navigate(page);
            if (qEnvironmentVariableIsSet("VEYRA_TEST_STAGE_TOP") && stage()) stage()->setZ(1000);
            QTimer::singleShot(2500, this, [this] {
                if (QQuickItem* s = stage())
                    veyra::log::info("mac-enhance-test", QStringLiteral("stage visible=%1 %2x%3 at %4,%5 native=%6x%7 parent=%8")
                        .arg(s->isVisible()).arg(s->width()).arg(s->height()).arg(s->x()).arg(s->y())
                        .arg(s->property("nativeWidth").toDouble()).arg(s->property("nativeHeight").toDouble())
                        .arg(s->parentItem() ? s->parentItem()->metaObject()->className() : "none").toStdString());
            });
            const QString set = qEnvironmentVariable("VEYRA_TEST_ENHANCE");   // e.g. "saturation=-100,aa=1"
            for (const QString& item : set.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString key = item.section('=', 0, 0), value = item.section('=', 1);
                if (key == QLatin1String("aa")) setAntiAliasEnabled(value.toInt() != 0);
                else if (key == QLatin1String("sr")) setSrEnabled(value.toInt() != 0);
                else if (key == QLatin1String("lut")) { importLut(value); setColourLut(QFileInfo(value).completeBaseName()); }
                else { setColorEnabled(true); setColourParameter(key, value.toDouble()); }
            }
        });
    }
}

QString MacBridge::enhLutDirectory() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/luts");
    QDir().mkpath(dir);
    return dir;
}
QString MacBridge::enhLooksFile() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/colour-looks.json");
}

// --- processing order and the stage ------------------------------------------
QVector<int> MacBridge::enhOrder() const {
    const auto& d = enhDoc();
    QVector<int> order;
    if (enhMode_ == 0) {
        for (int i = 0; i < d.nodes.size(); ++i) order << i;
        return order;
    }
    uint id = d.inputNext;
    for (int guard = 0; id > 1 && guard < 64; ++guard) {
        const int index = nodeIndexForId(id);
        if (index < 0) break;
        order << index;
        id = d.next.value(id, 1);
    }
    return order;
}

void MacBridge::enhChanged(bool chainLevel) {
    if (chainLevel) {
        emit chainChanged();
        emit antiAliasChanged();
        emit colourParamsChanged();
    }
    emit settingsChanged();
    if (enhTimer_) enhTimer_->start();
}

void MacBridge::pushEnhToStage() {
    QQuickItem* s = stage();
    const auto& d = enhDoc();
    QVector<QVariantMap> colours;
    double texture = 0, clarity = 0, dehaze = 0;
    bool fxaa = false, sr = false;
    double fxaaQuality = 0.5, srSharp = 0.25;
    for (int index : enhOrder()) {
        const EnhNode& n = d.nodes[index];
        if (!n.enabled) continue;
        if (n.type == QLatin1String("color")) {
            colours << n.colour;
            if (!(n.colour.value("groupBypassMask").toUInt() & (1u << 7))) {
                texture += n.colour.value("texture").toDouble();
                clarity += n.colour.value("clarity").toDouble();
                dehaze += n.colour.value("dehaze").toDouble();
            }
        } else if (n.type == QLatin1String("aa")) {
            fxaa = true;
            fxaaQuality = std::clamp(n.aa.value("quality", 1).toDouble() / 2.0, 0.0, 1.0);
        } else if (n.type == QLatin1String("sr")) {
            sr = true;
            srSharp = n.srSharpness;
        }
    }
    bool needLut = false;
    for (const auto& c : colours) needLut |= !enh::isNeutral(c);
    if (needLut && enhProvider_) {
        QElapsedTimer t;
        t.start();
        enhProvider_->set(enh::bake(colours, &resolveLut, nullptr));
        ++enhLutRevision_;
        veyra::log::info("mac-enhance", "colour LUT baked layers=" + std::to_string(colours.size()) +
                                            " ms=" + std::to_string(t.elapsed()));
    }
    if (!s) return;
    s->setProperty("lutEnabled", needLut);
    if (needLut) s->setProperty("lutUrl", QUrl(QStringLiteral("image://veyra-enhance/%1").arg(enhLutRevision_)));
    s->setProperty("lutSize", enh::kBakeSize);
    s->setProperty("fxaa", fxaa);
    s->setProperty("fxaaQuality", fxaaQuality);
    s->setProperty("sharpen", std::clamp(texture / 100.0, -1.0, 1.0));
    s->setProperty("clarity", std::clamp(clarity / 100.0, -1.0, 1.0));
    s->setProperty("dehaze", std::clamp(dehaze / 100.0, -1.0, 1.0));
    s->setProperty("sr", sr);
    s->setProperty("srSharpness", srSharp);
}

// --- colour -------------------------------------------------------------------
int MacBridge::colourIndex() const {
    const auto& d = enhDoc();
    if (selectedColour_ >= 0 && selectedColour_ < d.nodes.size() && d.nodes[selectedColour_].type == QLatin1String("color"))
        return selectedColour_;
    for (int i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].type == QLatin1String("color")) return i;
    return -1;
}
QVariantMap MacBridge::colourStateAt(int index) const {
    const auto& d = enhDoc();
    QVariantMap out;
    if (index >= 0 && index < d.nodes.size() && d.nodes[index].type == QLatin1String("color")) {
        out = d.nodes[index].colour;
        out["enabled"] = d.nodes[index].enabled;
    } else {
        out = enh::defaultColour();
        index = -1;
    }
    out["index"] = index;
    QVariantList samples;
    for (const auto& c : out.value("curves").toList()) samples << QVariant(enh::curveSamples(c.toList()));
    out["curveSamples"] = samples;
    return out;
}
QVariantMap MacBridge::colourState() const { return colourStateAt(colourIndex()); }
int MacBridge::selectedColourLayer() const { return colourIndex(); }
void MacBridge::setSelectedColourLayer(int v) {
    const auto& d = enhDoc();
    if (v < 0 || v >= d.nodes.size() || d.nodes[v].type != QLatin1String("color") || selectedColour_ == v) return;
    selectedColour_ = v;
    emit settingsChanged();
}

bool MacBridge::commitColourState(const QVariantMap& next, bool chainLevel) {
    const int index = colourIndex();
    if (index < 0) { notify(macText("Add a colour node to the chain first"), true); return false; }
    EnhNode& node = enhDoc().nodes[index];
    QVariantMap state = next;
    state.remove("index");
    state.remove("curveSamples");
    const bool enabled = state.value("enabled", node.enabled).toBool();
    QVariantMap before = node.colour;
    before["enabled"] = node.enabled;
    if (state == before) return true;
    if (!colourReplaying_) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool sameDrag = index == colourLastIndex_ && now - colourLastPushMs_ < 600 && !colourUndo_.isEmpty();
        if (!sameDrag) {
            colourUndo_.append({index, before});
            if (colourUndo_.size() > 64) colourUndo_.removeFirst();
        }
        colourLastPushMs_ = now;
        colourLastIndex_ = index;
        colourRedo_.clear();
    }
    const bool enabledChanged = node.enabled != enabled;
    node.colour = state;
    node.colour["enabled"] = enabled;
    node.enabled = enabled;
    enhChanged(chainLevel || enabledChanged);
    if (!chainLevel && !enabledChanged) emit colourParamsChanged();
    return true;
}

double MacBridge::colourParameter(const QString& name) const { return getPath(colourState(), name); }
bool MacBridge::setColourParameter(const QString& name, double value) {
    if (!std::isfinite(value)) return false;
    QVariantMap state = colourState();
    if (!setPath(state, name, value)) return false;
    return commitColourState(state);
}
bool MacBridge::resetColourParameter(const QString& name) {
    return setColourParameter(name, getPath(enh::defaultColour(), name));
}
double MacBridge::colorExposure() const { return colourParameter(QStringLiteral("exposure")); }
void MacBridge::setColorExposure(double v) { setColourParameter(QStringLiteral("exposure"), v); }
double MacBridge::colorContrast() const { return colourParameter(QStringLiteral("contrast")); }
void MacBridge::setColorContrast(double v) { setColourParameter(QStringLiteral("contrast"), v); }
double MacBridge::colorSaturation() const { return colourParameter(QStringLiteral("saturation")); }
void MacBridge::setColorSaturation(double v) { setColourParameter(QStringLiteral("saturation"), v); }
double MacBridge::colorTemperature() const { return colourParameter(QStringLiteral("temperature")); }
void MacBridge::setColorTemperature(double v) { setColourParameter(QStringLiteral("temperature"), v); }
bool MacBridge::colorEnabled() const { return colourState().value("enabled").toBool(); }
void MacBridge::setColorEnabled(bool v) {
    QVariantMap state = colourState();
    state["enabled"] = v;
    commitColourState(state);
}
QString MacBridge::colorStatus() const { return QString(); }

QVariantList MacBridge::colourParameters() const {
    QVariantList out;
    for (const auto& p : enh::params()) {
        out << QVariantMap{{"name", QString::fromUtf8(p.name)}, {"label", zh(p.label)}, {"group", zh(p.group)},
                           {"min", double(p.min)}, {"max", double(p.max)}, {"step", double(p.step)},
                           {"defaultValue", double(p.def)}, {"center", p.min < 0 && p.max > 0}};
    }
    return out;
}
QVariantList MacBridge::colourGroups() const {
    QStringList order;
    QHash<QString, QVariantList> buckets;
    for (const auto& p : enh::params()) {
        const QString group = zh(p.group);
        if (!buckets.contains(group)) order << group;
        buckets[group] << QVariantMap{{"name", QString::fromUtf8(p.name)}, {"label", zh(p.label)},
                                      {"min", double(p.min)}, {"max", double(p.max)}, {"center", p.min < 0 && p.max > 0}};
    }
    QVariantList groups;
    for (const auto& g : order) groups << QVariantMap{{"group", g}, {"items", buckets.value(g)}};
    return groups;
}

bool MacBridge::setColourCurve(int channel, const QVariantList& points) {
    if (channel < 0 || channel >= enh::kCurveCount || points.size() < 2 || points.size() > enh::kCurvePoints) return false;
    QVariantList clean;
    for (const auto& p : points) {
        const auto m = p.toMap();
        bool xok = false, yok = false;
        const double x = m.value("x").toDouble(&xok), y = m.value("y").toDouble(&yok);
        if (!xok || !yok || !std::isfinite(x) || !std::isfinite(y)) return false;
        clean << QVariantMap{{"x", x}, {"y", y}};
    }
    QVariantMap state = colourState();
    QVariantList curves = state.value("curves").toList();
    curves[channel] = clean;
    state["curves"] = curves;
    return commitColourState(state);
}
bool MacBridge::setColourWheel(int zone, double hue, double saturation, double luminance) {
    if (zone < 0 || zone >= enh::kZones || !std::isfinite(hue) || !std::isfinite(saturation) || !std::isfinite(luminance)) return false;
    QVariantMap state = colourState();
    QVariantList wheels = state.value("grading").toList();
    wheels[zone] = QVariantMap{{"hue", hue}, {"saturation", saturation}, {"luminance", luminance}};
    state["grading"] = wheels;
    return commitColourState(state);
}
bool MacBridge::setColourOption(const QString& name, int value) {
    QVariantMap state = colourState();
    if (name == QLatin1String("blackWhite") && (value == 0 || value == 1)) state["blackWhite"] = value != 0;
    else if (name == QLatin1String("groupBypassMask") && value >= 0 && value < 256) state["groupBypassMask"] = value;
    else if (name == QLatin1String("lutInputSpace") && value >= 0 && value <= 2) state["lutInputSpace"] = value;
    else return false;
    return commitColourState(state);
}
bool MacBridge::resetColourGroup(int group) {
    QVariantMap c = colourState();
    const QVariantMap d = enh::defaultColour();
    auto reset = [&](std::initializer_list<const char*> keys) { for (const char* k : keys) c[QLatin1String(k)] = d.value(QLatin1String(k)); };
    switch (group) {
    case -1: { const bool enabled = c.value("enabled").toBool(); c = d; c["enabled"] = enabled; break; }
    case 0: reset({"exposure", "contrast", "highlights", "shadows", "whites", "blacks"}); break;
    case 1: reset({"temperature", "tint", "vibrance", "saturation"}); break;
    case 2: reset({"paramHighlights", "paramLights", "paramDarks", "paramShadows", "curves", "splitHighlights", "splitMidtones", "splitShadows"}); break;
    case 3: reset({"mixerHue", "mixerSaturation", "mixerLuminance", "blackWhite", "blackWhiteMix"}); break;
    case 4: reset({"grading", "gradingBlending", "gradingBalance"}); break;
    case 5: reset({"calibrationShadowTint", "calibrationHue", "calibrationSaturation"}); break;
    case 6: reset({"lutName", "lutStrength", "lutInputSpace"}); break;
    case 7: reset({"texture", "clarity", "dehaze"}); break;
    default: return false;
    }
    if (group >= 0 && group < 7) c["groupBypassMask"] = c.value("groupBypassMask").toInt() & ~(1 << group);
    return commitColourState(c);
}

bool MacBridge::colourCanUndo() const { return !colourUndo_.isEmpty(); }
bool MacBridge::colourCanRedo() const { return !colourRedo_.isEmpty(); }
bool MacBridge::colourCanPaste() const { return !colourClipboard_.isEmpty(); }
bool MacBridge::colourUndo() {
    if (colourUndo_.isEmpty()) return false;
    const auto entry = colourUndo_.takeLast();
    auto& d = enhDoc();
    if (entry.first < 0 || entry.first >= d.nodes.size() || d.nodes[entry.first].type != QLatin1String("color")) return false;
    QVariantMap current = d.nodes[entry.first].colour;
    current["enabled"] = d.nodes[entry.first].enabled;
    colourRedo_.append({entry.first, current});
    selectedColour_ = entry.first;
    colourReplaying_ = true;
    commitColourState(entry.second, true);
    colourReplaying_ = false;
    return true;
}
bool MacBridge::colourRedo() {
    if (colourRedo_.isEmpty()) return false;
    const auto entry = colourRedo_.takeLast();
    auto& d = enhDoc();
    if (entry.first < 0 || entry.first >= d.nodes.size() || d.nodes[entry.first].type != QLatin1String("color")) return false;
    QVariantMap current = d.nodes[entry.first].colour;
    current["enabled"] = d.nodes[entry.first].enabled;
    colourUndo_.append({entry.first, current});
    selectedColour_ = entry.first;
    colourReplaying_ = true;
    commitColourState(entry.second, true);
    colourReplaying_ = false;
    return true;
}
void MacBridge::colourCopy() {
    colourClipboard_ = colourState();
    emit settingsChanged();
    notify(zh("调色已复制"));
}
bool MacBridge::colourPaste() {
    if (colourClipboard_.isEmpty()) return false;
    QVariantMap state = colourClipboard_;
    state["enabled"] = true;
    return commitColourState(state, true);
}

// --- LUT library and looks ----------------------------------------------------
QVariantList MacBridge::lutLibrary() const {
    QVariantList out;
    for (const auto& info : QDir(enhLutDirectory()).entryInfoList({QStringLiteral("*.cube")}, QDir::Files, QDir::Name | QDir::IgnoreCase))
        out << info.completeBaseName();
    return out;
}
void MacBridge::importLutDialog() {
    const QString path = QFileDialog::getOpenFileName(nullptr, macText("Import 3D LUT"), QDir::homePath(),
                                                      macText("3D LUT files (*.cube)"));
    if (path.isEmpty()) return;
    if (importLut(path)) setColourLut(QFileInfo(path).completeBaseName());
}
bool MacBridge::importLut(const QString& path) {
    QString local = path.startsWith(QLatin1String("file:")) ? QUrl(path).toLocalFile() : path;
    enh::CubeLut lut;
    QString error;
    if (!enh::parseCube(local, lut, &error)) {
        notify(macText("This LUT could not be read: ") + error, true);
        return false;
    }
    const QString target = enhLutDirectory() + QLatin1Char('/') + QFileInfo(local).completeBaseName() + QStringLiteral(".cube");
    if (QFileInfo(local).absoluteFilePath() != QFileInfo(target).absoluteFilePath()) {
        QFile::remove(target);
        if (!QFile::copy(local, target)) { notify(macText("The LUT could not be copied into the library"), true); return false; }
    }
    emit colourLibraryChanged();
    if (enhTimer_) enhTimer_->start();
    return true;
}
bool MacBridge::setColourLut(const QString& name) {
    if (!name.isEmpty() && !QFileInfo::exists(enhLutDirectory() + QLatin1Char('/') + name + QStringLiteral(".cube"))) {
        notify(macText("LUT not found in the library"), true);
        return false;
    }
    QVariantMap state = colourState();
    state["lutName"] = name;
    if (!name.isEmpty()) state["enabled"] = true;
    return commitColourState(state);
}

QVariantList MacBridge::readLooks() const {
    QFile f(enhLooksFile());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).array().toVariantList();
}
bool MacBridge::writeLooks(const QVariantList& looks) {
    QFile f(enhLooksFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(QJsonArray::fromVariantList(looks)).toJson());
    emit colourLibraryChanged();
    return true;
}
QVariantList MacBridge::colourLooks() const {
    QVariantList out;
    const auto looks = readLooks();
    for (int i = 0; i < looks.size(); ++i) {
        const auto look = looks[i].toMap();
        out << QVariantMap{{"index", i}, {"name", look.value("name")}, {"lutName", look.value("colour").toMap().value("lutName")}};
    }
    return out;
}
bool MacBridge::saveColourLook(const QString& name, bool replace) {
    const QString clean = name.trimmed();
    if (clean.isEmpty()) return false;
    QVariantList looks = readLooks();
    QVariantMap colour = colourState();
    colour.remove("index");
    colour.remove("curveSamples");
    for (int i = 0; i < looks.size(); ++i)
        if (looks[i].toMap().value("name").toString() == clean) {
            if (!replace) { notify(macText("A look with this name already exists"), true); return false; }
            looks[i] = QVariantMap{{"name", clean}, {"colour", colour}};
            return writeLooks(looks);
        }
    looks << QVariantMap{{"name", clean}, {"colour", colour}};
    return writeLooks(looks);
}
bool MacBridge::applyColourLook(int index) {
    const auto looks = readLooks();
    if (index < 0 || index >= looks.size()) return false;
    QVariantMap colour = enh::defaultColour();
    const auto saved = looks[index].toMap().value("colour").toMap();
    for (auto it = saved.begin(); it != saved.end(); ++it) colour[it.key()] = it.value();
    colour["enabled"] = true;
    return commitColourState(colour, true);
}
bool MacBridge::deleteColourLook(int index) {
    QVariantList looks = readLooks();
    if (index < 0 || index >= looks.size()) return false;
    looks.removeAt(index);
    return writeLooks(looks);
}
void MacBridge::importColourLookDialog() {
    const QString path = QFileDialog::getOpenFileName(nullptr, macText("Import colour look"), QDir::homePath(),
                                                      macText("Veyra colour looks (*.json)"));
    if (!path.isEmpty()) importColourLook(path);
}
bool MacBridge::importColourLook(const QString& path) {
    QFile f(path.startsWith(QLatin1String("file:")) ? QUrl(path).toLocalFile() : path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const auto look = QJsonDocument::fromJson(f.readAll()).object().toVariantMap();
    if (look.value("name").toString().isEmpty() || !look.value("colour").canConvert<QVariantMap>()) {
        notify(macText("This file is not a Veyra colour look"), true);
        return false;
    }
    QVariantList looks = readLooks();
    looks << look;
    return writeLooks(looks);
}
void MacBridge::exportColourLookDialog(int index) {
    const auto looks = readLooks();
    if (index < 0 || index >= looks.size()) return;
    const QString name = looks[index].toMap().value("name").toString();
    const QString path = QFileDialog::getSaveFileName(nullptr, macText("Export colour look"),
                                                      QDir::homePath() + QLatin1Char('/') + name + QStringLiteral(".json"),
                                                      macText("Veyra colour looks (*.json)"));
    if (!path.isEmpty()) exportColourLook(index, path);
}
bool MacBridge::exportColourLook(int index, const QString& path) {
    const auto looks = readLooks();
    if (index < 0 || index >= looks.size()) return false;
    QFile f(path.startsWith(QLatin1String("file:")) ? QUrl(path).toLocalFile() : path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(QJsonObject::fromVariantMap(looks[index].toMap())).toJson());
    return true;
}

// --- anti-aliasing and SR -------------------------------------------------------
QVariantMap MacBridge::aaMap(const EnhNode* node, int index) {
    const int method = node ? node->aa.value("method", 0).toInt() : 0;
    const int quality = node ? node->aa.value("quality", 1).toInt() : 1;
    return {{"index", index}, {"present", node != nullptr}, {"enabled", node && node->enabled},
            {"method", method}, {"quality", quality}, {"mlaaThreshold", 0.08}, {"mlaaStrength", 1.0},
            {"temporal", 0}, {"historyWeight", 0.5}, {"historyRejection", 8.0},
            {"summary", QStringLiteral("FXAA")}};
}
bool MacBridge::antiAliasEnabled() const {
    for (const auto& n : enhDoc().nodes) if (n.type == QLatin1String("aa")) return n.enabled;
    return false;
}
void MacBridge::setAntiAliasEnabled(bool v) {
    if (v != antiAliasEnabled()) setAntiAliasParameter(QStringLiteral("enabled"), v ? 1 : 0);
}
QVariantMap MacBridge::antiAliasParams() const {
    const auto& d = enhDoc();
    for (int i = 0; i < d.nodes.size(); ++i) if (d.nodes[i].type == QLatin1String("aa")) return aaMap(&d.nodes[i], i);
    return aaMap(nullptr, -1);
}
QVariantMap MacBridge::antiAliasParamsAt(int index) const {
    const auto& d = enhDoc();
    if (index < 0 || index >= d.nodes.size() || d.nodes[index].type != QLatin1String("aa")) return aaMap(nullptr, -1);
    return aaMap(&d.nodes[index], index);
}
bool MacBridge::setAntiAliasParameter(const QString& key, double value) {
    auto& d = enhDoc();
    for (int i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].type == QLatin1String("aa")) return setAntiAliasParameterAt(i, key, value);
    if (enhMode_ != 0) { notify(zh("请先添加抗锯齿节点"), true); return false; }
    const int index = addEffect(QStringLiteral("aa"));
    if (index < 0) return false;
    d.nodes[index].enabled = false;
    return setAntiAliasParameterAt(index, key, value);
}
bool MacBridge::setAntiAliasParameterAt(int index, const QString& key, double value) {
    auto& d = enhDoc();
    if (index < 0 || index >= d.nodes.size() || d.nodes[index].type != QLatin1String("aa") || !std::isfinite(value)) return false;
    EnhNode& n = d.nodes[index];
    const int whole = int(std::lround(value));
    if (key == QLatin1String("enabled")) {
        n.enabled = value != 0;
        enhChanged(true);
        return true;
    }
    if (key == QLatin1String("method")) {
        if (whole != 0) { notify(macText("On Mac, anti-aliasing uses FXAA"), true); emit antiAliasChanged(); return false; }
        n.aa["method"] = 0;
    } else if (key == QLatin1String("quality")) {
        if (whole < 0 || whole > 2) return false;
        n.aa["quality"] = whole;
    } else {
        return false;   // SMAA/MLAA/temporal keys do not exist on Mac
    }
    emit antiAliasChanged();
    enhChanged(false);
    return true;
}

bool MacBridge::srEnabled() const {
    for (const auto& n : enhDoc().nodes) if (n.type == QLatin1String("sr")) return n.enabled;
    return false;
}
void MacBridge::setSrEnabled(bool v) {
    auto& d = enhDoc();
    for (int i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].type == QLatin1String("sr")) { setEffectEnabled(i, v); return; }
    if (!v) return;
    if (enhMode_ != 0) { notify(zh("请先添加超分节点"), true); return; }
    addEffect(QStringLiteral("sr"));
}
QString MacBridge::srStageSummary() const {
    return srEnabled() ? macText("Spatial upscale (bicubic + sharpening)") : QString();
}
QVariantList MacBridge::srBackendChoices() const {
    QVariantList out;
    out << QVariantMap{{"id", QStringLiteral("0")}, {"label", QStringLiteral("DLSS SR")}, {"disabled", true}, {"note", nvidiaReason()}};
    out << QVariantMap{{"id", QStringLiteral("rtx")}, {"label", zh("RTX 视频超分 · 标准")}, {"disabled", true}, {"note", nvidiaReason()}};
    out << QVariantMap{{"id", QStringLiteral("5")}, {"label", QStringLiteral("FSR 3.1")}, {"disabled", true}, {"note", amdIntelReason()}};
    return out;
}
QVariantList MacBridge::nrRuntimeChoices() const {
    QVariantList out;
    out << QVariantMap{{"id", QStringLiteral("2")}, {"label", QStringLiteral("RTX 20–50 · SF 310.9.16")}, {"disabled", true}, {"note", nvidiaReason()}};
    out << QVariantMap{{"id", QStringLiteral("3")}, {"label", zh("RTX 50 · NVIDIA 原版")}, {"disabled", true}, {"note", nvidiaReason()}};
    out << QVariantMap{{"id", QStringLiteral("4")}, {"label", zh("RX 9000 · lmxxf（实验）")}, {"disabled", true}, {"note", amdIntelReason()}};
    return out;
}
QVariantList MacBridge::fgBackendChoices() const {
    QVariantList out;
    const QList<QPair<QString, QString>> backends{{"dlss", "DLSS FG"}, {"vfg", "NVIDIA VFG"}, {"fsr3", "FSR 3.1 FG"},
                                                  {"fsr4", "FSR 4 FG"}, {"amdfrc", "AMD FRC"}, {"xess", "XeSS FG"}, {"anr", "ANR FG"}};
    for (const auto& b : backends)
        out << QVariantMap{{"id", b.first}, {"label", b.second}, {"disabled", true}, {"note", fgReason()}};
    return out;
}

// --- capabilities ---------------------------------------------------------------
QVariantMap MacBridge::effectCapabilities() const {
    QVariantMap caps;
    const QVariantMap nvidia{{"available", false}, {"reason", nvidiaReason()}};
    const QVariantMap amd{{"available", false}, {"reason", amdIntelReason()}};
    const QVariantMap fg{{"available", false}, {"reason", fgReason()}};
    for (const char* k : {"nr0", "nr1", "nr2", "nr3", "nrAuto", "sr0", "sr1", "sr2", "sr3", "sr4", "sr16", "sr21", "hdr",
                          "hdr10Plus", "dlss", "vfg", "flow0", "flow1", "dlssgLite", "deepdvc"})
        caps.insert(QLatin1String(k), nvidia);
    for (const char* k : {"nr4", "sr5", "xess", "fsr3", "fsr4", "amdfrc", "anr", "flow2"}) caps.insert(QLatin1String(k), amd);
    caps.insert("frame-generation", fg);
    caps.insert("beauty", QVariantMap{{"available", false}, {"reason", macText("Beauty filters are not available on Mac")}});
    const QVariantMap ok{{"available", true}, {"reason", QString()}};
    caps.insert("color", ok);
    caps.insert("aa", ok);
    caps.insert("sr", ok);
    caps.insert("mac-sr", ok);
    return caps;
}
QVariantMap MacBridge::effectAvailability(const QString& id) const {
    const QVariantMap ok{{"available", true}, {"reason", QString()}};
    if (macType(id)) return ok;
    if (id == QLatin1String("frame-generation") || id.endsWith(QLatin1String("-fg")))
        return {{"available", false}, {"reason", fgReason()}};
    if (id == QLatin1String("beauty"))
        return {{"available", false}, {"reason", macText("Beauty filters are not available on Mac")}};
    if (id == QLatin1String("protection") || id == QLatin1String("exclusion") || id == QLatin1String("nr-stabilizer"))
        return {{"available", false}, {"reason", macText("Only used together with NR, which needs NVIDIA or AMD on Windows")}};
    const auto caps = effectCapabilities();
    if (caps.contains(id)) return caps.value(id).toMap();
    return {{"available", false}, {"reason", nvidiaReason()}};
}
QVariantMap MacBridge::effectDescriptor(const QString& type) const {
    const TypeInfo* t = typeInfo(type);
    if (!t) return {};
    return {{"id", type}, {"label", zh(t->label)}, {"maxInstances", t->maxInstances}, {"repeatable", t->repeatable},
            {"mustBeLast", t->mustBeLast}, {"justBeforeLast", t->justBeforeLast},
            {"changesResolution", t->changesResolution}, {"experimental", t->experimental}};
}

// --- chain ----------------------------------------------------------------------
QVariantList MacBridge::chain() const {
    QVariantList out;
    const auto& d = enhDoc();
    for (int i = 0; i < d.nodes.size(); ++i) {
        const EnhNode& n = d.nodes[i];
        const TypeInfo* t = typeInfo(n.type);
        out << QVariantMap{{"index", i}, {"id", enhMode_ == 1 ? n.id : uint(i + 2)}, {"type", n.type},
                           {"label", n.type == QLatin1String("sr") ? macText("Super resolution (spatial)") : zh(t ? t->label : "")},
                           {"enabled", n.enabled}, {"x", n.x}, {"y", n.y},
                           {"mustBeLast", t && t->mustBeLast}, {"justBeforeLast", t && t->justBeforeLast},
                           {"experimental", false}, {"changesResolution", t && t->changesResolution}};
    }
    return out;
}
QVariantList MacBridge::nodeConnections() const {
    QVariantList out;
    if (enhMode_ != 1) return out;
    const auto& d = enhDoc();
    out << QVariantMap{{"from", 0u}, {"to", d.inputNext}};
    for (const auto& n : d.nodes) {
        const uint to = d.next.value(n.id, 0);
        if (to) out << QVariantMap{{"from", n.id}, {"to", to}};
    }
    return out;
}
QVariantList MacBridge::effectCatalog() const {
    QVariantList out;
    for (const auto& t : kTypes) {
        const QString id = QString::fromLatin1(t.id);
        if (enhMode_ == 0 && id == QLatin1String("nr-stabilizer")) continue;
        const auto cap = effectAvailability(id);
        out << QVariantMap{{"id", id}, {"label", id == QLatin1String("sr") ? macText("Super resolution (spatial)") : zh(t.label)},
                           {"maxInstances", t.maxInstances}, {"repeatable", t.repeatable}, {"mustBeLast", t.mustBeLast},
                           {"justBeforeLast", t.justBeforeLast}, {"experimental", t.experimental},
                           {"disabled", !cap.value("available").toBool()}, {"note", cap.value("reason")}};
    }
    return out;
}
QString MacBridge::chainError() const { return QString(); }
bool MacBridge::chainValid() const { return true; }
int MacBridge::nodeMode() const { return enhMode_; }
void MacBridge::setNodeMode(int v) {
    if (v < 0 || v > 1 || v == enhMode_) return;
    enhMode_ = v;
    selectedColour_ = -1;
    colourUndo_.clear();
    colourRedo_.clear();
    enhChanged(true);
}

uint MacBridge::enhAddNode(const QString& type) {
    auto& d = enhDoc();
    EnhNode node;
    node.type = type;
    node.id = d.nextId++;
    node.enabled = true;
    if (type == QLatin1String("color")) { node.colour = enh::defaultColour(); node.colour["enabled"] = true; }
    if (type == QLatin1String("aa")) node.aa = {{"method", 0}, {"quality", 1}};
    int at = d.nodes.size();
    if (enhMode_ == 0) {
        // List order mirrors Windows' toChain(): colour layers, then AA, then SR.
        auto rank = [](const QString& t) { return t == QLatin1String("color") ? 0 : t == QLatin1String("aa") ? 1 : 2; };
        at = 0;
        while (at < d.nodes.size() && rank(d.nodes[at].type) <= rank(type)) ++at;
    } else {
        node.x = 120 + 40 * d.nodes.size();
        node.y = 120;
    }
    d.nodes.insert(at, node);
    return node.id;
}
int MacBridge::addEffect(const QString& type) {
    const auto cap = effectAvailability(type);
    if (!cap.value("available").toBool()) { notify(cap.value("reason").toString(), true); return -1; }
    const TypeInfo* t = typeInfo(type);
    int count = 0;
    for (const auto& n : enhDoc().nodes) count += n.type == type;
    if (t && count >= t->maxInstances) { notify(macText("This effect cannot be added more times"), true); return -1; }
    const uint id = enhAddNode(type);
    const int index = nodeIndexForId(id);
    if (type == QLatin1String("color")) selectedColour_ = index;
    enhChanged(true);
    return index;
}
bool MacBridge::removeEffect(int index) {
    auto& d = enhDoc();
    if (index < 0 || index >= d.nodes.size()) return false;
    const uint id = d.nodes[index].id;
    if (enhMode_ == 1) {
        const uint after = d.next.value(id, 0);
        if (d.inputNext == id) d.inputNext = after ? after : 1;
        for (auto it = d.next.begin(); it != d.next.end(); ++it) if (it.value() == id) it.value() = after ? after : 1;
        d.next.remove(id);
    }
    d.nodes.removeAt(index);
    selectedColour_ = -1;
    colourUndo_.clear();
    colourRedo_.clear();
    enhChanged(true);
    return true;
}
bool MacBridge::moveEffect(int from, int to) {
    auto& d = enhDoc();
    if (from < 0 || to < 0 || from >= d.nodes.size() || to >= d.nodes.size()) return false;
    if (from == to) return true;
    if (enhMode_ == 1) return insertNodeAfter(d.nodes[from].id, d.nodes[to].id);
    d.nodes.move(from, to);
    selectedColour_ = -1;
    colourUndo_.clear();
    colourRedo_.clear();
    enhChanged(true);
    return true;
}
bool MacBridge::setEffectEnabled(int index, bool enabled) {
    auto& d = enhDoc();
    if (index < 0 || index >= d.nodes.size()) return false;
    if (d.nodes[index].enabled == enabled) return true;
    d.nodes[index].enabled = enabled;
    if (d.nodes[index].type == QLatin1String("color")) d.nodes[index].colour["enabled"] = enabled;
    enhChanged(true);
    return true;
}
bool MacBridge::setEffectPosition(int index, double x, double y) {
    auto& d = enhDoc();
    if (index < 0 || index >= d.nodes.size() || !std::isfinite(x) || !std::isfinite(y)) return false;
    d.nodes[index].x = x;
    d.nodes[index].y = y;
    emit chainChanged();
    return true;
}
int MacBridge::nodeIndexForId(uint id) const {
    const auto& d = enhDoc();
    for (int i = 0; i < d.nodes.size(); ++i) if (d.nodes[i].id == id) return i;
    return -1;
}
int MacBridge::duplicateNode(uint id) {
    const int index = nodeIndexForId(id);
    if (index < 0) return -1;
    const EnhNode copy = enhDoc().nodes[index];
    const int added = addEffect(copy.type);
    if (added < 0) return -1;
    EnhNode& n = enhDoc().nodes[added];
    n.colour = copy.colour;
    n.aa = copy.aa;
    n.enabled = copy.enabled;
    n.x = copy.x + 40;
    n.y = copy.y + 40;
    enhChanged(true);
    return added;
}
bool MacBridge::resetNode(uint id) {
    const int index = nodeIndexForId(id);
    if (index < 0) return false;
    EnhNode& n = enhDoc().nodes[index];
    if (n.type == QLatin1String("color")) { n.colour = enh::defaultColour(); n.colour["enabled"] = n.enabled; }
    if (n.type == QLatin1String("aa")) n.aa = {{"method", 0}, {"quality", 1}};
    n.srSharpness = 0.25;
    enhChanged(true);
    return true;
}
bool MacBridge::connectNodes(uint from, uint to) {
    if (enhMode_ != 1 || from == to) return false;
    auto& d = enhDoc();
    if ((from != 0 && nodeIndexForId(from) < 0) || (to != 1 && nodeIndexForId(to) < 0)) return false;
    // One input and one output per card: drop whatever fed `to` before.
    if (to != 1) {
        if (d.inputNext == to) d.inputNext = 1;
        for (auto it = d.next.begin(); it != d.next.end();) it = it.value() == to ? d.next.erase(it) : std::next(it);
    }
    if (from == 0) d.inputNext = to;
    else d.next[from] = to;
    // Refuse a loop: the path from the input must end at the output.
    QSet<uint> seen;
    uint id = d.inputNext;
    while (id > 1) {
        if (seen.contains(id)) { d.next.remove(from); notify(macText("That connection would create a loop"), true); enhChanged(true); return false; }
        seen.insert(id);
        id = d.next.value(id, 0);
        if (!id) break;
    }
    enhChanged(true);
    return true;
}
bool MacBridge::disconnectNode(uint from) {
    if (enhMode_ != 1) return false;
    auto& d = enhDoc();
    if (from == 0) d.inputNext = 1;
    else d.next.remove(from);
    enhChanged(true);
    return true;
}
bool MacBridge::insertNodeAfter(uint id, uint after) {
    if (enhMode_ != 1) return false;
    auto& d = enhDoc();
    if (nodeIndexForId(id) < 0 || (after != 0 && nodeIndexForId(after) < 0) || id == after) return false;
    // Unlink id, then splice it in after `after`.
    const uint idNext = d.next.value(id, 0);
    if (d.inputNext == id) d.inputNext = idNext ? idNext : 1;
    for (auto it = d.next.begin(); it != d.next.end(); ++it) if (it.value() == id) it.value() = idNext ? idNext : 1;
    d.next.remove(id);
    const uint afterNext = after == 0 ? d.inputNext : d.next.value(after, 1);
    d.next[id] = afterNext;
    if (after == 0) d.inputNext = id;
    else d.next[after] = id;
    enhChanged(true);
    return true;
}
void MacBridge::resetCurrentPage() {
    EnhDocument fresh;
    if (enhMode_ == 0) {
        for (const char* type : {"color", "sr"}) {
            EnhNode node;
            node.type = QString::fromLatin1(type);
            node.id = fresh.nextId++;
            if (node.type == QLatin1String("color")) node.colour = enh::defaultColour();
            fresh.nodes << node;
        }
    }
    enhDoc() = fresh;
    selectedColour_ = -1;
    colourUndo_.clear();
    colourRedo_.clear();
    enhChanged(true);
}
