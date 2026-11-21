// Preferences, interface language, shortcuts, shell helpers, presets and the
// helper objects behind veyra.updates / playlist / exportQueueModel / moonlight / xbox.
#include "../MacBridge.h"
#include "PrefsObjects.h"
#include "veyra/Log.h"
#include "veyra/ui/UiLanguage.h"

#include <QAudioDevice>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMediaDevices>
#include <QMetaProperty>
#include <QProcess>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

namespace {
// Strings shared with the Windows bridge keep its Chinese source text, which is the
// catalog key (i18n/catalog.json) — so they arrive translated in every language.
QString tx(const char* s) { return QCoreApplication::translate("QmlPlayerBridge", s); }

const QVariantMap& shortcutDefaults() {
    static const QVariantMap d{{"playPause", "Space"}, {"fullscreen", "F"}, {"lock", "Ctrl+L"},
                               {"hold", "V"}, {"screenshot", "F7"}, {"record", "F8"}, {"replay", "F9"}, {"toggleMode", "Tab"},
                               {"gamePanel", "Home"}, {"gameExit", "Ctrl+Alt+Home"}};
    return d;
}
QString dataDir() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir;
}
bool writeJson(const QString& path, const QJsonDocument& doc) {
    QFile f(path + QStringLiteral(".tmp"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (f.write(doc.toJson(QJsonDocument::Indented)) < 0) return false;
    f.close();
    QFile::remove(path);
    return QFile::rename(path + QStringLiteral(".tmp"), path);
}
// Enhancement values a preset carries: every writable bridge property of these families.
bool presetProperty(const QString& name) {
    static const QStringList prefixes{"color", "colour", "antiAlias", "sharpen", "lut", "srEnabled", "videoSrQuality", "srOutput"};
    for (const auto& p : prefixes) if (name.startsWith(p)) return true;
    return false;
}
} // namespace

void MacBridge::initPrefs() {
    const QString dir = dataDir();
    prefsFile_ = dir + QStringLiteral("/preferences.json");
    presetsFile_ = dir + QStringLiteral("/presets.json");
    QFile f(prefsFile_);
    if (f.open(QIODevice::ReadOnly)) m_preferences = QJsonDocument::fromJson(f.readAll()).object().toVariantMap();

    m_dataDirectory = QDir::toNativeSeparators(dir);
    m_logFile = QDir::toNativeSeparators(dir + QStringLiteral("/logs/veyra.log"));
    m_appName = QStringLiteral("Veyra");
    m_version = QStringLiteral("2.1.9 (macOS)");
    m_reducedMotion = prefBool(QStringLiteral("reducedMotion"), false);
    m_defaultPage = prefString(QStringLiteral("defaultPage"), QStringLiteral("home"));
    m_shortcuts = shortcuts();
    m_softwareUiActive = false;
    m_uiScaleActive = 0;

    // Helper objects for the QObject* properties.
    m_updates = new MacUpdateChecker(this);
    auto* playlist = new MacPlaylist(dir + QStringLiteral("/playlist.json"), this);
    playlist->opener = [this](const QString& path) { openPath(path); };
    m_playlist = playlist;
    m_exportQueueModel = new MacExportQueue(this);
    m_moonlight = new MacStreamClient(this);
    m_xbox = new MacStreamClient(this);
    // liveTranslation / nvidiaVrrProfile stay null: the QML hides those features then.

    loadPresets();
    applyLanguage(false);   // translator installed before the QML is created
    refreshAudioDevices();
    auto* devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::audioOutputsChanged, this, [this] { refreshAudioDevices(); });
}

void MacBridge::attachPrefs(QQuickWindow* window) {
    if (auto* engine = qmlEngine()) engine->setUiLanguage(veyra::ui::i18n::current());
    connect(window, &QQuickWindow::sceneGraphInitialized, this, [this] { refreshComponentList(); }, Qt::QueuedConnection);
    refreshComponentList();
}

