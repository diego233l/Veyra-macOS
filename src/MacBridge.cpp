#include "MacBridge.h"
#include "veyra/Log.h"
#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVideoSink>
#include <functional>

MacBridge::MacBridge(QObject* parent) : MacBridgeBase(parent) {
    initPrefs();
    initPlayback();
    initEnhance();
    initCapture();
    initRemotePlay();
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
    attachRemotePlay(window);
    followHost();
}

void MacBridge::followHost() {
    if (!stage_ || !window_) return;
    if (!host_) host_ = window_->findChild<QQuickItem*>(QStringLiteral("videoHost"));
    const bool show = host_ && m_hasSource && host_->width() > 1 && host_->height() > 1;
    if (show) {
        // On Windows the picture is a native window above the whole scene. Here it is
        // an item, so it must sit above the page's opaque video card but below the
        // page's own overlays: inside the visible page's "videoArea" when there is one,
        // otherwise directly under the pages (minimal page, fullscreen).
        QQuickItem* container = host_->parentItem();
        // Visual-tree search: QML items are not always QObject children of their parents.
        QList<QQuickItem*> areas;
        std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
            for (QQuickItem* c : item->childItems()) {
                if (c == stage_) continue;
                if (c->objectName() == QLatin1String("videoArea")) areas << c;
                walk(c);
            }
        };
        walk(window_->contentItem());
        static int dbg = 0;
        if (qEnvironmentVariableIsSet("VEYRA_STAGE_DEBUG") && dbg++ % 60 == 0)
            for (QQuickItem* area : areas)
                veyra::log::info("mac-stage", QStringLiteral("area vis=%1 %2x%3 scene=%4,%5")
                    .arg(area->isVisible()).arg(area->width()).arg(area->height())
                    .arg(area->mapToScene({0,0}).x()).arg(area->mapToScene({0,0}).y()).toStdString());
        for (QQuickItem* area : areas) {
            if (!area->isVisible() || area->width() < 2 || area->height() < 2) continue;
            bool shown = true;
            for (QQuickItem* p = area; p; p = p->parentItem())
                if (!p->isVisible() || p->opacity() <= 0.0) { shown = false; break; }
            if (!shown) continue;
            const QRectF a = area->mapRectToScene(QRectF(0, 0, area->width(), area->height()));
            const QRectF h = host_->mapRectToScene(QRectF(0, 0, host_->width(), host_->height()));
            if (a.intersects(h)) { container = area; break; }
        }
        if (stage_->parentItem() != container) {
            stage_->setParentItem(container);
            if (container == host_->parentItem()) {
                stage_->setZ(0);
                stage_->stackAfter(host_);
            } else {
                stage_->setZ(0);
                const auto kids = container->childItems();
                if (!kids.isEmpty() && kids.first() != stage_) stage_->stackBefore(kids.first());
            }
        }
        const QRectF r = container->mapRectFromItem(host_, QRectF(0, 0, host_->width(), host_->height()));
        if (stage_->x() != r.x()) stage_->setX(r.x());
        if (stage_->y() != r.y()) stage_->setY(r.y());
        if (stage_->width() != r.width()) stage_->setWidth(r.width());
        if (stage_->height() != r.height()) stage_->setHeight(r.height());
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
