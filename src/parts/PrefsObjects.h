#pragma once
// Helper QObjects exposed through the bridge's QObject* properties (updates,
// playlist, exportQueueModel, moonlight, xbox). Owner: Prefs agent.
#include <QAbstractListModel>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <functional>

// veyra.updates — the Windows build checks GitHub for portable updates. The Mac
// port is built from source, so "check" only opens the releases page.
class MacUpdateChecker : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(QVariantMap portable READ portable NOTIFY changed)
public:
    using QObject::QObject;
    bool checking() const { return false; }
    QString latestVersion() const { return {}; }
    QVariantMap portable() const;
    Q_INVOKABLE void check();
    Q_INVOKABLE void startAutomaticCheck() {}
    Q_INVOKABLE void openRelease();
signals:
    void changed();
};

// veyra.playlist — same API and roles as Windows' PlaylistModel.
class MacPlaylist : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentChanged)
    Q_PROPERTY(int sortMode READ sortMode NOTIFY sortModeChanged)
    Q_PROPERTY(bool autoAdvance READ autoAdvance WRITE setAutoAdvance NOTIFY autoAdvanceChanged)
public:
    enum Role { NameRole = Qt::UserRole + 1, PathRole, SizeRole, ModifiedRole, CurrentRole, StateRole };
    explicit MacPlaylist(const QString& storeFile, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(entries_.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return int(entries_.size()); }
    bool busy() const { return false; }
    QString status() const { return status_; }
    int currentIndex() const { return current_; }
    int sortMode() const { return sortMode_; }
    bool autoAdvance() const { return autoAdvance_; }
    void setAutoAdvance(bool on);
    // Called by the bridge: the playing path and its state ("playing", "ended", "failed").
    void setPlayback(const QString& path, const QString& state);
    Q_INVOKABLE void addPaths(const QStringList& paths);
    Q_INVOKABLE void addUrls(const QList<QUrl>& urls);
    Q_INVOKABLE QString pathAt(int row) const;
    Q_INVOKABLE int indexOfPath(const QString& path) const;
    Q_INVOKABLE bool play(int row);
    Q_INVOKABLE bool next();
    Q_INVOKABLE bool previous();
    Q_INVOKABLE bool move(int from, int to);
    Q_INVOKABLE bool remove(int row);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void sortBy(int mode);
    Q_INVOKABLE bool save();
    std::function<void(const QString&)> opener;   // the bridge's openPath
signals:
    void countChanged();
    void busyChanged();
    void statusChanged();
    void currentChanged();
    void sortModeChanged();
    void autoAdvanceChanged();
    void playRequested(const QString& path);
    void error(const QString& text);
private:
    struct Entry { QString path; QString name; qint64 size = 0; QDateTime modified; QString state; };
    void load();
    void setStatus(const QString& s);
    void setCurrent(int row);
    void order();
    QString store_;
    QList<Entry> entries_;
    QString status_;
    int current_ = -1;
    int sortMode_ = 0;
    bool autoAdvance_ = true;
};

// veyra.exportQueueModel — export with enhancement is Windows-only; an empty queue.
class MacExportQueue : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(qulonglong selectedId READ selectedId WRITE setSelectedId NOTIFY changed)
public:
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& = {}) const override { return 0; }
    QVariant data(const QModelIndex&, int) const override { return {}; }
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return 0; }
    qulonglong selectedId() const { return selected_; }
    void setSelectedId(qulonglong id) { if (selected_ != id) { selected_ = id; emit changed(); } }
    Q_INVOKABLE qulonglong addFile(const QString&, const QString& = {}) { return 0; }
    Q_INVOKABLE bool removeItem(qulonglong) { return false; }
    Q_INVOKABLE bool moveItem(qulonglong, int) { return false; }
    Q_INVOKABLE bool retryItem(qulonglong) { return false; }
    Q_INVOKABLE void clearWaiting() {}
    Q_INVOKABLE QVariantMap get(int) const { return {}; }
signals:
    void changed();
private:
    qulonglong selected_ = 0;
};

// veyra.moonlight / veyra.xbox — streaming clients are Windows-only; inert.
class MacStreamClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantList hosts READ empty NOTIFY changed)
    Q_PROPERTY(QVariantList apps READ empty NOTIFY changed)
    Q_PROPERTY(QVariantList consoles READ empty NOTIFY changed)
public:
    using QObject::QObject;
    QVariantMap state() const;
    QVariantList empty() const { return {}; }
    Q_INVOKABLE void load() {}
    Q_INVOKABLE void unload() {}
    Q_INVOKABLE void refresh() {}
    Q_INVOKABLE void cancel() {}
    Q_INVOKABLE void select(const QVariant& = {}) {}
    Q_INVOKABLE bool set(const QString&, const QVariant& = {}) { return false; }
    Q_INVOKABLE void addHost(const QString& = {}) {}
    Q_INVOKABLE void pair(const QVariant& = {}) {}
    Q_INVOKABLE void forget(const QVariant& = {}) {}
    Q_INVOKABLE void quitApp(const QVariant& = {}) {}
    Q_INVOKABLE bool connectStream(const QVariant& = {}) { return false; }
    Q_INVOKABLE void signIn() {}
    Q_INVOKABLE void signOut() {}
    Q_INVOKABLE void openSignInPage() {}
signals:
    void changed();
};
