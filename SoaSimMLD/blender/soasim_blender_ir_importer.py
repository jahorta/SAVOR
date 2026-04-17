"""SoaSim Blender IR JSON importer (baseline).

This importer targets the current JSON payload exported by SoaSimMLD's
BlenderIrJsonExporter. It intentionally focuses on currently exported fields:
- vertex positions
- triangle corner vertex indices
- material metadata + textureName
- texture pixelDataBase64 (rgba8)
- indexEntries with transform + meshIndices
"""

from __future__ import annotations

import base64
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import bpy
import mathutils
from bpy.props import BoolProperty, StringProperty
from bpy.types import Collection, Image, Material, Mesh, Object
from bpy_extras.io_utils import ImportHelper

bl_info = {
    "name": "SoaSim Blender IR Importer",
    "author": "SoaSim",
    "version": (0, 1, 0),
    "blender": (3, 6, 0),
    "location": "File > Import > SoaSim Blender IR (.json)",
    "description": "Imports blender_ir_scene.json exported from SoaSimMLD",
    "category": "Import-Export",
}


@dataclass
class ImportStats:
    mesh_count: int = 0
    object_count: int = 0
    texture_count: int = 0
    material_count: int = 0
    warnings: int = 0
    warning_messages: list[str] = field(default_factory=list)

    def add_warning(self, message: str) -> None:
        self.warnings += 1
        self.warning_messages.append(message)
        print(f"[SoaSim Import Warning] {message}")


BLENDER_CUSTOM_INT_MIN = -(2**31)
BLENDER_CUSTOM_INT_MAX = (2**31) - 1
NJCM_TO_BLENDER_AXIS = mathutils.Quaternion((1.0, 0.0, 0.0), 1.5707963267948966)


def _read_int(value: Any, *, field_name: str) -> int:
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"Invalid integer for {field_name}: {value!r}") from exc


def _set_custom_int_property(
    owner: Any,
    key: str,
    raw_value: Any,
    stats: ImportStats,
    *,
    field_name: str,
) -> None:
    value = _read_int(raw_value, field_name=field_name)
    if BLENDER_CUSTOM_INT_MIN <= value <= BLENDER_CUSTOM_INT_MAX:
        owner[key] = value
        return

    owner[key] = str(value)
    stats.add_warning(
        (
            f"{field_name}={value} does not fit Blender custom int range "
            f"[{BLENDER_CUSTOM_INT_MIN}, {BLENDER_CUSTOM_INT_MAX}] "
            f"and was stored as string property '{key}'."
        )
    )


def _ensure_collection(name: str, parent: Collection | None = None) -> Collection:
    collection = bpy.data.collections.get(name)
    if collection is None:
        collection = bpy.data.collections.new(name)
        if parent is None:
            bpy.context.scene.collection.children.link(collection)
        else:
            parent.children.link(collection)
    return collection


def _clear_collection(collection: Collection) -> None:
    for obj in list(collection.objects):
        bpy.data.objects.remove(obj, do_unlink=True)


def _parse_json(path: str) -> dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        payload = json.load(handle)

    for required_key in ("meshes", "indexEntries", "textures"):
        if required_key not in payload:
            raise ValueError(f"Missing required top-level key: {required_key}")
    if "objectTrees" not in payload:
        payload["objectTrees"] = []

    return payload


def _decode_rgba8_image(texture: dict[str, Any], image_name: str) -> Image | None:
    width = int(texture.get("width", 0))
    height = int(texture.get("height", 0))
    pixel_format = str(texture.get("pixelFormat", "")).lower()
    encoded_pixels = texture.get("pixelDataBase64", "")

    if width <= 0 or height <= 0:
        return None
    if pixel_format != "rgba8":
        return None
    if not encoded_pixels:
        return None

    raw = base64.b64decode(encoded_pixels)
    expected_size = width * height * 4
    if len(raw) != expected_size:
        return None

    image = bpy.data.images.get(image_name)
    if image is None:
        image = bpy.data.images.new(name=image_name, width=width, height=height, alpha=True)
    else:
        image.scale(width, height)

    float_pixels = [channel / 255.0 for channel in raw]
    image.pixels = float_pixels
    image.pack()

    return image


