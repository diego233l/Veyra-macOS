// The picture. MacBridge places this item under the page controls and keeps it on
// the QML item "videoHost". Owner of the shader chain: the Enhance part.
// Contract (properties other parts set):
//   stillSource : url   — non-empty shows a still image instead of video
//   bypass      : bool  — show the untouched picture (hold-to-compare)
//   pictureRotation : int (0/90/180/270)
// child objectName "macVideoOutput" : VideoOutput (its videoSink feeds players/captures)
// Enhancement properties (set by MacBridge's Enhance part): lutUrl, lutEnabled,
// fxaa, fxaaQuality, sharpen, clarity, dehaze, sr, srSharpness.
//
// The source is rendered at its NATIVE size into a texture and the shader draws
// it aspect-fitted to the stage, so the spatial filters (FXAA, sharpen) work on
// real source pixels and the "super resolution" pass genuinely upscales.
import QtQuick
import QtMultimedia

Item {
    id: stage
    property url stillSource
    property bool bypass: false
    property int pictureRotation: 0

    property url lutUrl
    property bool lutEnabled: false
    property int lutSize: 33
    property bool fxaa: false
    property real fxaaQuality: 0.5
    property real sharpen: 0
    property real clarity: 0
    property real dehaze: 0
    property bool sr: false
    property real srSharpness: 0.25

    readonly property bool stillMode: stillSource.toString() !== ""
    readonly property real nativeWidth: stillMode ? still.implicitWidth : output.sourceRect.width
    readonly property real nativeHeight: stillMode ? still.implicitHeight : output.sourceRect.height
    readonly property bool hasPicture: nativeWidth > 1 && nativeHeight > 1
    clip: true

    Rectangle { anchors.fill: parent; color: "black" }

    VideoOutput {
        id: output
        objectName: "macVideoOutput"
        x: 0; y: 0
        // Native size, so the texture below carries real source pixels. Before the
        // first frame it fills the stage (sourceRect is still empty).
        width: stage.stillMode || output.sourceRect.width < 2 ? stage.width : output.sourceRect.width
        height: stage.stillMode || output.sourceRect.height < 2 ? stage.height : output.sourceRect.height
        fillMode: VideoOutput.Stretch
        visible: !stage.stillMode
    }
    Image {
        id: still
        objectName: "macImageOutput"
        x: 0; y: 0
        width: implicitWidth; height: implicitHeight
        visible: stage.stillMode
        source: stage.stillSource
        fillMode: Image.Stretch
        asynchronous: true
        smooth: true
    }
    ShaderEffectSource {
        id: frame
        sourceItem: stage.stillMode ? still : output
        hideSource: true
        live: true
        smooth: true
        visible: false
        textureSize: stage.hasPicture ? Qt.size(stage.nativeWidth, stage.nativeHeight) : Qt.size(0, 0)
    }
    Image {
        id: lutImage
        source: stage.lutUrl
        visible: false
        cache: false
        smooth: true
        mipmap: false
    }

    ShaderEffect {
        id: fx
        readonly property bool turned: (stage.pictureRotation % 180) !== 0
        readonly property real fitScale: !stage.hasPicture ? 1 :
            Math.min(stage.width / (turned ? stage.nativeHeight : stage.nativeWidth),
                     stage.height / (turned ? stage.nativeWidth : stage.nativeHeight))
        visible: stage.hasPicture
        width: stage.nativeWidth * fitScale
        height: stage.nativeHeight * fitScale
        anchors.centerIn: parent
        rotation: stage.pictureRotation

        property variant source: frame
        property variant lut: lutImage
        property point srcTexel: Qt.point(1 / Math.max(1, stage.nativeWidth), 1 / Math.max(1, stage.nativeHeight))
        property real lutEnabled: stage.lutEnabled && lutImage.status === Image.Ready ? 1 : 0
        property real lutSize: stage.lutSize
        property real fxaa: stage.fxaa ? 1 : 0
        property real fxaaQuality: stage.fxaaQuality
        property real sharpen: stage.sharpen
        property real clarity: stage.clarity
        property real dehaze: stage.dehaze
        property real sr: stage.sr && fitScale > 1.01 ? 1 : 0
        property real srSharpness: stage.srSharpness
        property real bypass: stage.bypass ? 1 : 0
        fragmentShader: "qrc:/shaders/enhance.frag.qsb"
    }
}
