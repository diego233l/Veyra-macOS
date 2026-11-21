// PS5 Remote Play for the macOS bridge — port of Veyra's src/ui/bridge/RemotePlay.cpp.
// Same data shapes and the same (Chinese source, catalog-translated) messages as Windows.
#include "../MacBridge.h"
#include "veyra/Log.h"
#include "veyra/ui/UiLanguage.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QQuickItem>
#include <functional>

// The minimal page (where PS5, capture and screen sources open) paints an opaque
// black plate under the picture: on Windows the native video window sat above it.
// The core places the picture inside an item named "videoArea", so the plate gets
// that name: the picture then draws on the plate and under the control pill.
bool MacBridge::tagMinimalStage(QQuickWindow* window) {
    if (!window) return false;
    std::function<QQuickItem*(QQuickItem*)> find = [&](QQuickItem* item) -> QQuickItem* {
        for (QQuickItem* c : item->childItems()) {
            if (c->objectName() == QLatin1String("videoInset")) return c;
            if (QQuickItem* f = find(c)) return f;
        }
        return nullptr;
    };
    QQuickItem* inset = find(window->contentItem());
    if (!inset || !inset->parentItem()) return false;
    for (QQuickItem* c : inset->parentItem()->childItems()) {
        if (c->objectName() == QLatin1String("videoArea")) return true;
        if (c->objectName().isEmpty() && qstrcmp(c->metaObject()->className(), "QQuickRectangle") == 0) {
            c->setObjectName(QStringLiteral("videoArea"));
            veyra::log::info("mac-stage", "minimal page plate tagged as videoArea");
            return true;
        }
    }
    return false;
}

// Pages may finish building after the window exists: retry for a while.
void MacBridge::tagMinimalStageSoon(QQuickWindow* window) {
    auto* retry = new QTimer(this);
    auto attempts = std::make_shared<int>(0);
    connect(retry, &QTimer::timeout, this, [this, window, retry, attempts] {
        if (tagMinimalStage(window) || ++*attempts > 60) { retry->stop(); retry->deleteLater(); }
    });
    retry->start(250);
}

#ifdef VEYRA_ENABLE_REMOTEPLAY
#include "../remoteplay/RemotePlaySession.h"
#include "veyra/remoteplay/ControllerInput.h"
#include "veyra/remoteplay/Discovery.h"
#include "veyra/remoteplay/ProfileStore.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QRandomGenerator>
#include <QSet>
#include <QVideoSink>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace rp = veyra::remoteplay;

namespace {
constexpr uint32_t kPs5Bitrates[] = {5000, 10000, 15000, 20000, 30000, 50000, 80000, 100000};

rp::VideoProfile ps5Video(const QVariantMap& f) {
    rp::VideoProfile p;
    const int q = f.value("quality").toInt();
    p.width = q < 2 ? 1280 : 1920; p.height = q < 2 ? 720 : 1080; p.fps = (q == 0 || q == 2) ? 30 : 60;
    p.codec = static_cast<rp::Codec>(std::clamp(f.value("codec").toInt(), 0, 2));
    p.bitrateKbps = kPs5Bitrates[std::clamp(f.value("bitrate").toInt(), 0, 7)];
    return p;
}
std::string asciiOnly(const QString& s) {
    std::string out;
    for (const QChar c : s) { if (c.unicode() > 127) return {}; out.push_back(char(c.unicode())); }
    return out;
}
std::string fill(const char* pattern, long long n) {
    std::string s = pattern;
    const auto at = s.find("{}");
    if (at != std::string::npos) s.replace(at, 2, std::to_string(n));
    return s;
}
QString tx(const std::string& chinese) { return veyra::ui::i18n::text(QString::fromStdString(chinese)); }
QString tx(const char* chinese) { return veyra::ui::i18n::text(QString::fromUtf8(chinese)); }
std::filesystem::path profilePath(const QString& id) { return rp::profileDirectory() / id.toStdString(); }
QSettings rpSettings() { return QSettings(); }
} // namespace

// ---------------------------------------------------------------------------

class MacBridge::RpState : public QObject {
public:
    explicit RpState(MacBridge* bridge) : QObject(bridge), bridge_(bridge) {}
    ~RpState() override { if (worker.joinable()) { worker.request_stop(); worker.join(); } session.reset(); }

    struct Outcome {
        std::string message;
        std::vector<std::pair<std::string, std::string>> hosts;
        QString savedPath;
    };

    // Dialog state.
    QVariantMap form;
    QString status;
    QString profile;            // selected pairing file name
    QVariantList profiles;
    bool busy = false;
    std::atomic<bool> done{false};
    std::mutex mutex;
    Outcome outcome;

