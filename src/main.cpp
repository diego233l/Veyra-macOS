// Veyra for macOS — entry point. Loads the original Veyra QML front end
// unchanged, against MacBridge instead of the Windows D3D12 engine.
#include "MacBridge.h"
#include "StartupMusic.h"
#include "veyra/Log.h"
#include "veyra/ui/UiLanguage.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QTimer>

namespace {
class ThumbnailProvider : public QQuickImageProvider {
public:
    ThumbnailProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString&, QSize* size, const QSize&) override {
        QImage img(16, 9, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        if (size) *size = img.size();
        return img;
    }
};

QString resourceDir() {
    const QString app = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
    const QString bundle = QDir::cleanPath(app + QStringLiteral("/../Resources"));
    if (QDir(bundle + QStringLiteral("/qml")).exists()) return bundle;
#endif
    return app;
}
} // namespace

int main(int argc, char** argv) {
    QApplication::setApplicationName(QStringLiteral("Veyra"));
    QApplication::setOrganizationName(QStringLiteral("Veyra"));
    QApplication::setApplicationVersion(QStringLiteral(VEYRA_VERSION));
    QApplication app(argc, argv);
    const QString res = resourceDir();
    app.setWindowIcon(QIcon(res + QStringLiteral("/qml/Veyra/logo-dusk.png")));

    MacBridge bridge;
    StartupMusic music(res + QStringLiteral("/media/startup-music.mp3"));
    QObject::connect(&bridge, &MacBridgeBase::mediaOpenRequested, &music, &StartupMusic::pause);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("veyra-thumb"), new ThumbnailProvider);
    engine.addImportPath(res + QStringLiteral("/qml"));
    engine.rootContext()->setContextProperty(QStringLiteral("vySoftwareUi"), false);
    engine.rootContext()->setContextProperty(QStringLiteral("veyra"), &bridge);
    engine.rootContext()->setContextProperty(QStringLiteral("vyMusic"), &music);
    QVariantMap testOptions;
    int exitAfterMs = 0;
    QString openPath, shotPath;
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString& a = args.at(i);
        const bool hasValue = i + 1 < args.size();
        if (a == QLatin1String("--exit-after") && hasValue) exitAfterMs = args.at(++i).toInt();
        else if (a == QLatin1String("--screenshot") && hasValue) shotPath = args.at(++i);
        else if (a == QLatin1String("--page") && hasValue) bridge.setInitialPage(args.at(++i));
        else if (a == QLatin1String("--dialog") && hasValue) testOptions.insert(QStringLiteral("dialog"), args.at(++i));
        else if (a == QLatin1String("--tab") && hasValue) testOptions.insert(QStringLiteral("tab"), args.at(++i));
        else if (a == QLatin1String("--reduced-motion")) testOptions.insert(QStringLiteral("reducedMotion"), true);
        else if (!a.startsWith(QLatin1Char('-'))) openPath = a;
    }
    engine.rootContext()->setContextProperty(QStringLiteral("vyTest"), testOptions);
    bridge.setQmlEngine(&engine);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Veyra", "Main");
    if (engine.rootObjects().isEmpty()) return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window) bridge.attachWindow(window);
    if (!openPath.isEmpty()) QTimer::singleShot(600, &bridge, [&bridge, openPath] { bridge.openPath(openPath); });
    if (!shotPath.isEmpty() && window && exitAfterMs > 0)
        QTimer::singleShot(exitAfterMs - 100, window, [window, shotPath] { window->grabWindow().save(shotPath); });
    if (exitAfterMs > 0) QTimer::singleShot(exitAfterMs, &app, &QCoreApplication::quit);
    return app.exec();
}
