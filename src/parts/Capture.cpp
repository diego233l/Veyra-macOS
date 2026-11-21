// Capture card / camera, screen and window capture, and recording on macOS.
// Data shapes follow the Windows bridge (src/ui/bridge/Capture.cpp, Recording.cpp)
// so the original dialogs (CaptureSourceDialog.qml, ScreenSourceDialog.qml) work.
#include "../MacBridge.h"
#include "veyra/Log.h"

#include <QAudioDevice>
#include <QAudioInput>
#include <QAudioOutput>
#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QCapturableWindow>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QScreen>
#include <QScreenCapture>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>
#include <QWindowCapture>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kAudioDisabled = -1;

QString pixelName(QVideoFrameFormat::PixelFormat f) {
    switch (f) {
    case QVideoFrameFormat::Format_NV12: return QStringLiteral("NV12");
    case QVideoFrameFormat::Format_P010: return QStringLiteral("P010");
    case QVideoFrameFormat::Format_YUYV: return QStringLiteral("YUY2");
    case QVideoFrameFormat::Format_UYVY: return QStringLiteral("UYVY");
    case QVideoFrameFormat::Format_Jpeg: return QStringLiteral("MJPEG");
    case QVideoFrameFormat::Format_BGRA8888:
    case QVideoFrameFormat::Format_BGRX8888: return QStringLiteral("BGRA");
    case QVideoFrameFormat::Format_ARGB8888:
    case QVideoFrameFormat::Format_XRGB8888: return QStringLiteral("ARGB");
    case QVideoFrameFormat::Format_YUV420P: return QStringLiteral("I420");
    default: break;
    }
    const QString s = QVideoFrameFormat::pixelFormatToString(f);
    return s.isEmpty() ? QStringLiteral("?") : s;
}

QString fpsText(double fps) {
    const double r = std::round(fps * 100.0) / 100.0;
    return std::abs(r - std::round(r)) < 0.005 ? QString::number(int(std::round(r))) : QString::number(r, 'f', 2);
}

QSettings& capSettings() {
    static QSettings s;
    return s;
}
} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
void MacBridge::initCapture() {
    QSettings& s = capSettings();
    m_captureDeviceId = s.value(QStringLiteral("capture/device")).toString();
    m_captureResolutionId = s.value(QStringLiteral("capture/resolution")).toString();
    m_captureFrameRateId = s.value(QStringLiteral("capture/rate")).toString();
    m_captureFormatKey = s.value(QStringLiteral("capture/format")).toString();
    m_captureAudioChoice = s.value(QStringLiteral("capture/audio"), kAudioDisabled).toInt();
    m_captureFlipVertical = s.value(QStringLiteral("capture/flip"), false).toBool();
    capScreenOptions_ = QVariantMap{{"kind", 1}, {"method", 0}, {"fps", 0}, {"cursor", true}, {"fill", false},
                                    {"left", 0}, {"top", 0}, {"right", 0}, {"bottom", 0}, {"passthrough", false}};
    const QVariantMap saved = s.value(QStringLiteral("screen/options")).toMap();
    for (auto it = saved.begin(); it != saved.end(); ++it)
        if (capScreenOptions_.contains(it.key())) capScreenOptions_[it.key()] = it.value();
    m_screenTargetId = s.value(QStringLiteral("screen/target")).toString();
    m_recordingState = QVariantMap{{"active", false}, {"finishing", false}, {"seconds", 0}, {"failed", false}};
    m_recording = false;
    // Remote play, virtual camera and replay buffer are Windows-only components.
    m_virtualCameraEnabled = false;
    m_virtualCameraState = QVariantMap{{"active", false}, {"installed", false}, {"available", false}};
    m_replayEnabled = false;
    m_captureVirtualAudioInstalled = false;

    // Another source is opening: our capture stops (unless we asked for it).
    connect(this, &MacBridgeBase::mediaOpenRequested, this, [this] {
        if (capSelfOpening_) return;
        if (!capActiveKind_.isEmpty()) capStop(false);
    });
    auto* devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::videoInputsChanged, this, [this] { refreshCaptureDevices(); });
    connect(devices, &QMediaDevices::audioInputsChanged, this, [this] { refreshMicrophones(); emit captureChanged(); });
    refreshMicrophones();
}