    // Stream state.
    std::unique_ptr<rp::RemotePlaySession> session;
    QString host;
    bool viewOnly = false;
    bool opened = false;          // first frame shown, source published
    bool selfOpening = false;
    bool watching = false;
    rp::ControllerInput pad;
    bool padStarted = false;
    QAudioSink* audio = nullptr;
    QIODevice* audioDevice = nullptr;
    int audioChannels = 0;
    QTimer* timer = nullptr;
    int ticks = 0;
    QSet<int> keys;
    bool keyboardHinted = false;
    // Declared last: destroyed (stop requested + joined) before everything it writes.
    std::jthread worker;

    bool capturingKeyboard() const {
        if (!session || viewOnly || pad.connected()) return false;
        if (bridge_->m_sourceKind != QLatin1String("ps5")) return false;
        if (QGuiApplication::focusWindow() == nullptr) return false;
        // Typing into a QML text field (login PIN, host...) is never game input.
        if (QObject* focus = QGuiApplication::focusObject()) {
            const char* cls = focus->metaObject()->className();
            if (qstrcmp(cls, "QQuickTextInput") == 0 || qstrcmp(cls, "QQuickTextField") == 0 ||
                qstrcmp(cls, "QQuickTextEdit") == 0 || qstrcmp(cls, "QQuickTextArea") == 0) return false;
            if (focus->inherits("QQuickTextInput") || focus->inherits("QQuickTextEdit")) return false;
        }
        return true;
    }
    static bool mapped(int key) {
        switch (key) {
        case Qt::Key_W: case Qt::Key_A: case Qt::Key_S: case Qt::Key_D:
        case Qt::Key_I: case Qt::Key_J: case Qt::Key_K: case Qt::Key_L:
        case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right:
        case Qt::Key_Space: case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Backspace:
        case Qt::Key_F: case Qt::Key_R: case Qt::Key_Q: case Qt::Key_E: case Qt::Key_Z: case Qt::Key_C:
        case Qt::Key_G: case Qt::Key_H: case Qt::Key_Tab: case Qt::Key_O: case Qt::Key_P: case Qt::Key_T:
            return true;
        default: return false;
        }
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) return QObject::eventFilter(watched, event);
        auto* key = static_cast<QKeyEvent*>(event);
        if (!capturingKeyboard() || !mapped(key->key()) || (key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier)))
            return QObject::eventFilter(watched, event);
        if (!key->isAutoRepeat()) {
            if (event->type() == QEvent::KeyPress) keys.insert(key->key()); else keys.remove(key->key());
        }
        return true;   // the stream owns these keys while it has the keyboard
    }
    rp::ControllerState keyboardState(bool focused) const {
        rp::ControllerState s;
        s.inputActive = focused;
        if (!focused) return s;
        auto has = [&](int k) { return keys.contains(k); };
        using B = rp::ControllerState;
        if (has(Qt::Key_Space) || has(Qt::Key_Return) || has(Qt::Key_Enter)) s.buttons |= B::Cross;
        if (has(Qt::Key_Backspace)) s.buttons |= B::Circle;
        if (has(Qt::Key_F)) s.buttons |= B::Square;
        if (has(Qt::Key_R)) s.buttons |= B::Triangle;
        if (has(Qt::Key_Up)) s.buttons |= B::Up;
        if (has(Qt::Key_Down)) s.buttons |= B::Down;
        if (has(Qt::Key_Left)) s.buttons |= B::Left;
        if (has(Qt::Key_Right)) s.buttons |= B::Right;
        if (has(Qt::Key_Q)) s.buttons |= B::L1;
        if (has(Qt::Key_E)) s.buttons |= B::R1;
        if (has(Qt::Key_G)) s.buttons |= B::L3;
        if (has(Qt::Key_H)) s.buttons |= B::R3;
        if (has(Qt::Key_Tab)) s.buttons |= B::Options;
        if (has(Qt::Key_O)) s.buttons |= B::Share;
        if (has(Qt::Key_P)) s.buttons |= B::PS;
        if (has(Qt::Key_T)) s.buttons |= B::Touchpad;
        if (has(Qt::Key_Z)) s.l2 = 255;
        if (has(Qt::Key_C)) s.r2 = 255;
        auto axis = [&](int neg, int pos) -> int16_t { return int16_t((has(pos) ? 32767 : 0) - (has(neg) ? 32767 : 0)); };
        s.leftX = axis(Qt::Key_A, Qt::Key_D); s.leftY = axis(Qt::Key_W, Qt::Key_S);
        s.rightX = axis(Qt::Key_J, Qt::Key_L); s.rightY = axis(Qt::Key_I, Qt::Key_K);
        return s;
    }

private:
    MacBridge* bridge_;
};

// ---------------------------------------------------------------------------

