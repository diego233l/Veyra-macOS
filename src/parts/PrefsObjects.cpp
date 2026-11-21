#include "PrefsObjects.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSet>
#include <algorithm>

namespace {
QString tx(const char* s) { return QCoreApplication::translate("QmlPlayerBridge", s); }
bool supported(const QString& path) {
    static const QSet<QString> suffixes{
        "mp4", "mkv", "mov", "avi", "webm", "ts", "m2ts", "mts", "m4v", "wmv",
        "mpg", "mpeg", "vob", "flv", "ogv", "asf", "3gp", "jpg", "jpeg", "png", "bmp", "webp", "heic", "tif", "tiff"};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}
const char* kReleases = "https://github.com/Likely7/Veyra-NRVideo/releases";
}

// --- updates ---------------------------------------------------------------
QVariantMap MacUpdateChecker::portable() const {
    return {{"available", false}, {"downloading", false}, {"scheduled", false}, {"progress", 0.0},
            {"sizeText", QString()}, {"releaseNotes", QString()}};
}
void MacUpdateChecker::check() { openRelease(); }
void MacUpdateChecker::openRelease() { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kReleases))); }

// --- stream clients ----------------------------------------------------------
QVariantMap MacStreamClient::state() const {
    return {{"streaming", false}, {"stream", QVariantMap{}}, {"state", QStringLiteral("unavailable")}, {"message", QString()}};
}

// --- export queue --------------------------------------------------------------
QHash<int, QByteArray> MacExportQueue::roleNames() const {
    return {{Qt::UserRole + 1, "itemId"}, {Qt::UserRole + 2, "name"}, {Qt::UserRole + 3, "input"}, {Qt::UserRole + 4, "output"},
            {Qt::UserRole + 5, "state"}, {Qt::UserRole + 6, "progress"}, {Qt::UserRole + 7, "note"}, {Qt::UserRole + 8, "duration"},
            {Qt::UserRole + 9, "current"}, {Qt::UserRole + 10, "locked"}, {Qt::UserRole + 11, "inspecting"}};
}

// --- playlist ------------------------------------------------------------------
MacPlaylist::MacPlaylist(const QString& storeFile, QObject* parent) : QAbstractListModel(parent), store_(storeFile) { load(); }