void MacBridge::attachCapture(QQuickWindow*) {
    if (!m_captureDeviceId.isEmpty()) capLoadFormats();
    // Test hook: VEYRA_TEST_NAVIGATE=capture|screen opens that dialog after start-up.
    const QString testDialog = qEnvironmentVariable("VEYRA_TEST_NAVIGATE");
    if (!testDialog.isEmpty()) QTimer::singleShot(900, this, [this, testDialog] {
        if (testDialog == QLatin1String("screen")) openScreenCaptureDialog();
        else if (testDialog == QLatin1String("capture")) openCaptureDialog();
        else if (testDialog == QLatin1String("startscreen")) { emit navigate(QStringLiteral("min")); refreshCaptureTargets(); startScreenCapture(false); }
        else if (testDialog == QLatin1String("startcapture")) startCaptureSession();
        else if (testDialog == QLatin1String("record")) {
            refreshCaptureTargets(); startScreenCapture(false);
            QTimer::singleShot(800, this, [this] { startRecording(); });
            QTimer::singleShot(3000, this, [this] { stopRecording(); });
        }
        else emit navigate(testDialog);
    });
    m_hasCaptureSession = !capSettings().value(QStringLiteral("capture/lastDevice")).toString().isEmpty();
}

QVariant MacBridge::capPref(const char* key, const QVariant& fallback) const {
    const QVariant v = preferences().value(QLatin1String(key));
    return v.isValid() ? v : fallback;
}

void MacBridge::capPersist() {
    QSettings& s = capSettings();
    s.setValue(QStringLiteral("capture/device"), m_captureDeviceId);
    s.setValue(QStringLiteral("capture/resolution"), m_captureResolutionId);
    s.setValue(QStringLiteral("capture/rate"), m_captureFrameRateId);
    s.setValue(QStringLiteral("capture/format"), m_captureFormatKey);
    s.setValue(QStringLiteral("capture/audio"), m_captureAudioChoice);
    s.setValue(QStringLiteral("capture/flip"), m_captureFlipVertical);
}

// ---------------------------------------------------------------------------
// capture card / camera
// ---------------------------------------------------------------------------
QVariantList MacBridge::captureDevices() const {
    QVariantList out;
    for (const QCameraDevice& d : QMediaDevices::videoInputs())
        out << QVariantMap{{"id", QString::fromUtf8(d.id())}, {"label", d.description()}, {"embeddedAudio", false}};
    return out;
}

QString MacBridge::captureDeviceLabel() const {
    for (const QCameraDevice& d : QMediaDevices::videoInputs())
        if (QString::fromUtf8(d.id()) == m_captureDeviceId) return d.description();
    return m_captureDeviceId.isEmpty() ? macText("No device selected") : macText("Device not connected");
}

void MacBridge::refreshCaptureDevices() {
    capLoadFormats();
    emit captureChanged();
}

void MacBridge::capLoadFormats() {
    capFormats_.clear();
    for (const QCameraDevice& d : QMediaDevices::videoInputs()) {
        if (QString::fromUtf8(d.id()) != m_captureDeviceId) continue;
        const auto formats = d.videoFormats();
        for (int i = 0; i < formats.size(); ++i) {
            const QCameraFormat& f = formats.at(i);
            CapFormat c;
            c.index = i;
            c.width = f.resolution().width();
            c.height = f.resolution().height();
            c.fps = f.maxFrameRate();
            c.pixel = pixelName(f.pixelFormat());
            c.key = QStringLiteral("%1x%2@%3@%4#%5").arg(c.width).arg(c.height).arg(fpsText(c.fps), c.pixel).arg(i);
            capFormats_ << c;
        }
    }
    // Largest first, then the higher rate — the order the Windows dialog lists.
    std::stable_sort(capFormats_.begin(), capFormats_.end(), [](const CapFormat& a, const CapFormat& b) {
        if (a.width * a.height != b.width * b.height) return a.width * a.height > b.width * b.height;
        return a.fps > b.fps;
    });
    if (!capFormats_.isEmpty() && m_captureResolutionId.isEmpty()) {
        m_captureResolutionId = QStringLiteral("%1x%2").arg(capFormats_.first().width).arg(capFormats_.first().height);
        m_captureFrameRateId = capRateId(capFormats_.first().fps);
    }
}

QString MacBridge::capRateId(double fps) const { return fpsText(fps); }

const MacBridge::CapFormat* MacBridge::capSelectedFormat() const {
    const CapFormat* byKey = nullptr;
    const CapFormat* byChoice = nullptr;
    for (const CapFormat& f : capFormats_) {
        if (!m_captureFormatKey.isEmpty() && f.key == m_captureFormatKey) byKey = &f;
        const QString res = QStringLiteral("%1x%2").arg(f.width).arg(f.height);
        if (!byChoice && res == m_captureResolutionId && capRateId(f.fps) == m_captureFrameRateId) byChoice = &f;
    }
    if (byKey) {
        const QString res = QStringLiteral("%1x%2").arg(byKey->width).arg(byKey->height);
        if (res == m_captureResolutionId && capRateId(byKey->fps) == m_captureFrameRateId) return byKey;
    }
    return byChoice;
}

void MacBridge::setCaptureDeviceId(QString v) {
    if (m_captureDeviceId == v) return;
    m_captureDeviceId = v;
    m_captureResolutionId.clear();
    m_captureFrameRateId.clear();
    m_captureFormatKey.clear();
    m_captureStatus.clear();
    capLoadFormats();
    capPersist();
    emit captureChanged();
}

QVariantList MacBridge::captureFormats() const {
    QVariantList out;
    for (const CapFormat& f : capFormats_) {
        const bool mjpeg = f.pixel == QLatin1String("MJPEG");
        out << QVariantMap{{"id", f.key},
                           {"label", QStringLiteral("%1×%2 · %3 fps · %4").arg(f.width).arg(f.height).arg(fpsText(f.fps), f.pixel)},
                           {"tier", 0}, {"costHint", false}, {"driverCurrent", false},
                           {"width", f.width}, {"height", f.height}, {"fps", f.fps}, {"pixel", f.pixel},
                           {"scan", QString()}, {"interlaced", false}, {"disabled", false},
                           {"rangeCandidate", false}, {"danger", mjpeg}};
    }
    return out;
}

QString MacBridge::captureFormatLabel() const {
    if (const CapFormat* f = capSelectedFormat())
        return QStringLiteral("%1×%2 · %3 fps · %4").arg(f->width).arg(f->height).arg(fpsText(f->fps), f->pixel);
    return {};
}

void MacBridge::setCaptureFormatKey(QString v) {
    for (const CapFormat& f : capFormats_) {
        if (f.key != v) continue;
        m_captureFormatKey = v;
        m_captureResolutionId = QStringLiteral("%1x%2").arg(f.width).arg(f.height);
        m_captureFrameRateId = capRateId(f.fps);
        m_captureStatus.clear();
        capPersist();
        emit captureChanged();
        return;
    }
    m_captureStatus = macText("This device does not offer that format; choose resolution, frame rate and pixel format again.");
    emit notice(m_captureStatus, true);
    emit captureChanged();
}

QVariantList MacBridge::captureResolutionOptions() const {
    QVariantList out;
    QStringList seen;
    for (const CapFormat& f : capFormats_) {
        const QString id = QStringLiteral("%1x%2").arg(f.width).arg(f.height);
        if (seen.contains(id)) continue;
        seen << id;
        out << QVariantMap{{"id", id}, {"label", QStringLiteral("%1×%2").arg(f.width).arg(f.height)}, {"disabled", false}, {"danger", false}};
    }
    return out;
}

void MacBridge::setCaptureResolutionId(QString v) {
    QString id = v.trimmed().toLower();
    id.remove(QLatin1Char(' '));
    id.replace(QChar(0x00D7), QLatin1Char('x'));
    if (m_captureResolutionId == id) return;
    const CapFormat* best = nullptr;
    for (const CapFormat& f : capFormats_)
        if (QStringLiteral("%1x%2").arg(f.width).arg(f.height) == id && (!best || f.fps > best->fps)) best = &f;
    if (!best) {
        emit notice(macText("This device does not offer that format; choose resolution, frame rate and pixel format again."), true);
        return;
    }
    m_captureResolutionId = id;
    bool rateOk = false;
    for (const CapFormat& f : capFormats_)
        if (QStringLiteral("%1x%2").arg(f.width).arg(f.height) == id && capRateId(f.fps) == m_captureFrameRateId) rateOk = true;
    if (!rateOk) m_captureFrameRateId = capRateId(best->fps);
    m_captureFormatKey.clear();
    capPersist();
    emit captureChanged();
}

QVariantList MacBridge::captureFrameRateOptions() const {
    QVariantList out;
    QStringList seen;
    for (const CapFormat& f : capFormats_) {
        if (QStringLiteral("%1x%2").arg(f.width).arg(f.height) != m_captureResolutionId) continue;
        const QString id = capRateId(f.fps);
        if (seen.contains(id)) continue;
        seen << id;
        out << QVariantMap{{"id", id}, {"label", id + QStringLiteral(" fps")}, {"disabled", false}, {"danger", false}};
    }
    return out;
}

void MacBridge::setCaptureFrameRateId(QString v) {
    if (m_captureResolutionId.isEmpty()) { emit notice(macText("Choose a resolution first"), true); return; }
    if (m_captureFrameRateId == v) return;
    m_captureFrameRateId = v;
    m_captureFormatKey.clear();
    capPersist();
    emit captureChanged();
}

QVariantList MacBridge::capturePixelOptions() const {
    QVariantList out;
    for (const CapFormat& f : capFormats_) {
        if (QStringLiteral("%1x%2").arg(f.width).arg(f.height) != m_captureResolutionId || capRateId(f.fps) != m_captureFrameRateId)
            continue;
        const bool mjpeg = f.pixel == QLatin1String("MJPEG");
        out << QVariantMap{{"id", f.key}, {"label", f.pixel}, {"disabled", false}, {"danger", mjpeg}};
    }
    return out;
}

QString MacBridge::capturePixelId() const {
    const CapFormat* f = capSelectedFormat();
    return f ? f->key : QString();
}

void MacBridge::setCapturePixelId(QString v) {
    if (m_captureResolutionId.isEmpty()) { emit notice(macText("Choose a resolution first"), true); return; }
    setCaptureFormatKey(v);
}

QVariantList MacBridge::captureAudioInputs() const {
    QVariantList out{QVariantMap{{"id", kAudioDisabled}, {"label", macText("Do not capture audio")}}};
    const auto inputs = QMediaDevices::audioInputs();
    for (int i = 0; i < inputs.size(); ++i)
        out << QVariantMap{{"id", i}, {"label", inputs.at(i).description()}};
    return out;
}

void MacBridge::setCaptureAudioChoice(int v) {
    if (v < kAudioDisabled || v >= QMediaDevices::audioInputs().size()) return;
    if (m_captureAudioChoice == v) return;
    m_captureAudioChoice = v;
    capPersist();
    emit captureChanged();
}

void MacBridge::setCaptureFlipVertical(bool v) {
    if (m_captureFlipVertical == v) return;
    m_captureFlipVertical = v;
    capPersist();
    emit settingsChanged();
}

void MacBridge::openCaptureDialog() {
    refreshCaptureDevices();
    emit navigate(QStringLiteral("capture"));
}

bool MacBridge::capEnsureSession() {
    if (!capSession_) capSession_ = new QMediaCaptureSession(this);
    QVideoSink* sink = videoSink();
    if (!sink) { emit notice(macText("The video view is not ready yet"), true); return false; }
    capSession_->setVideoSink(sink);
    return true;
}

void MacBridge::capAttachAudio(bool) {
    if (!capSession_) return;
    const auto inputs = QMediaDevices::audioInputs();
    const int choice = m_captureAudioChoice;
    if (capActiveKind_ == QLatin1String("capture") && choice >= 0 && choice < inputs.size()) {
        if (!capAudioIn_) capAudioIn_ = new QAudioInput(this);
        capAudioIn_->setDevice(inputs.at(choice));
        capSession_->setAudioInput(capAudioIn_);
        // Local monitoring (设置 → 本机音频监听, pref captureAudioMonitor) is on unless switched off.
        const bool monitor = capPref("captureAudioMonitor", true).toBool();
        if (monitor) {
            if (!capMonitor_) capMonitor_ = new QAudioOutput(this);
            capMonitor_->setVolume(float(m_volume > 0 ? m_volume : 1.0));
            capMonitor_->setMuted(m_muted);
            capSession_->setAudioOutput(capMonitor_);
        } else {
            capSession_->setAudioOutput(nullptr);
        }
        return;
    }
    capSession_->setAudioOutput(nullptr);
    // No source audio: a chosen microphone can still be recorded.
    if (capPref("recordMic", false).toBool()) {
        const QString micId = capPref("recordMicDevice", QString()).toString();
        QAudioDevice mic = QMediaDevices::defaultAudioInput();
        for (const QAudioDevice& d : inputs) if (QString::fromUtf8(d.id()) == micId) mic = d;
        if (!mic.isNull()) {
            if (!capAudioIn_) capAudioIn_ = new QAudioInput(this);
            capAudioIn_->setDevice(mic);
            capSession_->setAudioInput(capAudioIn_);
            return;
        }
    }
    capSession_->setAudioInput(nullptr);
}

void MacBridge::capWatchFirstFrame(const QString& kind, const QString& name) {
    QVideoSink* sink = videoSink();
    if (!sink) return;
    disconnect(capFrameConn_);
    capFrameConn_ = connect(sink, &QVideoSink::videoFrameChanged, this, [this, kind, name](const QVideoFrame& frame) {
        if (!frame.isValid() || capActiveKind_ != kind) return;
        disconnect(capFrameConn_);
        const QSize s = frame.size();
        double fps = 0;
        if (kind == QLatin1String("capture")) {
            if (const CapFormat* f = capSelectedFormat()) fps = f->fps;
        }
        m_liveOpeningText.clear();
        setSourceOpen(kind, name, s.width(), s.height(), fps);
        m_captureStatus.clear();
        emit captureChanged();
        veyra::log::info("capture", QStringLiteral("%1 first frame %2x%3").arg(kind).arg(s.width()).arg(s.height()).toStdString());
    });
}