void MacBridge::initRemotePlay() {
    rp_ = new RpState(this);
    m_ps5Control = QVariantMap{{"available", false}, {"enabled", false}, {"state", "off"}};
    rp_->timer = new QTimer(this);
    rp_->timer->setTimerType(Qt::PreciseTimer);
    connect(rp_->timer, &QTimer::timeout, this, [this] { rpTick(); });
    // Another source opening ends the stream (the console has one Remote Play slot).
    connect(this, &MacBridgeBase::mediaOpenRequested, this, [this] {
        if (rp_->selfOpening || !rp_->session) return;
        rpStopSession(false);
    });
    // Saved host shown on the home tile without opening the dialog.
    QTimer::singleShot(0, this, [this] { ps5Load(); });
}

void MacBridge::attachRemotePlay(QQuickWindow* window) {
    qApp->installEventFilter(rp_);
    tagMinimalStageSoon(window);
    // Test hooks (harmless in normal use):
    //   VEYRA_TEST_PS5=scan|pair-invalid|connect-unpaired   exercise the dialog commands offline
    //   VEYRA_TEST_PS5_STREAM=<file.h264>                    decode an Annex-B stream into the picture
    const QString test = qEnvironmentVariable("VEYRA_TEST_PS5");
    if (test == QLatin1String("scan")) QTimer::singleShot(500, this, [this] { ps5Scan(); });
    else if (test == QLatin1String("pair-invalid")) QTimer::singleShot(500, this, [this] { ps5Pair(QStringLiteral("123")); });
    else if (test == QLatin1String("connect-unpaired")) QTimer::singleShot(500, this, [this] { ps5Connect(); });
    else if (test == QLatin1String("connect-fake")) QTimer::singleShot(500, this, [this] {
        // A dummy pairing to TEST-NET-1 (never routed): exercises connect → failure → recovery offline.
        rp::NativeConnectRequest r; r.host = "192.0.2.10";
        r.credentials.registrationKey = {'1','2','3','4','5','6','7','8'};
        const auto path = rp::profileDirectory() / "0123456789abcdef0123456789abcdef.dat";
        rp::saveProfile(path, r);
        rp_->profile = QStringLiteral("0123456789abcdef0123456789abcdef.dat");
        ps5Load();
        ps5Connect();
    });
    const QString stream = qEnvironmentVariable("VEYRA_TEST_PS5_STREAM");
    if (!stream.isEmpty()) QTimer::singleShot(500, this, [this, stream] {
        auto& i = *rp_;
        i.host = QStringLiteral("test");
        i.viewOnly = true;
        i.selfOpening = true; beginOpening(QStringLiteral("PS5 · test")); i.selfOpening = false;
        i.session = rp::RemotePlaySession::createFileTest(stream.toStdString(), videoSink());
        i.session->start();
        i.timer->start(8);
        emit navigate(qEnvironmentVariable("VEYRA_TEST_PS5_PAGE", QStringLiteral("min")));
    });
}

bool MacBridge::remotePlayActive() const { return rp_ && rp_->session != nullptr; }

void MacBridge::openPs5Dialog() { emit navigate(QStringLiteral("ps5")); }

void MacBridge::rpPublish() {
    QVariantMap out = rp_->form;
    out["status"] = rp_->status;
    out["busy"] = rp_->busy;
    out["profile"] = rp_->profile;
    out["available"] = true;
    out["active"] = rp_->session != nullptr;
    const auto c = rp_->pad.capabilities();
    out["controller"] = c.connected;
    out["gyro"] = c.gyro && c.accel;
    out["calibrating"] = c.calibrating;
    m_ps5 = out;
    m_ps5Profiles = rp_->profiles;
    if (qEnvironmentVariableIsSet("VEYRA_TEST_PS5")) veyra::log::info("remoteplay-test", "status busy=" + std::to_string(rp_->busy) + " " + rp_->status.toStdString());
    emit ps5Changed();
}

