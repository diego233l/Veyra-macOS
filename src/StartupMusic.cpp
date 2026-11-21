#include "StartupMusic.h"
#include "veyra/Log.h"
#include <QAudioOutput>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QUrl>

StartupMusic::StartupMusic(const QString& path, QObject* parent) : QObject(parent), path_(path) {
    available_ = !path.isEmpty() && QFileInfo::exists(path);
}
void StartupMusic::setState(const QString& s) {
    if (state_ == s) return;
    state_ = s;
    emit stateChanged();
}
void StartupMusic::setMuted(bool m) {
    if (muted_ == m) return;
    muted_ = m;
    if (output_) output_->setMuted(m);
    emit mutedChanged();
}
void StartupMusic::prepare(double volume) {
    if (!available_ || player_) return;
    player_ = new QMediaPlayer(this);
    output_ = new QAudioOutput(this);
    output_->setVolume(float(volume));
    output_->setMuted(muted_);
    player_->setAudioOutput(output_);
    player_->setSource(QUrl::fromLocalFile(path_));
    connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus s) {
        if (s == QMediaPlayer::EndOfMedia) { finished_ = true; setState(QStringLiteral("ended")); }
    });
    setState(QStringLiteral("loading"));
}
void StartupMusic::play() {
    if (!available_ || finished_) return;
    if (!player_) prepare(0.6);
    player_->play();
    setState(QStringLiteral("playing"));
}
void StartupMusic::pause() {
    if (!player_) { finished_ = true; setState(QStringLiteral("paused")); return; }
    player_->stop();
    finished_ = true;   // once per launch: a stopped intro never comes back
    setState(QStringLiteral("paused"));
}
double StartupMusic::position() const { return player_ && state_ == QStringLiteral("playing") ? player_->position() / 1000.0 : -1.0; }
