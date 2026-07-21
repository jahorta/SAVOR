import QtQuick
import QtQuick3D

Item {
    id: root
    property bool showGrounds: true
    property bool showLinks: true
    property bool showRoute: true
    property bool showCollisions: true
    property bool showTriggers: true
    property bool showMovingObjects: true
    property bool showUnknowns: true

    property var groundMeshes: []
    property var linkMeshes: []
    property var routeMeshes: []
    property var collisionMeshes: []
    property var triggerMeshes: []
    property var movingObjectMeshes: []
    property var unknownMeshes: []

    property vector3d cameraTarget: Qt.vector3d(0, 0, 0)
    property real cameraDistance: 500
    // 0 = orbit only, 1 = set start, 2 = set goal.
    property int endpointMode: 0

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
                scale: Qt.vector3d(root.orbitDistance * 0.00015, root.orbitDistance * 0.00015, root.orbitDistance * 0.00015)
                materials: DefaultMaterial {
                    diffuseColor: "#FFD54A"
                    lighting: DefaultMaterial.NoLighting
                }
            }
        }

        Repeater3D {
            model: root.groundMeshes.length
            delegate: Model {
                property var mesh: (index >= 0 && index < root.groundMeshes.length)
                    ? root.groundMeshes[index]
                    : ({})
                property int surfaceIndex: Number(mesh.surfaceIndex)
                objectName: "groundSurface:" + surfaceIndex

                visible: root.showGrounds && (mesh.visible !== false)
                pickable: root.endpointMode !== 0
                geometry: mesh.geometry
                materials: DefaultMaterial {
                    diffuseColor: mesh.color
                    cullMode: Material.NoCulling
                    lighting: DefaultMaterial.NoLighting
                    opacity: 0.5
                }
            }
        }

        Repeater3D {
            model: root.linkMeshes.length
            delegate: Model {
                property var mesh: (index >= 0 && index < root.linkMeshes.length)
                    ? root.linkMeshes[index]
                    : ({})

                visible: root.showLinks && (mesh.visible !== false)
                pickable: false
                geometry: mesh.geometry
                materials: DefaultMaterial {
                    diffuseColor: mesh.color
                    cullMode: Material.NoCulling
                    lighting: DefaultMaterial.NoLighting
                }
            }
        }

        Repeater3D {
            model: root.routeMeshes.length
            delegate: Model {
                property var mesh: (index >= 0 && index < root.routeMeshes.length)
                    ? root.routeMeshes[index]
                    : ({})

                visible: root.showRoute && (mesh.visible !== false)
                pickable: false
                geometry: mesh.geometry
                materials: DefaultMaterial {
                    diffuseColor: mesh.color
                    cullMode: Material.NoCulling
                    lighting: DefaultMaterial.NoLighting
                }
            }
        }

        Repeater3D {
            model: root.collisionMeshes.length
            delegate: Model {
                property var mesh: (index >= 0 && index < root.collisionMeshes.length)
                    ? root.collisionMeshes[index]
                    : ({})

                visible: root.showCollisions && (mesh.visible !== false)
                pickable: false
                geometry: mesh.geometry
                materials: DefaultMaterial {
                    diffuseColor: mesh.color
                    cullMode: Material.NoCulling
                    opacity: 0.65
                }
            }
        }

        Repeater3D {
            model: root.triggerMeshes.length
            delegate: Model {
                property var mesh: (index >= 0 && index < root.triggerMeshes.length)
                    ? root.triggerMeshes[index]
                    : ({})
                property int regionIndex: Number(mesh.regionIndex)
                objectName: mesh.goalPickable === true
                    ? "triggerRegion:" + regionIndex
                    : ""

                visible: root.showTriggers && (mesh.visible !== false)
                pickable: root.endpointMode === 2 && mesh.goalPickable === true
                geometry: mesh.geometry
                materials: DefaultMaterial {
                    diffuseColor: mesh.color
                    cullMode: Material.NoCulling
                    opacity: 0.7
                }
            }
        }

        Repeater3D {
            model: root.movingObjectMeshes.length
            delegate: Model {
                property var mesh: (index >= 0 && index < root.movingObjectMeshes.length)
                    ? root.movingObjectMeshes[index]
                    : ({})

                visible: root.showMovingObjects && (mesh.visible !== false)
                pickable: false
                geometry: mesh.geometry
                materials: DefaultMaterial {
                    diffuseColor: mesh.color
                    cullMode: Material.NoCulling
                    opacity: 0.7
                }
            }
        }

        Repeater3D {
            model: root.unknownMeshes.length
            delegate: Model {
                property var mesh: (index >= 0 && index < root.unknownMeshes.length)
                    ? root.unknownMeshes[index]
                    : ({})

                visible: root.showUnknowns && (mesh.visible !== false)
                pickable: false
                geometry: mesh.geometry
                materials: DefaultMaterial {
                    diffuseColor: mesh.color
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
        property real pressX: 0
        property real pressY: 0
        property bool dragging: false
        readonly property real clickDragThreshold: 6

        onPressed: function(mouse) {
            pressX = mouse.x
            pressY = mouse.y
            lastX = mouse.x
            lastY = mouse.y
            dragging = false
        }

        onPositionChanged: function(mouse) {
            const dx = mouse.x - lastX
            const dy = mouse.y - lastY
            lastX = mouse.x
            lastY = mouse.y

            if (mouse.buttons & Qt.LeftButton) {
                const totalDx = mouse.x - pressX
                const totalDy = mouse.y - pressY
                if (!dragging && ((totalDx * totalDx) + (totalDy * totalDy)) >=
                        (clickDragThreshold * clickDragThreshold)) {
                    dragging = true
                }
                if (!dragging)
                    return
                root.orbitYaw -= dx * 0.28
                root.orbitPitch = root.clamp(root.orbitPitch - dy * 0.22, -89, 89)
            } else if ((mouse.buttons & Qt.RightButton) || (mouse.buttons & Qt.MiddleButton)) {
                root.panByPixels(dx, dy)
            }
        }

        onReleased: function(mouse) {
            if (mouse.button === Qt.LeftButton && !dragging && root.endpointMode !== 0) {
                const hit = sceneView.pick(mouse.x, mouse.y)
                const objectName = hit.objectHit ? hit.objectHit.objectName : ""
                if (objectName.indexOf("groundSurface:") === 0) {
                    const surfaceIndex = Number(objectName.substring("groundSurface:".length))
                    viewerWindow.handleGroundPick(
                        surfaceIndex,
                        hit.scenePosition.x,
                        hit.scenePosition.y,
                        hit.scenePosition.z)
                } else if (root.endpointMode === 2 &&
                           objectName.indexOf("triggerRegion:") === 0) {
                    const regionIndex = Number(objectName.substring("triggerRegion:".length))
                    viewerWindow.handleTriggerGoalPick(regionIndex)
                }
            }
            dragging = false
        }

        onCanceled: dragging = false

        onWheel: function(wheel) {
            const direction = wheel.angleDelta.y > 0 ? -1 : 1
            const nextDistance = root.orbitDistance * (1.0 + direction * 0.12)
            root.orbitDistance = Math.max(root.minOrbitDistance, nextDistance)
        }
    }
}