void MacBridge::ps5Load() {
    auto& i = *rp_;
    try {
        const auto directory = rp::profileDirectory();
        if (i.profile.isEmpty()) {
            const QString last = rpSettings().value("remoteplay/lastProfile").toString();
            if (!last.isEmpty() && !last.contains('/') && !last.contains(':')) i.profile = last;
        }
        i.profiles.clear();
        unsigned failures = 0;
        QString firstHost;
        std::error_code ec; size_t scanned = 0;
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end && i.profiles.size() < 64 && scanned < 256; it.increment(ec), ++scanned) {
            const auto id = it->path().stem().string();
            if (it->path().extension() != ".dat" || id.size() != 32 ||
                !std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) continue;
            rp::ProfileLoadError error{};
            if (auto saved = rp::loadProfile(it->path(), &error))
                i.profiles << QVariantMap{{"id", QString::fromStdString(it->path().filename().string())}, {"label", QString::fromStdString(saved->host)}};
            else if (error != rp::ProfileLoadError::Missing) ++failures;
        }
        bool known = false;
        for (const auto& p : i.profiles) known = known || p.toMap().value("id").toString() == i.profile;
        if (!known) i.profile = i.profiles.isEmpty() ? QString() : i.profiles.front().toMap().value("id").toString();
        const QSettings settings = rpSettings();
        i.form = QVariantMap{{"host", QString()}, {"account", QString()}, {"quality", 3}, {"codec", 0}, {"bitrate", 2}, {"viewOnly", false},
                             {"decode", std::clamp(settings.value("remoteplay/decodeMode", 0).toInt(), 0, 2)},
                             {"sampling", std::clamp(settings.value("remoteplay/fineSampling", 1).toInt(), 0, 1)}};
        if (!i.profile.isEmpty())
            if (auto saved = rp::loadProfile(profilePath(i.profile))) {
                i.form["host"] = QString::fromStdString(saved->host);
                i.form["account"] = QString::fromStdString(rp::accountIdToBase64(saved->credentials.accountId));
                i.form["quality"] = (saved->video.height == 1080 ? 2 : 0) + (saved->video.fps == 60 ? 1 : 0);
                i.form["codec"] = int(saved->video.codec);
                const auto found = std::find(std::begin(kPs5Bitrates), std::end(kPs5Bitrates), saved->video.bitrateKbps);
                i.form["bitrate"] = found == std::end(kPs5Bitrates) ? 2 : int(found - std::begin(kPs5Bitrates));
                i.form["viewOnly"] = saved->viewOnly;
                firstHost = QString::fromStdString(saved->host);
            }
        if (i.status.isEmpty())
            i.status = failures ? tx("有配对存档无法读取或解密，详情见日志。请使用原 Windows 账户；不要删除原件。")
                     : !i.profile.isEmpty() ? tx("已加载保存的主机，直接点“连接”，无需重新填写 8 位配对码。")
                     : tx("首次使用：PS5 设置 → 系统 → 远程游玩 → 关联设备，填写账号 ID 与 8 位配对码后配对。");
        if (m_remotePlayHost != firstHost) setRemotePlayHost(firstHost);
        veyra::log::info("remoteplay-ui", "profiles=" + std::to_string(i.profiles.size()) + " readFailures=" + std::to_string(failures));
    } catch (...) {
        i.status = tx("无法读取远程游玩目录；原有配对文件不会被改动。");
    }
    rpPublish();
}

bool MacBridge::ps5Set(const QString& key, const QVariant& value) {
    static const QHash<QString, std::pair<int, int>> ranges{{"quality", {0, 3}}, {"codec", {0, 2}}, {"bitrate", {0, 7}}, {"decode", {0, 2}}, {"sampling", {0, 1}}};
    if (ranges.contains(key)) {
        bool ok = false; const int n = value.toInt(&ok); const auto [low, high] = ranges.value(key);
        if (!ok || n < low || n > high) return false;
        rp_->form[key] = n;
    } else if (key == QLatin1String("viewOnly")) rp_->form[key] = value.toBool();
    else if (key == QLatin1String("host") || key == QLatin1String("account")) rp_->form[key] = value.toString().trimmed();
    else return false;
    rpPublish();
    return true;
}

void MacBridge::ps5SelectProfile(const QString& id) {
    if (rp_->busy) return;
    rp_->profile = id;
    QSettings settings = rpSettings();
    settings.setValue("remoteplay/lastProfile", id);
    rp_->status.clear();
    ps5Load();
}

// One worker at a time, as Windows; results are picked up in rpTick().
#define VEYRA_PS5_LAUNCH(body)                                                                              \
    do {                                                                                                    \
        auto& st = *rp_;                                                                                    \
        if (st.busy) return;                                                                                \
        if (st.worker.joinable()) st.worker.join();                                                         \
        st.busy = true; st.done = false; st.status = tx("正在处理…可随时取消"); rpPublish();                \
        if (!st.timer->isActive()) st.timer->start(8);                                                      \
        st.worker = std::jthread([&st, fn = std::move(body)](std::stop_token stop) mutable {                \
            RpState::Outcome result;                                                                        \
            try { result = fn(stop); } catch (...) { result.message = "操作失败；请检查网络、输入和可用磁盘空间。"; } \
            { std::lock_guard lock(st.mutex); st.outcome = std::move(result); }                             \
            st.done = true;                                                                                 \
        });                                                                                                 \
    } while (0)

