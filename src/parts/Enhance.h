// Part of MacBridge's class body (included inside the class). Owner: Enhance agent.
// Rules: only override virtuals of MacBridgeBase and add private members/helpers.
// No Q_INVOKABLE / signals / Q_PROPERTY here (moc does not see this file).
public:
    void initEnhance();                          // called from the MacBridge constructor
    void attachEnhance(QQuickWindow* window);  // called once the QML window exists
private:
