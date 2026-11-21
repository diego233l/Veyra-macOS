#include "veyra/Log.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <cstdio>

namespace veyra::log {
void write(const char* level, const std::string& area, const std::string& message) {
    static QMutex lock;
    static QFile file;
    QMutexLocker guard(&lock);
    if (!file.isOpen()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/logs");
        QDir().mkpath(dir);
        file.setFileName(dir + QStringLiteral("/veyra.log"));
        file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
    const QByteArray line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + ' ' + level + " [" +
                            QByteArray::fromStdString(area) + "] " + QByteArray::fromStdString(message) + '\n';
    std::fputs(line.constData(), stderr);
    if (file.isOpen()) { file.write(line); file.flush(); }
}
}
