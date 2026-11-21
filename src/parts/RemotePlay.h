// Part of MacBridge's class body (included inside the class). Owner: RemotePlay (RP-CORE).
// Rules: only override virtuals of MacBridgeBase and add private members/helpers.
// No Q_INVOKABLE / signals / Q_PROPERTY here (moc does not see this file).
//
// PS5 Remote Play: Veyra's chiaki-ng based implementation (pairing, discovery,
// wake, saved pairings, streaming with controller/keyboard input), driving the
// original Ps5SourceDialog. Built when VEYRA_ENABLE_REMOTEPLAY is on; otherwise
// openPs5Dialog explains that this build has no Remote Play.
public:
    void initRemotePlay();                       // called from the MacBridge constructor
    void attachRemotePlay(QQuickWindow* window); // called once the QML window exists

    void openPs5Dialog() override;
    void ps5Load() override;
    bool ps5Set(const QString& key, const QVariant& value) override;
    void ps5SelectProfile(const QString& id) override;
    void ps5Scan() override;
    void ps5Pair(const QString& pin) override;
    bool ps5Connect() override;
    void ps5Wake() override;
    void ps5Forget() override;
    void ps5OpenAccountChecker() override;
    void ps5SendLoginPin(const QString& pin) override;
    void ps5Cancel() override;
    void ps5Calibrate() override;
    void ps5ControlSet(const QString& key, const QVariant& value) override;
    void ps5ControlRestart() override;

    bool remotePlayActive() const;              // a PS5 stream is opening or running
private:
    class RpState;                               // defined in RemotePlay.cpp (QObject child of the bridge)
    RpState* rp_ = nullptr;
    void rpPublish();                            // m_ps5 / m_ps5Profiles / m_remotePlayState → QML
    void rpTick();                               // 8 ms: input, audio; ~10 Hz: status
    void rpStopSession(bool clear);
    bool tagMinimalStage(QQuickWindow* window);
    void tagMinimalStageSoon(QQuickWindow* window);
