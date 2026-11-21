// Part of MacBridge's class body (included inside the class). Owner: Enhance agent.
// Rules: only override virtuals of MacBridgeBase and add private members/helpers.
// No Q_INVOKABLE / signals / Q_PROPERTY here (moc does not see this file).
//
// The effect chain on macOS: colour (any number of layers, baked into one LUT),
// anti-aliasing (FXAA), spatial super resolution (bicubic + RCAS-style sharpening),
// texture / clarity / dehaze (colour node "effects" group, run in the shader).
// Everything that needs NVIDIA/AMD/Intel Windows runtimes is reported unavailable.
public:
    void initEnhance();                          // called from the MacBridge constructor
    void attachEnhance(QQuickWindow* window);    // called once the QML window exists

    // --- colour ---
    double colorExposure() const override;
    void setColorExposure(double v) override;
    double colorContrast() const override;
    void setColorContrast(double v) override;
    double colorSaturation() const override;
    void setColorSaturation(double v) override;
    double colorTemperature() const override;
    void setColorTemperature(double v) override;
    bool colorEnabled() const override;
    void setColorEnabled(bool v) override;
    QString colorStatus() const override;
    QVariantList colourParameters() const override;
    QVariantList colourGroups() const override;
    int selectedColourLayer() const override;
    void setSelectedColourLayer(int v) override;
    QVariantMap colourState() const override;
    QVariantMap colourStateAt(int index) const override;
    double colourParameter(const QString& name) const override;
    bool setColourParameter(const QString& name, double value) override;
    bool resetColourParameter(const QString& name) override;
    bool setColourCurve(int channel, const QVariantList& points) override;
    bool setColourWheel(int zone, double hue, double saturation, double luminance) override;
    bool setColourOption(const QString& name, int value) override;
    bool resetColourGroup(int group) override;
    bool colourCanUndo() const override;
    bool colourCanRedo() const override;
    bool colourCanPaste() const override;
    bool colourUndo() override;
    bool colourRedo() override;
    void colourCopy() override;
    bool colourPaste() override;
    QVariantList lutLibrary() const override;
    void importLutDialog() override;
    bool importLut(const QString& path) override;
    bool setColourLut(const QString& name) override;
    QVariantList colourLooks() const override;
    bool saveColourLook(const QString& name, bool replace) override;
    bool applyColourLook(int index) override;
    bool deleteColourLook(int index) override;
    void importColourLookDialog() override;
    bool importColourLook(const QString& path) override;
    void exportColourLookDialog(int index) override;
    bool exportColourLook(int index, const QString& path) override;

    // --- anti-aliasing / super resolution ---
    bool antiAliasEnabled() const override;
    void setAntiAliasEnabled(bool v) override;
    QVariantMap antiAliasParams() const override;
    QVariantMap antiAliasParamsAt(int index) const override;
    bool setAntiAliasParameter(const QString& key, double value) override;
    bool setAntiAliasParameterAt(int index, const QString& key, double value) override;
    bool srEnabled() const override;
    void setSrEnabled(bool v) override;
    QString srStageSummary() const override;
    QVariantList srBackendChoices() const override;
    QVariantList nrRuntimeChoices() const override;
    QVariantList fgBackendChoices() const override;

    // --- chain ---
    QVariantList chain() const override;
    QVariantList nodeConnections() const override;
    QVariantList effectCatalog() const override;
    QString chainError() const override;
    bool chainValid() const override;
    int nodeMode() const override;
    void setNodeMode(int v) override;
    QVariantMap effectCapabilities() const override;
    QVariantMap effectAvailability(const QString& id) const override;
    QVariantMap effectDescriptor(const QString& type) const override;
    int addEffect(const QString& type) override;
    bool removeEffect(int index) override;
    bool moveEffect(int from, int to) override;
    bool setEffectEnabled(int index, bool enabled) override;
    bool setEffectPosition(int index, double x, double y) override;
    int nodeIndexForId(uint id) const override;
    int duplicateNode(uint id) override;
    bool resetNode(uint id) override;
    bool connectNodes(uint from, uint to) override;
    bool disconnectNode(uint from) override;
    bool insertNodeAfter(uint id, uint after) override;
    void resetCurrentPage() override;

private:
    struct EnhNode {
        QString type;           // "color" | "aa" | "sr"
        uint id = 0;
        bool enabled = false;
        double x = 0, y = 0;
        QVariantMap colour;     // colour nodes: enh::defaultColour() shape
        QVariantMap aa;         // aa nodes: method, quality
        double srSharpness = 0.25;
    };
    struct EnhDocument {
        QVector<EnhNode> nodes;
        QHash<uint, uint> next;  // node mode wiring: id -> next id (1 = output)
        uint inputNext = 1;
        uint nextId = 2;
    };
    EnhDocument& enhDoc() { return enhDocs_[enhMode_]; }
    const EnhDocument& enhDoc() const { return enhDocs_[enhMode_]; }
    int colourIndex() const;
    bool commitColourState(const QVariantMap& state, bool chainLevel = false);
    QVector<int> enhOrder() const;           // node indices in processing order
    void enhChanged(bool chainLevel);        // emit signals + schedule stage update
    void pushEnhToStage();
    QString enhLutDirectory() const;
    QString enhLooksFile() const;
    QVariantList readLooks() const;
    bool writeLooks(const QVariantList& looks);
    uint enhAddNode(const QString& type);
    static QVariantMap aaMap(const EnhNode* node, int index);

    EnhDocument enhDocs_[2];
    int enhMode_ = 0;
    int selectedColour_ = -1;
    QVector<QPair<int, QVariantMap>> colourUndo_, colourRedo_;
    qint64 colourLastPushMs_ = 0;
    int colourLastIndex_ = -1;
    bool colourReplaying_ = false;
    QVariantMap colourClipboard_;
    class QTimer* enhTimer_ = nullptr;
    quint64 enhLutRevision_ = 0;
    class EnhLutProvider* enhProvider_ = nullptr;