bool MacBridge::startCaptureSession() {
    QCameraDevice device;
    for (const QCameraDevice& d : QMediaDevices::videoInputs())
        if (QString::fromUtf8(d.id()) == m_captureDeviceId) device = d;
    if (device.isNull()) { emit notice(macText("Choose a capture device and format first"), true); return false; }
    if (capFormats_.isEmpty()) capLoadFormats();
    const CapFormat* format = capSelectedFormat();
    if (!format && !capFormats_.isEmpty()) {
        emit notice(macText("Choose a resolution and pixel format first"), true);
        return false;
    }
    capSelfOpening_ = true;
    if (!capActiveKind_.isEmpty()) capStop(false);
    beginOpening(device.description());
    capSelfOpening_ = false;
    if (!capEnsureSession()) { clearSource(); return false; }

    if (!capCamera_) {
        capCamera_ = new QCamera(this);
        connect(capCamera_, &QCamera::errorOccurred, this, [this](QCamera::Error, const QString& text) {
            m_captureStatus = text.isEmpty() ? macText("The capture device could not be opened") : text;
            emit notice(m_captureStatus, true);
            if (capActiveKind_ == QLatin1String("capture")) capStop(true);
            emit captureChanged();
        });
    }
    capCamera_->setCameraDevice(device);
    if (format && format->index >= 0 && format->index < device.videoFormats().size())
        capCamera_->setCameraFormat(device.videoFormats().at(format->index));
    capSession_->setScreenCapture(nullptr);
    capSession_->setWindowCapture(nullptr);
    capSession_->setCamera(capCamera_);
    capActiveKind_ = QStringLiteral("capture");
    capActiveLabel_ = device.description();
    capAttachAudio(false);
    capWatchFirstFrame(capActiveKind_, capActiveLabel_);
    capCamera_->start();

    QSettings& s = capSettings();
    s.setValue(QStringLiteral("capture/lastDevice"), m_captureDeviceId);
    s.setValue(QStringLiteral("capture/lastDeviceName"), device.description());
    s.setValue(QStringLiteral("capture/lastFormatLabel"), captureFormatLabel());
    capPersist();
    m_hasCaptureSession = true;
    m_captureStatus = macText("Connecting…");
    m_liveOpeningText = m_captureStatus;
    m_captureSessionSummary = captureSessionSummary();
    emit captureChanged();
    veyra::log::info("capture", QStringLiteral("start device=%1 format=%2").arg(device.description(), captureFormatLabel()).toStdString());
    return true;
}

void MacBridge::resumeCaptureSession() {
    const QString last = capSettings().value(QStringLiteral("capture/lastDevice")).toString();
    if (last.isEmpty()) { emit notice(macText("No capture session yet"), true); return; }
    if (m_captureDeviceId != last) {
        m_captureDeviceId = last;
        capLoadFormats();
    }
    startCaptureSession();
}

bool MacBridge::hasCaptureSession() const {
    return !capSettings().value(QStringLiteral("capture/lastDevice")).toString().isEmpty();
}

QString MacBridge::captureSessionSummary() const {
    QStringList parts;
    const QString name = capSettings().value(QStringLiteral("capture/lastDeviceName")).toString();
    if (!name.isEmpty()) parts << name;
    const QString format = capSettings().value(QStringLiteral("capture/lastFormatLabel")).toString();
    if (!format.isEmpty()) parts << format.section(QStringLiteral(" · "), 0, 1);
    return parts.join(QStringLiteral(" · "));
}

bool MacBridge::openCaptureDriverSettings(bool) {
    emit notice(macText("Driver settings dialogs are a Windows feature; on macOS use the capture device's own app."), true);
    return false;
}
void MacBridge::setupCaptureVirtualAudio(bool) {
    emit notice(macText("The virtual audio driver (VB-CABLE) is Windows-only. On macOS pick the capture device's audio input."), true);
}
void MacBridge::openCaptureWindowsListen() {
    emit notice(macText("On macOS, choose the capture audio input here; Veyra plays it through the Mac's output."), false);
}
void MacBridge::openCaptureVirtualAudioVendor() { setupCaptureVirtualAudio(false); }

void MacBridge::capStop(bool clear) {
    if (capRecorder_ && capRecorder_->recorderState() != QMediaRecorder::StoppedState) capRecorder_->stop();
    disconnect(capFrameConn_);
    if (capCamera_) capCamera_->stop();
    if (capScreen_) capScreen_->setActive(false);
    if (capWindow_) capWindow_->setActive(false);
    if (capSession_) {
        capSession_->setCamera(nullptr);
        capSession_->setScreenCapture(nullptr);
        capSession_->setWindowCapture(nullptr);
        capSession_->setAudioInput(nullptr);
        capSession_->setAudioOutput(nullptr);
        capSession_->setVideoSink(nullptr);
    }
    const bool wasActive = !capActiveKind_.isEmpty();
    if (!m_liveOpeningText.isEmpty()) { m_liveOpeningText.clear(); emit snapshotChanged(); }
    capActiveKind_.clear();
    capActiveLabel_.clear();
    if (clear && wasActive && (m_sourceKind == QLatin1String("capture") || m_sourceKind == QLatin1String("screen")))
        clearSource();
}

