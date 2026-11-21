// Part of MacBridge's class body (included inside the class). Owner: Prefs agent.
// Rules: only override virtuals of MacBridgeBase and add private members/helpers.
// No Q_INVOKABLE / signals / Q_PROPERTY here (moc does not see this file).
public:
    void initPrefs();                          // called from the MacBridge constructor
    void attachPrefs(QQuickWindow* window);  // called once the QML window exists
private:
