import QtQuick
import QtQuick3D
import QtQuick3D.Helpers

Item {
    id: root
    property bool showGrounds: true
    property bool showLinks: true
    property bool showTriggers: true
    property bool showUnknowns: true

    property var groundMeshes: []
    property var linkMeshes: []
    property var triggerMeshes: []
    property var unknownMeshes: []

    property vector3d cameraTarget: Qt.vector3d(0, 0, 0)
    property real cameraDistance: 500

    View3D {
        anchors.fill: parent

        environment: SceneEnvironment {
            clearColor: "#20252d"
            backgroundMode: SceneEnvironment.Color
        }

        Node {
            id: cameraPivot
            position: root.cameraTarget

            PerspectiveCamera {
                id: camera
                position: Qt.vector3d(0, root.cameraDistance * 0.35, root.cameraDistance)
                clipNear: 0.1
                clipFar: 500000
            }
        }

        DirectionalLight {
            eulerRotation: Qt.vector3d(-45, -35, 0)
            brightness: 1.2
        }

        DirectionalLight {
            eulerRotation: Qt.vector3d(45, 140, 0)
            brightness: 0.45
        }

        OrbitCameraController {
            camera: camera
            origin: cameraPivot
            xSpeed: 0.35
            ySpeed: 0.35
            xInvert: false
            yInvert: false
        }

        Repeater3D {
            model: root.groundMeshes
            delegate: Model {
                visible: root.showGrounds
                geometry: modelData.geometry
                materials: DefaultMaterial {
                    diffuseColor: modelData.color
                    cullMode: Material.NoCulling
                    lighting: DefaultMaterial.FragmentLighting
                }
            }
        }

        Repeater3D {
            model: root.linkMeshes
            delegate: Model {
                visible: root.showLinks
                geometry: modelData.geometry
                materials: DefaultMaterial {
                    diffuseColor: modelData.color
                    cullMode: Material.NoCulling
                    lighting: DefaultMaterial.NoLighting
                }
            }
        }

        Repeater3D {
            model: root.triggerMeshes
            delegate: Model {
                visible: root.showTriggers
                geometry: modelData.geometry
                materials: DefaultMaterial {
                    diffuseColor: modelData.color
                    cullMode: Material.NoCulling
                    opacity: 0.7
                }
            }
        }

        Repeater3D {
            model: root.unknownMeshes
            delegate: Model {
                visible: root.showUnknowns
                geometry: modelData.geometry
                materials: DefaultMaterial {
                    diffuseColor: modelData.color
                    cullMode: Material.NoCulling
                    opacity: 0.7
                }
            }
        }
    }
}