// ---------------------------------------------------------------------------
// screen / window capture
// ---------------------------------------------------------------------------
QVariantList MacBridge::screenTargets() const {
    QVariantList out;
    const int kind = capScreenOptions_.value(QStringLiteral("kind")).toInt();
    if (kind == 1) {
        const auto screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            const QScreen* s = screens.at(i);
            const QSize px = s->size() * s->devicePixelRatio();
            QString label = s->name().isEmpty() ? macText("Display %1").arg(i + 1) : s->name();
            label += QStringLiteral(" · %1x%2").arg(px.width()).arg(px.height());
            if (s->refreshRate() > 0) label += QStringLiteral(" · %1Hz").arg(int(std::round(s->refreshRate())));
            out << QVariantMap{{"id", QStringLiteral("screen:%1").arg(i)}, {"label", label}, {"width", px.width()},
                               {"height", px.height()}, {"otherDesktop", false}, {"desktop", 0}};
        }
    } else {
        const auto windows = QWindowCapture::capturableWindows();
        for (int i = 0; i < windows.size(); ++i) {
            const QString title = windows.at(i).description().trimmed();
            if (title.isEmpty()) continue;
            out << QVariantMap{{"id", QStringLiteral("window:%1:%2").arg(i).arg(title)}, {"label", title}, {"width", 0},
                               {"height", 0}, {"otherDesktop", false}, {"desktop", 0}};
        }
    }
    return out;
}

QString MacBridge::screenTargetLabel() const {
    for (const QVariant& v : screenTargets()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("id")).toString() == m_screenTargetId) return m.value(QStringLiteral("label")).toString();
    }
    return m_screenTargetId.isEmpty() ? QString() : macText("Target not found");
}

QVariantMap MacBridge::screenOptions() const {
    QVariantMap d = capScreenOptions_;
    d[QStringLiteral("method")] = 0;   // one method on macOS: ScreenCaptureKit / CoreGraphics
    return d;
}

bool MacBridge::setScreenOption(const QString& key, const QVariant& value) {
    static const QHash<QString, std::pair<int, int>> ranges{{"kind", {0, 1}}, {"method", {0, 1}}, {"fps", {0, 5}},
        {"left", {0, 16384}}, {"top", {0, 16384}}, {"right", {0, 16384}}, {"bottom", {0, 16384}}};
    if (ranges.contains(key)) {
        bool ok = false;
        const int n = value.toInt(&ok);
        const auto [low, high] = ranges.value(key);
        if (!ok || n < low || n > high) { emit notice(macText("Value out of range"), true); return false; }
        capScreenOptions_[key] = n;
    } else if (key == QLatin1String("cursor") || key == QLatin1String("fill") || key == QLatin1String("passthrough")) {
        if (key == QLatin1String("passthrough") && value.toBool()) {
            emit notice(macText("Game mode (click-through) is Windows-only."), true);
            return false;
        }
        capScreenOptions_[key] = value.toBool();
    } else {
        return false;
    }
    capSettings().setValue(QStringLiteral("screen/options"), capScreenOptions_);
    if (key == QLatin1String("kind")) { m_screenTargetId.clear(); refreshCaptureTargets(); }
    emit captureChanged();
    return true;
}

void MacBridge::refreshCaptureTargets() {
    const QVariantList targets = screenTargets();
    bool found = false;
    for (const QVariant& v : targets) found = found || v.toMap().value(QStringLiteral("id")).toString() == m_screenTargetId;
    if (!found) m_screenTargetId = targets.isEmpty() ? QString() : targets.first().toMap().value(QStringLiteral("id")).toString();
    emit captureChanged();
}

void MacBridge::openScreenCaptureDialog() {
    refreshCaptureTargets();
    emit navigate(QStringLiteral("screen"));
}

