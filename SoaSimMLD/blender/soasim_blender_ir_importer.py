"""SoaSim Blender IR JSON importer (baseline).

This importer targets the current JSON payload exported by SoaSimMLD's
BlenderIrJsonExporter. It intentionally focuses on currently exported fields:
- vertex positions
- triangle corner vertex indices
- material metadata + textureName
- texture pixelDataBase64 (rgba8)
- indexEntries with transform + meshIndices, including decoded GRND meshes
"""

from __future__ import annotations

import base64
import json
import math
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, NamedTuple

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
    debug_lines: int = 0

    def add_warning(self, message: str) -> None:
        self.warnings += 1
        self.warning_messages.append(message)
        print(f"[SoaSim Import Warning] {message}")


class TextureLookup(NamedTuple):
    by_name: dict[str, Image]
    by_id: dict[int, Image]


BLENDER_CUSTOM_INT_MIN = -(2**31)
BLENDER_CUSTOM_INT_MAX = (2**31) - 1
NJCM_TO_BLENDER_AXIS = mathutils.Quaternion((1.0, 0.0, 0.0), 1.5707963267948966)
NJD_EVAL_UNIT_POS = 1 << 0
NJD_EVAL_UNIT_ANG = 1 << 1
NJD_EVAL_UNIT_SCL = 1 << 2
GRND_COLLISION_VISUAL_SOURCE_Y_OFFSET = 0.05


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


def _flip_rgba8_vertically(raw: bytes, width: int, height: int) -> bytes:
    flipped = bytearray(len(raw))
    row_stride = width * 4
    for y in range(height):
        src = y * row_stride
        dst = (height - 1 - y) * row_stride
        flipped[dst : dst + row_stride] = raw[src : src + row_stride]
    return bytes(flipped)


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
    has_transparency = any(raw[i] != 255 for i in range(3, len(raw), 4))

    image = bpy.data.images.get(image_name)
    if image is None:
        image = bpy.data.images.new(name=image_name, width=width, height=height, alpha=True)
    else:
        image.scale(width, height)

    raw = _flip_rgba8_vertically(raw, width, height)
    float_pixels = [channel / 255.0 for channel in raw]
    image.pixels = float_pixels
    image.alpha_mode = "STRAIGHT"
    image.update()
    image.pack()
    image["soasim_has_transparency"] = has_transparency

    return image


