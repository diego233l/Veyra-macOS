// Part of MacBridge's class body (included inside the class). Owner: Capture agent.
// Rules: only override virtuals of MacBridgeBase and add private members/helpers.
// No Q_INVOKABLE / signals / Q_PROPERTY here (moc does not see this file).
//
// Capture card / camera, screen and window capture and recording through Qt
// Multimedia (AVFoundation + ScreenCaptureKit on macOS). Remote-play sources,
// the virtual camera, the replay buffer and the virtual audio driver have no
// macOS implementation; their commands explain that instead of doing nothing.
public:
    void initCapture();                          // called from the MacBridge constructor
    void attachCapture(QQuickWindow* window);    // called once the QML window exists

    // --- capture card / camera ---------------------------------------------
    QVariantList captureDevices() const override;
    QString captureDeviceLabel() const override;
    void setCaptureDeviceId(QString v) override;
    QVariantList captureFormats() const override;
    QString captureFormatLabel() const override;
    void setCaptureFormatKey(QString v) override;
    QVariantList captureResolutionOptions() const override;
    void setCaptureResolutionId(QString v) override;
    QVariantList captureFrameRateOptions() const override;
    void setCaptureFrameRateId(QString v) override;
    QVariantList capturePixelOptions() const override;
    QString capturePixelId() const override;
    void setCapturePixelId(QString v) override;
    QVariantList captureAudioInputs() const override;
    void setCaptureAudioChoice(int v) override;
    void setCaptureFlipVertical(bool v) override;
    void refreshCaptureDevices() override;
    void openCaptureDialog() override;
    bool startCaptureSession() override;
    void resumeCaptureSession() override;
    bool hasCaptureSession() const override;
    QString captureSessionSummary() const override;
    bool openCaptureDriverSettings(bool inputSelector = false) override;
    void setupCaptureVirtualAudio(bool uninstall) override;
    void openCaptureWindowsListen() override;
    void openCaptureVirtualAudioVendor() override;

    // --- screen / window capture --------------------------------------------
    QVariantList screenTargets() const override;
    QString screenTargetLabel() const override;
    QVariantMap screenOptions() const override;
    bool setScreenOption(const QString& key, const QVariant& value) override;
    void refreshCaptureTargets() override;
    void openScreenCaptureDialog() override;
    bool startScreenCapture(bool cropPreview = false) override;
    bool applyScreenCrop(int left, int top, int right, int bottom) override;

    // --- recording -----------------------------------------------------------
    QString recordingDirectory() const override;
    int recordingBitrateMbps() const override;
    bool recordingHevc() const override;
    bool recordingAudio() const override;
    QVariantList microphoneDevices() const override;
    void refreshMicrophones() override;
    void startRecording(const QString& source = QString()) override;
    void stopRecording(const QString& source = QString()) override;
    void toggleRecording(const QString& source = QString()) override;
    void chooseRecordingDirectory() override;
    void openRecordingDirectory() override;

    // --- not available on macOS ------------------------------------------------
    void openMoonlightDialog() override;
    void openXboxDialog() override;
    void moonlightCapture(bool on) override;
    void setVirtualCameraEnabled(bool enabled) override;
    void installVirtualCamera() override;
    void uninstallVirtualCamera() override;
    void saveReplay(const QString& source = QString()) override;

private:
    struct CapFormat {
        QString key;          // stable id: index into the device's format list
        int width = 0, height = 0;
        double fps = 0;
        QString pixel;
        int index = -1;
    };
    void capLoadFormats();
    const CapFormat* capSelectedFormat() const;
    QString capRateId(double fps) const;
    void capPersist();
    void capStop(bool clear);
    bool capEnsureSession();
    void capAttachAudio(bool forRecording);
    void capWatchFirstFrame(const QString& kind, const QString& name);
    QVariant capPref(const char* key, const QVariant& fallback = {}) const;
    void recUpdate();

    class QMediaCaptureSession* capSession_ = nullptr;
    class QCamera* capCamera_ = nullptr;
    class QScreenCapture* capScreen_ = nullptr;
    class QWindowCapture* capWindow_ = nullptr;
    class QAudioInput* capAudioIn_ = nullptr;
    class QAudioOutput* capMonitor_ = nullptr;
    class QMediaRecorder* capRecorder_ = nullptr;
    class QTimer* capRecTimer_ = nullptr;
    QList<CapFormat> capFormats_;
    QString capActiveKind_;              // "capture" | "screen" | ""
    QString capActiveLabel_;
    bool capSelfOpening_ = false;        // our own mediaOpenRequested must not stop us
    QMetaObject::Connection capFrameConn_;
    QVariantMap capScreenOptions_;
