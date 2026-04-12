import QtQuick
import QtQuick3D

Item {
    id: root
    property bool showGrounds: true
    property bool showLinks: true
    property bool showCollisions: true
    property bool showTriggers: true
    property bool showUnknowns: true

    property var groundMeshes: []
    property var linkMeshes: []
    property var collisionMeshes: []
    property var triggerMeshes: []
    property var unknownMeshes: []

    property vector3d cameraTarget: Qt.vector3d(0, 0, 0)
    property real cameraDistance: 500

    property vector3d orbitCenter: Qt.vector3d(0, 0, 0)
    property real orbitDistance: 500
    property real orbitYaw: 0
    property real orbitPitch: -20

    readonly property real minOrbitDistance: 20

    onCameraTargetChanged: orbitCenter = cameraTarget
    onCameraDistanceChanged: orbitDistance = Math.max(minOrbitDistance, cameraDistance)

    function degToRad(degrees) {
        return degrees * (Math.PI / 180.0)
    }

    function clamp(value, low, high) {
        return Math.max(low, Math.min(high, value))
    }

    function vecAdd(a, b) {
        return Qt.vector3d(a.x + b.x, a.y + b.y, a.z + b.z)
    }

    function vecScale(v, s) {
        return Qt.vector3d(v.x * s, v.y * s, v.z * s)
    }

    function vecCross(a, b) {
        return Qt.vector3d(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x)
    }

    function vecLength(v) {
        return Math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
    }

    function vecNormalize(v) {
        const len = vecLength(v)
        if (len <= 0.0001)
            return Qt.vector3d(0, 0, 0)
        return vecScale(v, 1.0 / len)
    }

    function cameraForward() {
        const yaw = degToRad(orbitYaw)
        const pitch = degToRad(orbitPitch)
        const cp = Math.cos(pitch)
        const sp = Math.sin(pitch)
        const cy = Math.cos(yaw)
        const sy = Math.sin(yaw)

        return vecNormalize(Qt.vector3d(-sy * cp, sp, -cy * cp))
    }

    function panByPixels(dx, dy) {
        const forward = cameraForward()
        const worldUp = Qt.vector3d(0, 1, 0)
        const right = vecNormalize(vecCross(forward, worldUp))
        const up = vecNormalize(vecCross(right, forward))

        const panScale = Math.max(orbitDistance, minOrbitDistance) * 0.0025
        const panDelta = vecAdd(vecScale(right, -dx * panScale), vecScale(up, dy * panScale))
        orbitCenter = vecAdd(orbitCenter, panDelta)
    }

    Component.onCompleted: {
        orbitCenter = cameraTarget
        orbitDistance = Math.max(minOrbitDistance, cameraDistance)
    }

    View3D {
        id: sceneView
        anchors.fill: parent

        environment: SceneEnvironment {
            clearColor: "#050505"
            backgroundMode: SceneEnvironment.Color
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
            aoEnabled: false
        }

        Node {
            id: cameraPivot
            position: root.orbitCenter
            eulerRotation: Qt.vector3d(root.orbitPitch, root.orbitYaw, 0)

            PerspectiveCamera {
                id: camera
                position: Qt.vector3d(0, 0, root.orbitDistance)
                clipNear: 0.1
                clipFar: 500000
            }

            DirectionalLight {
                eulerRotation: Qt.vector3d(-5, 180, 0)
                brightness: 1.15
                ambientColor: Qt.rgba(0.30, 0.30, 0.30, 1.0)
            }
        }

        DirectionalLight {
            eulerRotation: Qt.vector3d(-45, -35, 0)
            brightness: 0.55
            ambientColor: Qt.rgba(0.20, 0.20, 0.20, 1.0)
        }

        Node {
            position: root.orbitCenter
            Model {
                source: "#Sphere"
                scale: Qt.vector3d(root.orbitDistance * 0.008, root.orbitDistance * 0.008, root.orbitDistance * 0.008)
                materials: DefaultMaterial {
                    diffuseColor: "#FFD54A"
                    lighting: DefaultMaterial.NoLighting
                }
            }
        }

        Repeater3D {
            model: root.groundMeshes
            delegate: Model {
                visible: root.showGrounds && (modelData.visible !== false)
                geometry: modelData.geometry
                materials: DefaultMaterial {
                    diffuseColor: modelData.color
                    cullMode: Material.NoCulling
                    lighting: DefaultMaterial.NoLighting
                    opacity: 0.5
                }
            }
        }

        Repeater3D {
            model: root.linkMeshes
            delegate: Model {
                visible: root.showLinks && (modelData.visible !== false)
                geometry: modelData.geometry
                materials: DefaultMaterial {
                    diffuseColor: modelData.color
                    cullMode: Material.NoCulling
                    lighting: DefaultMaterial.NoLighting
                }
            }
        }

        Repeater3D {
            model: root.collisionMeshes
            delegate: Model {
                visible: root.showCollisions && (modelData.visible !== false)
                geometry: modelData.geometry
                materials: DefaultMaterial {
                    diffuseColor: modelData.color
                    cullMode: Material.NoCulling
                    opacity: 0.65
                }
            }
        }

        Repeater3D {
            model: root.triggerMeshes
            delegate: Model {
                visible: root.showTriggers && (modelData.visible !== false)
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
                visible: root.showUnknowns && (modelData.visible !== false)
                geometry: modelData.geometry
                materials: DefaultMaterial {
                    diffuseColor: modelData.color
                    cullMode: Material.NoCulling
                    opacity: 0.7
                }
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        hoverEnabled: true

        property real lastX: 0
        property real lastY: 0

        onPressed: function(mouse) {
            lastX = mouse.x
            lastY = mouse.y
        }

        onPositionChanged: function(mouse) {
            const dx = mouse.x - lastX
            const dy = mouse.y - lastY
            lastX = mouse.x
            lastY = mouse.y

            if (mouse.buttons & Qt.LeftButton) {
                root.orbitYaw -= dx * 0.28
                root.orbitPitch = root.clamp(root.orbitPitch - dy * 0.22, -89, 89)
            } else if ((mouse.buttons & Qt.RightButton) || (mouse.buttons & Qt.MiddleButton)) {
                root.panByPixels(dx, dy)
            }
        }

        onWheel: function(wheel) {
            const direction = wheel.angleDelta.y > 0 ? -1 : 1
            const nextDistance = root.orbitDistance * (1.0 + direction * 0.12)
            root.orbitDistance = Math.max(root.minOrbitDistance, nextDistance)
        }
    }
}
