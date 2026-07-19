#include "NavigationAreaLoader.h"

#include "../Projection/RegionMeshProjector.h"

#include "SpiceMLD/Parsing/MldParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace savor::navigation {
namespace {

using SpiceTransform = spice::mld::model::Transform;
using SpiceMesh = spice::mld::model::MeshData;

constexpr float kNormalEpsilon = 1.0e-6F;

struct Matrix4 {
    float values[4][4]{};
};

[[nodiscard]] Matrix4 identityMatrix() {
    Matrix4 out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out.values[i][i] = 1.0F;
    }
    return out;
}

[[nodiscard]] Matrix4 multiply(const Matrix4& lhs, const Matrix4& rhs) {
    Matrix4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            for (std::size_t inner = 0; inner < 4; ++inner) {
                out.values[row][column] += lhs.values[row][inner] * rhs.values[inner][column];
            }
        }
    }
    return out;
}

[[nodiscard]] Matrix4 transformMatrix(const SpiceTransform& transform) {
    const float x = transform.rotation.x;
    const float y = transform.rotation.y;
    const float z = transform.rotation.z;
    const float w = transform.rotation.w;

    Matrix4 rotation = identityMatrix();
    rotation.values[0][0] = 1.0F - (2.0F * ((y * y) + (z * z)));
    rotation.values[0][1] = 2.0F * ((x * y) - (z * w));
    rotation.values[0][2] = 2.0F * ((x * z) + (y * w));
    rotation.values[1][0] = 2.0F * ((x * y) + (z * w));
    rotation.values[1][1] = 1.0F - (2.0F * ((x * x) + (z * z)));
    rotation.values[1][2] = 2.0F * ((y * z) - (x * w));
    rotation.values[2][0] = 2.0F * ((x * z) - (y * w));
    rotation.values[2][1] = 2.0F * ((y * z) + (x * w));
    rotation.values[2][2] = 1.0F - (2.0F * ((x * x) + (y * y)));

    Matrix4 scale = identityMatrix();
    scale.values[0][0] = transform.scale.x;
    scale.values[1][1] = transform.scale.y;
    scale.values[2][2] = transform.scale.z;

    Matrix4 translation = identityMatrix();
    translation.values[0][3] = transform.position.x;
    translation.values[1][3] = transform.position.y;
    translation.values[2][3] = transform.position.z;
    return multiply(translation, multiply(rotation, scale));
}

[[nodiscard]] NavigationVec3 transformPoint(const Matrix4& matrix, const NavigationVec3& source) {
    return NavigationVec3{
        (matrix.values[0][0] * source.x) + (matrix.values[0][1] * source.y) +
            (matrix.values[0][2] * source.z) + matrix.values[0][3],
        (matrix.values[1][0] * source.x) + (matrix.values[1][1] * source.y) +
            (matrix.values[1][2] * source.z) + matrix.values[1][3],
        (matrix.values[2][0] * source.x) + (matrix.values[2][1] * source.y) +
            (matrix.values[2][2] * source.z) + matrix.values[2][3],
    };
}

[[nodiscard]] NavigationVec3 add(const NavigationVec3& lhs, const NavigationVec3& rhs) {
    return NavigationVec3{ lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z };
}

[[nodiscard]] NavigationVec3 subtract(const NavigationVec3& lhs, const NavigationVec3& rhs) {
    return NavigationVec3{ lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z };
}