bool MacBridge::startScreenCapture(bool cropPreview) {
    if (m_screenTargetId.isEmpty()) { emit notice(macText("Choose a capture target first"), true); return false; }
    const QString label = screenTargetLabel();
    capSelfOpening_ = true;
    if (!capActiveKind_.isEmpty()) capStop(false);
    beginOpening(label);
    capSelfOpening_ = false;
    if (!capEnsureSession()) { clearSource(); return false; }
    capSession_->setCamera(nullptr);
    capSession_->setScreenCapture(nullptr);
    capSession_->setWindowCapture(nullptr);

    auto fail = [this](const QString& text) {
        emit notice(text.isEmpty() ? macText("Screen capture failed. Allow Veyra in System Preferences → Security & Privacy → Screen Recording, then reopen it.") : text, true);
        if (capActiveKind_ == QLatin1String("screen")) capStop(true);
    };
    if (m_screenTargetId.startsWith(QLatin1String("screen:"))) {
        const int index = m_screenTargetId.section(QLatin1Char(':'), 1, 1).toInt();
        const auto screens = QGuiApplication::screens();
        if (index < 0 || index >= screens.size()) { clearSource(); emit notice(macText("Target not found"), true); return false; }
        if (!capScreen_) {
            capScreen_ = new QScreenCapture(this);
            connect(capScreen_, &QScreenCapture::errorOccurred, this, [fail](QScreenCapture::Error, const QString& t) { fail(t); });
        }
        capScreen_->setScreen(screens.at(index));
        capSession_->setScreenCapture(capScreen_);
        capActiveKind_ = QStringLiteral("screen");
        capActiveLabel_ = label;
        capWatchFirstFrame(capActiveKind_, label);
        capScreen_->setActive(true);
    } else {
        const int index = m_screenTargetId.section(QLatin1Char(':'), 1, 1).toInt();
        const QString title = m_screenTargetId.section(QLatin1Char(':'), 2);
        const auto windows = QWindowCapture::capturableWindows();
        QCapturableWindow target;
        if (index >= 0 && index < windows.size() && windows.at(index).description().trimmed() == title) target = windows.at(index);
        else for (const auto& w : windows) if (w.description().trimmed() == title) { target = w; break; }
        if (!target.isValid()) { clearSource(); emit notice(macText("The window is no longer available; refresh the list"), true); return false; }
        if (!capWindow_) {
            capWindow_ = new QWindowCapture(this);
            connect(capWindow_, &QWindowCapture::errorOccurred, this, [fail](QWindowCapture::Error, const QString& t) { fail(t); });
        }
        capWindow_->setWindow(target);
        capSession_->setWindowCapture(capWindow_);
        capActiveKind_ = QStringLiteral("screen");
        capActiveLabel_ = label;
        capWatchFirstFrame(capActiveKind_, label);
        capWindow_->setActive(true);
    }
    capAttachAudio(false);
    capSettings().setValue(QStringLiteral("screen/target"), m_screenTargetId);
    m_liveOpeningText = macText("Connecting…");
    if (cropPreview) emit notice(macText("Cropping is applied by the picture view on macOS."), false);
    veyra::log::info("screen", QStringLiteral("start target=%1 label=%2").arg(m_screenTargetId, label).toStdString());
    emit captureChanged();
    return true;
}

bool MacBridge::applyScreenCrop(int left, int top, int right, int bottom) {
    capScreenOptions_[QStringLiteral("left")] = left;
    capScreenOptions_[QStringLiteral("top")] = top;
    capScreenOptions_[QStringLiteral("right")] = right;
    capScreenOptions_[QStringLiteral("bottom")] = bottom;
    capSettings().setValue(QStringLiteral("screen/options"), capScreenOptions_);
    emit captureChanged();
    return true;
}

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------
QString MacBridge::recordingDirectory() const {
    const QString chosen = capPref("recordDir", QString()).toString();
    if (!chosen.isEmpty()) return chosen;
    const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return QDir(movies.isEmpty() ? QDir::homePath() : movies).filePath(QStringLiteral("Veyra"));
}
int MacBridge::recordingBitrateMbps() const { return std::clamp(capPref("recordBitrateMbps", 0).toInt(), 0, 400); }
bool MacBridge::recordingHevc() const { return capPref("recordHevc", false).toBool(); }
bool MacBridge::recordingAudio() const { return capPref("recordAudio", true).toBool(); }

QVariantList MacBridge::microphoneDevices() const { return m_microphoneDevices; }
void MacBridge::refreshMicrophones() {
    QVariantList list;
    for (const QAudioDevice& d : QMediaDevices::audioInputs())
        list << QVariantMap{{"id", QString::fromUtf8(d.id())}, {"label", d.description()}};
    m_microphoneDevices = list;
    emit microphoneDevicesChanged();
}

void MacBridge::recUpdate() {
    if (!capRecorder_) return;
    const bool active = capRecorder_->recorderState() == QMediaRecorder::RecordingState;
    m_recordingState[QStringLiteral("active")] = active;
    m_recordingState[QStringLiteral("seconds")] = int(capRecorder_->duration() / 1000);
    m_recordingState[QStringLiteral("path")] = capRecorder_->actualLocation().toLocalFile();
    m_recording = active;
    emit recordingChanged();
}

