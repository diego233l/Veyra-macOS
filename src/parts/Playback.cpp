// MacBridge — playback area: files, still images, network streams, transport,
// snapshot readouts, recent files and screenshots. Qt Multimedia (AVFoundation /
// FFmpeg backend on macOS) replaces the Windows D3D12 engine.
#include "../MacBridge.h"
#include "veyra/Log.h"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMediaFormat>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kMinRate = 0.25;
constexpr double kMaxRate = 4.0;
constexpr double kMaxGain = 1.0;   // QAudioOutput cannot amplify above 1.0
constexpr int kMaxRecent = 12;
const char* kRecentKey = "recent/files";
const char* kLastKey = "recent/lastSource";
const char* kShotDirKey = "screenshots/directory";

QString defaultShotDir() {
    return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/Veyra");
}
} // namespace

QString MacBridge::mmss(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    const int total = int(seconds);
    const int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

bool MacBridge::isStillImagePath(const QString& path) {
    static const QStringList exts{"png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff", "gif", "heic", "heif"};
    return exts.contains(QFileInfo(path).suffix().toLower());
}

void MacBridge::initPlayback() {
    m_maximumVolume = kMaxGain;
    m_minimumPlaybackRate = kMinRate;
    m_maximumPlaybackRate = kMaxRate;
    m_playbackRate = 1.0;
    m_playbackSpeed = 1.0;
    m_positionText = mmss(0);
    m_durationText = mmss(0);
    QSettings settings;
    m_volume = std::clamp(settings.value(QStringLiteral("playback/volume"), 1.0).toDouble(), 0.0, kMaxGain);
    m_screenshotDirectory = settings.value(QLatin1String(kShotDirKey), defaultShotDir()).toString();
    loadRecents();
    m_lastSource = settings.value(QLatin1String(kLastKey)).toMap();
    if (m_lastSource.value(QStringLiteral("kind")).toString() == QLatin1String("file"))
        m_lastSource[QStringLiteral("summary")] = QFileInfo(m_lastSource.value(QStringLiteral("label")).toString()).fileName();

    playbackTick_ = new QTimer(this);
    playbackTick_->setInterval(100);
    connect(playbackTick_, &QTimer::timeout, this, &MacBridge::tickPlayback);
    if (qEnvironmentVariable("VEYRA_PB_DEBUG") == QLatin1String("top")) {
        auto* raise = new QTimer(this);
        connect(raise, &QTimer::timeout, this, [this] { if (stage()) stage()->setZ(1000); });
        raise->start(200);
    }
    // Test hook: VEYRA_PB_SHOT=<ms> takes a screenshot that long after start.
    if (const int ms = qEnvironmentVariableIntValue("VEYRA_PB_SHOT"); ms > 0)
        QTimer::singleShot(ms, this, [this] { takeScreenshot(); });

    // Another source (capture, screen) is opening: give the picture up.
    connect(this, &MacBridgeBase::mediaOpenRequested, this, [this] {
        if (selfOpening_) return;
        detachPlayer();
    });
}

void MacBridge::attachPlayback(QQuickWindow*) {
    // The player is created lazily; nothing to bind until a file is opened.
}

void MacBridge::ensurePlayer() {
    if (player_) return;
    player_ = new QMediaPlayer(this);
    audioOut_ = new QAudioOutput(this);
    audioOut_->setVolume(float(m_volume));
    audioOut_->setMuted(m_muted);
    player_->setAudioOutput(audioOut_);

    connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (playPath_.isEmpty()) return;
        switch (status) {
        case QMediaPlayer::LoadedMedia:
        case QMediaPlayer::BufferedMedia: {
            if (!m_openingSource) break;
            const auto meta = player_->metaData();
            QSize size = meta.value(QMediaMetaData::Resolution).toSize();
            if (QVideoSink* sink = videoSink(); sink && !sink->videoSize().isEmpty()) size = sink->videoSize();
            double fps = meta.value(QMediaMetaData::VideoFrameRate).toDouble();
            const QString name = networkSource_ ? playPath_ : QFileInfo(playPath_).fileName();
            setSourceOpen(QStringLiteral("file"), name, size.width(), size.height(), fps);
            m_isCapture = false;
            m_statusText = macText("播放中");
            updateTracks();
            player_->play();
            playbackTick_->start();
            tickPlayback();
            break;
        }
        case QMediaPlayer::EndOfMedia:
            m_paused = true;
            m_statusText = macText("已暂停");
            emit snapshotChanged();
            break;
        case QMediaPlayer::InvalidMedia:
            break;   // errorOccurred reports it
        default:
            break;
        }
    });
    connect(player_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString& text) {
        if (error == QMediaPlayer::NoError || playPath_.isEmpty()) return;
        veyra::log::error("mac-playback", text.toStdString());
        const QString name = QFileInfo(playPath_).fileName();
        detachPlayer();
        m_failed = true;
        m_statusText = text;
        emit snapshotChanged();
        notify(macText("Could not open %1: %2").arg(name.isEmpty() ? playPath_ : name, text), true);
    });
    connect(player_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState) { tickPlayback(); });
    connect(player_, &QMediaPlayer::tracksChanged, this, [this] { updateTracks(); emit snapshotChanged(); });
}

