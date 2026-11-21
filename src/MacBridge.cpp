#include "MacBridge.h"
#include "veyra/Log.h"
#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVideoSink>

MacBridge::MacBridge(QObject* parent) : MacBridgeBase(parent) {
    initPrefs();
    initPlayback();
    initEnhance();
    initCapture();
}

QString MacBridge::macText(const char* english) {
    return QCoreApplication::translate("MacBridge", english);
}

void MacBridge::attachWindow(QQuickWindow* window) {
    window_ = window;
    host_ = window->findChild<QQuickItem*>(QStringLiteral("videoHost"));
    QQmlComponent component(engine_, QUrl(QStringLiteral("qrc:/mac/VideoStage.qml")));
    auto* obj = component.create(engine_->rootContext());
    stage_ = qobject_cast<QQuickItem*>(obj);
    if (!stage_) {
        veyra::log::error("mac-stage", component.errorString().toStdString());
    } else {
        // Directly above the backdrop, below every page: the controls draw over the picture.
        QQuickItem* root = window->contentItem()->childItems().value(0);
        QQuickItem* parentItem = host_ ? host_->parentItem() : window->contentItem();
        stage_->setParentItem(parentItem);
        stage_->setZ(0);
        if (host_) stage_->stackAfter(host_);
        (void)root;
        stage_->setVisible(false);
    }
    connect(window, &QQuickWindow::afterAnimating, this, &MacBridge::followHost);
    attachPrefs(window);
    attachPlayback(window);
    attachEnhance(window);
    attachCapture(window);
    followHost();
}

void MacBridge::followHost() {
    if (!stage_) return;
    if (!host_) host_ = window_ ? window_->findChild<QQuickItem*>(QStringLiteral("videoHost")) : nullptr;
    const bool show = host_ && m_hasSource && host_->width() > 1 && host_->height() > 1;
    if (show) {
        if (stage_->x() != host_->x()) stage_->setX(host_->x());
        if (stage_->y() != host_->y()) stage_->setY(host_->y());
        if (stage_->width() != host_->width()) stage_->setWidth(host_->width());
        if (stage_->height() != host_->height()) stage_->setHeight(host_->height());
    }
    if (stage_->isVisible() != show) stage_->setVisible(show);
}

QVideoSink* MacBridge::videoSink() const {
    if (!stage_) return nullptr;
    auto* out = stage_->findChild<QObject*>(QStringLiteral("macVideoOutput"));
    return out ? out->property("videoSink").value<QVideoSink*>() : nullptr;
}

void MacBridge::showStill(const QUrl& image) {
    if (stage_) stage_->setProperty("stillSource", image);
}

void MacBridge::beginOpening(const QString& name) {
    emit mediaOpenRequested();
    m_openingSource = true;
    m_failed = false;
    m_sourceName = name;
    m_statusText = name;
    emit snapshotChanged();
}

void MacBridge::setSourceOpen(const QString& kind, const QString& name, int width, int height, double fps) {
    m_openingSource = false;
    m_hasSource = true;
    m_running = true;
    m_failed = false;
    m_sourceKind = kind;
    m_isImage = kind == QLatin1String("image");
    m_isCapture = kind == QLatin1String("capture") || kind == QLatin1String("screen");
    m_sourceName = name;
    m_sourceTitle = name;
    m_sourceWidth = width;
    m_sourceHeight = height;
    m_sourceFps = fps;
    m_sourceAspect = height > 0 ? double(width) / height : 16.0 / 9.0;
    m_previewAspect = m_sourceAspect;
    emit snapshotChanged();
    followHost();
}

void MacBridge::clearSource() {
    m_openingSource = false;
    m_hasSource = false;
    m_running = false;
    m_paused = false;
    m_isImage = false;
    m_isCapture = false;
    m_sourceKind.clear();
    m_position = m_duration = m_progress = 0;
    showStill(QUrl());
    emit snapshotChanged();
    followHost();
}