def _build_texture_lookup(textures: list[dict[str, Any]], stats: ImportStats) -> dict[str, Image]:
    images_by_name: dict[str, Image] = {}

    for index, texture in enumerate(textures):
        texture_name = str(texture.get("textureName", "")).strip() or f"texture_{index:04d}"
        image = _decode_rgba8_image(texture, image_name=texture_name)
        if image is None:
            stats.warnings += 1
            continue

        image["soasim_texture_name"] = texture_name
        _set_custom_int_property(
            image,
            "soasim_source_offset",
            texture.get("sourceOffset", 0),
            stats,
            field_name=f"textures[{index}].sourceOffset",
        )
        _set_custom_int_property(
            image,
            "soasim_source_size",
            texture.get("sourceSize", 0),
            stats,
            field_name=f"textures[{index}].sourceSize",
        )
        image["soasim_encoded_format"] = str(texture.get("encodedFormat", ""))
        images_by_name[texture_name] = image
        stats.texture_count += 1

    return images_by_name


def _build_material(
    material_data: dict[str, Any],
    texture_lookup: dict[str, Image],
    stats: ImportStats,
    *,
    mesh_field_name: str,
    material_index: int,
) -> Material:
    material_hash = int(material_data.get("materialHash", 0))
    name = f"SoaMat_{material_hash:016x}"
    material = bpy.data.materials.get(name)
    if material is None:
        material = bpy.data.materials.new(name=name)

    material.use_nodes = True
    _set_custom_int_property(
        material,
        "soasim_poly_type",
        material_data.get("polyType", 0),
        stats,
        field_name=f"{mesh_field_name}.materials[{material_index}].polyType",
    )
    _set_custom_int_property(
        material,
        "soasim_chunk_flags",
        material_data.get("chunkFlags", 0),
        stats,
        field_name=f"{mesh_field_name}.materials[{material_index}].chunkFlags",
    )
    _set_custom_int_property(
        material,
        "soasim_material_state_key",
        material_data.get("materialStateKey", 0),
        stats,
        field_name=f"{mesh_field_name}.materials[{material_index}].materialStateKey",
    )
    _set_custom_int_property(
        material,
        "soasim_texture_id",
        material_data.get("textureId", 0),
        stats,
        field_name=f"{mesh_field_name}.materials[{material_index}].textureId",
    )
    material["soasim_texture_name"] = str(material_data.get("textureName", ""))

    node_tree = material.node_tree
    assert node_tree is not None
    nodes = node_tree.nodes
    links = node_tree.links

    bsdf = nodes.get("Principled BSDF")
    if bsdf is None:
        bsdf = nodes.new(type="ShaderNodeBsdfPrincipled")

    output = nodes.get("Material Output")
    if output is None:
        output = nodes.new(type="ShaderNodeOutputMaterial")

    if not any(link.from_node == bsdf and link.to_node == output for link in links):
        links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])

    texture_name = str(material_data.get("textureName", "")).strip()
    if texture_name and texture_name in texture_lookup:
        image_node = None
        for node in nodes:
            if node.type == "TEX_IMAGE" and node.name == "SoaTexture":
                image_node = node
                break
        if image_node is None:
            image_node = nodes.new(type="ShaderNodeTexImage")
            image_node.name = "SoaTexture"

        image_node.image = texture_lookup[texture_name]
        if not any(link.from_node == image_node and link.to_node == bsdf for link in links):
            links.new(image_node.outputs["Color"], bsdf.inputs["Base Color"])

    return material


def _triangles_from_corners(corners: list[Any]) -> list[tuple[int, int, int]]:
    triangles: list[tuple[int, int, int]] = []
    for i in range(0, len(corners) - 2, 3):
        a = int(corners[i])
        b = int(corners[i + 1])
        c = int(corners[i + 2])
        triangles.append((a, b, c))
    return triangles


