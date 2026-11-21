// Part of MacBridge's class body (included inside the class). Owner: Playback agent.
// Rules: only override virtuals of MacBridgeBase and add private members/helpers.
// No Q_INVOKABLE / signals / Q_PROPERTY here (moc does not see this file).
public:
    void initPlayback();                          // called from the MacBridge constructor
    void attachPlayback(QQuickWindow* window);    // called once the QML window exists

    // --- opening ---------------------------------------------------------
    void openFileDialog() override;
    void openPath(const QString& path) override;
    void openDroppedUrls(const QList<QUrl>& urls) override;
    void openUrl(const QUrl& url) override;
    void openNetworkDialog() override;
    bool openNetworkAddress(const QString& address) override;
    void resumeLastSource() override;
    void showPlaylist() override;
    void addPlaylistFilesDialog() override;
    void addPlaylistFolderDialog() override;

    // --- transport -------------------------------------------------------
    void togglePlayPause() override;
    void stopPlayback() override;
    void seekTo(double seconds) override;
    void seekBy(double seconds) override;
    void stepFrame(int direction) override;
    void returnToLive() override;
    double timelineSeconds(double fraction) const override;
    QString formatTime(double seconds) const override;
    void holdOriginal(bool held) override;
    void rotatePictureClockwise() override;
    void setVolume(double v) override;
    void setMuted(bool v) override;
    void setPlaybackRate(double v) override;
    void setSelectedAudioTrack(int v) override;

    // --- recents ---------------------------------------------------------
    void refreshRecentFiles() override;
    void clearRecentFiles() override;

    // --- screenshots -----------------------------------------------------
    void takeScreenshot() override;
    void saveFrameDialog() override;
    void openScreenshotDirectory() override;
    void chooseScreenshotDirectory() override;

private:
    static QString mmss(double seconds);
    static bool isStillImagePath(const QString& path);
    void ensurePlayer();
    void detachPlayer();
    void openStill(const QString& path);
    void goToPlayerPage();
    void rememberFile(const QString& path);
    void loadRecents();
    void tickPlayback();
    void updateTracks();
    bool grabPicture(const QString& path);

    class QMediaPlayer* player_ = nullptr;
    class QAudioOutput* audioOut_ = nullptr;
    class QTimer* playbackTick_ = nullptr;
    QString playPath_;          // the file or URL the player owns ("" = none)
    bool playerOwnsStage_ = false;
    bool selfOpening_ = false;  // mediaOpenRequested raised by this part
    bool networkSource_ = false;
    bool firstFrameSeen_ = false;