// --- preference storage -------------------------------------------------------
QString MacBridge::prefString(const QString& key, const QString& fallback) const {
    const auto v = m_preferences.value(key);
    return v.isValid() ? v.toString() : fallback;
}
bool MacBridge::prefBool(const QString& key, bool fallback) const {
    const auto v = m_preferences.value(key);
    return v.isValid() ? v.toBool() : fallback;
}
int MacBridge::prefInt(const QString& key, int fallback) const {
    bool ok = false;
    const int n = m_preferences.value(key).toInt(&ok);
    return ok ? n : fallback;
}
bool MacBridge::savePrefs() {
    if (writeJson(prefsFile_, QJsonDocument(QJsonObject::fromVariantMap(m_preferences)))) return true;
    veyra::log::warn("ui-prefs", "preferences not saved: " + prefsFile_.toStdString());
    return false;
}

bool MacBridge::setPreference(const QString& key, const QVariant& value) {
    static const QHash<QString, std::pair<int, int>> ranges{
        {"subtitleSize", {16, 56}}, {"subtitleFont", {0, 5}}, {"subtitleOutline", {0, 3}},
        {"subtitleMargin", {0, 240}}, {"subtitleLines", {0, 8}}, {"listInspectorWidth", {376, 4000}},
        {"networkPrebuffer", {-1, 60}}, {"networkTimeshift", {0, 600}}, {"recordBitrateMbps", {0, 400}},
        {"recordFps", {0, 240}}, {"recordSplitSeconds", {0, 86400}}, {"recordSplitMiB", {0, 1048576}},
        {"virtualCameraFps", {0, 120}}, {"replaySeconds", {15, 600}}, {"backdrop", {0, 2}}};
    static const QStringList flags{"recordHevc", "recordAudio", "recordMic", "recordMicTrack", "replayEnabled", "subtitleEnabled",
        "subtitleBackground", "subtitleFit", "subtitleSecondLanguage", "audioForceStereo", "captureAudioMonitor", "holdCompare",
        "magewellLowLatency", "blackmagicNativeCapture", "captureShowAllFrameRates", "cinePillHidden", "exportStopsPlayback",
        "obsGameCapture", "autoResume", "fullscreenMemoryProtection", "prewarmEnhancement", "hdrFgCompatibility",
        "computeOnMonitorGpu", "deinterlace", "networkAutoReconnect", "introAnimation", "introMusic", "exclusionOverlay",
        "rememberPosition", "dockPinned", "exportCompletionSound"};
    static const QHash<QString, QStringList> choices{
        {"palette", {"dusk", "classic"}}, {"accent", {"orange", "white"}}, {"overlayCompat", {"auto", "off"}},
        {"uiRenderer", {"software", "gpu"}}, {"gpuPriority", {"normal", "high", "realtime"}},
        {"decode", {"auto", "software", "hardware"}}, {"networkBuffer", {"auto", "off", "small", "medium", "large"}},
        {"networkCorrupt", {"conceal", "keyframe"}}, {"nrResidualView", {"web", "list"}}, {"nrCorrectionView", {"web", "list"}},
        {"windowSize", {"1280x800", "1600x1000", "1920x1200", "last"}},
        {"language", {"auto", "zh-CN", "zh-TW", "en", "ja", "ko", "es", "fr", "de", "pt-BR", "ru"}}};
    QVariant stored;
    if (ranges.contains(key)) {
        bool ok = false; const int n = value.toInt(&ok);
        const auto [low, high] = ranges.value(key);
        if (!ok || n < low || n > high) return false;
        stored = n;
    } else if (flags.contains(key)) {
        stored = value.toBool();
    } else if (choices.contains(key)) {
        if (!choices.value(key).contains(value.toString())) return false;
        stored = value.toString();
    } else if (key == QLatin1String("uiScale")) {
        const int n = value.toInt();
        if (n != 0 && n != 100 && n != 125 && n != 150) return false;
        stored = n;
    } else if (key == QLatin1String("sliderKeyStep")) {
        const double v = value.toDouble();
        if (!(qFuzzyCompare(v, 1.0) || qFuzzyCompare(v, 0.1) || qFuzzyCompare(v, 0.01))) return false;
        stored = v;
    } else if (key == QLatin1String("screenshotDir") || key == QLatin1String("recordDir")) {
        const QString dir = value.toString();
        if (!dir.isEmpty() && !QFileInfo(dir).isDir()) return false;
        stored = dir;
    } else if (key == QLatin1String("audioDevice") || key == QLatin1String("recordMicDevice") ||
               key == QLatin1String("monitorGpu") || key == QLatin1String("updateDownloadRoute")) {
        stored = value.toString();
    } else {
        // Keys only the Mac parts use (e.g. capture choices): stored as given.
        if (key.isEmpty() || key.size() > 64) return false;
        stored = value;
    }
    if (m_preferences.value(key) == stored) return true;
    const auto previous = m_preferences;
    m_preferences[key] = stored;
    if (!savePrefs()) {
        m_preferences = previous;
        emit notice(tx("偏好保存失败，已保留原设置"), true);
        return false;
    }
    veyra::log::info("ui-prefs", key.toStdString() + "=" + stored.toString().toStdString());
    if (key == QLatin1String("language")) applyLanguage(true);
    if (key == QLatin1String("audioDevice")) refreshAudioDevices();
    emit preferencesChanged();
    return true;
}