void MacBridge::detachPlayer() {
    if (!player_) return;
    const bool had = !playPath_.isEmpty();
    playPath_.clear();
    playerOwnsStage_ = false;
    playbackTick_->stop();
    player_->stop();
    player_->setVideoSink(nullptr);
    player_->setSource(QUrl());
    if (had) veyra::log::info("mac-playback", "player detached");
}

// --- opening ----------------------------------------------------------------

void MacBridge::openFileDialog() {
    const QString path = QFileDialog::getOpenFileName(
        nullptr, macText("打开视频或图片"), QString(),
        macText("媒体文件 (*.mp4 *.mkv *.mov *.avi *.webm *.ts *.m2ts *.jpg *.jpeg *.png *.bmp *.webp);;所有文件 (*)"));
    if (!path.isEmpty()) openPath(path);
}

void MacBridge::openPath(const QString& path) {
    if (path.isEmpty()) return;
    const QUrl asUrl(path);
    const bool network = !asUrl.scheme().isEmpty() && asUrl.scheme().size() > 1 && !asUrl.isLocalFile()
                         && asUrl.scheme() != QLatin1String("file");
    if (!network && QFileInfo(path).isDir()) {
        if (QObject* list = playlist()) QMetaObject::invokeMethod(list, "addPaths", Q_ARG(QStringList, QStringList{path}));
        emit playlistRequested();
        return;
    }
    const QString local = network ? path : (asUrl.isLocalFile() ? asUrl.toLocalFile() : path);
    if (!network && !QFileInfo(local).isFile()) {
        notify(macText("文件不存在：%1").arg(local), true);
        return;
    }
    if (!network && isStillImagePath(local)) { openStill(local); return; }

    selfOpening_ = true;
    beginOpening(network ? path : QFileInfo(local).fileName());
    selfOpening_ = false;
    goToPlayerPage();
    m_statusText = macText("正在打开…");
    ensurePlayer();
    detachPlayer();
    networkSource_ = network;
    playPath_ = local;
    playerOwnsStage_ = true;
    firstFrameSeen_ = false;
    showStill(QUrl());
    if (QVideoSink* sink = videoSink()) player_->setVideoSink(sink);
    player_->setPlaybackRate(m_playbackRate);
    player_->setSource(network ? QUrl(path) : QUrl::fromLocalFile(local));
    veyra::log::info("mac-playback", "open " + local.toStdString());
    if (!network) rememberFile(local);
    if (QObject* list = playlist()) QMetaObject::invokeMethod(list, "setPlayback", Qt::DirectConnection,
                                                              Q_ARG(QString, local), Q_ARG(QString, QStringLiteral("opening")));
}

// Opening from home switches to a page with a video area (as on Windows).
void MacBridge::goToPlayerPage() {
    const QString page = currentPage();
    if (page.isEmpty() || page == QLatin1String("home")) emit navigate(QStringLiteral("pro"));
}