def _read_optional_texture_id(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _build_texture_lookup(textures: list[dict[str, Any]], stats: ImportStats) -> TextureLookup:
    images_by_name: dict[str, Image] = {}
    images_by_id: dict[int, Image] = {}

    for index, texture in enumerate(textures):
        texture_id = _read_optional_texture_id(texture.get("textureId"))
        texture_name = str(texture.get("textureName", "")).strip()
        if not texture_name:
            texture_name = f"texture_{texture_id}" if texture_id is not None else f"texture_{index:04d}"
        image = _decode_rgba8_image(texture, image_name=texture_name)
        if image is None:
            stats.add_warning(
                f"Texture {texture_name} was not decoded as RGBA8 and will not be displayable."
            )
            continue

        image["soasim_texture_name"] = texture_name
        if texture_id is not None:
            _set_custom_int_property(
                image,
                "soasim_texture_id",
                texture_id,
                stats,
                field_name=f"textures[{index}].textureId",
            )
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
        if texture_id is not None:
            images_by_id[texture_id] = image
        stats.texture_count += 1

    return TextureLookup(by_name=images_by_name, by_id=images_by_id)


def _resolve_material_texture(
    material_data: dict[str, Any],
    texture_lookup: TextureLookup,
) -> Image | None:
    texture_name = str(material_data.get("textureName", "")).strip()
    if texture_name and texture_name in texture_lookup.by_name:
        return texture_lookup.by_name[texture_name]

    texture_id = _read_optional_texture_id(material_data.get("textureId"))
    if texture_id is not None and texture_id in texture_lookup.by_id:
        return texture_lookup.by_id[texture_id]

    return None


def _material_cache_name(material_data: dict[str, Any]) -> str:
    material_hash = int(material_data.get("materialHash", 0))
    texture_id = _read_optional_texture_id(material_data.get("textureId"))
    texture_name = str(material_data.get("textureName", "")).strip()
    if texture_name:
        safe_texture_name = "".join(
            ch if ch.isalnum() or ch in ("_", "-") else "_"
            for ch in texture_name
        )
        return f"SoaMat_{material_hash:016x}_{safe_texture_name}"
    if texture_id is not None:
        return f"SoaMat_{material_hash:016x}_tex_{texture_id}"
    return f"SoaMat_{material_hash:016x}"


def _safe_name_part(value: str, fallback: str) -> str:
    safe_value = "".join(
        ch if ch.isalnum() or ch in ("_", "-") else "_"
        for ch in value.strip()
    ).strip("_")
    return safe_value or fallback


def _hex_name_part(value: Any) -> str:
    try:
        numeric = int(value)
    except (TypeError, ValueError):
        numeric = 0
    return f"0x{numeric:X}"


def _entry_root_name(entry_index: int, entry_id: int, fxn_name: str) -> str:
    entry_number = entry_id if entry_id >= 0 else entry_index
    safe_fxn_name = _safe_name_part(fxn_name, "entry")
    return f"{entry_number:03d}_{safe_fxn_name}"


def _mesh_object_name(mesh_data: dict[str, Any]) -> str:
    return (
        f"obj_{_hex_name_part(mesh_data.get('sourceObjectAddress', 0))}"
        f"_attach_{_hex_name_part(mesh_data.get('sourceAttachOffset', 0))}"
    )


def _node_object_name(entry_name: str, node_idx: int, tree: dict[str, Any]) -> str:
    base_name = f"{entry_name}_Node_{node_idx}"
    if node_idx != 0:
        return base_name
    return f"{base_name}_obj_{_hex_name_part(tree.get('sourceObjectAddress', 0))}"


def _attach_object_name(entry_name: str, node_idx: int, attach_offset: Any) -> str:
    return f"{entry_name}_Node_{node_idx}_attach_{_hex_name_part(attach_offset)}"


def _configure_material_alpha(
    material: Material,
    uses_alpha: bool,
    *,
    prefer_blended: bool = False,
) -> None:
    if hasattr(material, "surface_render_method"):
        if uses_alpha:
            preferred_method = "BLENDED" if prefer_blended else "DITHERED"
            fallback_method = "DITHERED" if prefer_blended else "BLENDED"
            try:
                enum_items = material.bl_rna.properties["surface_render_method"].enum_items
                enum_ids = {item.identifier for item in enum_items}
                if preferred_method in enum_ids:
                    material.surface_render_method = preferred_method
                elif fallback_method in enum_ids:
                    material.surface_render_method = fallback_method
            except (AttributeError, KeyError, TypeError):
                material.surface_render_method = preferred_method
        else:
            try:
                enum_items = material.bl_rna.properties["surface_render_method"].enum_items
                if any(item.identifier == "OPAQUE" for item in enum_items):
                    material.surface_render_method = "OPAQUE"
            except (AttributeError, KeyError, TypeError):
                pass

    if hasattr(material, "blend_method"):
        material.blend_method = ("BLEND" if prefer_blended else "HASHED") if uses_alpha else "OPAQUE"

    if hasattr(material, "shadow_method"):
        material.shadow_method = ("BLEND" if prefer_blended else "HASHED") if uses_alpha else "OPAQUE"


def _set_principled_alpha(bsdf: Any, alpha: float) -> None:
    alpha_input = bsdf.inputs.get("Alpha") if hasattr(bsdf.inputs, "get") else None
    if alpha_input is not None:
        alpha_input.default_value = alpha


def _set_first_existing_input_default(bsdf: Any, names: tuple[str, ...], value: float) -> None:
    if not hasattr(bsdf.inputs, "get"):
        return
    for name in names:
        socket = bsdf.inputs.get(name)
        if socket is not None:
            socket.default_value = value
            return


def _configure_principled_surface(bsdf: Any, material_data: dict[str, Any]) -> None:
    no_specular = _read_bool(material_data.get("noSpecular"), False)

    _set_first_existing_input_default(bsdf, ("Metallic",), 0.0)
    _set_first_existing_input_default(bsdf, ("Roughness",), 1.0)
    _set_first_existing_input_default(
        bsdf,
        ("Specular IOR Level", "Specular", "Specular Tint"),
        0.0 if no_specular else 0.2,
    )


def _set_material_alpha_value(material: Material, bsdf: Any, alpha: float) -> None:
    diffuse = list(material.diffuse_color)
    if len(diffuse) >= 4:
        diffuse[3] = alpha
        material.diffuse_color = diffuse
    _set_principled_alpha(bsdf, alpha)


def _configure_texture_extension(image_node: Any) -> None:
    image_node.extension = "REPEAT"


def _read_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on")
    return default


def _ensure_link(links: Any, from_socket: Any, to_socket: Any) -> None:
    for link in links:
        if link.from_socket == from_socket and link.to_socket == to_socket:
            return
    links.new(from_socket, to_socket)


def _clear_input_links(links: Any, input_socket: Any) -> None:
    for link in list(input_socket.links):
        links.remove(link)


def _named_math_node(nodes: Any, name: str, operation: str) -> Any:
    node = nodes.get(name)
    if node is None:
        node = nodes.new(type="ShaderNodeMath")
        node.name = name
        node.label = name
    node.operation = operation
    node.use_clamp = False
    return node


def _build_sampler_axis(
    nodes: Any,
    links: Any,
    source_socket: Any,
    *,
    axis_name: str,
    clamp_axis: bool,
    mirror_axis: bool,
) -> Any:
    if clamp_axis:
        max_node = _named_math_node(nodes, f"Soa{axis_name}ClampMin", "MAXIMUM")
        max_node.inputs[1].default_value = -1.0
        min_node = _named_math_node(nodes, f"Soa{axis_name}ClampMax", "MINIMUM")
        min_node.inputs[1].default_value = 1.0
        _clear_input_links(links, max_node.inputs[0])
        _clear_input_links(links, min_node.inputs[0])
        _ensure_link(links, source_socket, max_node.inputs[0])
        _ensure_link(links, max_node.outputs[0], min_node.inputs[0])
        return min_node.outputs[0]

    if mirror_axis:
        mod_node = _named_math_node(nodes, f"Soa{axis_name}MirrorModulo", "MODULO")
        mod_node.inputs[1].default_value = 2.0
        sub_node = _named_math_node(nodes, f"Soa{axis_name}MirrorCenter", "SUBTRACT")
        sub_node.inputs[1].default_value = 1.0
        abs_node = _named_math_node(nodes, f"Soa{axis_name}MirrorAbs", "ABSOLUTE")
        inv_node = _named_math_node(nodes, f"Soa{axis_name}MirrorInvert", "SUBTRACT")
        inv_node.inputs[0].default_value = 1.0

        _clear_input_links(links, mod_node.inputs[0])
        _clear_input_links(links, sub_node.inputs[0])
        _clear_input_links(links, abs_node.inputs[0])
        _clear_input_links(links, inv_node.inputs[1])

        _ensure_link(links, source_socket, mod_node.inputs[0])
        _ensure_link(links, mod_node.outputs[0], sub_node.inputs[0])
        _ensure_link(links, sub_node.outputs[0], abs_node.inputs[0])
        _ensure_link(links, abs_node.outputs[0], inv_node.inputs[1])
        return inv_node.outputs[0]

    return source_socket


def _configure_texture_sampler(
    nodes: Any,
    links: Any,
    image_node: Any,
    *,
    clamp_u: bool,
    clamp_v: bool,
    mirror_u: bool,
    mirror_v: bool,
) -> None:
    tex_coord = nodes.get("SoaTexCoord")
    if tex_coord is None:
        tex_coord = nodes.new(type="ShaderNodeTexCoord")
        tex_coord.name = "SoaTexCoord"

    separate = nodes.get("SoaUVSeparate")
    if separate is None:
        separate = nodes.new(type="ShaderNodeSeparateXYZ")
        separate.name = "SoaUVSeparate"

    combine = nodes.get("SoaUVCombine")
    if combine is None:
        combine = nodes.new(type="ShaderNodeCombineXYZ")
        combine.name = "SoaUVCombine"
    combine.inputs["Z"].default_value = 0.0

    _clear_input_links(links, separate.inputs["Vector"])
    _ensure_link(links, tex_coord.outputs["UV"], separate.inputs["Vector"])

    u_socket = _build_sampler_axis(
        nodes,
        links,
        separate.outputs["X"],
        axis_name="U",
        clamp_axis=clamp_u,
        mirror_axis=mirror_u,
    )
    v_socket = _build_sampler_axis(
        nodes,
        links,
        separate.outputs["Y"],
        axis_name="V",
        clamp_axis=clamp_v,
        mirror_axis=mirror_v,
    )

    _clear_input_links(links, combine.inputs["X"])
    _clear_input_links(links, combine.inputs["Y"])
    _clear_input_links(links, image_node.inputs["Vector"])
    _ensure_link(links, u_socket, combine.inputs["X"])
    _ensure_link(links, v_socket, combine.inputs["Y"])
    _ensure_link(links, combine.outputs["Vector"], image_node.inputs["Vector"])


def _enable_custom_normals_shading(mesh: Mesh) -> None:
    if hasattr(mesh, "use_auto_smooth"):
        mesh.use_auto_smooth = True
        return

    if len(mesh.polygons) > 0:
        mesh.polygons.foreach_set("use_smooth", [True] * len(mesh.polygons))


def _reset_material_nodes(material: Material) -> tuple[Any, Any]:
    node_tree = material.node_tree
    assert node_tree is not None
    node_tree.links.clear()
    node_tree.nodes.clear()

    bsdf = node_tree.nodes.new(type="ShaderNodeBsdfPrincipled")
    bsdf.name = "Principled BSDF"
    output = node_tree.nodes.new(type="ShaderNodeOutputMaterial")
    output.name = "Material Output"
    node_tree.links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])
    return bsdf, output