void MacBridge::startRecording(const QString&) {
    if (m_recording) { emit notice(macText("Recording is already running"), true); return; }
    if (!m_hasSource) { emit notice(macText("Nothing is playing, so there is nothing to record"), true); return; }
    if (capActiveKind_.isEmpty() || !capSession_) {
        emit notice(macText("On macOS, recording works for capture card, camera and screen sources. Use Export for files."), true);
        return;
    }
    const QString dir = recordingDirectory();
    if (!QDir().mkpath(dir)) { emit notice(macText("Cannot create the recording folder: %1").arg(dir), true); return; }
    if (!capRecorder_) {
        capRecorder_ = new QMediaRecorder(this);
        capRecTimer_ = new QTimer(this);
        capRecTimer_->setInterval(500);
        connect(capRecTimer_, &QTimer::timeout, this, &MacBridge::recUpdate);
        connect(capRecorder_, &QMediaRecorder::recorderStateChanged, this, [this](QMediaRecorder::RecorderState s) {
            recUpdate();
            if (s == QMediaRecorder::StoppedState) {
                capRecTimer_->stop();
                m_recordingState[QStringLiteral("finishing")] = false;
                const QString path = capRecorder_->actualLocation().toLocalFile();
                if (!m_recordingState.value(QStringLiteral("failed")).toBool() && !path.isEmpty())
                    emit notice(macText("Recording saved: %1").arg(path), false);
                emit recordingChanged();
            }
        });
        connect(capRecorder_, &QMediaRecorder::errorOccurred, this, [this](QMediaRecorder::Error, const QString& text) {
            m_recordingState[QStringLiteral("failed")] = true;
            m_recordingState[QStringLiteral("error")] = text;
            emit notice(macText("Recording failed: %1").arg(text), true);
            recUpdate();
        });
    }
    capSession_->setRecorder(capRecorder_);
    // Audio follows 设置 → 录制; the capture session's audio input is what gets recorded.
    if (!recordingAudio()) capSession_->setAudioInput(nullptr);
    else capAttachAudio(true);
    QMediaFormat format(QMediaFormat::MPEG4);
    format.setVideoCodec(recordingHevc() ? QMediaFormat::VideoCodec::H265 : QMediaFormat::VideoCodec::H264);
    format.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    capRecorder_->setMediaFormat(format);
    capRecorder_->setQuality(QMediaRecorder::HighQuality);
    if (recordingBitrateMbps() > 0) {
        capRecorder_->setEncodingMode(QMediaRecorder::AverageBitRateEncoding);
        capRecorder_->setVideoBitRate(recordingBitrateMbps() * 1000 * 1000);
    } else {
        capRecorder_->setEncodingMode(QMediaRecorder::ConstantQualityEncoding);
    }
    const int fpsLimit = std::clamp(capPref("recordFps", 0).toInt(), 0, 240);
    if (fpsLimit > 0) capRecorder_->setVideoFrameRate(fpsLimit);
    const QString name = QStringLiteral("Veyra_%1.mp4").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    capRecorder_->setOutputLocation(QUrl::fromLocalFile(QDir(dir).filePath(name)));
    m_recordingState = QVariantMap{{"active", true}, {"finishing", false}, {"seconds", 0}, {"failed", false},
                                   {"path", QDir(dir).filePath(name)}};
    m_recording = true;
    emit recordingChanged();
    capRecorder_->record();
    capRecTimer_->start();
    veyra::log::info("record", ("start " + QDir(dir).filePath(name)).toStdString());
}

void MacBridge::stopRecording(const QString&) {
    if (!m_recording || !capRecorder_) return;
    m_recordingState[QStringLiteral("finishing")] = true;
    capRecorder_->stop();
    m_recording = false;
    m_recordingState[QStringLiteral("active")] = false;
    emit recordingChanged();
}

void MacBridge::toggleRecording(const QString& source) {
    if (m_recording) stopRecording(source); else startRecording(source);
}

void MacBridge::chooseRecordingDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(nullptr, macText("Choose where recordings are saved"), recordingDirectory());
    if (!dir.isEmpty()) {
        setPreference(QStringLiteral("recordDir"), dir);
        emit recordingChanged();
    }
}

void MacBridge::openRecordingDirectory() {
    const QString dir = recordingDirectory();
    QDir().mkpath(dir);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) emit notice(macText("Cannot open folder: %1").arg(dir), true);
}

// ---------------------------------------------------------------------------
// Windows-only sources and outputs
// ---------------------------------------------------------------------------
void MacBridge::openPs5Dialog() {
    emit notice(macText("PS5 Remote Play is not available in the macOS version. Sony's PS Remote Play app for Mac can be captured with Screen capture."), true);
}
void MacBridge::openMoonlightDialog() {
    emit notice(macText("PC streaming (Moonlight) is not available in the macOS version. Moonlight for Mac can be captured with Screen capture."), true);
}
void MacBridge::openXboxDialog() {
    emit notice(macText("Xbox streaming is not available in the macOS version. Xbox Cloud Gaming in a browser can be captured with Screen capture."), true);
}
void MacBridge::moonlightCapture(bool) { openMoonlightDialog(); }
bool MacBridge::ps5Connect() { openPs5Dialog(); return false; }
void MacBridge::setVirtualCameraEnabled(bool enabled) {
    if (enabled) emit notice(macText("The Veyra virtual camera is Windows-only."), true);
}
void MacBridge::installVirtualCamera() { setVirtualCameraEnabled(true); }
void MacBridge::uninstallVirtualCamera() {}
void MacBridge::saveReplay(const QString&) {
    emit notice(macText("The instant replay buffer is Windows-only. Use Record on capture or screen sources."), true);
}
