#pragma once
// The 日落 startup music, played once per launch (same contract as Windows' StartupMusic).
#include <QObject>
#include <QString>
class QMediaPlayer;
class QAudioOutput;

class StartupMusic : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
public:
    explicit StartupMusic(const QString& path, QObject* parent = nullptr);
    bool available() const { return available_; }
    QString state() const { return state_; }
    bool muted() const { return muted_; }
    void setMuted(bool muted);
    Q_INVOKABLE void prepare(double volume);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE double position() const;
signals:
    void stateChanged();
    void mutedChanged();
private:
    void setState(const QString& s);
    QString path_;
    bool available_ = false;
    bool muted_ = false;
    bool finished_ = false;
    QString state_ = QStringLiteral("idle");
    QMediaPlayer* player_ = nullptr;
    QAudioOutput* output_ = nullptr;
};