def _build_material(
    material_data: dict[str, Any],
    texture_lookup: TextureLookup,
    stats: ImportStats,
    *,
    mesh_field_name: str,
    material_index: int,
) -> Material:
    name = _material_cache_name(material_data)
    material = bpy.data.materials.get(name)
    if material is None:
        material = bpy.data.materials.new(name=name)

    material.use_nodes = True
    bsdf, output = _reset_material_nodes(material)
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
    chunk_flags = int(material_data.get("chunkFlags", 0))
    use_texture = _read_bool(material_data.get("useTexture"), True)
    use_alpha = _read_bool(material_data.get("useAlpha"), False)
    no_alpha_test = _read_bool(material_data.get("noAlphaTest"), False)
    double_sided = _read_bool(material_data.get("doubleSided"), False)
    clamp_u = _read_bool(material_data.get("clampU"), (chunk_flags & 0x4) != 0)
    clamp_v = _read_bool(material_data.get("clampV"), (chunk_flags & 0x8) != 0)
    mirror_u = _read_bool(material_data.get("mirrorU"), (chunk_flags & 0x1) != 0)
    mirror_v = _read_bool(material_data.get("mirrorV"), (chunk_flags & 0x2) != 0)

    node_tree = material.node_tree
    assert node_tree is not None
    nodes = node_tree.nodes
    links = node_tree.links

    _configure_principled_surface(bsdf, material_data)

    material.use_backface_culling = not double_sided
    if hasattr(material, "use_backface_culling_shadow"):
        material.use_backface_culling_shadow = material.use_backface_culling

    texture_image = _resolve_material_texture(material_data, texture_lookup)
    if texture_image is not None:
        image_node = None
        for node in nodes:
            if node.type == "TEX_IMAGE" and node.name == "SoaTexture":
                image_node = node
                break
        if image_node is None:
            image_node = nodes.new(type="ShaderNodeTexImage")
            image_node.name = "SoaTexture"

        image_node.image = texture_image
        _configure_texture_extension(image_node)
        _configure_texture_sampler(
            nodes,
            links,
            image_node,
            clamp_u=clamp_u,
            clamp_v=clamp_v,
            mirror_u=mirror_u,
            mirror_v=mirror_v,
        )
        if use_texture:
            _ensure_link(links, image_node.outputs["Color"], bsdf.inputs["Base Color"])
            if use_alpha:
                _ensure_link(links, image_node.outputs["Alpha"], bsdf.inputs["Alpha"])
                diffuse = list(material.diffuse_color)
                if len(diffuse) >= 4:
                    diffuse[3] = 1.0
                    material.diffuse_color = diffuse
                _configure_material_alpha(material, True)
            else:
                _clear_input_links(links, bsdf.inputs["Alpha"])
                _set_material_alpha_value(material, bsdf, 1.0)
                _configure_material_alpha(material, False)
        else:
            _clear_input_links(links, bsdf.inputs["Base Color"])
            _clear_input_links(links, bsdf.inputs["Alpha"])
            _set_material_alpha_value(material, bsdf, 0.25)
            _configure_material_alpha(material, True, prefer_blended=True)
        material["soasim_texture_bound"] = True
    else:
        _clear_input_links(links, bsdf.inputs["Base Color"])
        _clear_input_links(links, bsdf.inputs["Alpha"])
        _set_material_alpha_value(material, bsdf, 0.25)
        _configure_material_alpha(material, True, prefer_blended=True)
        texture_id = material_data.get("textureId", None)
        texture_name = str(material_data.get("textureName", "")).strip()
        if texture_id not in (None, 0xFFFF, "65535") or texture_name:
            stats.add_warning(
                (
                    f"Material {material.name} could not resolve texture "
                    f"id={texture_id!r} name={texture_name!r}."
                )
            )
        material["soasim_texture_bound"] = False

    return material