void MacBridge::ps5Scan() {
    auto work = [](std::stop_token stop) {
        RpState::Outcome out;
        auto report = rp::discoverLocalPs5(stop);
        for (const auto& line : report.diagnostics) veyra::log::info("remoteplay-discovery", line);
        for (auto& h : report.hosts) {
            std::string id = h.consoleId;
            std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            out.hosts.push_back({h.host, id});
        }
        out.message = stop.stop_requested() ? "已取消查找" : out.hosts.empty()
            ? (report.error ? fill("主机搜索发生错误（{}），详情见日志；可手填 IP。", report.error) : std::string("搜索完成，未发现 PS5；确认同一局域网，或手动填写 IP。"))
            : fill("发现 {} 台 PS5。", (long long)out.hosts.size());
        return out;
    };
    VEYRA_PS5_LAUNCH(work);
}

void MacBridge::ps5Pair(const QString& pinText) {
    auto host = asciiOnly(rp_->form.value("host").toString()), id = asciiOnly(rp_->form.value("account").toString()), pin = asciiOnly(pinText.trimmed());
    auto account = rp::accountIdFromBase64(id);
    if (!account) account = rp::accountIdFromDecimal(id);
    if (!rp::validHost(host) || !account || !rp::parsePairingPin(pin)) {
        rp_->status = tx("请填写有效主机地址、Account ID 和 8 位配对码。"); rpPublish(); return;
    }
    // Same host + account reuses its file; otherwise a fresh random name.
    std::filesystem::path target;
    for (const auto& p : rp_->profiles) {
        const auto path = profilePath(p.toMap().value("id").toString());
        if (auto saved = rp::loadProfile(path); saved && saved->host == host && saved->credentials.accountId == *account) target = path;
    }
    if (target.empty()) {
        std::string name;
        for (int k = 0; k < 16; ++k) {
            static const char* hex = "0123456789abcdef";
            const auto b = QRandomGenerator::system()->generate() & 0xff;
            name += hex[b >> 4]; name += hex[b & 15];
        }
        target = rp::profileDirectory() / (name + ".dat");
    }
    const auto format = ps5Video(rp_->form);
    auto work = [host = std::move(host), account = *account, pin = std::move(pin), format, target](std::stop_token stop) mutable {
        auto result = rp::pairLocalPs5(host, account, pin, stop);
        std::fill(pin.begin(), pin.end(), '\0');
        RpState::Outcome out;
        if (result.result.ok && !stop.stop_requested()) {
            rp::NativeConnectRequest request; request.host = host; request.video = format; request.credentials = std::move(result.credentials);
            static const char* hex = "0123456789abcdef";
            for (auto byte : result.mac) { request.consoleId += hex[byte >> 4]; request.consoleId += hex[byte & 15]; }
            if (request.consoleId == "000000000000") request.consoleId.clear();
            if (rp::saveProfile(target, request)) { out.savedPath = QString::fromStdString(target.filename().string()); out.message = "配对成功并已加密保存；现在可以连接。"; }
            else out.message = "配对成功，但保存失败；请检查用户目录权限。";
        } else {
            out.message = result.canceled || stop.stop_requested() ? std::string("已取消配对") : fill("配对失败（{}），检查 PS5 配对码、Account ID 与网络。", result.result.code);
        }
        return out;
    };
    VEYRA_PS5_LAUNCH(work);
}