def _build_mesh(mesh_data: dict[str, Any], texture_lookup: dict[str, Image], stats: ImportStats) -> Object:
    mesh_name = str(mesh_data.get("label", "SoaMesh"))
    mesh_field_name = f"meshes[{mesh_name}]"

    vertices_data = mesh_data.get("vertices", [])
    vertices = []
    for vertex in vertices_data:
        pos = vertex.get("position", [0.0, 0.0, 0.0])
        source_position = mathutils.Vector((float(pos[0]), float(pos[1]), float(pos[2])))
        blender_position = NJCM_TO_BLENDER_AXIS @ source_position
        vertices.append((blender_position.x, blender_position.y, blender_position.z))

    triangles: list[tuple[int, int, int]] = []
    poly_material_indices: list[int] = []
    for tri_set in mesh_data.get("triangleSets", []):
        local_triangles = _triangles_from_corners(tri_set.get("corners", []))
        material_index = int(tri_set.get("materialIndex", 0))
        for tri in local_triangles:
            triangles.append(tri)
            poly_material_indices.append(material_index)

    mesh = bpy.data.meshes.new(mesh_name)
    mesh.from_pydata(vertices, [], triangles)
    mesh.validate(verbose=False)
    mesh.update()

    material_slots: list[Material] = []
    for material_index, material_data in enumerate(mesh_data.get("materials", [])):
        material_slots.append(
            _build_material(
                material_data,
                texture_lookup,
                stats,
                mesh_field_name=mesh_field_name,
                material_index=material_index,
            )
        )

    for material in material_slots:
        mesh.materials.append(material)

    for poly_index, poly in enumerate(mesh.polygons):
        if poly_index >= len(poly_material_indices):
            continue
        mat_index = poly_material_indices[poly_index]
        if 0 <= mat_index < len(mesh.materials):
            poly.material_index = mat_index
        else:
            stats.warnings += 1

    diagnostics = mesh_data.get("diagnostics", {})
    _set_custom_int_property(
        mesh,
        "soasim_source_object_address",
        mesh_data.get("sourceObjectAddress", 0),
        stats,
        field_name=f"{mesh_field_name}.sourceObjectAddress",
    )
    _set_custom_int_property(
        mesh,
        "soasim_source_chunk_offset",
        mesh_data.get("sourceChunkOffset", 0),
        stats,
        field_name=f"{mesh_field_name}.sourceChunkOffset",
    )
    _set_custom_int_property(
        mesh,
        "soasim_source_attach_offset",
        mesh_data.get("sourceAttachOffset", 0),
        stats,
        field_name=f"{mesh_field_name}.sourceAttachOffset",
    )
    _set_custom_int_property(
        mesh,
        "soasim_diag_degenerate",
        diagnostics.get("degenerateTriangleCount", 0),
        stats,
        field_name=f"{mesh_field_name}.diagnostics.degenerateTriangleCount",
    )
    _set_custom_int_property(
        mesh,
        "soasim_diag_out_of_range",
        diagnostics.get("outOfRangeIndexCount", 0),
        stats,
        field_name=f"{mesh_field_name}.diagnostics.outOfRangeIndexCount",
    )
    _set_custom_int_property(
        mesh,
        "soasim_diag_cache_replay",
        diagnostics.get("cacheReplayTriangleCount", 0),
        stats,
        field_name=f"{mesh_field_name}.diagnostics.cacheReplayTriangleCount",
    )

    obj = bpy.data.objects.new(mesh_name, mesh)
    stats.mesh_count += 1
    stats.material_count += len(material_slots)
    return obj


def _apply_transform(obj: Object, transform: dict[str, Any]) -> None:
    position = transform.get("position", [0.0, 0.0, 0.0])
    quat = transform.get("rotation", [0.0, 0.0, 0.0, 1.0])
    scale = transform.get("scale", [1.0, 1.0, 1.0])

    source_position = mathutils.Vector((float(position[0]), float(position[1]), float(position[2])))
    blender_position = NJCM_TO_BLENDER_AXIS @ source_position
    obj.location = (blender_position.x, blender_position.y, blender_position.z)

    source_rotation = mathutils.Quaternion(
        (float(quat[3]), float(quat[0]), float(quat[1]), float(quat[2]))
    )
    blender_rotation = NJCM_TO_BLENDER_AXIS @ source_rotation @ NJCM_TO_BLENDER_AXIS.conjugated()
    obj.rotation_mode = "QUATERNION"
    obj.rotation_quaternion = blender_rotation
    obj.scale = (float(scale[0]), float(scale[1]), float(scale[2]))


def _create_empty(name: str) -> Object:
    return bpy.data.objects.new(name, None)