def _read_corner_vertex_index(corner: Any) -> int:
    if isinstance(corner, dict):
        return int(corner.get("vertexIndex", 0))
    return int(corner)


def _triangles_from_corners(corners: list[Any]) -> list[tuple[int, int, int]]:
    triangles: list[tuple[int, int, int]] = []
    for i in range(0, len(corners) - 2, 3):
        a = _read_corner_vertex_index(corners[i])
        b = _read_corner_vertex_index(corners[i + 1])
        c = _read_corner_vertex_index(corners[i + 2])
        triangles.append((a, b, c))
    return triangles


def _triangle_has_area(
    vertices: list[tuple[float, float, float]],
    tri: tuple[int, int, int],
) -> bool:
    if tri[0] == tri[1] or tri[1] == tri[2] or tri[2] == tri[0]:
        return False
    if tri[0] < 0 or tri[1] < 0 or tri[2] < 0:
        return False
    if tri[0] >= len(vertices) or tri[1] >= len(vertices) or tri[2] >= len(vertices):
        return False

    a = mathutils.Vector(vertices[tri[0]])
    b = mathutils.Vector(vertices[tri[1]])
    c = mathutils.Vector(vertices[tri[2]])
    return (b - a).cross(c - a).length_squared > 1.0e-10


def _triangle_geometry_key(
    vertices: list[tuple[float, float, float]],
    tri: tuple[int, int, int],
) -> tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]:
    return tuple(
        sorted(
            (
                round(vertices[index][0], 6),
                round(vertices[index][1], 6),
                round(vertices[index][2], 6),
            )
            for index in tri
        )
    )