bool MacBridge::ps5Connect() {
    auto& i = *rp_;
    if (i.busy) return false;
    const auto path = i.profile.isEmpty() ? std::filesystem::path{} : profilePath(i.profile);
    auto saved = path.empty() ? std::nullopt : rp::loadProfile(path);
    const auto host = asciiOnly(i.form.value("host").toString());
    if (!saved || !rp::validHost(host)) { i.status = tx("请先完成配对，并填写有效主机地址。"); rpPublish(); return false; }
    saved->host = host; saved->video = ps5Video(i.form); saved->viewOnly = i.form.value("viewOnly").toBool();
    if (!rp::saveProfile(path, *saved)) { i.status = tx("保存连接设置失败，请检查目录权限。"); rpPublish(); return false; }
    const int decode = std::clamp(i.form.value("decode").toInt(), 0, 2);
    QSettings settings = rpSettings();
    settings.setValue("remoteplay/decodeMode", decode);
    settings.setValue("remoteplay/fineSampling", i.form.value("sampling").toInt());

    rpStopSession(false);
    i.host = QString::fromStdString(host);
    i.viewOnly = saved->viewOnly;
    i.opened = false;
    const QString label = QStringLiteral("PS5 · ") + i.host;
    i.selfOpening = true;
    beginOpening(label);              // other sources (player, capture) let go of the picture
    i.selfOpening = false;
    m_statusText = tx("正在连接 PS5 · %1").replace("%1", i.host);
    emit snapshotChanged();

    i.session = std::make_unique<rp::RemotePlaySession>(std::move(*saved), rp::RemotePlaySession::DecodeMode(decode), videoSink());
    i.session->start();
    if (!i.viewOnly && !i.padStarted) {
        i.padStarted = i.pad.start();
        if (!i.padStarted) veyra::log::warn("remoteplay-input", "SDL gamepad initialization failed");
    }
    i.keys.clear();
    i.keyboardHinted = false;
    // Audio: 48 kHz stereo int16 with a short device buffer (low latency).
    QAudioFormat format; format.setSampleRate(48000); format.setChannelCount(2); format.setSampleFormat(QAudioFormat::Int16);
    i.audio = new QAudioSink(QMediaDevices::defaultAudioOutput(), format, this);
    i.audio->setBufferSize(48000 * 4 * 80 / 1000);
    i.audio->setVolume(m_muted ? 0.0 : std::clamp(m_volume, 0.0, 1.0));
    i.audioDevice = i.audio->start();
    i.watching = true;
    i.timer->start(8);

    // Remembered for 继续上次 (same shape as Windows rememberSource).
    m_lastSource = QVariantMap{{"kind", "ps5"}, {"label", i.host}, {"summary", label}};
    settings.setValue("recent/lastSource", m_lastSource);
    emit recentFilesChanged();

    i.status = tx("连接已开始。需要登录 PIN 时在下方提交。关闭面板不停止串流。");
    veyra::log::info("remoteplay-ui", "connect quality=" + std::to_string(i.form.value("quality").toInt()) + " codec=" + std::to_string(i.form.value("codec").toInt()) +
                     " bitrate=" + std::to_string(i.form.value("bitrate").toInt()) + " decode=" + std::to_string(decode) + " viewOnly=" + (i.viewOnly ? "1" : "0"));
    rpPublish();
    return true;
}

void MacBridge::ps5Wake() {
    auto saved = rp_->profile.isEmpty() ? std::nullopt : rp::loadProfile(profilePath(rp_->profile));
    if (!saved) { rp_->status = tx("需要先配对才能唤醒。"); rpPublish(); return; }
    saved->host = asciiOnly(rp_->form.value("host").toString());
    auto request = std::make_shared<rp::NativeConnectRequest>(std::move(*saved));
    auto work = [request](std::stop_token stop) {
        RpState::Outcome out;
        if (stop.stop_requested()) { out.message = "已取消唤醒"; return out; }
        auto r = rp::wakeLocalPs5(*request);
        out.message = r.ok ? std::string("已发送唤醒请求，等待 PS5 开机后再连接。") : fill("唤醒失败（{}）；确认 PS5 处于待机而不是关机。", r.code);
        return out;
    };
    VEYRA_PS5_LAUNCH(work);
}

void MacBridge::ps5Forget() {
    if (rp_->busy || rp_->profile.isEmpty()) return;
    std::error_code ec;
    const bool removed = std::filesystem::remove(profilePath(rp_->profile), ec);
    rp_->status = removed ? tx("配对已删除。现有串流会保留；下次连接需选择其他主机或重新配对。") : tx("删除失败，请检查目录权限。");
    if (removed) rp_->profile.clear();
    ps5Load();
}

void MacBridge::ps5OpenAccountChecker() {
    const bool opened = QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.psntools.com/psn/checker")));
    rp_->status = opened ? tx("在网页获取 PSN Account ID 后，复制到配对栏，再填写主机的 8 位配对码。") : tx("无法打开浏览器，请检查默认浏览器设置。");
    rpPublish();
}

void MacBridge::ps5SendLoginPin(const QString& pin) {
    const auto text = pin.trimmed();
    if (text.isEmpty() || !std::all_of(text.begin(), text.end(), [](QChar c) { return c.isDigit(); })) {
        rp_->status = tx("登录 PIN 必须为数字。"); rpPublish(); return;
    }
    if (rp_->session) rp_->session->submitLoginPin(text.toStdString());
}

void MacBridge::ps5Cancel() {
    if (rp_->busy && rp_->worker.joinable()) { rp_->worker.request_stop(); return; }
    if (rp_->session) rpStopSession(true);
}

void MacBridge::ps5Calibrate() {
    rp_->status = rp_->pad.calibrate() ? tx("将手柄平放并保持静止：采集 120 个稳定样本完成校准；10 秒内不稳定则保留原校准。")
                                       : tx("请先连接串流及带陀螺仪的电脑手柄。仅观看模式不使用电脑手柄。");
    rpPublish();
}