QHash<int, QByteArray> MacPlaylist::roleNames() const {
    return {{NameRole, "entryName"}, {PathRole, "entryPath"}, {SizeRole, "entrySize"}, {ModifiedRole, "entryModified"},
            {CurrentRole, "entryCurrent"}, {StateRole, "entryState"}};
}
QVariant MacPlaylist::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const auto& e = entries_[index.row()];
    switch (role) {
    case Qt::DisplayRole: case NameRole: return e.name;
    case PathRole: return QDir::toNativeSeparators(e.path);
    case SizeRole: return QLocale().formattedDataSize(e.size);
    case ModifiedRole: return e.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    case CurrentRole: return index.row() == current_;
    case StateRole: return index.row() == current_ ? e.state : QString();
    }
    return {};
}
void MacPlaylist::setStatus(const QString& s) { if (status_ != s) { status_ = s; emit statusChanged(); } }
void MacPlaylist::setCurrent(int row) {
    if (current_ == row) return;
    const int old = current_;
    current_ = row;
    if (old >= 0 && old < count()) emit dataChanged(index(old), index(old));
    if (row >= 0 && row < count()) emit dataChanged(index(row), index(row));
    emit currentChanged();
}
void MacPlaylist::setAutoAdvance(bool on) {
    if (autoAdvance_ == on) return;
    autoAdvance_ = on;
    emit autoAdvanceChanged();
    save();
}
void MacPlaylist::setPlayback(const QString& path, const QString& state) {
    const int row = indexOfPath(path);
    setCurrent(row);
    if (row < 0) return;
    entries_[row].state = state;
    emit dataChanged(index(row), index(row));
    if (state == QLatin1String("ended") && autoAdvance_) next();
}
void MacPlaylist::addPaths(const QStringList& paths) {
    QList<Entry> added;
    auto consider = [&](const QString& file) {
        if (!supported(file) || indexOfPath(file) >= 0) return;
        if (std::any_of(added.begin(), added.end(), [&](const Entry& e) { return e.path == file; })) return;
        const QFileInfo info(file);
        added.push_back({info.absoluteFilePath(), info.fileName(), info.size(), info.lastModified(), {}});
    };
    for (const QString& p : paths) {
        const QFileInfo info(p);
        if (info.isDir()) {
            QDirIterator it(p, QDir::Files, QDirIterator::Subdirectories);
            QStringList files;
            while (it.hasNext()) files << it.next();
            files.sort(Qt::CaseInsensitive);
            for (const auto& f : files) consider(f);
        } else if (info.isFile()) {
            consider(info.absoluteFilePath());
        }
    }
    if (!added.isEmpty()) {
        beginInsertRows({}, count(), count() + int(added.size()) - 1);
        entries_ += added;
        endInsertRows();
        emit countChanged();
        if (sortMode_ != 0) sortBy(sortMode_);
        save();
        setStatus(tx("已添加 %1 项 · 列表共 %2 项").arg(added.size()).arg(count()));
    } else {
        setStatus(tx("未添加新条目：文件已在列表中，或没有支持的媒体文件"));
    }
}
void MacPlaylist::addUrls(const QList<QUrl>& urls) {
    QStringList paths;
    for (const auto& u : urls) if (u.isLocalFile()) paths << u.toLocalFile();
    addPaths(paths);
}
QString MacPlaylist::pathAt(int row) const { return row >= 0 && row < count() ? entries_[row].path : QString(); }
int MacPlaylist::indexOfPath(const QString& path) const {
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    for (int i = 0; i < count(); ++i) if (entries_[i].path == clean) return i;
    return -1;
}
bool MacPlaylist::play(int row) {
    if (row < 0 || row >= count()) return false;
    setCurrent(row);
    emit playRequested(entries_[row].path);
    if (opener) opener(entries_[row].path);
    return true;
}
bool MacPlaylist::next() { return count() > 0 && play(current_ + 1 < count() ? current_ + 1 : 0); }
bool MacPlaylist::previous() { return count() > 0 && play(current_ > 0 ? current_ - 1 : count() - 1); }
bool MacPlaylist::move(int from, int to) {
    if (from < 0 || from >= count() || to < 0 || to >= count() || from == to) return false;
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    const QString currentPath = pathAt(current_);
    entries_.move(from, to);
    endMoveRows();
    current_ = indexOfPath(currentPath);
    emit currentChanged();
    if (sortMode_ != 0) { sortMode_ = 0; emit sortModeChanged(); }
    save();
    return true;
}
bool MacPlaylist::remove(int row) {
    if (row < 0 || row >= count()) return false;
    const QString currentPath = pathAt(current_);
    beginRemoveRows({}, row, row);
    entries_.removeAt(row);
    endRemoveRows();
    current_ = indexOfPath(currentPath);
    emit countChanged();
    emit currentChanged();
    save();
    return true;
}
void MacPlaylist::clear() {
    beginResetModel();
    entries_.clear();
    current_ = -1;
    endResetModel();
    emit countChanged();
    emit currentChanged();
    setStatus(QString());
    save();
}
void MacPlaylist::order() {
    auto by = [this](auto less) { std::stable_sort(entries_.begin(), entries_.end(), less); };
    switch (sortMode_) {
    case 1: by([](const Entry& a, const Entry& b) { return QString::localeAwareCompare(a.name, b.name) < 0; }); break;
    case 2: by([](const Entry& a, const Entry& b) { return QString::localeAwareCompare(a.name, b.name) > 0; }); break;
    case 3: by([](const Entry& a, const Entry& b) { return a.modified > b.modified; }); break;
    case 4: by([](const Entry& a, const Entry& b) { return a.modified < b.modified; }); break;
    case 5: by([](const Entry& a, const Entry& b) { return a.size > b.size; }); break;
    case 6: by([](const Entry& a, const Entry& b) { return a.size < b.size; }); break;
    default: break;
    }
}
void MacPlaylist::sortBy(int mode) {
    if (mode < 0 || mode > 6) return;
    const QString currentPath = pathAt(current_);
    beginResetModel();
    sortMode_ = mode;
    order();
    current_ = indexOfPath(currentPath);
    endResetModel();
    emit sortModeChanged();
    emit currentChanged();
    save();
}
bool MacPlaylist::save() {
    QJsonArray items;
    for (const auto& e : entries_) items.append(QJsonObject{{"path", e.path}});
    const QJsonObject root{{"version", 1}, {"sort", sortMode_}, {"autoAdvance", autoAdvance_}, {"entries", items}};
    QDir().mkpath(QFileInfo(store_).absolutePath());
    QFile f(store_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return true;
}
void MacPlaylist::load() {
    QFile f(store_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto root = QJsonDocument::fromJson(f.readAll()).object();
    sortMode_ = std::clamp(root.value("sort").toInt(), 0, 6);
    autoAdvance_ = root.value("autoAdvance").toBool(true);
    for (const auto v : root.value("entries").toArray()) {
        const QFileInfo info(v.toObject().value("path").toString());
        if (!info.isFile()) continue;
        entries_.push_back({info.absoluteFilePath(), info.fileName(), info.size(), info.lastModified(), {}});
    }
}