def _append_triangle_corner_attributes(
    triangle_corners: tuple[Any, Any, Any],
    uv_values: list[tuple[float, float] | None],
    color_values: list[tuple[float, float, float, float] | None],
) -> None:
    for corner in triangle_corners:
        if not isinstance(corner, dict):
            uv_values.append(None)
            color_values.append(None)
            continue

        if bool(corner.get("hasUv", False)):
            u = float(corner.get("u", 0.0))
            v = float(corner.get("v", 0.0))
            uv_values.append((u, 1.0 - v))
        else:
            uv_values.append(None)

        color = corner.get("color", [1.0, 1.0, 1.0, 1.0])
        if bool(corner.get("hasColor", False)) and len(color) >= 4:
            color_values.append(
                (float(color[0]), float(color[1]), float(color[2]), float(color[3]))
            )
        else:
            color_values.append(None)


def _build_mesh(mesh_data: dict[str, Any], texture_lookup: TextureLookup, stats: ImportStats) -> Object:
    mesh_name = _mesh_object_name(mesh_data)
    mesh_field_name = f"meshes[{mesh_name}]"
    label = str(mesh_data.get("label", ""))
    is_grnd_mesh = label.startswith("GRND_")

    vertices_data = mesh_data.get("vertices", [])
    vertices = []
    for vertex in vertices_data:
        pos = vertex.get("position", [0.0, 0.0, 0.0])
        source_y = float(pos[1])
        if is_grnd_mesh:
            source_y += GRND_COLLISION_VISUAL_SOURCE_Y_OFFSET
        source_position = mathutils.Vector((float(pos[0]), source_y, float(pos[2])))
        blender_position = NJCM_TO_BLENDER_AXIS @ source_position
        vertices.append((blender_position.x, blender_position.y, blender_position.z))

    triangles: list[tuple[int, int, int]] = []
    poly_material_indices: list[int] = []
    poly_flat_flags: list[bool] = []
    uv_values: list[tuple[float, float] | None] = []
    color_values: list[tuple[float, float, float, float] | None] = []
    seen_triangle_geometry: set[
        tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]
    ] = set()
    for tri_set in mesh_data.get("triangleSets", []):
        corners = tri_set.get("corners", [])
        material_index = int(tri_set.get("materialIndex", 0))
        material_data = {}
        if 0 <= material_index < len(mesh_data.get("materials", [])):
            material_data = mesh_data["materials"][material_index]
        flat_shading = bool(material_data.get("flatShading", False))
        for corner_index in range(0, len(corners) - 2, 3):
            triangle_corners = (
                corners[corner_index],
                corners[corner_index + 1],
                corners[corner_index + 2],
            )
            tri = (
                _read_corner_vertex_index(triangle_corners[0]),
                _read_corner_vertex_index(triangle_corners[1]),
                _read_corner_vertex_index(triangle_corners[2]),
            )
            if not _triangle_has_area(vertices, tri):
                continue
            geometry_key = _triangle_geometry_key(vertices, tri)
            if geometry_key in seen_triangle_geometry:
                continue
            seen_triangle_geometry.add(geometry_key)

            triangles.append(tri)
            poly_material_indices.append(material_index)
            poly_flat_flags.append(flat_shading)
            _append_triangle_corner_attributes(triangle_corners, uv_values, color_values)

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
        if poly_index < len(poly_flat_flags):
            poly.use_smooth = not poly_flat_flags[poly_index]

    if any(value is not None for value in uv_values) and len(uv_values) == len(mesh.loops):
        uv_layer = mesh.uv_layers.new(name="SoaUV")
        for loop_index, uv in enumerate(uv_values):
            if uv is not None:
                uv_layer.data[loop_index].uv = uv
        mesh.uv_layers.active = uv_layer

    if any(value is not None for value in color_values) and len(color_values) == len(mesh.loops):
        color_layer = mesh.color_attributes.new(
            name="SoaColor",
            type="BYTE_COLOR",
            domain="CORNER",
        )
        for loop_index, color in enumerate(color_values):
            if color is not None:
                color_layer.data[loop_index].color = color

    diagnostics = mesh_data.get("diagnostics", {})
    _set_custom_int_property(
        mesh,
        "soasim_source_object_address",
        mesh_data.get("sourceObjectAddress", 0),
        stats,
        field_name=f"{mesh_field_name}.sourceObjectAddress",
    )
    mesh["soasim_label"] = label
    if is_grnd_mesh:
        mesh["soasim_visual_source_y_offset"] = GRND_COLLISION_VISUAL_SOURCE_Y_OFFSET
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