void MacBridge::openStill(const QString& path) {
    selfOpening_ = true;
    beginOpening(QFileInfo(path).fileName());
    selfOpening_ = false;
    goToPlayerPage();
    detachPlayer();
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!reader.canRead()) {
        m_openingSource = false;
        m_failed = true;
        emit snapshotChanged();
        notify(macText("Could not open %1: %2").arg(QFileInfo(path).fileName(), reader.errorString()), true);
        return;
    }
    playPath_.clear();
    showStill(QUrl::fromLocalFile(path));
    setSourceOpen(QStringLiteral("image"), QFileInfo(path).fileName(), size.width(), size.height(), 0.0);
    m_duration = m_position = m_progress = 0;
    m_paused = false;
    m_timelineSeekable = false;
    m_sourceRateText = QStringLiteral("%1×%2").arg(size.width()).arg(size.height());
    m_sourceFormatText = m_sourceRateText;
    m_sourceSummary = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
    m_statusText = QFileInfo(path).fileName();
    emit snapshotChanged();
    rememberFile(path);
}

void MacBridge::openDroppedUrls(const QList<QUrl>& urls) {
    if (urls.isEmpty()) return;
    if (urls.size() == 1) {
        const QUrl url = urls.front();
        if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isDir()) { openUrl(url); return; }
    }
    QStringList paths;
    for (const QUrl& u : urls) if (u.isLocalFile()) paths << u.toLocalFile();
    if (QObject* list = playlist()) {
        QMetaObject::invokeMethod(list, "addPaths", Q_ARG(QStringList, paths));
        emit playlistRequested();
    } else if (!paths.isEmpty()) {
        openPath(paths.front());
    }
}

void MacBridge::openUrl(const QUrl& url) {
    if (!url.isLocalFile()) { openNetworkAddress(url.toString(QUrl::FullyEncoded)); return; }
    openPath(url.toLocalFile());
}

void MacBridge::openNetworkDialog() { emit navigate(QStringLiteral("network")); }

bool MacBridge::openNetworkAddress(const QString& address) {
    const QUrl url(address.trimmed(), QUrl::StrictMode);
    static const QStringList schemes{"http", "https", "rtsp", "rtmp", "rtp", "udp", "hls"};
    if (!url.isValid() || url.host().isEmpty() || !schemes.contains(url.scheme().toLower())) {
        notify(macText("请输入有效的 HTTP、HTTPS、RTSP、RTP 或 UDP 视频地址"), true);
        return false;
    }
    openPath(url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded));
    return true;
}

void MacBridge::resumeLastSource() {
    const QString kind = m_lastSource.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("file")) {
        const QString path = m_lastSource.value(QStringLiteral("label")).toString();
        if (!QFileInfo(path).isFile()) { notify(macText("上次的视频文件不存在：%1").arg(path), true); return; }
        openPath(path);
        return;
    }
    if (kind == QLatin1String("capture")) { resumeCaptureSession(); return; }
    if (kind == QLatin1String("ps5")) { ps5Load(); if (!ps5Connect()) openPs5Dialog(); return; }
}

void MacBridge::showPlaylist() { emit playlistRequested(); }

void MacBridge::addPlaylistFilesDialog() {
    const QStringList paths = QFileDialog::getOpenFileNames(nullptr, macText("添加文件到播放列表"), {},
        macText("媒体文件 (*.mp4 *.mkv *.mov *.avi *.webm *.ts *.m2ts *.jpg *.jpeg *.png *.bmp *.webp);;所有文件 (*)"));
    if (paths.isEmpty()) return;
    if (QObject* list = playlist()) { QMetaObject::invokeMethod(list, "addPaths", Q_ARG(QStringList, paths)); emit playlistRequested(); }
    else openPath(paths.front());
}

void MacBridge::addPlaylistFolderDialog() {
    const QString path = QFileDialog::getExistingDirectory(nullptr, macText("导入文件夹到播放列表"));
    if (path.isEmpty()) return;
    if (QObject* list = playlist()) { QMetaObject::invokeMethod(list, "addPaths", Q_ARG(QStringList, QStringList{path})); emit playlistRequested(); }
}