def import_blender_ir_json(
    json_path: str,
    clear_target_collection: bool,
    target_collection_name: str,
) -> ImportStats:
    payload = _parse_json(json_path)
    stats = ImportStats()

    root_collection = _ensure_collection(target_collection_name)
    source_collection = _ensure_collection(f"{target_collection_name}_SourceMeshes", parent=root_collection)

    if clear_target_collection:
        _clear_collection(root_collection)
        _clear_collection(source_collection)

    texture_lookup = _build_texture_lookup(payload.get("textures", []), stats)

    mesh_objects: list[Object] = []
    for mesh_data in payload.get("meshes", []):
        mesh_obj = _build_mesh(mesh_data, texture_lookup, stats)
        source_collection.objects.link(mesh_obj)
        mesh_obj.hide_viewport = True
        mesh_obj.hide_render = True
        mesh_objects.append(mesh_obj)

    object_trees: list[dict[str, Any]] = payload.get("objectTrees", [])

    for entry in payload.get("indexEntries", []):
        transform = entry.get("transform", {})
        entry_id = int(entry.get("sourceEntryId", 0))
        fxn_name = str(entry.get("fxnName", ""))
        entry_root = _create_empty(f"SoaEntry_{entry_id}")
        _apply_transform(entry_root, transform)
        _set_custom_int_property(
            entry_root,
            "soasim_source_entry_id",
            entry_id,
            stats,
            field_name=f"indexEntries[{entry_id}].sourceEntryId",
        )
        _set_custom_int_property(
            entry_root,
            "soasim_tbl_id",
            entry.get("tblId", 0),
            stats,
            field_name=f"indexEntries[{entry_id}].tblId",
        )
        entry_root["soasim_fxn_name"] = fxn_name
        entry_root["soasim_object_addresses"] = ",".join(
            str(int(v)) for v in entry.get("objectAddresses", [])
        )
        root_collection.objects.link(entry_root)
        stats.object_count += 1

        tree_indices = entry.get("objectTreeIndices", [])
        if not tree_indices:
            mesh_indices = entry.get("meshIndices", [])
            for slot, mesh_index in enumerate(mesh_indices):
                mi = int(mesh_index)
                if mi < 0 or mi >= len(mesh_objects):
                    stats.warnings += 1
                    continue
                source_obj = mesh_objects[mi]
                instance_name = f"SoaInst_{entry_id}_{slot}_{source_obj.name}"
                instance_obj = bpy.data.objects.new(instance_name, source_obj.data)
                instance_obj.parent = entry_root
                instance_obj["soasim_mesh_index"] = mi
                root_collection.objects.link(instance_obj)
                stats.object_count += 1
            continue

        for slot, tree_index in enumerate(tree_indices):
            ti = int(tree_index)
            if ti < 0 or ti >= len(object_trees):
                stats.warnings += 1
                continue

            tree = object_trees[ti]
            tree_root_name = (
                f"SoaTree_{entry_id}_{slot}_"
                f"{int(tree.get('sourceObjectAddress', 0))}_"
                f"{int(tree.get('sourceChunkOffset', 0))}"
            )
            tree_root = _create_empty(tree_root_name)
            tree_root.parent = entry_root
            _set_custom_int_property(
                tree_root,
                "soasim_tree_index",
                ti,
                stats,
                field_name=f"indexEntries[{entry_id}].objectTreeIndices[{slot}]",
            )
            _set_custom_int_property(
                tree_root,
                "soasim_source_object_address",
                tree.get("sourceObjectAddress", 0),
                stats,
                field_name=f"objectTrees[{ti}].sourceObjectAddress",
            )
            _set_custom_int_property(
                tree_root,
                "soasim_source_chunk_offset",
                tree.get("sourceChunkOffset", 0),
                stats,
                field_name=f"objectTrees[{ti}].sourceChunkOffset",
            )
            root_collection.objects.link(tree_root)
            stats.object_count += 1

            nodes = tree.get("nodes", [])
            node_objects: list[Object | None] = [None] * len(nodes)
            for node_idx, node in enumerate(nodes):
                node_name = f"{tree_root_name}_Node_{node_idx}"
                node_obj = _create_empty(node_name)
                _apply_transform(node_obj, node.get("localTransform", {}))
                _set_custom_int_property(
                    node_obj,
                    "soasim_node_index",
                    node_idx,
                    stats,
                    field_name=f"objectTrees[{ti}].nodes[{node_idx}].nodeIndex",
                )
                _set_custom_int_property(
                    node_obj,
                    "soasim_source_node_offset",
                    node.get("sourceNodeOffset", 0),
                    stats,
                    field_name=f"objectTrees[{ti}].nodes[{node_idx}].sourceNodeOffset",
                )
                _set_custom_int_property(
                    node_obj,
                    "soasim_source_eval_flags",
                    node.get("sourceEvalFlags", 0),
                    stats,
                    field_name=f"objectTrees[{ti}].nodes[{node_idx}].sourceEvalFlags",
                )
                _set_custom_int_property(
                    node_obj,
                    "soasim_source_attach_offset",
                    node.get("sourceAttachOffset", 0),
                    stats,
                    field_name=f"objectTrees[{ti}].nodes[{node_idx}].sourceAttachOffset",
                )
                root_collection.objects.link(node_obj)
                node_objects[node_idx] = node_obj
                stats.object_count += 1

            for node_idx, node in enumerate(nodes):
                node_obj = node_objects[node_idx]
                if node_obj is None:
                    continue

                parent_idx = node.get("parentNodeIndex")
                if parent_idx is None:
                    node_obj.parent = tree_root
                else:
                    pi = int(parent_idx)
                    if 0 <= pi < len(node_objects) and node_objects[pi] is not None:
                        node_obj.parent = node_objects[pi]
                    else:
                        node_obj.parent = tree_root
                        stats.warnings += 1

                mesh_index = node.get("meshIndex")
                if mesh_index is None:
                    continue
                mi = int(mesh_index)
                if mi < 0 or mi >= len(mesh_objects):
                    stats.add_warning(
                        f"Invalid mesh index {mi} at objectTrees[{ti}].nodes[{node_idx}].meshIndex."
                    )
                    continue

                source_obj = mesh_objects[mi]
                attach_name = f"{node_obj.name}_{source_obj.name}"
                attach_obj = bpy.data.objects.new(attach_name, source_obj.data)
                attach_obj.parent = node_obj
                attach_obj["soasim_mesh_index"] = mi
                root_collection.objects.link(attach_obj)
                stats.object_count += 1

    return stats