void MacBridge::applyLanguage(bool notifyAll) {
    namespace i18n = veyra::ui::i18n;
    const QString code = i18n::resolve(prefString(QStringLiteral("language"), QStringLiteral("auto")));
    if (!i18n::apply(code)) emit notice(tx("这个语言的翻译没有找到，界面保持简体中文"), true);
    m_uiLanguage = i18n::current();
    m_defaultPageLabel = defaultPageLabel();
    if (!notifyAll) return;
    if (auto* engine = qmlEngine()) {
        engine->setUiLanguage(i18n::current());
        engine->retranslate();
    }
    // Strings the bridge builds itself follow once their properties notify.
    const QMetaObject* meta = metaObject();
    QSet<int> fired;
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        const QMetaMethod notify = meta->property(i).notifySignal();
        if (notify.isValid() && notify.parameterCount() == 0 && !fired.contains(notify.methodIndex())) {
            fired.insert(notify.methodIndex());
            notify.invoke(this, Qt::DirectConnection);
        }
    }
    refreshAudioDevices();
}
QString MacBridge::uiLanguage() const { return veyra::ui::i18n::current(); }

// --- pages ---------------------------------------------------------------------
void MacBridge::setReducedMotion(bool v) {
    if (m_reducedMotion == v) return;
    m_reducedMotion = v;
    m_preferences[QStringLiteral("reducedMotion")] = v;
    savePrefs();
    emit settingsChanged();
}
void MacBridge::setDefaultPage(QString v) {
    static const QStringList pages{"home", "min", "pro", "last"};
    if (!pages.contains(v) || m_defaultPage == v) return;
    m_defaultPage = v;
    m_preferences[QStringLiteral("defaultPage")] = v;
    savePrefs();
    m_defaultPageLabel = defaultPageLabel();
    emit settingsChanged();
}
QString MacBridge::defaultPageLabel() const {
    if (m_defaultPage == QLatin1String("min")) return tx("极简模式");
    if (m_defaultPage == QLatin1String("pro")) return tx("专业模式");
    if (m_defaultPage == QLatin1String("last")) return tx("上次");
    return tx("首页");
}
QString MacBridge::initialPage() const {
    if (!initialPageOverride_.isEmpty()) return initialPageOverride_;
    if (m_defaultPage == QLatin1String("last")) {
        const QString last = prefString(QStringLiteral("lastPage"));
        return last.isEmpty() ? QStringLiteral("home") : last;
    }
    return m_defaultPage.isEmpty() ? QStringLiteral("home") : m_defaultPage;
}
void MacBridge::setInitialPage(QString v) {
    initialPageOverride_ = v;
    m_initialPage = v;
    emit settingsChanged();
}
void MacBridge::setCurrentPage(QString v) {
    m_currentPage = v;
    static const QStringList remembered{"home", "min", "pro", "node", "set", "exp"};
    const QString store = v == QLatin1String("node") ? QStringLiteral("pro") : v;
    if (remembered.contains(v) && prefString(QStringLiteral("lastPage")) != store) {
        m_preferences[QStringLiteral("lastPage")] = store;
        savePrefs();
    }
}