// --- transport --------------------------------------------------------------

void MacBridge::togglePlayPause() {
    if (!player_ || playPath_.isEmpty()) return;
    if (player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();
    else {
        if (player_->mediaStatus() == QMediaPlayer::EndOfMedia) player_->setPosition(0);
        player_->play();
    }
    tickPlayback();
}

void MacBridge::stopPlayback() {
    if (m_sourceKind == QLatin1String("ps5") || remotePlayActive()) { ps5Cancel(); return; }
    const bool wasPlayer = !playPath_.isEmpty();
    if (QObject* list = playlist(); list && wasPlayer)
        QMetaObject::invokeMethod(list, "setPlayback", Qt::DirectConnection,
                                  Q_ARG(QString, playPath_), Q_ARG(QString, QStringLiteral("stopped")));
    detachPlayer();
    if (wasPlayer || m_isImage) clearSource();
}

void MacBridge::seekTo(double seconds) {
    if (!player_ || playPath_.isEmpty() || !player_->isSeekable()) return;
    const double d = player_->duration() / 1000.0;
    seconds = std::clamp(seconds, 0.0, d > 0 ? d : seconds);
    player_->setPosition(qint64(seconds * 1000.0));
    tickPlayback();
}

void MacBridge::seekBy(double seconds) { seekTo(std::max(0.0, m_position + seconds)); }

void MacBridge::stepFrame(int direction) {
    if (!direction || !player_ || playPath_.isEmpty()) return;
    if (player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();
    const double fps = m_sourceFps > 1 ? m_sourceFps : 60.0;
    seekTo(std::max(0.0, m_position + (direction > 0 ? 1.0 : -1.0) / fps));
}

void MacBridge::returnToLive() {}

double MacBridge::timelineSeconds(double fraction) const { return std::clamp(fraction, 0.0, 1.0) * m_duration; }

QString MacBridge::formatTime(double seconds) const { return mmss(seconds); }

void MacBridge::holdOriginal(bool held) {
    if (stage()) stage()->setProperty("bypass", held);
}

void MacBridge::rotatePictureClockwise() {
    m_previewRotation = (m_previewRotation + 90) % 360;
    if (stage()) stage()->setProperty("pictureRotation", m_previewRotation);
    emit compareChanged();
    emit snapshotChanged();
}

void MacBridge::setVolume(double v) {
    if (!std::isfinite(v)) return;
    v = std::clamp(v, 0.0, kMaxGain);
    if (audioOut_) audioOut_->setVolume(float(v));
    if (m_volume == v) return;
    m_volume = v;
    QSettings().setValue(QStringLiteral("playback/volume"), v);
    emit snapshotChanged();
}

void MacBridge::setMuted(bool v) {
    if (audioOut_) audioOut_->setMuted(v);
    if (m_muted == v) return;
    m_muted = v;
    emit snapshotChanged();
}

void MacBridge::setPlaybackRate(double v) {
    if (!std::isfinite(v) || v < kMinRate || v > kMaxRate) return;
    if (player_) player_->setPlaybackRate(v);
    m_playbackRate = v;
    m_playbackSpeed = v;
    emit snapshotChanged();
}

void MacBridge::setSelectedAudioTrack(int v) {
    if (!player_) return;
    const int count = int(player_->audioTracks().size());
    if (v < 0 || v >= count) return;
    player_->setActiveAudioTrack(v);
    m_selectedAudioTrack = v;
    emit snapshotChanged();
}

void MacBridge::updateTracks() {
    QVariantList out;
    if (player_) {
        const auto tracks = player_->audioTracks();
        for (int i = 0; i < tracks.size(); ++i) {
            const auto& t = tracks.at(i);
            QStringList parts;
            const auto lang = t.value(QMediaMetaData::Language).value<QLocale::Language>();
            if (lang != QLocale::AnyLanguage && lang != QLocale::C) parts << QLocale::languageToString(lang);
            const QString title = t.value(QMediaMetaData::Title).toString();
            if (!title.isEmpty()) parts << title;
            const auto codec = t.value(QMediaMetaData::AudioCodec).value<QMediaFormat::AudioCodec>();
            if (codec != QMediaFormat::AudioCodec::Unspecified) parts << QMediaFormat::audioCodecName(codec);
            out << QVariantMap{{"index", i},
                               {"label", parts.isEmpty() ? macText("音轨 %1").arg(i + 1) : parts.join(QStringLiteral(" · "))},
                               {"channels", 0}};
        }
        m_selectedAudioTrack = std::max(0, player_->activeAudioTrack());
    }
    m_audioTracks = out;
}

void MacBridge::tickPlayback() {
    if (!player_ || playPath_.isEmpty() || m_openingSource) return;
    bool changed = false;
    auto set = [&changed](auto& member, const auto& value) {
        if (member != value) { member = value; changed = true; }
    };
    const double pos = player_->position() / 1000.0;
    const double dur = std::max(0.0, player_->duration() / 1000.0);
    set(m_position, pos);
    set(m_duration, dur);
    set(m_progress, dur > 0.01 ? std::clamp(pos / dur, 0.0, 1.0) : 0.0);
    set(m_positionText, mmss(pos));
    set(m_durationText, networkSource_ && dur <= 0 ? macText("直播") : mmss(dur));
    const bool playing = player_->playbackState() == QMediaPlayer::PlayingState;
    set(m_paused, !playing);
    set(m_running, true);
    set(m_timelineSeekable, player_->isSeekable() && dur > 0);
    set(m_statusText, playing ? macText("播放中") : macText("已暂停"));
    if (QVideoSink* sink = videoSink()) {
        const QSize s = sink->videoSize();
        if (!s.isEmpty()) {
            set(m_sourceWidth, s.width());
            set(m_sourceHeight, s.height());
            const double aspect = double(s.width()) / s.height();
            set(m_sourceAspect, aspect);
            set(m_previewAspect, aspect);
        }
    }
    if (m_sourceFps <= 0) {
        const double fps = player_->metaData().value(QMediaMetaData::VideoFrameRate).toDouble();
        if (fps > 0) set(m_sourceFps, fps);
    }
    QString rate;
    if (m_sourceHeight > 0) {
        const double fps = m_sourceFps;
        const bool whole = std::abs(fps - std::round(fps)) < 0.01;
        rate = fps <= 0 ? QStringLiteral("%1p").arg(m_sourceHeight)
                        : QStringLiteral("%1p%2").arg(m_sourceHeight)
                              .arg(whole ? QString::number(int(std::round(fps))) : QString::number(fps, 'f', 2));
    }
    set(m_sourceRateText, rate);
    set(m_sourceFormatText, rate);
    set(m_sourceSummary, m_sourceWidth ? QStringLiteral("%1x%2").arg(m_sourceWidth).arg(m_sourceHeight) : QString());
    set(m_playbackSpeed, playing ? player_->playbackRate() : 0.0);
    if (qEnvironmentVariableIsSet("VEYRA_PB_DEBUG") && stage()) {
        if (qEnvironmentVariable("VEYRA_PB_DEBUG") == QLatin1String("top")) stage()->setZ(1000);
        QVideoSink* sink = videoSink();
        veyra::log::info("mac-playback-debug", QStringLiteral("stage vis=%1 %2x%3@%4,%5 z=%6 sink=%7 frame=%8 parent=%9")
            .arg(stage()->isVisible()).arg(stage()->width()).arg(stage()->height()).arg(stage()->x()).arg(stage()->y())
            .arg(stage()->z()).arg(sink != nullptr).arg(sink ? sink->videoFrame().isValid() : false)
            .arg(stage()->parentItem() ? stage()->parentItem()->metaObject()->className() : "none").toStdString());
    }
    if (changed) emit snapshotChanged();
}

// --- recents ----------------------------------------------------------------

void MacBridge::loadRecents() {
    QVariantList out;
    for (const QString& path : QSettings().value(QLatin1String(kRecentKey)).toStringList())
        out << QVariantMap{{"path", path}, {"label", QFileInfo(path).fileName()}, {"exists", QFileInfo(path).isFile()}};
    m_recentFiles = out;
}

void MacBridge::rememberFile(const QString& path) {
    QSettings settings;
    QStringList list = settings.value(QLatin1String(kRecentKey)).toStringList();
    list.removeAll(path);
    list.prepend(path);
    while (list.size() > kMaxRecent) list.removeLast();
    settings.setValue(QLatin1String(kRecentKey), list);
    loadRecents();
    emit recentFilesChanged();
    const QVariantMap last{{"kind", QStringLiteral("file")}, {"label", path}};
    settings.setValue(QLatin1String(kLastKey), last);
    m_lastSource = last;
    m_lastSource[QStringLiteral("summary")] = QFileInfo(path).fileName();
    emit preferencesChanged();
}

void MacBridge::refreshRecentFiles() { loadRecents(); emit recentFilesChanged(); }

void MacBridge::clearRecentFiles() {
    QSettings().remove(QLatin1String(kRecentKey));
    m_recentFiles.clear();
    emit recentFilesChanged();
}

// --- screenshots ------------------------------------------------------------

bool MacBridge::grabPicture(const QString& path) {
    QQuickItem* item = stage();
    if (!window() || !item || !item->isVisible()) return false;
    const QImage full = window()->grabWindow();
    if (full.isNull()) return false;
    const qreal dpr = full.devicePixelRatio() > 0 ? full.devicePixelRatio() : 1.0;
    const QRectF scene = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    QRect crop(qRound(scene.x() * dpr), qRound(scene.y() * dpr), qRound(scene.width() * dpr), qRound(scene.height() * dpr));
    crop = crop.intersected(full.rect());
    QImage picture = full.copy(crop);
    // Trim the letterbox: the source aspect inside the stage.
    if (m_sourceWidth > 0 && m_sourceHeight > 0 && !picture.isNull()) {
        const QSizeF fitted = QSizeF(m_sourceWidth, m_sourceHeight).scaled(picture.size(), Qt::KeepAspectRatio);
        const QRect inner(qRound((picture.width() - fitted.width()) / 2), qRound((picture.height() - fitted.height()) / 2),
                          qRound(fitted.width()), qRound(fitted.height()));
        if (inner.isValid() && (m_previewRotation % 180) == 0) picture = picture.copy(inner);
    }
    return !picture.isNull() && picture.save(path);
}

void MacBridge::takeScreenshot() {
    if (!m_hasSource) { notify(macText("没有正在播放的画面，无法截图"), true); return; }
    const QString folder = m_screenshotDirectory.isEmpty() ? defaultShotDir() : m_screenshotDirectory;
    if (!QDir().mkpath(folder)) { notify(macText("截图目录不可用：%1").arg(folder), true); return; }
    const QString path = QDir(folder).filePath(QStringLiteral("veyra-%1.png")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
    if (!grabPicture(path)) { notify(macText("Screenshot failed"), true); return; }
    m_lastScreenshot = path;
    emit preferencesChanged();
    notify(macText("截图已保存：%1").arg(path), false);
}

void MacBridge::saveFrameDialog() {
    if (!m_hasSource) { notify(macText("没有可保存的画面"), true); return; }
    const QString suggested = QDir(m_screenshotDirectory).filePath(QFileInfo(m_sourceName).completeBaseName() + QStringLiteral("-veyra.png"));
    const QString path = QFileDialog::getSaveFileName(nullptr, macText("导出当前画面"), suggested,
                                                      macText("PNG 图片 (*.png);;JPEG 图片 (*.jpg)"));
    if (path.isEmpty()) return;
    if (grabPicture(path)) notify(macText("截图已保存：%1").arg(path), false);
    else notify(macText("Screenshot failed"), true);
}

void MacBridge::openScreenshotDirectory() {
    QDir().mkpath(m_screenshotDirectory);
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_screenshotDirectory));
}

void MacBridge::chooseScreenshotDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(nullptr, macText("选择截图目录"), m_screenshotDirectory);
    if (dir.isEmpty()) return;
    m_screenshotDirectory = dir;
    QSettings().setValue(QLatin1String(kShotDirKey), dir);
    emit preferencesChanged();
}