[[nodiscard]] NavigationVec3 cross(const NavigationVec3& lhs, const NavigationVec3& rhs) {
    return NavigationVec3{
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

[[nodiscard]] NavigationVec3 normalize(const NavigationVec3& value) {
    const float lengthSquared = (value.x * value.x) + (value.y * value.y) + (value.z * value.z);
    if (lengthSquared <= kNormalEpsilon) {
        return NavigationVec3{ 0.0F, 1.0F, 0.0F };
    }
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return NavigationVec3{ value.x * inverseLength, value.y * inverseLength, value.z * inverseLength };
}

[[nodiscard]] NavigationVec3 transformNormal(const Matrix4& matrix, const NavigationVec3& source) {
    const float a00 = matrix.values[0][0];
    const float a01 = matrix.values[0][1];
    const float a02 = matrix.values[0][2];
    const float a10 = matrix.values[1][0];
    const float a11 = matrix.values[1][1];
    const float a12 = matrix.values[1][2];
    const float a20 = matrix.values[2][0];
    const float a21 = matrix.values[2][1];
    const float a22 = matrix.values[2][2];

    const float cofactor00 = (a11 * a22) - (a12 * a21);
    const float cofactor01 = (a12 * a20) - (a10 * a22);
    const float cofactor02 = (a10 * a21) - (a11 * a20);
    const float determinant = (a00 * cofactor00) + (a01 * cofactor01) + (a02 * cofactor02);
    if (std::abs(determinant) <= kNormalEpsilon) {
        return normalize(source);
    }

    const float inverseDeterminant = 1.0F / determinant;
    const float inverse00 = cofactor00 * inverseDeterminant;
    const float inverse01 = ((a02 * a21) - (a01 * a22)) * inverseDeterminant;
    const float inverse02 = ((a01 * a12) - (a02 * a11)) * inverseDeterminant;
    const float inverse10 = cofactor01 * inverseDeterminant;
    const float inverse11 = ((a00 * a22) - (a02 * a20)) * inverseDeterminant;
    const float inverse12 = ((a02 * a10) - (a00 * a12)) * inverseDeterminant;
    const float inverse20 = cofactor02 * inverseDeterminant;
    const float inverse21 = ((a01 * a20) - (a00 * a21)) * inverseDeterminant;
    const float inverse22 = ((a00 * a11) - (a01 * a10)) * inverseDeterminant;

    return normalize(NavigationVec3{
        (inverse00 * source.x) + (inverse10 * source.y) + (inverse20 * source.z),
        (inverse01 * source.x) + (inverse11 * source.y) + (inverse21 * source.z),
        (inverse02 * source.x) + (inverse12 * source.y) + (inverse22 * source.z),
    });
}

[[nodiscard]] NavigationTransform copyTransform(const SpiceTransform& source) {
    return NavigationTransform{
        .position = NavigationVec3{ source.position.x, source.position.y, source.position.z },
        .rotation = NavigationQuat{ source.rotation.x, source.rotation.y, source.rotation.z, source.rotation.w },
        .scale = NavigationVec3{ source.scale.x, source.scale.y, source.scale.z },
    };
}

void appendDiagnostic(std::vector<NavigationDiagnostic>& diagnostics,
    const NavigationDiagnosticSeverity severity,
    std::string message) {
    diagnostics.push_back(NavigationDiagnostic{
        .severity = severity,
        .message = std::move(message),
    });
}

void appendDiagnosticOnce(std::vector<NavigationDiagnostic>& diagnostics,
    const NavigationDiagnosticSeverity severity,
    const std::string& message) {
    const auto duplicate = std::find_if(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.severity == severity && diagnostic.message == message;
    });
    if (duplicate == diagnostics.end()) {
        appendDiagnostic(diagnostics, severity, message);
    }
}

[[nodiscard]] NavigationDiagnosticSeverity convertSeverity(const spice::mld::parsing::ParseDiagnostic::Severity severity) {
    switch (severity) {
    case spice::mld::parsing::ParseDiagnostic::Severity::Warning:
        return NavigationDiagnosticSeverity::Warning;
    case spice::mld::parsing::ParseDiagnostic::Severity::Error:
        return NavigationDiagnosticSeverity::Error;
    case spice::mld::parsing::ParseDiagnostic::Severity::Info:
    default:
        return NavigationDiagnosticSeverity::Info;
    }
}

[[nodiscard]] NavigationDiagnosticSeverity convertSeverity(const spice::mld::model::MldDiagnostic::Severity severity) {
    switch (severity) {
    case spice::mld::model::MldDiagnostic::Severity::Warning:
        return NavigationDiagnosticSeverity::Warning;
    case spice::mld::model::MldDiagnostic::Severity::Error:
        return NavigationDiagnosticSeverity::Error;
    case spice::mld::model::MldDiagnostic::Severity::Info:
    default:
        return NavigationDiagnosticSeverity::Info;
    }
}

[[nodiscard]] std::string normalizeFxn(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](const unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](const unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[nodiscard]] std::vector<std::uint32_t> nonZeroValues(
    const std::shared_ptr<spice::mld::model::U32List>& source) {
    std::vector<std::uint32_t> out{};
    if (!source) {
        return out;
    }
    out.reserve(source->values.size());
    std::copy_if(source->values.begin(), source->values.end(), std::back_inserter(out), [](const auto value) {
        return value != 0U;
    });
    return out;
}

[[nodiscard]] NavigationMesh convertMesh(const SpiceMesh& source,
    const Matrix4& world,
    std::vector<NavigationDiagnostic>& diagnostics,
    const std::string& context) {
    NavigationMesh out{};
    out.vertices.reserve(source.vertices.size());
    for (const auto& sourceVertex : source.vertices) {
        NavigationMeshVertex vertex{};
        vertex.position = transformPoint(world, NavigationVec3{
            sourceVertex.position.x,
            sourceVertex.position.y,
            sourceVertex.position.z,
        });
        vertex.hasSourceNormal = sourceVertex.hasNormal;
        if (sourceVertex.hasNormal) {
            vertex.normal = transformNormal(world, NavigationVec3{
                sourceVertex.normal.x,
                sourceVertex.normal.y,
                sourceVertex.normal.z,
            });
        }
        vertex.rawUserAttributesU32 = sourceVertex.rawUserAttributesU32;
        out.vertices.push_back(std::move(vertex));
    }

    std::size_t rejectedTriangles = 0;
    const std::size_t triangleCount = source.indices.size() / 3U;
    out.indices.reserve(triangleCount * 3U);
    out.triangleMetadata.reserve(triangleCount);
    for (std::size_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
        const std::size_t base = triangleIndex * 3U;
        const auto a = source.indices[base + 0U];
        const auto b = source.indices[base + 1U];
        const auto c = source.indices[base + 2U];
        if (a >= out.vertices.size() || b >= out.vertices.size() || c >= out.vertices.size()) {
            ++rejectedTriangles;
            continue;
        }
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);

        NavigationTriangleMetadata metadata{};
        if (triangleIndex < source.triangleMetadata.size()) {
            metadata.rawU16 = source.triangleMetadata[triangleIndex].rawU16;
            metadata.present = true;
        }
        out.triangleMetadata.push_back(metadata);
    }

    if ((source.indices.size() % 3U) != 0U) {
        appendDiagnostic(diagnostics, NavigationDiagnosticSeverity::Warning,
            context + " has trailing non-triangle indices; they were ignored.");
    }
    if (rejectedTriangles > 0U) {
        appendDiagnostic(diagnostics, NavigationDiagnosticSeverity::Warning,
            context + " rejected " + std::to_string(rejectedTriangles) + " triangle(s) with invalid indices.");
    }

    std::vector<NavigationVec3> generatedNormals(out.vertices.size());
    for (std::size_t index = 0; index + 2U < out.indices.size(); index += 3U) {
        const auto a = out.indices[index + 0U];
        const auto b = out.indices[index + 1U];
        const auto c = out.indices[index + 2U];
        const auto faceNormal = cross(
            subtract(out.vertices[b].position, out.vertices[a].position),
            subtract(out.vertices[c].position, out.vertices[a].position));
        if (!out.vertices[a].hasSourceNormal) {
            generatedNormals[a] = add(generatedNormals[a], faceNormal);
        }
        if (!out.vertices[b].hasSourceNormal) {
            generatedNormals[b] = add(generatedNormals[b], faceNormal);
        }
        if (!out.vertices[c].hasSourceNormal) {
            generatedNormals[c] = add(generatedNormals[c], faceNormal);
        }
    }
    for (std::size_t vertexIndex = 0; vertexIndex < out.vertices.size(); ++vertexIndex) {
        if (!out.vertices[vertexIndex].hasSourceNormal) {
            out.vertices[vertexIndex].normal = normalize(generatedNormals[vertexIndex]);
        }
    }
    return out;
}