// --- shortcuts -------------------------------------------------------------------
QVariantMap MacBridge::shortcuts() const {
    QVariantMap out = shortcutDefaults();
    const auto custom = m_preferences.value(QStringLiteral("shortcuts")).toMap();
    QStringList taken;
    for (auto it = custom.begin(); it != custom.end(); ++it)
        if (out.contains(it.key())) { out[it.key()] = it.value(); taken << it.value().toString(); }
    for (auto it = out.begin(); it != out.end(); ++it)
        if (!custom.contains(it.key()) && taken.contains(it.value().toString())) it.value() = QString();
    return out;
}
bool MacBridge::setShortcut(const QString& action, const QString& sequence) {
    if (!shortcutDefaults().contains(action)) return false;
    const QKeySequence seq(sequence, QKeySequence::PortableText);
    if (seq.isEmpty() || seq.count() != 1) { emit notice(tx("无效的按键"), true); return false; }
    const QString text = seq.toString(QKeySequence::PortableText);
    static const QStringList reserved{"Esc", "F11", "Left", "Right", "Up", "Down", "Ctrl+O", "Ctrl+E",
                                      "B", "Z", "X", "T", "Y", "Shift+Z", "Shift+X", "Alt+Return", "Ctrl+Q", "Ctrl+W"};
    if (reserved.contains(text)) { emit notice(tx("%1 已被固定功能占用").arg(text), true); return false; }
    const auto current = shortcuts();
    for (auto it = current.begin(); it != current.end(); ++it)
        if (it.key() != action && it.value().toString() == text) { emit notice(tx("%1 已用于其他操作").arg(text), true); return false; }
    auto custom = m_preferences.value(QStringLiteral("shortcuts")).toMap();
    custom[action] = text;
    m_preferences[QStringLiteral("shortcuts")] = custom;
    if (!savePrefs()) { emit notice(tx("快捷键保存失败"), true); return false; }
    m_shortcuts = shortcuts();
    emit preferencesChanged();
    return true;
}
void MacBridge::resetShortcuts() {
    m_preferences.remove(QStringLiteral("shortcuts"));
    savePrefs();
    m_shortcuts = shortcuts();
    emit preferencesChanged();
}

// --- window ----------------------------------------------------------------------
void MacBridge::rememberWindowSize(int width, int height) {
    if (width < 400 || height < 200 || width > 16384 || height > 16384) return;
    const QString v = QStringLiteral("%1x%2").arg(width).arg(height);
    if (m_preferences.value(QStringLiteral("lastWindow")).toString() == v) return;
    m_preferences[QStringLiteral("lastWindow")] = v;
    savePrefs();
}
QRect MacBridge::screenAvailableAt(int x, int y) const {
    QScreen* screen = QGuiApplication::screenAt(QPoint(x, y));
    if (!screen) screen = QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry() : QRect(0, 0, 1440, 875);
}
QRect MacBridge::launchScreenAvailable() const {
    const QPoint at = QCursor::pos();
    return screenAvailableAt(at.x(), at.y());
}

// --- folders, links, diagnostics ---------------------------------------------------
void MacBridge::openLogFolder() {
    const QString dir = QFileInfo(QDir::fromNativeSeparators(m_logFile)).absolutePath();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}