def _set_parent_with_identity_inverse(obj: Object, parent: Object) -> None:
    obj.parent = parent
    obj.matrix_parent_inverse = mathutils.Matrix.Identity(4)


def _transform_to_matrix(transform: dict[str, Any]) -> mathutils.Matrix:
    position = transform.get("position", [0.0, 0.0, 0.0])
    quat = transform.get("rotation", [0.0, 0.0, 0.0, 1.0])
    scale = transform.get("scale", [1.0, 1.0, 1.0])

    source_position = mathutils.Vector((float(position[0]), float(position[1]), float(position[2])))
    blender_position = NJCM_TO_BLENDER_AXIS @ source_position

    source_rotation = mathutils.Quaternion(
        (float(quat[3]), float(quat[0]), float(quat[1]), float(quat[2]))
    )
    blender_rotation = NJCM_TO_BLENDER_AXIS @ source_rotation @ NJCM_TO_BLENDER_AXIS.conjugated()
    blender_scale = mathutils.Vector((float(scale[0]), float(scale[1]), float(scale[2])))
    return mathutils.Matrix.LocRotScale(blender_position, blender_rotation, blender_scale)


def _entry_transform_to_matrix(transform: dict[str, Any]) -> mathutils.Matrix:
    position = transform.get("position", [0.0, 0.0, 0.0])
    rotation_raw = transform.get("rotationRaw", [0.0, 0.0, 0.0])
    scale = transform.get("scale", [1.0, 1.0, 1.0])

    blender_position = mathutils.Vector((
        float(position[0]),
        -float(position[2]),
        float(position[1]),
    ))
    blender_rotation = mathutils.Euler((
        math.radians(float(rotation_raw[0])),
        math.radians(float(rotation_raw[2])),
        math.radians(float(rotation_raw[1])),
    ), "XYZ").to_quaternion()
    blender_scale = mathutils.Vector((
        float(scale[0]),
        float(scale[2]),
        float(scale[1]),
    ))
    return mathutils.Matrix.LocRotScale(blender_position, blender_rotation, blender_scale)


