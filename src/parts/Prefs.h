// Part of MacBridge's class body (included inside the class). Owner: Prefs agent.
// Rules: only override virtuals of MacBridgeBase and add private members/helpers.
// No Q_INVOKABLE / signals / Q_PROPERTY here (moc does not see this file).
public:
    void initPrefs();                          // called from the MacBridge constructor
    void attachPrefs(QQuickWindow* window);    // called once the QML window exists

    // --- preferences / shell ---------------------------------------------
    bool setPreference(const QString& key, const QVariant& value) override;
    void setReducedMotion(bool v) override;
    void setDefaultPage(QString v) override;
    void setCurrentPage(QString v) override;
    void setInitialPage(QString v) override;
    QString initialPage() const override;
    QString defaultPageLabel() const override;
    QString uiLanguage() const override;
    QVariantMap shortcuts() const override;
    bool setShortcut(const QString& action, const QString& sequence) override;
    void resetShortcuts() override;
    void rememberWindowSize(int width, int height) override;
    QRect screenAvailableAt(int x, int y) const override;
    QRect launchScreenAvailable() const override;
    void openLogFolder() override;
    void openDataFolder() override;
    void openProjectPage() override;
    void openFeedbackPage() override;
    void openReleasesPage() override;
    bool openSponsorLink(const QString& id) override;
    QString diagnosticsReport() const override;
    void copyDiagnostics() override;
    void refreshComponentList() override;
    void refreshAudioDevices() override;
    void quit() override;
    void restartApplication() override;
    void restartForOverlayCompatibility() override { restartApplication(); }

    // --- presets ------------------------------------------------------------
    QVariantList presets() const override;
    QString currentPresetName() const override;
    QVariantList presetChoices() const override;
    QVariantList presetSaveParts() const override;
    int defaultPresetIndex() const override;
    bool applyPresetIndex(int index) override;
    bool savePresetAs(const QString& name, int contentsMask, bool nodeMode) override;
    bool deletePreset(int index) override;
    bool renamePreset(int index, const QString& name) override;
    bool duplicatePreset(int index) override;
    bool setDefaultPreset(int index) override;
    void importPresetDialog() override;
    bool importPreset(const QString& path) override;
    void exportPresetDialog(int index) override;
    bool exportPreset(int index, const QString& path) override;

    // Helpers other parts may use.
    QString prefString(const QString& key, const QString& fallback = {}) const;
    bool prefBool(const QString& key, bool fallback) const;
    int prefInt(const QString& key, int fallback) const;
    QObject* playlistObject() const { return m_playlist; }   // MacPlaylist (PrefsObjects.h)
private:
    bool savePrefs();
    void applyLanguage(bool notifyAll);
    QVariantMap presetValues() const;     // current enhancement values by property name
    bool savePresets();
    void loadPresets();
    QString prefsFile_;
    QString presetsFile_;
    QString initialPageOverride_;
    QVariantList presetStore_;            // [{name, nodeMode, contents, values}]
    int presetDefault_ = -1;