void MacBridge::openDataFolder() { QDesktopServices::openUrl(QUrl::fromLocalFile(dataDir())); }
void MacBridge::openProjectPage() {
    if (!QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Likely7/Veyra-NRVideo#readme"))))
        emit notice(tx("无法打开浏览器"), true);
}
void MacBridge::openFeedbackPage() {
    copyDiagnostics();
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Likely7/Veyra-NRVideo/issues")));
    emit notice(tx("诊断信息已复制，可直接粘贴到反馈里"), false);
}
void MacBridge::openReleasesPage() {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Likely7/Veyra-NRVideo/releases")));
}
bool MacBridge::openSponsorLink(const QString& id) {
    static const QHash<QString, QString> links{
        {"douyin", "https://v.douyin.com/9s06Bf2Inqc/"}, {"x", "https://x.com/likely2002"},
        {"github", "https://github.com/Likely7"}, {"youtube", "https://www.youtube.com/@Likely7Ai"},
        {"patreon", "https://www.patreon.com/cw/Likely7"},
        {"patreonCoffee", "https://www.patreon.com/Likely7/posts/buy-me-coffee-175018110"}};
    if (!links.contains(id)) return false;
    if (QDesktopServices::openUrl(QUrl(links.value(id)))) return true;
    emit notice(tx("无法打开浏览器"), true);
    return false;
}
QString MacBridge::diagnosticsReport() const {
    QStringList lines;
    lines << QStringLiteral("Veyra %1").arg(m_version)
          << QStringLiteral("OS: %1 (%2, %3)").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(), QSysInfo::currentCpuArchitecture())
          << QStringLiteral("Qt: %1").arg(QString::fromLatin1(qVersion()))
          << QStringLiteral("Language: %1").arg(veyra::ui::i18n::current())
          << QStringLiteral("Data: %1").arg(m_dataDirectory)
          << QStringLiteral("Log: %1").arg(m_logFile);
    for (const auto& c : m_componentList) {
        const auto m = c.toMap();
        lines << QStringLiteral("%1: %2").arg(m.value("name").toString(), m.value("detail").toString());
    }
    lines << QStringLiteral("Source: %1 %2x%3 %4 fps").arg(m_sourceKind.isEmpty() ? QStringLiteral("none") : m_sourceKind)
                 .arg(m_sourceWidth).arg(m_sourceHeight).arg(m_sourceFps, 0, 'f', 2);
    return lines.join(QLatin1Char('\n'));
}
void MacBridge::copyDiagnostics() {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(diagnosticsReport());
        emit notice(tx("诊断信息已复制"), false);
    }
}
void MacBridge::refreshComponentList() {
    QVariantList out;
    auto add = [&](const QString& name, const QString& detail, bool loaded) {
        out << QVariantMap{{"name", name}, {"detail", detail}, {"loaded", loaded}, {"experimental", false}};
    };
    add(QStringLiteral("macOS"), QSysInfo::prettyProductName() + QStringLiteral(" · ") + QSysInfo::currentCpuArchitecture(), true);
    add(QStringLiteral("Qt"), QString::fromLatin1(qVersion()), true);
    const QByteArray backend = qgetenv("QT_MEDIA_BACKEND");
    add(QStringLiteral("Qt Multimedia"), backend.isEmpty() ? QStringLiteral("FFmpeg") : QString::fromLatin1(backend), true);
    QString api = QStringLiteral("—");
    if (auto* w = window()) {
        if (auto* ri = w->rendererInterface()) {
            switch (ri->graphicsApi()) {
            case QSGRendererInterface::Metal: api = QStringLiteral("Metal"); break;
            case QSGRendererInterface::OpenGL: api = QStringLiteral("OpenGL"); break;
            case QSGRendererInterface::Vulkan: api = QStringLiteral("Vulkan"); break;
            case QSGRendererInterface::Software: api = QStringLiteral("Software"); break;
            default: api = QStringLiteral("RHI"); break;
            }
        }
    }
    add(QStringLiteral("Graphics"), api, api != QStringLiteral("—"));
    if (out == m_componentList) return;
    m_componentList = out;
    emit componentListChanged();
}
void MacBridge::refreshAudioDevices() {
    QVariantList out;
    out << QVariantMap{{"id", QString()}, {"label", tx("跟随系统默认")}, {"isDefault", false}};
    const QString chosen = prefString(QStringLiteral("audioDevice"));
    bool listed = chosen.isEmpty();
    const auto def = QMediaDevices::defaultAudioOutput();
    QString activeName = def.description();
    for (const auto& d : QMediaDevices::audioOutputs()) {
        const QString id = QString::fromUtf8(d.id());
        const bool isDefault = d.id() == def.id();
        if (id == chosen) { listed = true; activeName = d.description(); }
        out << QVariantMap{{"id", id}, {"label", d.description() + (isDefault ? tx(" · 当前默认") : QString())}, {"isDefault", isDefault}};
    }
    if (!listed) out << QVariantMap{{"id", chosen}, {"label", tx("已保存的设备（未连接）")}, {"isDefault", false}, {"missing", true}};
    m_audioDevices = out;
    m_audioOutputFallback = !listed;
    m_audioOutputStatus = activeName.isEmpty() ? tx("尚未打开音频输出") : activeName;
    if (!listed) m_audioOutputStatus = tx("所选设备不可用，已回落到系统默认：") + m_audioOutputStatus;
    emit audioDevicesChanged();
}
void MacBridge::quit() { QCoreApplication::quit(); }
void MacBridge::restartApplication() {
    QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
    QCoreApplication::quit();
}

