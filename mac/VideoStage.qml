// The picture. MacBridge places this item under the page controls and keeps it on
// the QML item "videoHost". Owner of the shader chain: Enhance agent.
// Contract (properties other parts set):
//   stillSource : url   — non-empty shows a still image instead of video
//   bypass      : bool  — show the untouched picture (hold-to-compare)
//   pictureRotation : int (0/90/180/270)
// child objectName "macVideoOutput" : VideoOutput (its videoSink feeds players/captures)
import QtQuick
import QtMultimedia

Item {
    id: stage
    property url stillSource
    property bool bypass: false
    property int pictureRotation: 0
    clip: true
    Rectangle { anchors.fill: parent; color: "black" }
    VideoOutput {
        id: output
        objectName: "macVideoOutput"
        anchors.fill: parent
        visible: stage.stillSource.toString() === ""
        fillMode: VideoOutput.PreserveAspectFit
        orientation: stage.pictureRotation
    }
    Image {
        id: still
        objectName: "macImageOutput"
        anchors.fill: parent
        visible: stage.stillSource.toString() !== ""
        source: stage.stillSource
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        rotation: stage.pictureRotation
    }
}
