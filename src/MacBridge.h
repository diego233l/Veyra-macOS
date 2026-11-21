#pragma once
// macOS implementation of the bridge the Veyra QML talks to as `veyra`.
// MacBridgeBase (generated from Windows' QmlPlayerBridge.h) carries every
// property and command with a neutral default; MacBridge overrides what macOS
// can do. The class body is split into parts (src/parts/*.h) by area.
#include "generated/MacBridgeBase.h"
#include <QPointer>
#include <QUrl>
#include <QQuickItem>
#include <QQuickWindow>
class QQmlEngine;
class QQuickWindow;
class QQuickItem;
class QVideoSink;

class MacBridge : public MacBridgeBase {
    Q_OBJECT
public:
    explicit MacBridge(QObject* parent = nullptr);
    void setQmlEngine(QQmlEngine* engine) { engine_ = engine; }
    QQmlEngine* qmlEngine() const { return engine_; }
    // Called by main.cpp after Main.qml is created.
    void attachWindow(QQuickWindow* window);

    // ---- shared core (MacBridge.cpp) — every part may use these ----------
    QQuickWindow* window() const { return window_; }
    // The video stage (mac/VideoStage.qml): sits under the page controls and
    // follows the QML item "videoHost". Properties are documented in that file.
    QQuickItem* stage() const { return stage_; }
    QVideoSink* videoSink() const;            // feed QMediaPlayer / QMediaCaptureSession here
    void showStill(const QUrl& image);        // still-image source (empty url = video mode)
    // Source state. kind: "file" | "image" | "capture" | "screen" | "" (none).
    void beginOpening(const QString& name);   // openingSource=true, emits mediaOpenRequested
    void setSourceOpen(const QString& kind, const QString& name, int width, int height, double fps);
    void clearSource();
    void notify(const QString& text, bool isError = false) { emit notice(text, isError); }
    // Mac-only strings: write the English text; translated via i18n/mac-catalog.json.
    static QString macText(const char* english);

#include "parts/Playback.h"
#include "parts/Enhance.h"
#include "parts/Capture.h"
#include "parts/Prefs.h"

private:
    void followHost();
    QQmlEngine* engine_ = nullptr;
    QPointer<QQuickWindow> window_;
    QPointer<QQuickItem> stage_;
    QPointer<QQuickItem> host_;
};