// --- presets ---------------------------------------------------------------------
QVariantMap MacBridge::presetValues() const {
    QVariantMap values;
    const QMetaObject* meta = metaObject();
    for (int i = 0; i < meta->propertyCount(); ++i) {
        const QMetaProperty p = meta->property(i);
        const QString name = QString::fromLatin1(p.name());
        if (p.isWritable() && presetProperty(name)) values[name] = p.read(this);
    }
    return values;
}
void MacBridge::loadPresets() {
    QFile f(presetsFile_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto root = QJsonDocument::fromJson(f.readAll()).object();
    presetStore_ = root.value(QStringLiteral("presets")).toArray().toVariantList();
    presetDefault_ = root.value(QStringLiteral("default")).toInt(-1);
    if (presetDefault_ >= presetStore_.size()) presetDefault_ = -1;
}
bool MacBridge::savePresets() {
    const QJsonObject root{{"version", 1}, {"default", presetDefault_}, {"presets", QJsonArray::fromVariantList(presetStore_)}};
    const bool ok = writeJson(presetsFile_, QJsonDocument(root));
    if (!ok) emit notice(tx("预设保存失败"), true);
    emit presetsChanged();
    return ok;
}
QVariantList MacBridge::presets() const {
    QVariantList out;
    for (int i = 0; i < presetStore_.size(); ++i) {
        const auto e = presetStore_.at(i).toMap();
        out << QVariantMap{{"index", i}, {"name", e.value("name")}, {"note", e.value("note").toString()}, {"builtin", false},
                           {"nodeMode", e.value("nodeMode").toBool()}, {"contents", e.value("contents").toInt()}};
    }
    return out;
}
QString MacBridge::currentPresetName() const {
    const auto now = presetValues();
    for (const auto& v : presetStore_) {
        const auto e = v.toMap();
        const auto stored = e.value("values").toMap();
        bool same = !stored.isEmpty();
        for (auto it = stored.begin(); same && it != stored.end(); ++it)
            same = now.value(it.key()).toString() == it.value().toString();
        if (same) return e.value("name").toString();
    }
    return tx("自定义");
}
QVariantList MacBridge::presetChoices() const {
    QVariantList out;
    out << QVariantMap{{"id", QStringLiteral("-1")}, {"label", tx("当前播放设置")}};
    for (int i = 0; i < presetStore_.size(); ++i)
        out << QVariantMap{{"id", QString::number(i)}, {"label", presetStore_.at(i).toMap().value("name").toString()}, {"disabled", false}};
    return out;
}
QVariantList MacBridge::presetSaveParts() const {
    QVariantList out;
    out << QVariantMap{{"id", "color"}, {"label", tx("色彩")},
                       {"summary", m_colorEnabled ? tx("已启用") : tx("未启用")}, {"meaningful", m_colorEnabled}};
    QStringList chainParts;
    for (const auto& n : m_chain) chainParts << n.toMap().value("label").toString();
    out << QVariantMap{{"id", "chain"}, {"label", tx("效果链")},
                       {"summary", chainParts.isEmpty() ? tx("空") : chainParts.join(QStringLiteral(" → "))},
                       {"meaningful", !chainParts.isEmpty()}};
    return out;
}
int MacBridge::defaultPresetIndex() const { return presetDefault_; }
bool MacBridge::applyPresetIndex(int index) {
    if (index < 0 || index >= presetStore_.size()) return false;
    const auto e = presetStore_.at(index).toMap();
    const auto values = e.value("values").toMap();
    for (auto it = values.begin(); it != values.end(); ++it) setProperty(it.key().toUtf8().constData(), it.value());
    emit presetsChanged();
    emit settingsChanged();
    emit notice(tx("已应用预设：") + e.value("name").toString(), false);
    return true;
}
bool MacBridge::savePresetAs(const QString& name, int contentsMask, bool nodeMode) {
    const QString clean = name.trimmed();
    if (clean.isEmpty()) return false;
    QVariantMap entry{{"name", clean}, {"nodeMode", nodeMode}, {"contents", contentsMask}, {"values", presetValues()}};
    for (int i = 0; i < presetStore_.size(); ++i)
        if (presetStore_.at(i).toMap().value("name").toString() == clean) { presetStore_[i] = entry; return savePresets(); }
    presetStore_ << entry;
    return savePresets();
}
bool MacBridge::deletePreset(int index) {
    if (index < 0 || index >= presetStore_.size()) return false;
    presetStore_.removeAt(index);
    if (presetDefault_ == index) presetDefault_ = -1;
    else if (presetDefault_ > index) --presetDefault_;
    return savePresets();
}
bool MacBridge::renamePreset(int index, const QString& name) {
    if (index < 0 || index >= presetStore_.size() || name.trimmed().isEmpty()) return false;
    auto e = presetStore_.at(index).toMap();
    e["name"] = name.trimmed();
    presetStore_[index] = e;
    return savePresets();
}
bool MacBridge::duplicatePreset(int index) {
    if (index < 0 || index >= presetStore_.size()) return false;
    auto e = presetStore_.at(index).toMap();
    e["name"] = e.value("name").toString() + tx(" 副本");
    presetStore_.insert(index + 1, e);
    return savePresets();
}
bool MacBridge::setDefaultPreset(int index) {
    if (index < -1 || index >= presetStore_.size()) return false;
    presetDefault_ = index;
    return savePresets();
}
void MacBridge::importPresetDialog() {
    const QString path = QFileDialog::getOpenFileName(nullptr, tx("导入预设"), QDir::homePath(), QStringLiteral("Veyra (*.json *.veyrapreset)"));
    if (!path.isEmpty()) importPreset(path);
}
bool MacBridge::importPreset(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { emit notice(tx("无法读取预设文件"), true); return false; }
    const auto obj = QJsonDocument::fromJson(f.readAll()).object();
    if (!obj.contains(QStringLiteral("values"))) { emit notice(tx("无法读取预设文件"), true); return false; }
    auto entry = obj.toVariantMap();
    if (entry.value("name").toString().isEmpty()) entry["name"] = QFileInfo(path).completeBaseName();
    presetStore_ << entry;
    return savePresets();
}
void MacBridge::exportPresetDialog(int index) {
    if (index < 0 || index >= presetStore_.size()) return;
    const QString name = presetStore_.at(index).toMap().value("name").toString();
    const QString path = QFileDialog::getSaveFileName(nullptr, tx("导出预设"), QDir::homePath() + QLatin1Char('/') + name + QStringLiteral(".json"),
                                                      QStringLiteral("Veyra (*.json)"));
    if (!path.isEmpty()) exportPreset(index, path);
}
bool MacBridge::exportPreset(int index, const QString& path) {
    if (index < 0 || index >= presetStore_.size()) return false;
    return writeJson(path, QJsonDocument(QJsonObject::fromVariantMap(presetStore_.at(index).toMap())));
}