def _apply_transform(obj: Object, transform: dict[str, Any]) -> None:
    obj.rotation_mode = "QUATERNION"
    obj.matrix_basis = _transform_to_matrix(transform)


def _apply_entry_transform(obj: Object, transform: dict[str, Any]) -> None:
    obj.rotation_mode = "QUATERNION"
    obj.matrix_basis = _entry_transform_to_matrix(transform)


def _apply_identity_local_transform(obj: Object) -> None:
    obj.rotation_mode = "QUATERNION"
    obj.matrix_basis = mathutils.Matrix.Identity(4)


def _matrix_to_compact_string(matrix: mathutils.Matrix) -> str:
    rows: list[str] = []
    for row in matrix:
        rows.append(f"[{row[0]:.6f},{row[1]:.6f},{row[2]:.6f},{row[3]:.6f}]")
    return "[" + ",".join(rows) + "]"


def _write_debug_log(debug_lines: list[str], target_collection_name: str, stats: ImportStats) -> None:
    if not debug_lines:
        return

    text_name = f"{target_collection_name}_ParityDebug"
    text_block = bpy.data.texts.get(text_name)
    if text_block is None:
        text_block = bpy.data.texts.new(text_name)
    else:
        text_block.clear()
    text_block.write("\n".join(debug_lines))

    stats.debug_lines = len(debug_lines)
    print(f"[SoaSim Parity Debug] wrote {len(debug_lines)} lines to Blender text '{text_name}'")


def _resolve_node_transform(node: dict[str, Any]) -> dict[str, Any]:
    transform = dict(node.get("localTransform", {}))
    eval_flags = int(node.get("sourceEvalFlags", 0))

    if (eval_flags & NJD_EVAL_UNIT_POS) != 0:
        transform["position"] = [0.0, 0.0, 0.0]
    if (eval_flags & NJD_EVAL_UNIT_ANG) != 0:
        transform["rotation"] = [0.0, 0.0, 0.0, 1.0]
    if (eval_flags & NJD_EVAL_UNIT_SCL) != 0:
        transform["scale"] = [1.0, 1.0, 1.0]

    return transform


def _create_empty(name: str) -> Object:
    return bpy.data.objects.new(name, None)