void MacBridge::ps5ControlSet(const QString&, const QVariant& value) {
    if (value.toBool()) notify(macText("PS5 controller passthrough for a capture card is not available on Mac yet; use PS5 Remote Play instead."), true);
    emit ps5ControlChanged();
}
void MacBridge::ps5ControlRestart() {}

void MacBridge::rpStopSession(bool clear) {
    auto& i = *rp_;
    if (!i.session) return;
    veyra::log::info("remoteplay", "stop session");
    i.session->setController({});
    i.session->stop();
    i.session.reset();
    if (i.audio) { i.audio->stop(); i.audio->deleteLater(); i.audio = nullptr; i.audioDevice = nullptr; }
    if (i.padStarted) { i.pad.releaseEffects(); i.pad.stop(); i.padStarted = false; }
    i.keys.clear();
    i.opened = false;
    if (clear && m_sourceKind == QLatin1String("ps5")) clearSource();
    else if (clear && m_openingSource) clearSource();
    i.status = tx("PS5 串流已停止，可以重新连接。");
    m_remotePlayState.clear();
    rpPublish();
    emit snapshotChanged();
}

void MacBridge::rpTick() {
    auto& i = *rp_;
    bool changed = false;
    // Finished worker (scan / pair / wake).
    if (i.busy && i.done.load()) {
        if (i.worker.joinable()) i.worker.join();
        i.busy = false;
        RpState::Outcome r;
        { std::lock_guard lock(i.mutex); r = std::move(i.outcome); }
        i.status = tx(r.message);
        if (!r.savedPath.isEmpty()) {
            i.profile = r.savedPath;
            QSettings settings = rpSettings();
            settings.setValue("remoteplay/lastProfile", i.profile);
            const auto status = i.status; ps5Load(); i.status = status;
        }
        if (!r.hosts.empty()) {
            const auto path = i.profile.isEmpty() ? std::filesystem::path{} : profilePath(i.profile);
            auto selected = path.empty() ? std::nullopt : rp::loadProfile(path);
            bool matched = false;
            if (selected) for (const auto& [host, id] : r.hosts) {
                if ((!selected->consoleId.empty() && selected->consoleId == id) || (selected->consoleId.empty() && selected->host == host)) {
                    selected->host = host; if (id.size() == 12) selected->consoleId = id;
                    if (rp::saveProfile(path, *selected)) { i.form["host"] = QString::fromStdString(host); matched = true; }
                    break;
                }
            }
            if (!selected && r.hosts.size() == 1) i.form["host"] = QString::fromStdString(r.hosts.front().first);
            else if (selected && !matched) i.status = tx("发现主机，但无法确认是已配对的这一台。保留原地址，可核对后手动修改。");
            else if (!selected && r.hosts.size() > 1) i.status = tx("发现多台 PS5，请填写要配对的主机 IP，避免选错主机。");
        }
        changed = true;
    }
    if (!i.session) {
        if (changed) rpPublish();
        if (!i.busy) i.timer->stop();
        return;
    }

    const bool focused = QGuiApplication::focusWindow() != nullptr;
    // Input every 8 ms: the pad when one is connected, otherwise the keyboard.
    if (!i.viewOnly) {
        rp::ControllerState state;
        if (i.pad.connected()) state = i.pad.poll(focused);
        else {
            if (i.padStarted) (void)i.pad.poll(focused);   // keeps SDL enumerating hot-plugged pads
            state = i.keyboardState(focused);
        }
        i.session->setController(state);
        i.pad.feedback(i.session->takeFeedback(), focused);
    }
    // Audio: write what fits; a late block is dropped rather than delaying the stream.
    while (auto block = i.session->tryAudio()) {
        if (!i.audioDevice || !block->channels) continue;
        QByteArray bytes;
        if (block->channels == 2) bytes = QByteArray(reinterpret_cast<const char*>(block->samples.data()), qsizetype(block->samples.size() * 2));
        else {
            bytes.resize(qsizetype(block->samples.size() * 4));
            auto* out = reinterpret_cast<int16_t*>(bytes.data());
            for (size_t k = 0; k < block->samples.size(); ++k) out[2 * k] = out[2 * k + 1] = block->samples[k];
        }
        if (i.audio->bytesFree() >= bytes.size()) i.audioDevice->write(bytes);
    }
    if (i.audio) i.audio->setVolume(m_muted ? 0.0 : std::clamp(m_volume, 0.0, 1.0));

    if (++i.ticks % 12 && !changed) return;   // status ~10 Hz
    const auto s = i.session->status();
    if (!i.opened && s.streaming && s.width > 0) {
        i.opened = true;
        const QString label = QStringLiteral("PS5 · ") + i.host;
        setSourceOpen(QStringLiteral("ps5"), label, s.width, s.height, s.profile.fps);
        m_isCapture = true;
        m_timelineSeekable = false;
        m_paused = false;
        m_statusText = label;
        m_sourceFormatText = QStringLiteral("%1p%2").arg(s.height).arg(s.profile.fps);
        m_sourceRateText = m_sourceFormatText;
        m_sourceSummary = QStringLiteral("%1x%2").arg(s.width).arg(s.height);
        emit snapshotChanged();
        if (!i.viewOnly && !i.pad.connected() && !i.keyboardHinted) {
            i.keyboardHinted = true;
            notify(macText("No gamepad found: keyboard controls are WASD / arrows, Space ✕, Backspace ○, F □, R △, Q/E L1/R1, Z/C L2/R2, Tab Options, P PS button."), false);
        }
    }
    if (i.opened && (s.width != m_sourceWidth || s.height != m_sourceHeight) && s.width > 0) {
        m_sourceWidth = s.width; m_sourceHeight = s.height;
        m_sourceAspect = double(s.width) / s.height; m_previewAspect = m_sourceAspect;
        emit snapshotChanged();
    }
    QString message;
    if (s.failed) message = tx(s.message.empty() ? std::string("PS5 串流已停止，可以重新连接。") : s.message);
    else if (s.recovering || !s.message.empty()) message = tx(s.message);
    else if (s.streaming) {
        const auto& p = s.profile;
        message = tx(std::string("生效：") + std::to_string(p.height) + "p / " + std::to_string(p.fps) + " fps / " +
                     (p.codec == rp::Codec::H264 ? "H.264" : p.codec == rp::Codec::H265Hdr ? "H.265 HDR" : "H.265") +
                     " / 请求 " + std::to_string(p.bitrateKbps / 1000) + " Mbps · 实收视频 " + QString::number(s.videoMbps, 'f', 1).toStdString() + " Mbps");
        const auto c = i.pad.capabilities();
        if (i.viewOnly) message += tx("\n仅观看：电脑输入已关闭，手柄由 PS5 处理。");
        else if (!c.connected) message += tx("\n未检测到电脑手柄。") + QLatin1Char(' ') + macText("Keyboard controls are active.");
        else message += QStringLiteral("\n") + QString::fromStdString(c.name);
        message += QStringLiteral(" · ") + (s.hardwareDecode ? macText("VideoToolbox hardware decode") : macText("software decode"));
    } else if (s.state == rp::SessionState::LoginPinRequired) {
        message = macText("The PS5 asks for its login PIN: enter it in the PS5 dialog.");
    } else message = tx("连接已开始。需要登录 PIN 时在下方提交。关闭面板不停止串流。");
    if (message != i.status) { i.status = message; changed = true; }
    if (m_remotePlayState != message) { m_remotePlayState = message; emit snapshotChanged(); }
    if (s.failed) {
        notify(message, true);
        rpStopSession(true);
        i.status = message;
        changed = true;
    }
    if (changed) rpPublish();
}