class IMPORT_SCENE_OT_soasim_blender_ir(bpy.types.Operator, ImportHelper):
    bl_idname = "import_scene.soasim_blender_ir"
    bl_label = "Import SoaSim Blender IR"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".json"
    filter_glob: StringProperty(default="*.json", options={"HIDDEN"})

    target_collection_name: StringProperty(
        name="Collection",
        default="SoaSim_Imported",
        description="Collection to place imported SoaSim objects",
    )

    clear_target_collection: BoolProperty(
        name="Clear Target Collection",
        default=False,
        description="Delete existing objects in target collections before import",
    )

    def execute(self, context: bpy.types.Context) -> set[str]:
        del context
        json_path = str(Path(self.filepath))

        try:
            stats = import_blender_ir_json(
                json_path=json_path,
                clear_target_collection=self.clear_target_collection,
                target_collection_name=self.target_collection_name,
            )
        except Exception as exc:  # Blender operator-level error boundary
            self.report({"ERROR"}, f"SoaSim import failed: {exc}")
            return {"CANCELLED"}

        for warning in stats.warning_messages[:5]:
            self.report({"WARNING"}, warning)
        if len(stats.warning_messages) > 5:
            self.report(
                {"WARNING"},
                f"{len(stats.warning_messages) - 5} additional warnings written to the Blender console.",
            )

        self.report(
            {"INFO"},
            (
                "SoaSim import complete: "
                f"meshes={stats.mesh_count}, objects={stats.object_count}, "
                f"textures={stats.texture_count}, materials={stats.material_count}, "
                f"warnings={stats.warnings}"
            ),
        )
        return {"FINISHED"}


def menu_func_import(self: bpy.types.TOPBAR_MT_file_import, _context: bpy.types.Context) -> None:
    self.layout.operator(
        IMPORT_SCENE_OT_soasim_blender_ir.bl_idname,
        text="SoaSim Blender IR (.json)",
    )


CLASSES = (
    IMPORT_SCENE_OT_soasim_blender_ir,
)


def register() -> None:
    for klass in CLASSES:
        bpy.utils.register_class(klass)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)


def unregister() -> None:
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    for klass in reversed(CLASSES):
        bpy.utils.unregister_class(klass)


if __name__ == "__main__":
    register()