def import_blender_ir_json(
    json_path: str,
    clear_target_collection: bool,
    target_collection_name: str,
    emit_parity_debug: bool,
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
    debug_lines: list[str] = []

    for entry_index, entry in enumerate(payload.get("indexEntries", [])):
        transform = entry.get("transform", {})
        entry_id = int(entry.get("sourceEntryId", 0))
        fxn_name = str(entry.get("fxnName", ""))
        entry_root = _create_empty(_entry_root_name(entry_index, entry_id, fxn_name))
        _apply_entry_transform(entry_root, transform)
        if emit_parity_debug:
            debug_lines.append(
                (
                    f"ENTRY sourceEntryId={entry_id} tblId={int(entry.get('tblId', 0))} "
                    f"fxnName={fxn_name} "
                    f"basis={_matrix_to_compact_string(entry_root.matrix_basis)} "
                    f"world={_matrix_to_compact_string(entry_root.matrix_world)}"
                )
            )
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
        entry_root["soasim_ground_addresses"] = ",".join(
            str(int(v)) for v in entry.get("groundAddresses", [])
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
                _set_parent_with_identity_inverse(instance_obj, entry_root)
                _apply_identity_local_transform(instance_obj)
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
            nodes = tree.get("nodes", [])
            node_objects: list[Object | None] = [None] * len(nodes)
            for node_idx, node in enumerate(nodes):
                node_name = _node_object_name(entry_root.name, node_idx, tree)
                node_obj = _create_empty(node_name)
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
                if node_idx == 0:
                    _set_custom_int_property(
                        node_obj,
                        "soasim_tree_index",
                        ti,
                        stats,
                        field_name=f"indexEntries[{entry_id}].objectTreeIndices[{slot}]",
                    )
                    _set_custom_int_property(
                        node_obj,
                        "soasim_source_object_address",
                        tree.get("sourceObjectAddress", 0),
                        stats,
                        field_name=f"objectTrees[{ti}].sourceObjectAddress",
                    )
                    _set_custom_int_property(
                        node_obj,
                        "soasim_source_chunk_offset",
                        tree.get("sourceChunkOffset", 0),
                        stats,
                        field_name=f"objectTrees[{ti}].sourceChunkOffset",
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
                    _set_parent_with_identity_inverse(node_obj, entry_root)
                else:
                    pi = int(parent_idx)
                    if 0 <= pi < len(node_objects) and node_objects[pi] is not None:
                        _set_parent_with_identity_inverse(node_obj, node_objects[pi])
                    else:
                        _set_parent_with_identity_inverse(node_obj, entry_root)
                        stats.warnings += 1
                _apply_transform(node_obj, _resolve_node_transform(node))
                if emit_parity_debug:
                    debug_lines.append(
                        (
                            f"NODE treeIndex={ti} nodeIndex={node_idx} "
                            f"parentNodeIndex={parent_idx} "
                            f"sourceNodeOffset={int(node.get('sourceNodeOffset', 0))} "
                            f"sourceEvalFlags=0x{int(node.get('sourceEvalFlags', 0)):X} "
                            f"basis={_matrix_to_compact_string(node_obj.matrix_basis)} "
                            f"world={_matrix_to_compact_string(node_obj.matrix_world)}"
                        )
                    )

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
                attach_name = _attach_object_name(entry_root.name, node_idx, node.get("sourceAttachOffset", 0))
                attach_obj = bpy.data.objects.new(attach_name, source_obj.data)
                _set_parent_with_identity_inverse(attach_obj, node_obj)
                _apply_identity_local_transform(attach_obj)
                attach_obj["soasim_mesh_index"] = mi
                _set_custom_int_property(
                    attach_obj,
                    "soasim_node_index",
                    node_idx,
                    stats,
                    field_name=f"objectTrees[{ti}].nodes[{node_idx}].nodeIndex",
                )
                _set_custom_int_property(
                    attach_obj,
                    "soasim_source_attach_offset",
                    node.get("sourceAttachOffset", 0),
                    stats,
                    field_name=f"objectTrees[{ti}].nodes[{node_idx}].sourceAttachOffset",
                )
                root_collection.objects.link(attach_obj)
                stats.object_count += 1

        for slot, mesh_index in enumerate(entry.get("meshIndices", [])):
            mi = int(mesh_index)
            if mi < 0 or mi >= len(mesh_objects):
                stats.warnings += 1
                continue
            source_obj = mesh_objects[mi]
            if not str(source_obj.data.get("soasim_label", "")).startswith("GRND_"):
                continue
            instance_name = f"SoaInst_{entry_id}_grnd_{slot}_{source_obj.name}"
            instance_obj = bpy.data.objects.new(instance_name, source_obj.data)
            _set_parent_with_identity_inverse(instance_obj, entry_root)
            _apply_identity_local_transform(instance_obj)
            instance_obj["soasim_mesh_index"] = mi
            root_collection.objects.link(instance_obj)
            stats.object_count += 1

    if emit_parity_debug:
        _write_debug_log(debug_lines, target_collection_name, stats)

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

    emit_parity_debug: BoolProperty(
        name="Emit Parity Debug Log",
        default=False,
        description=(
            "Write per-entry and per-node transform matrices into a Blender Text datablock "
            "named '<Collection>_ParityDebug' for SAIO parity comparison."
        ),
    )

    def execute(self, context: bpy.types.Context) -> set[str]:
        del context
        json_path = str(Path(self.filepath))

        try:
            stats = import_blender_ir_json(
                json_path=json_path,
                clear_target_collection=self.clear_target_collection,
                target_collection_name=self.target_collection_name,
                emit_parity_debug=self.emit_parity_debug,
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
                f"warnings={stats.warnings}, debugLines={stats.debug_lines}"
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