#else  // !VEYRA_ENABLE_REMOTEPLAY ------------------------------------------------

class MacBridge::RpState : public QObject { public: using QObject::QObject; };

void MacBridge::initRemotePlay() {
    m_ps5 = QVariantMap{{"available", false}, {"busy", false}, {"active", false}, {"host", QString()}, {"account", QString()}, {"status", QString()}, {"profile", QString()}};
    m_ps5Control = QVariantMap{{"available", false}, {"enabled", false}, {"state", "off"}};
}
void MacBridge::attachRemotePlay(QQuickWindow* window) { tagMinimalStageSoon(window); }
bool MacBridge::remotePlayActive() const { return false; }
void MacBridge::openPs5Dialog() {
    emit notice(macText("PS5 Remote Play is not available in the macOS version. Sony's PS Remote Play app for Mac can be captured with Screen capture."), true);
}
void MacBridge::ps5Load() {}
bool MacBridge::ps5Set(const QString&, const QVariant&) { return false; }
void MacBridge::ps5SelectProfile(const QString&) {}
void MacBridge::ps5Scan() { openPs5Dialog(); }
void MacBridge::ps5Pair(const QString&) { openPs5Dialog(); }
bool MacBridge::ps5Connect() { openPs5Dialog(); return false; }
void MacBridge::ps5Wake() {}
void MacBridge::ps5Forget() {}
void MacBridge::ps5OpenAccountChecker() {}
void MacBridge::ps5SendLoginPin(const QString&) {}
void MacBridge::ps5Cancel() {}
void MacBridge::ps5Calibrate() {}
void MacBridge::ps5ControlSet(const QString&, const QVariant&) {}
void MacBridge::ps5ControlRestart() {}
void MacBridge::rpPublish() {}
void MacBridge::rpTick() {}
void MacBridge::rpStopSession(bool) {}

#endif