void updateBounds(NavigationBounds& bounds, const NavigationVec3& point) {
    if (!bounds.valid) {
        bounds.minimum = point;
        bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

[[nodiscard]] std::vector<Matrix4> buildNodeWorldMatrices(
    const spice::mld::model::GobjData& decoded,
    std::vector<NavigationDiagnostic>& diagnostics,
    const std::uint32_t blockOffset) {
    std::vector<std::optional<Matrix4>> memo(decoded.nodes.size());
    std::vector<bool> active(decoded.nodes.size(), false);

    std::function<Matrix4(std::size_t)> resolve = [&](const std::size_t nodeIndex) -> Matrix4 {
        if (memo[nodeIndex].has_value()) {
            return *memo[nodeIndex];
        }
        if (active[nodeIndex]) {
            appendDiagnostic(diagnostics, NavigationDiagnosticSeverity::Warning,
                "GOBJ block " + std::to_string(blockOffset) + " contains a transform cycle; the node used identity ancestry.");
            return identityMatrix();
        }
        active[nodeIndex] = true;
        Matrix4 parent = identityMatrix();
        const auto parentIndex = decoded.nodes[nodeIndex].parentNodeIndex;
        if (parentIndex.has_value()) {
            if (*parentIndex < decoded.nodes.size()) {
                parent = resolve(*parentIndex);
            } else {
                appendDiagnostic(diagnostics, NavigationDiagnosticSeverity::Warning,
                    "GOBJ block " + std::to_string(blockOffset) + " contains an invalid parent-node index.");
            }
        }
        active[nodeIndex] = false;
        memo[nodeIndex] = multiply(parent, transformMatrix(decoded.nodes[nodeIndex].transform));
        return *memo[nodeIndex];
    };

    std::vector<Matrix4> out(decoded.nodes.size());
    for (std::size_t nodeIndex = 0; nodeIndex < decoded.nodes.size(); ++nodeIndex) {
        out[nodeIndex] = resolve(nodeIndex);
    }
    return out;
}

} // namespace

NavigationAreaLoadResult NavigationAreaLoader::loadFile(const std::filesystem::path& path) const {
    NavigationAreaLoadResult loadResult{};
    if (path.empty()) {
        appendDiagnostic(loadResult.diagnostics, NavigationDiagnosticSeverity::Error,
            "No MLD file path was provided.");
        return loadResult;
    }

    std::error_code fileSizeError{};
    const std::uintmax_t fileSize = std::filesystem::file_size(path, fileSizeError);
    if (fileSizeError) {
        appendDiagnostic(loadResult.diagnostics, NavigationDiagnosticSeverity::Error,
            "Could not inspect MLD file: " + path.string());
        return loadResult;
    }
    if (fileSize == 0U) {
        appendDiagnostic(loadResult.diagnostics, NavigationDiagnosticSeverity::Error,
            "Selected MLD file is empty: " + path.string());
        return loadResult;
    }
    if (fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        appendDiagnostic(loadResult.diagnostics, NavigationDiagnosticSeverity::Error,
            "Selected MLD file is too large to read in one pass: " + path.string());
        return loadResult;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        appendDiagnostic(loadResult.diagnostics, NavigationDiagnosticSeverity::Error,
            "Could not open MLD file: " + path.string());
        return loadResult;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        appendDiagnostic(loadResult.diagnostics, NavigationDiagnosticSeverity::Error,
            "Could not read complete MLD file: " + path.string());
        return loadResult;
    }

    try {
        const auto byteSpan = std::span<const std::uint8_t>(bytes.data(), bytes.size());
        const spice::mld::parsing::MldParser parser{};
        const auto file = parser.parseBytes(byteSpan);

        NavigationAreaModel model{};
        model.source.path = path;
        model.source.byteSize = fileSize;
        appendDiagnostic(model.diagnostics, NavigationDiagnosticSeverity::Info,
            "Coordinate policy: identity (calibration pending).");
        for (const auto& diagnostic : file.parseDiagnostics) {
            appendDiagnostic(model.diagnostics, convertSeverity(diagnostic.severity), diagnostic.message);
        }
        if (file.parseStatus == spice::mld::model::MldParseStatus::Failed) {
            if (model.diagnostics.size() == 1U) {
                appendDiagnostic(model.diagnostics, NavigationDiagnosticSeverity::Error,
                    "SpiceMLD could not parse the selected MLD file.");
            }
            loadResult.diagnostics = std::move(model.diagnostics);
            return loadResult;
        }

        spice::mld::parsing::ParseOptions options{};
        // Coordinate calibration is intentionally centralized here. The first slice preserves SPICE's
        // native coordinate space; any future axis/scale/winding change must be applied to canonical
        // ground resources and transient wall projection in this adapter rather than in a renderer.
        options.coordinates = spice::mld::parsing::CoordinatePolicy{};
        options.preserveUnknownEntries = true;
        options.buildBlenderIntermediateIr = true;
        options.exportBlenderIrJson = false;
        options.extractGrndGobjBlocks = true;
        const auto parse = parser.project(file, options);
        for (const auto& diagnostic : parse.diagnostics) {
            appendDiagnosticOnce(model.diagnostics, convertSeverity(diagnostic.severity), diagnostic.message);
        }
        for (const auto& diagnostic : parse.blenderIrDiagnostics) {
            appendDiagnosticOnce(model.diagnostics, NavigationDiagnosticSeverity::Warning, diagnostic);
        }

        std::set<std::uint32_t> groundReferencedAddresses{};
        std::set<std::uint32_t> objectReferencedAddresses{};
        for (const auto& record : file.entries) {
            const auto grounds = nonZeroValues(record.entry.groundAddresses);
            groundReferencedAddresses.insert(grounds.begin(), grounds.end());
            const auto objects = nonZeroValues(record.entry.objectAddresses);
            objectReferencedAddresses.insert(objects.begin(), objects.end());
        }
        for (const auto address : objectReferencedAddresses) {
            if (groundReferencedAddresses.contains(address)) {
                continue;
            }
            const auto resource = file.groundResources.find(address);
            if (resource != file.groundResources.end() &&
                resource->second.kind == spice::mld::model::MldGroundResource::Kind::Gobj) {
                ++model.skippedObjectRoleGobjCount;
            }
        }

        std::set<std::pair<std::uint32_t, std::uint32_t>> processedGroundResources{};
        std::size_t expectedGroundResources = 0;
        std::size_t representedGroundResources = 0;
        std::size_t grndSurfaceCount = 0;
        std::size_t gobjSurfaceCount = 0;

        for (const auto& record : file.entries) {
            const auto& entry = record.entry;
            const auto linkedEntryIds = nonZeroValues(entry.groundLinks);
            for (const auto groundAddress : nonZeroValues(entry.groundAddresses)) {
                if (!processedGroundResources.emplace(entry.entryId, groundAddress).second) {
                    continue;
                }
                ++expectedGroundResources;
                bool represented = false;
                const auto resourceIt = file.groundResources.find(groundAddress);
                if (resourceIt != file.groundResources.end() && resourceIt->second.gobj.has_value()) {
                    const auto& gobj = *resourceIt->second.gobj;
                    const auto nodeMatrices = buildNodeWorldMatrices(gobj, model.diagnostics, groundAddress);
                    const Matrix4 entryMatrix = transformMatrix(entry.transform);
                    for (std::size_t nodeIndex = 0; nodeIndex < gobj.nodes.size(); ++nodeIndex) {
                        const auto& node = gobj.nodes[nodeIndex];
                        if (node.streamMesh.vertices.empty() || node.streamMesh.indices.empty()) {
                            continue;
                        }
                        NavigationSurface surface{};
                        surface.sourceKey = NavigationSurfaceSourceKey{
                            .sourceEntryId = entry.entryId,
                            .sourceBlockOffset = groundAddress,
                            .sourceNodeOffset = node.sourceNodeOffset,
                        };
                        surface.sourceKind = NavigationSurfaceSourceKind::Gobj;
                        surface.sourceTableIndex = entry.tableIndex;
                        surface.tblId = entry.tblId;
                        surface.fxnName = entry.fxnName;
                        surface.linkedEntryIds = linkedEntryIds;
                        surface.mesh = convertMesh(node.streamMesh,
                            multiply(entryMatrix, nodeMatrices[nodeIndex]), model.diagnostics,
                            "GOBJ ground block " + std::to_string(groundAddress));
                        if (surface.mesh.vertices.empty() || surface.mesh.indices.empty()) {
                            continue;
                        }
                        for (const auto& vertex : surface.mesh.vertices) {
                            updateBounds(model.bounds, vertex.position);
                        }
                        model.surfaces.push_back(std::move(surface));
                        ++gobjSurfaceCount;
                        represented = true;
                    }
                } else if (resourceIt != file.groundResources.end() && resourceIt->second.grnd.has_value()) {
                    NavigationSurface surface{};
                    surface.sourceKey = NavigationSurfaceSourceKey{
                        .sourceEntryId = entry.entryId,
                        .sourceBlockOffset = groundAddress,
                        .sourceNodeOffset = 0U,
                    };
                    surface.sourceKind = NavigationSurfaceSourceKind::Grnd;
                    surface.sourceTableIndex = entry.tableIndex;
                    surface.tblId = entry.tblId;
                    surface.fxnName = entry.fxnName;
                    surface.linkedEntryIds = linkedEntryIds;
                    surface.mesh = convertMesh(resourceIt->second.grnd->mesh, transformMatrix(entry.transform),
                        model.diagnostics, "GRND block " + std::to_string(groundAddress));
                    if (!surface.mesh.vertices.empty() && !surface.mesh.indices.empty()) {
                        for (const auto& vertex : surface.mesh.vertices) {
                            updateBounds(model.bounds, vertex.position);
                        }
                        model.surfaces.push_back(std::move(surface));
                        ++grndSurfaceCount;
                        represented = true;
                    }
                }

                if (represented) {
                    ++representedGroundResources;
                } else {
                    appendDiagnostic(model.diagnostics, NavigationDiagnosticSeverity::Warning,
                        "Ground resource entry=" + std::to_string(entry.entryId) +
                        " offset=" + std::to_string(groundAddress) + " produced no usable navigation surface.");
                }
            }
        }

        for (const auto& collision : parse.world.collisions) {
            model.regions.push_back(NavigationRegion{
                .kind = NavigationRegionKind::Collision,
                .sourceEntryId = collision.sourceEntryId,
                .fxnName = collision.fxnName,
                .tblId = collision.tblId,
                .transform = copyTransform(collision.transform),
                .objectAddresses = collision.objectAddresses,
            });
        }
        for (const auto& trigger : parse.world.triggers) {
            model.regions.push_back(NavigationRegion{
                .kind = NavigationRegionKind::Trigger,
                .sourceEntryId = trigger.sourceEntryId,
                .fxnName = trigger.fxnName,
                .tblId = trigger.tblId,
                .transform = copyTransform(trigger.transform),
                .objectAddresses = trigger.objectAddresses,
            });
        }
        for (const auto& unknown : parse.world.unknownEntries) {
            const bool isMovingObject = normalizeFxn(unknown.fxnName) == "motscpt";
            model.regions.push_back(NavigationRegion{
                .kind = isMovingObject ? NavigationRegionKind::MovingObject : NavigationRegionKind::Unknown,
                .sourceEntryId = unknown.sourceEntryId,
                .fxnName = unknown.fxnName,
                .tblId = unknown.tblId,
                .transform = copyTransform(unknown.transform),
                .rawPayloadSize = unknown.rawPayload.size(),
            });
            model.unknownEntryCount += isMovingObject ? 0U : 1U;
        }

        std::size_t wallRegionCount = 0;
        std::size_t wallMeshInstanceCount = 0;
        std::size_t wallVertexCount = 0;
        std::size_t wallTriangleCount = 0;
        std::size_t triggerRegionCount = 0;
        std::size_t triggerMeshInstanceCount = 0;
        std::size_t triggerVertexCount = 0;
        std::size_t triggerTriangleCount = 0;
        std::size_t movingObjectRegionCount = 0;
        std::size_t movingObjectMeshInstanceCount = 0;
        std::size_t movingObjectVertexCount = 0;
        std::size_t movingObjectTriangleCount = 0;
        std::vector<RegionMeshProjectionTarget> projectionTargets{};
        for (const auto& region : model.regions) {
            const bool isWall = region.kind == NavigationRegionKind::Collision &&
                normalizeFxn(region.fxnName) == "wall";
            if (isWall || region.kind == NavigationRegionKind::Trigger ||
                region.kind == NavigationRegionKind::MovingObject) {
                projectionTargets.push_back(RegionMeshProjectionTarget{
                    .kind = region.kind,
                    .sourceEntryId = region.sourceEntryId,
                    .tblId = region.tblId,
                });
            }
        }

        std::optional<RegionMeshProjectionResult> regionProjection{};
        if (parse.blenderIrScene.has_value()) {
            regionProjection = RegionMeshProjector{}.project(
                *parse.blenderIrScene,
                std::span<const RegionMeshProjectionTarget>{ projectionTargets });
            for (const auto& diagnostic : regionProjection->diagnostics) {
                appendDiagnosticOnce(model.diagnostics, diagnostic.severity, diagnostic.message);
            }
        }
        std::vector<bool> claimedProjectionRegions(
            regionProjection.has_value() ? regionProjection->regions.size() : 0U,
            false);
        for (auto& region : model.regions) {
            const bool isWall = region.kind == NavigationRegionKind::Collision &&
                normalizeFxn(region.fxnName) == "wall";
            const bool isTrigger = region.kind == NavigationRegionKind::Trigger;
            const bool isMovingObject = region.kind == NavigationRegionKind::MovingObject;
            if (!isWall && !isTrigger && !isMovingObject) {
                continue;
            }
            if (isWall) {
                ++wallRegionCount;
            } else if (isTrigger) {
                ++triggerRegionCount;
            } else {
                ++movingObjectRegionCount;
            }

            const ProjectedNavigationRegion* projected = nullptr;
            if (regionProjection.has_value()) {
                for (std::size_t index = 0; index < regionProjection->regions.size(); ++index) {
                    const auto& candidate = regionProjection->regions[index];
                    if (!claimedProjectionRegions[index] &&
                        candidate.kind == region.kind && candidate.sourceEntryId == region.sourceEntryId &&
                        candidate.tblId == region.tblId) {
                        claimedProjectionRegions[index] = true;
                        projected = &candidate;
                        break;
                    }
                }
            }
            if (projected == nullptr || !projected->complete) {
                if (isWall) {
                    ++model.failedWallRegionCount;
                    appendDiagnosticOnce(model.diagnostics, NavigationDiagnosticSeverity::Warning,
                        "Wall collision entry=" + std::to_string(region.sourceEntryId) +
                        " tbl=" + std::to_string(region.tblId) + " has no usable projected mesh.");
                } else if (isTrigger) {
                    ++model.failedTriggerRegionCount;
                    appendDiagnosticOnce(model.diagnostics, NavigationDiagnosticSeverity::Warning,
                        "Trigger entry=" + std::to_string(region.sourceEntryId) +
                        " tbl=" + std::to_string(region.tblId) +
                        " has no usable projected mesh; the Qt prototype will use an approximate cube marker.");
                } else {
                    ++model.failedMovingObjectRegionCount;
                    appendDiagnosticOnce(model.diagnostics, NavigationDiagnosticSeverity::Warning,
                        "Moving object entry=" + std::to_string(region.sourceEntryId) +
                        " tbl=" + std::to_string(region.tblId) +
                        " has no usable projected mesh; the Qt prototype will use an approximate cube marker.");
                }
                continue;
            }
            region.meshes = projected->meshes;
            if (isWall) {
                wallMeshInstanceCount += region.meshes.size();
            } else if (isTrigger) {
                triggerMeshInstanceCount += region.meshes.size();
            } else {
                movingObjectMeshInstanceCount += region.meshes.size();
            }
            for (const auto& regionMesh : region.meshes) {
                if (isWall) {
                    wallVertexCount += regionMesh.mesh.vertices.size();
                    wallTriangleCount += regionMesh.mesh.indices.size() / 3U;
                } else if (isTrigger) {
                    triggerVertexCount += regionMesh.mesh.vertices.size();
                    triggerTriangleCount += regionMesh.mesh.indices.size() / 3U;
                } else {
                    movingObjectVertexCount += regionMesh.mesh.vertices.size();
                    movingObjectTriangleCount += regionMesh.mesh.indices.size() / 3U;
                }
                for (const auto& vertex : regionMesh.mesh.vertices) {
                    updateBounds(model.bounds, vertex.position);
                }
            }
        }
        model.hasCompleteWallGeometry = model.failedWallRegionCount == 0U;
        model.hasCompleteTriggerGeometry = model.failedTriggerRegionCount == 0U;
        model.hasCompleteMovingObjectGeometry = model.failedMovingObjectRegionCount == 0U;

        std::unordered_map<std::uint32_t, std::vector<NavigationSurfaceSourceKey>> surfacesByEntry{};
        for (const auto& surface : model.surfaces) {
            surfacesByEntry[surface.sourceKey.sourceEntryId].push_back(surface.sourceKey);
        }
        std::set<std::pair<std::uint32_t, std::uint32_t>> linkDedup{};
        for (const auto& record : file.entries) {
            const auto& entry = record.entry;
            for (const auto targetEntryId : nonZeroValues(entry.groundLinks)) {
                if (!linkDedup.emplace(entry.entryId, targetEntryId).second) {
                    continue;
                }
                NavigationGroundLink link{};
                link.sourceEntryId = entry.entryId;
                link.targetEntryId = targetEntryId;
                if (const auto found = surfacesByEntry.find(entry.entryId); found != surfacesByEntry.end()) {
                    link.sourceSurfaces = found->second;
                }
                if (const auto found = surfacesByEntry.find(targetEntryId); found != surfacesByEntry.end()) {
                    link.targetSurfaces = found->second;
                }
                if (link.sourceSurfaces.empty() || link.targetSurfaces.empty()) {
                    link.resolution = NavigationGroundLinkResolution::Missing;
                } else if (link.sourceSurfaces.size() == 1U && link.targetSurfaces.size() == 1U) {
                    link.resolution = NavigationGroundLinkResolution::Resolved;
                } else {
                    link.resolution = NavigationGroundLinkResolution::Ambiguous;
                }
                model.groundLinks.push_back(std::move(link));
            }
        }

        model.failedGroundResourceCount = expectedGroundResources - representedGroundResources;
        model.hasCompleteGroundGeometry = expectedGroundResources > 0U && model.failedGroundResourceCount == 0U;

        std::size_t collisionCount = 0;
        std::size_t triggerCount = 0;
        for (const auto& region : model.regions) {
            collisionCount += region.kind == NavigationRegionKind::Collision ? 1U : 0U;
            triggerCount += region.kind == NavigationRegionKind::Trigger ? 1U : 0U;
        }
        std::size_t resolvedLinks = 0;
        std::size_t ambiguousLinks = 0;
        std::size_t missingLinks = 0;
        for (const auto& link : model.groundLinks) {
            resolvedLinks += link.resolution == NavigationGroundLinkResolution::Resolved ? 1U : 0U;
            ambiguousLinks += link.resolution == NavigationGroundLinkResolution::Ambiguous ? 1U : 0U;
            missingLinks += link.resolution == NavigationGroundLinkResolution::Missing ? 1U : 0U;
        }

        std::ostringstream summary{};
        summary << "Navigation model summary: expectedGroundResources=" << expectedGroundResources
                << ", representedGroundResources=" << representedGroundResources
                << ", GRND surfaces=" << grndSurfaceCount
                << ", ground-role GOBJ surfaces=" << gobjSurfaceCount
                << ", collisions=" << collisionCount
                << ", wallRegions=" << wallRegionCount
                << ", wallMeshes=" << wallMeshInstanceCount
                << ", wallVertices=" << wallVertexCount
                << ", wallTriangles=" << wallTriangleCount
                << ", failedWallRegions=" << model.failedWallRegionCount
                << ", triggers=" << triggerCount
                << ", triggerRegions=" << triggerRegionCount
                << ", triggerMeshes=" << triggerMeshInstanceCount
                << ", triggerVertices=" << triggerVertexCount
                << ", triggerTriangles=" << triggerTriangleCount
                << ", failedTriggerRegions=" << model.failedTriggerRegionCount
                << ", movingObjectRegions=" << movingObjectRegionCount
                << ", movingObjectMeshes=" << movingObjectMeshInstanceCount
                << ", movingObjectVertices=" << movingObjectVertexCount
                << ", movingObjectTriangles=" << movingObjectTriangleCount
                << ", failedMovingObjectRegions=" << model.failedMovingObjectRegionCount
                << ", unknown=" << model.unknownEntryCount
                << ", skippedObjectRoleGobj=" << model.skippedObjectRoleGobjCount << '.';
        appendDiagnostic(model.diagnostics, NavigationDiagnosticSeverity::Info, summary.str());
        appendDiagnostic(model.diagnostics, NavigationDiagnosticSeverity::Info,
            "Provisional ground links: resolved=" + std::to_string(resolvedLinks) +
            ", ambiguous=" + std::to_string(ambiguousLinks) +
            ", missing=" + std::to_string(missingLinks) +
            ". Link evidence is not pathfinding adjacency in this slice.");
        appendDiagnostic(model.diagnostics, NavigationDiagnosticSeverity::Info,
            "Spice search-world evidence: surfaces=" + std::to_string(parse.searchWorld.surfaces.size()) +
            ", regions=" + std::to_string(parse.searchWorld.regions.size()) + '.');

        if (model.surfaces.empty()) {
            appendDiagnostic(model.diagnostics, NavigationDiagnosticSeverity::Error,
                "MLD parse produced no usable ground navigation surfaces.");
            loadResult.diagnostics = std::move(model.diagnostics);
            return loadResult;
        }

        loadResult.status = model.hasCompleteGroundGeometry && model.hasCompleteWallGeometry
            ? NavigationAreaLoadStatus::Complete
            : NavigationAreaLoadStatus::Partial;
        loadResult.diagnostics = model.diagnostics;
        loadResult.model = std::move(model);
        return loadResult;
    } catch (const std::exception& error) {
        appendDiagnostic(loadResult.diagnostics, NavigationDiagnosticSeverity::Error,
            "Unexpected SpiceMLD load failure: " + std::string(error.what()));
        return loadResult;
    }
}

} // namespace savor::navigation
