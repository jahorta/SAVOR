#include "../SoaSimMLD/SoaSimMLD.h"
#include "../SoaSimSCT/SoaSimSCT.h"
#include "../Compression/Aklz.h"
#include "../Sa3Dport/Testing/Slice2TestApi.h"
#include "../Sa3Dport/Testing/Slice5TestApi.h"
#include "../Sa3Dport/Testing/Slice6TestApi.h"
#include "../Sa3Dport/Testing/Slice7TestApi.h"
#include "../Sa3Dport/Testing/Slice8TestApi.h"
#include "../Sa3Dport/Testing/Slice9TestApi.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {

std::vector<std::uint8_t> readAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }

    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0) {
        return {};
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

bool writeAllBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

struct CliOptions {
    std::filesystem::path inputDir{};
    std::filesystem::path outputDir{};
    bool runAbSa3dPortVsSa3dBridge = false;
};

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  SoaSimFileParsing [input_dir] [output_dir] [--ab-sa3d-port-vs-sa3d-bridge]\n\n"
        << "Notes:\n"
        << "  - input_dir defaults to SoaSimFileParsing/inputs\n"
        << "  - output_dir defaults to SoaSimFileParsing/parsed\n"
        << "  - --ab-sa3d-port-vs-sa3d-bridge enables A/B mode for .mld files.\n"
        << "  - Bridge executable path is auto-discovered at <SoaSimFileParsing.exe_dir>/sa3d_bridge/SA3DRefRunner.exe.\n"
        << "  - In A/B mode, all slices (0..9) run automatically per fixture using a per-fixture NJ block manifest.\n";
}

std::optional<CliOptions> parseCliOptions(int argc, char** argv, const std::filesystem::path& sourceDir) {
    CliOptions options{};
    options.inputDir = sourceDir / "inputs";
    options.outputDir = sourceDir / "parsed";

    int positionalIndex = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return std::nullopt;
        }
        if (arg == "--ab-sa3d-port-vs-sa3d-bridge") {
            options.runAbSa3dPortVsSa3dBridge = true;
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            return std::nullopt;
        }

        if (positionalIndex == 0) {
            options.inputDir = std::filesystem::path(arg);
        } else if (positionalIndex == 1) {
            options.outputDir = std::filesystem::path(arg);
        } else {
            std::cerr << "Unexpected positional argument: " << arg << "\n";
            return std::nullopt;
        }
        ++positionalIndex;
    }

    return options;
}

std::string quotePath(const std::filesystem::path& path) {
    std::string value = path.string();
    std::string escaped{};
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char c : value) {
        if (c == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

std::string jsonEscape(std::string value) {
    std::string escaped{};
    escaped.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(c);
            break;
        }
    }
    return escaped;
}

void writeFixtureManifestFromInputDir(const std::filesystem::path& inputDir, const std::filesystem::path& outputDir) {
    std::vector<std::filesystem::path> mldFiles{};
    for (const auto& entry : std::filesystem::directory_iterator(inputDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (toLowerCopy(entry.path().extension().string()) == ".mld") {
            mldFiles.push_back(entry.path().filename());
        }
    }

    std::sort(mldFiles.begin(), mldFiles.end());
    const auto manifestOutPath = outputDir / "FIXTURE_MANIFEST.generated.json";
    std::ofstream manifestOut(manifestOutPath, std::ios::binary);
    manifestOut << "{\n";
    manifestOut << "  \"schema\": \"soasim_fixture_manifest_v1\",\n";
    manifestOut << "  \"generated_by\": \"SoaSimFileParsing\",\n";
    manifestOut << "  \"fixture_root\": \"SoaSimFileParsing/inputs\",\n";
    manifestOut << "  \"fixtures\": [\n";
    for (std::size_t i = 0; i < mldFiles.size(); ++i) {
        const auto stem = mldFiles[i].stem().string();
        manifestOut << "    {\n";
        manifestOut << "      \"id\": \"" << stem << "\",\n";
        manifestOut << "      \"mld_path\": \"SoaSimFileParsing/inputs/" << mldFiles[i].string() << "\"\n";
        manifestOut << "    }";
        if (i + 1 < mldFiles.size()) {
            manifestOut << ",";
        }
        manifestOut << "\n";
    }
    manifestOut << "  ]\n";
    manifestOut << "}\n";
}

std::optional<std::filesystem::path> maybeInvokeDotnetBridge(
    const std::filesystem::path& processDir,
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& fixtureManifestPath,
    const std::filesystem::path& blockManifestPath,
    int slice) {
    const auto bridgePath = processDir / "sa3d_bridge" / "SA3DRefRunner.exe";
    if (!std::filesystem::exists(bridgePath)) {
        std::cerr << "[SoaSimFileParsing] WARNING: .NET bridge executable does not exist: "
                  << bridgePath.string() << "\n";
        return std::nullopt;
    }

    const auto bridgeOutPath = outputDir / (inputPath.stem().string() + ".slice_" + std::to_string(slice) + ".sa3d.reference.json");
    const auto command = quotePath(bridgePath) +
        " run-one" +
        " --input " + quotePath(inputPath) +
        " --out " + quotePath(outputDir) +
        " --output-file " + quotePath(bridgeOutPath) +
        " --manifest " + quotePath(fixtureManifestPath) +
        " --block-manifest " + quotePath(blockManifestPath) +
        " --slice " + std::to_string(slice);
    const auto systemCommand = "cmd /c \"" + command + "\"";
    const int exitCode = std::system(systemCommand.c_str());
    if (exitCode != 0) {
        std::cerr << "[SoaSimFileParsing] WARNING: .NET bridge run failed with exit code "
                  << exitCode << " for input " << inputPath.string() << "\n";
        if (!std::filesystem::exists(bridgeOutPath)) {
            return std::nullopt;
        }
    }
    if (!std::filesystem::exists(bridgeOutPath)) {
        std::cerr << "[SoaSimFileParsing] WARNING: .NET bridge did not emit expected output: "
                  << bridgeOutPath.string() << "\n";
        return std::nullopt;
    }
    return bridgeOutPath;
}

std::string toBlockKindLabel(const soasim::mld::parsing::ExtractedNjBlock::Kind kind) {
    switch (kind) {
    case soasim::mld::parsing::ExtractedNjBlock::Kind::Object:
        return "object";
    case soasim::mld::parsing::ExtractedNjBlock::Kind::Motion:
        return "motion";
    default:
        return "unknown";
    }
}

std::optional<soasim::mld::parsing::ExtractedNjBlock> normalizeBlockForSa3dBridge(
    const soasim::mld::parsing::ExtractedNjBlock& block) {
    if (block.bytes.empty()) {
        return std::nullopt;
    }

    auto normalized = block;
    if (block.kind == soasim::mld::parsing::ExtractedNjBlock::Kind::Object) {
        constexpr std::size_t kMldObjectHeaderSize = 0x10u;
        if (block.bytes.size() <= kMldObjectHeaderSize) {
            return std::nullopt;
        }

        normalized.offset += static_cast<std::uint32_t>(kMldObjectHeaderSize);
        normalized.bytes.assign(block.bytes.begin() + static_cast<std::ptrdiff_t>(kMldObjectHeaderSize), block.bytes.end());
        normalized.size = normalized.bytes.size();
    }

    return normalized;
}

void writeFixtureBlockManifest(
    const std::filesystem::path& outPath,
    const std::string_view fixtureId,
    const std::vector<std::filesystem::path>& blockInputPaths,
    const std::vector<soasim::mld::parsing::ExtractedNjBlock>& extractedBlocks) {
    std::ofstream out(outPath, std::ios::binary);
    out << "{\n";
    out << "  \"schema\": \"soasim_fixture_block_manifest_v1\",\n";
    out << "  \"fixture_id\": \"" << jsonEscape(std::string(fixtureId)) << "\",\n";
    out << "  \"blocks\": [\n";
    for (std::size_t i = 0; i < blockInputPaths.size() && i < extractedBlocks.size(); ++i) {
        const auto& block = extractedBlocks[i];
        out << "    {\n";
        out << "      \"index\": " << i << ",\n";
        out << "      \"kind\": \"" << toBlockKindLabel(block.kind) << "\",\n";
        out << "      \"offset\": " << block.offset << ",\n";
        out << "      \"size\": " << block.size << ",\n";
        out << "      \"includes_njtl_prefix\": " << (block.includesNjtlPrefix ? "true" : "false") << ",\n";
        out << "      \"path\": \"" << jsonEscape(std::filesystem::absolute(blockInputPaths[i]).string()) << "\"\n";
        out << "    }";
        if (i + 1 < blockInputPaths.size() && i + 1 < extractedBlocks.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool containsJsonProperty(const std::string& json, const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    return json.find(needle) != std::string::npos;
}

std::optional<std::size_t> findJsonPropertyValueStart(const std::string& json, const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const auto colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) {
        return std::nullopt;
    }

    auto valueStart = colonPos + 1;
    while (valueStart < json.size() && std::isspace(static_cast<unsigned char>(json[valueStart]))) {
        ++valueStart;
    }
    return valueStart;
}

std::optional<int> readJsonIntProperty(const std::string& json, const std::string_view key) {
    const auto valueStart = findJsonPropertyValueStart(json, key);
    if (!valueStart.has_value() || *valueStart >= json.size()) {
        return std::nullopt;
    }

    auto end = *valueStart;
    if (json[end] == '-') {
        ++end;
    }
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) {
        ++end;
    }
    if (end == *valueStart || (json[*valueStart] == '-' && end == *valueStart + 1)) {
        return std::nullopt;
    }

    try {
        return std::stoi(json.substr(*valueStart, end - *valueStart));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> readJsonStringProperty(const std::string& json, const std::string_view key) {
    const auto valueStart = findJsonPropertyValueStart(json, key);
    if (!valueStart.has_value() || *valueStart >= json.size() || json[*valueStart] != '"') {
        return std::nullopt;
    }

    std::string result{};
    for (std::size_t i = *valueStart + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') {
            return result;
        }
        if (c == '\\' && i + 1 < json.size()) {
            result.push_back(json[++i]);
            continue;
        }
        result.push_back(c);
    }

    return std::nullopt;
}

std::optional<bool> readJsonBoolProperty(const std::string& json, const std::string_view key) {
    const auto valueStart = findJsonPropertyValueStart(json, key);
    if (!valueStart.has_value()) {
        return std::nullopt;
    }
    if (json.compare(*valueStart, 4, "true") == 0) {
        return true;
    }
    if (json.compare(*valueStart, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

void fnvUpdateByte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
}

void fnvUpdateU32(std::uint64_t& hash, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        fnvUpdateByte(hash, static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

void fnvUpdateString(std::uint64_t& hash, std::string_view value) {
    for (const char c : value) {
        fnvUpdateByte(hash, static_cast<std::uint8_t>(c));
    }
    fnvUpdateByte(hash, 0);
}

std::string hex64(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i) {
        result[static_cast<std::size_t>(i)] = digits[value & 0xFu];
        value >>= 4;
    }
    return result;
}

struct Slice2Probe {
    std::size_t blockCount = 0;
    std::string blockMapHash = "cbf29ce484222325";
    std::size_t diagnosticCount = 0;
};

struct StagedSa3dProbe {
    std::size_t slice3ModelBlockCount = 0;
    std::size_t slice3ParsedModelCount = 0;
    std::size_t slice3NodeCount = 0;
    std::size_t slice3AttachRefCount = 0;
    std::size_t slice3GraphErrorCount = 0;
    std::string slice3StructuralHash = "cbf29ce484222325";
    std::size_t slice3DiagnosticCount = 0;
    std::string firstSlice3Diagnostic{};

    std::size_t slice4ChunkAttachCount = 0;
    std::size_t slice4VertexChunkCount = 0;
    std::size_t slice4VertexCount = 0;
    std::size_t slice4WeightedVertexChunkCount = 0;
    std::string slice4StructuralHash = "cbf29ce484222325";
    std::size_t slice4DiagnosticCount = 0;

    std::size_t slice5PolyChunkCount = 0;
    std::size_t slice5NullPolyChunkCount = 0;
    std::size_t slice5BitsChunkCount = 0;
    std::size_t slice5TextureChunkCount = 0;
    std::size_t slice5MaterialChunkCount = 0;
    std::size_t slice5MaterialBumpChunkCount = 0;
    std::size_t slice5StripChunkCount = 0;
    std::size_t slice5PolyCornerCount = 0;
    std::string slice5StructuralHash = "cbf29ce484222325";
    std::string slice5TypeHash = "cbf29ce484222325";
    std::string slice5AttributeHash = "cbf29ce484222325";
    std::string slice5ByteSizeHash = "cbf29ce484222325";
    std::string slice5StripMetaHash = "cbf29ce484222325";
    std::size_t slice5DiagnosticCount = 0;
    std::string firstAttachDiagnostic{};

    std::size_t slice6ModelFileCheckCount = 0;
    std::size_t slice6ParsedModelFileCount = 0;
    std::size_t slice6NodeCount = 0;
    std::size_t slice6AttachRefCount = 0;
    std::size_t slice6ChunkAttachCount = 0;
    std::size_t slice6PolyChunkCount = 0;
    std::string slice6StructuralHash = "cbf29ce484222325";
    std::size_t slice6DiagnosticCount = 0;
    std::string firstSlice6Diagnostic{};

    std::size_t slice7MotionBlockCount = 0;
    std::size_t slice7ParsedMotionCount = 0;
    std::size_t slice7NodeCount = 0;
    std::size_t slice7KeyframeSetCount = 0;
    std::size_t slice7ChannelCount = 0;
    std::size_t slice7KeyframeCount = 0;
    std::string slice7StructuralHash = "cbf29ce484222325";
    std::size_t slice7DiagnosticCount = 0;
    std::string firstSlice7Diagnostic{};

    std::size_t slice8AnimationFileCheckCount = 0;
    std::size_t slice8ParsedAnimationFileCount = 0;
    std::size_t slice8NodeCount = 0;
    std::size_t slice8KeyframeSetCount = 0;
    std::size_t slice8ChannelCount = 0;
    std::size_t slice8KeyframeCount = 0;
    std::string slice8StructuralHash = "cbf29ce484222325";
    std::size_t slice8DiagnosticCount = 0;
    std::string firstSlice8Diagnostic{};

    std::size_t slice9AttachCount = 0;
    std::size_t slice9BufferMeshCount = 0;
    std::size_t slice9BufferVertexCount = 0;
    std::size_t slice9BufferCornerCount = 0;
    std::size_t slice9BufferTriangleCornerCount = 0;
    std::size_t slice9WeightedMeshCount = 0;
    std::size_t slice9WeightedVertexCount = 0;
    std::size_t slice9WeightedTriangleSetCount = 0;
    std::size_t slice9WeightedTriangleCornerCount = 0;
    std::string slice9StructuralHash = "cbf29ce484222325";
    std::size_t slice9DiagnosticCount = 0;
    std::string firstSlice9Diagnostic{};
};

Slice2Probe buildSlice2Probe(const std::vector<soasim::mld::parsing::ExtractedNjBlock>& blocks) {
    Slice2Probe result{};
    std::uint64_t hash = 14695981039346656037ull;

    for (std::size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex) {
        const auto& block = blocks[blockIndex];
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(block.bytes.data()),
            block.bytes.size());
        const auto scan = Sa3Dport::Testing::Slice2::ScanNjBlocks(bytes);
        result.diagnosticCount += scan.diagnostics.size();

        for (const auto& item : scan.blocks) {
            ++result.blockCount;
            const std::string_view role = Sa3Dport::Testing::Slice2::RoleName(item.role);
            fnvUpdateU32(hash, static_cast<std::uint32_t>(blockIndex));
            fnvUpdateU32(hash, static_cast<std::uint32_t>(block.offset + item.offset));
            fnvUpdateU32(hash, item.offset);
            fnvUpdateU32(hash, item.header);
            fnvUpdateU32(hash, item.size);
            fnvUpdateString(hash, role);
        }
    }

    result.blockMapHash = hex64(hash);
    return result;
}

void updateSlice3ProbeWithNode(StagedSa3dProbe& result,
                               std::uint64_t& slice3Hash,
                               const Sa3Dport::ObjectData::NodePtr& node) {
    if (!node) {
        return;
    }

    ++result.slice3NodeCount;
    fnvUpdateU32(slice3Hash, static_cast<std::uint32_t>(node->attributes));
    fnvUpdateU32(slice3Hash, node->attach_address == 0 ? 0u : 1u);
    fnvUpdateU32(slice3Hash, node->child() ? 1u : 0u);
    fnvUpdateU32(slice3Hash, node->next() ? 1u : 0u);

    if (node->attach_address != 0) {
        ++result.slice3AttachRefCount;
    }
}

void updateAttachProbeWithNode(StagedSa3dProbe& result,
                               std::uint64_t& slice4Hash,
                               std::uint64_t& slice5Hash,
                               std::uint64_t& slice5TypeHash,
                               std::uint64_t& slice5AttributeHash,
                               std::uint64_t& slice5ByteSizeHash,
                               std::uint64_t& slice5StripMetaHash,
                               const Sa3Dport::ObjectData::NodePtr& node) {
    if (!node) {
        return;
    }
    const auto chunkAttach = std::dynamic_pointer_cast<Sa3Dport::Mesh::Chunk::ChunkAttach>(node->attach);
    if (!chunkAttach) {
        return;
    }

    ++result.slice4ChunkAttachCount;
    fnvUpdateU32(slice4Hash, static_cast<std::uint32_t>(chunkAttach->vertex_chunks.size()));
    fnvUpdateU32(slice4Hash, static_cast<std::uint32_t>(chunkAttach->poly_chunks.size()));

    for (const auto& vertexChunk : chunkAttach->vertex_chunks) {
        if (!vertexChunk.has_value()) {
            fnvUpdateU32(slice4Hash, 0u);
            continue;
        }

        ++result.slice4VertexChunkCount;
        result.slice4VertexCount += vertexChunk->vertices.size();
        if (vertexChunk->has_weight()) {
            ++result.slice4WeightedVertexChunkCount;
        }
        fnvUpdateU32(slice4Hash, static_cast<std::uint32_t>(vertexChunk->type));
        fnvUpdateU32(slice4Hash, vertexChunk->attributes);
        fnvUpdateU32(slice4Hash, vertexChunk->index_offset);
        fnvUpdateU32(slice4Hash, static_cast<std::uint32_t>(vertexChunk->vertices.size()));
    }

    for (const auto& polyChunk : chunkAttach->poly_chunks) {
        if (!polyChunk.has_value()) {
            ++result.slice5NullPolyChunkCount;
            fnvUpdateU32(slice5Hash, 0u);
            continue;
        }

        ++result.slice5PolyChunkCount;
        const auto& chunk = *polyChunk;
        fnvUpdateU32(slice5Hash, static_cast<std::uint32_t>(chunk->type));
        fnvUpdateU32(slice5Hash, chunk->attributes);
        fnvUpdateU32(slice5Hash, chunk->byte_size());
        fnvUpdateU32(slice5TypeHash, static_cast<std::uint32_t>(chunk->type));
        fnvUpdateU32(slice5AttributeHash, chunk->attributes);
        fnvUpdateU32(slice5ByteSizeHash, chunk->byte_size());

        if (std::dynamic_pointer_cast<Sa3Dport::Mesh::Chunk::PolyChunks::BitsChunk>(chunk)) {
            ++result.slice5BitsChunkCount;
        } else if (std::dynamic_pointer_cast<Sa3Dport::Mesh::Chunk::PolyChunks::TextureChunk>(chunk)) {
            ++result.slice5TextureChunkCount;
        } else if (std::dynamic_pointer_cast<Sa3Dport::Mesh::Chunk::PolyChunks::MaterialBumpChunk>(chunk)) {
            ++result.slice5MaterialBumpChunkCount;
        } else if (std::dynamic_pointer_cast<Sa3Dport::Mesh::Chunk::PolyChunks::MaterialChunk>(chunk)) {
            ++result.slice5MaterialChunkCount;
        } else if (const auto strip = std::dynamic_pointer_cast<Sa3Dport::Mesh::Chunk::PolyChunks::StripChunk>(chunk)) {
            ++result.slice5StripChunkCount;
            fnvUpdateU32(slice5Hash, static_cast<std::uint32_t>(strip->strips.size()));
            fnvUpdateU32(slice5Hash, strip->triangle_attribute_count);
            fnvUpdateU32(slice5StripMetaHash, static_cast<std::uint32_t>(strip->strips.size()));
            fnvUpdateU32(slice5StripMetaHash, strip->triangle_attribute_count);
            for (const auto& stripData : strip->strips) {
                result.slice5PolyCornerCount += stripData.corners.size();
                fnvUpdateU32(slice5Hash, static_cast<std::uint32_t>(stripData.corners.size()));
                fnvUpdateU32(slice5StripMetaHash, static_cast<std::uint32_t>(stripData.corners.size()));
            }
        }
    }
}

void updateSlice6ProbeWithNode(StagedSa3dProbe& result,
                               std::uint64_t& slice6Hash,
                               const Sa3Dport::ObjectData::NodePtr& node) {
    if (!node) {
        return;
    }

    ++result.slice6NodeCount;
    fnvUpdateU32(slice6Hash, static_cast<std::uint32_t>(node->attributes));
    fnvUpdateU32(slice6Hash, node->attach ? 1u : 0u);
    fnvUpdateU32(slice6Hash, node->child() ? 1u : 0u);
    fnvUpdateU32(slice6Hash, node->next() ? 1u : 0u);

    if (node->attach) {
        ++result.slice6AttachRefCount;
    }

    const auto chunkAttach = std::dynamic_pointer_cast<Sa3Dport::Mesh::Chunk::ChunkAttach>(node->attach);
    if (!chunkAttach) {
        return;
    }

    ++result.slice6ChunkAttachCount;
    fnvUpdateU32(slice6Hash, static_cast<std::uint32_t>(chunkAttach->vertex_chunks.size()));
    fnvUpdateU32(slice6Hash, static_cast<std::uint32_t>(chunkAttach->poly_chunks.size()));

    for (const auto& polyChunk : chunkAttach->poly_chunks) {
        if (!polyChunk.has_value()) {
            fnvUpdateU32(slice6Hash, 0u);
            continue;
        }

        ++result.slice6PolyChunkCount;
        const auto& chunk = *polyChunk;
        fnvUpdateU32(slice6Hash, static_cast<std::uint32_t>(chunk->type));
        fnvUpdateU32(slice6Hash, chunk->attributes);
        fnvUpdateU32(slice6Hash, chunk->byte_size());
    }
}

void updateSlice7ProbeWithMotion(StagedSa3dProbe& result,
                                 std::uint64_t& slice7Hash,
                                 const Sa3Dport::Animation::Motion& motion) {
    ++result.slice7ParsedMotionCount;
    result.slice7NodeCount += motion.node_count;
    fnvUpdateU32(slice7Hash, motion.node_count);
    fnvUpdateU32(slice7Hash, static_cast<std::uint32_t>(motion.interpolation_mode));
    fnvUpdateU32(slice7Hash, motion.short_rot ? 1u : 0u);
    fnvUpdateU32(slice7Hash, static_cast<std::uint32_t>(motion.manual_keyframe_types));
    fnvUpdateU32(slice7Hash, static_cast<std::uint32_t>(motion.keyframe_types()));
    fnvUpdateU32(slice7Hash, motion.frame_count());

    for (const auto& [nodeIndex, keyframes] : motion.keyframes) {
        ++result.slice7KeyframeSetCount;
        fnvUpdateU32(slice7Hash, static_cast<std::uint32_t>(nodeIndex));
        fnvUpdateU32(slice7Hash, static_cast<std::uint32_t>(keyframes.type));
        fnvUpdateU32(slice7Hash, keyframes.keyframe_count);
        fnvUpdateU32(slice7Hash, static_cast<std::uint32_t>(keyframes.channels.size()));
        for (const auto& channel : keyframes.channels) {
            ++result.slice7ChannelCount;
            result.slice7KeyframeCount += channel.count;
            fnvUpdateU32(slice7Hash, static_cast<std::uint32_t>(channel.type));
            fnvUpdateU32(slice7Hash, channel.count);
            fnvUpdateU32(slice7Hash, channel.first_frame);
            fnvUpdateU32(slice7Hash, channel.last_frame);
        }
    }
}

void updateSlice8ProbeWithMotion(StagedSa3dProbe& result,
                                 std::uint64_t& slice8Hash,
                                 const Sa3Dport::Animation::Motion& motion) {
    ++result.slice8ParsedAnimationFileCount;
    result.slice8NodeCount += motion.node_count;
    fnvUpdateU32(slice8Hash, motion.node_count);
    fnvUpdateU32(slice8Hash, static_cast<std::uint32_t>(motion.interpolation_mode));
    fnvUpdateU32(slice8Hash, motion.short_rot ? 1u : 0u);
    fnvUpdateU32(slice8Hash, static_cast<std::uint32_t>(motion.manual_keyframe_types));
    fnvUpdateU32(slice8Hash, static_cast<std::uint32_t>(motion.keyframe_types()));
    fnvUpdateU32(slice8Hash, motion.frame_count());

    for (const auto& [nodeIndex, keyframes] : motion.keyframes) {
        ++result.slice8KeyframeSetCount;
        fnvUpdateU32(slice8Hash, static_cast<std::uint32_t>(nodeIndex));
        fnvUpdateU32(slice8Hash, static_cast<std::uint32_t>(keyframes.type));
        fnvUpdateU32(slice8Hash, keyframes.keyframe_count);
        fnvUpdateU32(slice8Hash, static_cast<std::uint32_t>(keyframes.channels.size()));
        for (const auto& channel : keyframes.channels) {
            ++result.slice8ChannelCount;
            result.slice8KeyframeCount += channel.count;
            fnvUpdateU32(slice8Hash, static_cast<std::uint32_t>(channel.type));
            fnvUpdateU32(slice8Hash, channel.count);
            fnvUpdateU32(slice8Hash, channel.first_frame);
            fnvUpdateU32(slice8Hash, channel.last_frame);
        }
    }
}

void updateSlice9ProbeWithSummary(StagedSa3dProbe& result,
                                  std::uint64_t& slice9Hash,
                                  const Sa3Dport::Testing::Slice9::NormalizationSummary& summary) {
    result.slice9AttachCount += summary.attach_count;
    result.slice9BufferMeshCount += summary.buffer_mesh_count;
    result.slice9BufferVertexCount += summary.buffer_vertex_count;
    result.slice9BufferCornerCount += summary.buffer_corner_count;
    result.slice9BufferTriangleCornerCount += summary.buffer_triangle_corner_count;
    result.slice9WeightedMeshCount += summary.weighted_mesh_count;
    result.slice9WeightedVertexCount += summary.weighted_vertex_count;
    result.slice9WeightedTriangleSetCount += summary.weighted_triangle_set_count;
    result.slice9WeightedTriangleCornerCount += summary.weighted_triangle_corner_count;

    fnvUpdateU32(slice9Hash, summary.attach_count);
    fnvUpdateU32(slice9Hash, summary.buffer_mesh_count);
    fnvUpdateU32(slice9Hash, summary.buffer_vertex_count);
    fnvUpdateU32(slice9Hash, summary.buffer_corner_count);
    fnvUpdateU32(slice9Hash, summary.buffer_triangle_corner_count);
    fnvUpdateU32(slice9Hash, summary.weighted_mesh_count);
    fnvUpdateU32(slice9Hash, summary.weighted_vertex_count);
    fnvUpdateU32(slice9Hash, summary.weighted_triangle_set_count);
    fnvUpdateU32(slice9Hash, summary.weighted_triangle_corner_count);
}

StagedSa3dProbe buildStagedSa3dProbe(const std::vector<soasim::mld::parsing::ExtractedNjBlock>& blocks) {
    StagedSa3dProbe result{};
    std::uint64_t slice3Hash = 14695981039346656037ull;
    std::uint64_t slice4Hash = 14695981039346656037ull;
    std::uint64_t slice5Hash = 14695981039346656037ull;
    std::uint64_t slice5TypeHash = 14695981039346656037ull;
    std::uint64_t slice5AttributeHash = 14695981039346656037ull;
    std::uint64_t slice5ByteSizeHash = 14695981039346656037ull;
    std::uint64_t slice5StripMetaHash = 14695981039346656037ull;
    std::uint64_t slice6Hash = 14695981039346656037ull;
    std::uint64_t slice7Hash = 14695981039346656037ull;
    std::uint64_t slice8Hash = 14695981039346656037ull;
    std::uint64_t slice9Hash = 14695981039346656037ull;
    std::optional<std::uint32_t> lastModelNodeCount{};

    for (std::size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex) {
        const auto& block = blocks[blockIndex];
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(block.bytes.data()),
            block.bytes.size());
        const auto scan = Sa3Dport::Testing::Slice2::ScanNjBlocks(bytes);
        for (const auto& item : scan.blocks) {
            if (item.role == Sa3Dport::Testing::Slice2::NJBlockRole::Animation) {
                ++result.slice7MotionBlockCount;
                if (!lastModelNodeCount.has_value()) {
                    ++result.slice7DiagnosticCount;
                    if (result.firstSlice7Diagnostic.empty()) {
                        result.firstSlice7Diagnostic = "no preceding model node count";
                    }
                    continue;
                }
                try {
                    const auto motion = Sa3Dport::Testing::Slice7::ReadMotionBlock(bytes, *lastModelNodeCount);
                    fnvUpdateU32(slice7Hash, static_cast<std::uint32_t>(blockIndex));
                    fnvUpdateU32(slice7Hash, item.offset);
                    updateSlice7ProbeWithMotion(result, slice7Hash, motion);
                } catch (const std::exception& ex) {
                    ++result.slice7DiagnosticCount;
                    if (result.firstSlice7Diagnostic.empty()) {
                        result.firstSlice7Diagnostic = ex.what();
                    }
                }

                if (!lastModelNodeCount.has_value()) {
                    ++result.slice8DiagnosticCount;
                    if (result.firstSlice8Diagnostic.empty()) {
                        result.firstSlice8Diagnostic = "no preceding model node count";
                    }
                    continue;
                }
                try {
                    if (Sa3Dport::Testing::Slice8::CheckIsAnimationFile(bytes)) {
                        ++result.slice8AnimationFileCheckCount;
                    }
                    const auto animationFile = Sa3Dport::Testing::Slice8::ReadAnimationFile(bytes, *lastModelNodeCount);
                    fnvUpdateU32(slice8Hash, static_cast<std::uint32_t>(blockIndex));
                    fnvUpdateU32(slice8Hash, item.offset);
                    fnvUpdateU32(slice8Hash, animationFile.animation_block_address.value_or(0));
                    updateSlice8ProbeWithMotion(result, slice8Hash, animationFile.animation);
                } catch (const std::exception& ex) {
                    ++result.slice8DiagnosticCount;
                    if (result.firstSlice8Diagnostic.empty()) {
                        result.firstSlice8Diagnostic = ex.what();
                    }
                }
                continue;
            }

            if (item.role != Sa3Dport::Testing::Slice2::NJBlockRole::Model) {
                continue;
            }

            ++result.slice3ModelBlockCount;
            const auto modelAddress = item.offset + 8u;
            const auto format = item.header == 0x4D434A4Eu
                ? Sa3Dport::ObjectData::Enums::ModelFormat::SA2
                : Sa3Dport::ObjectData::Enums::ModelFormat::SA1;
            try {
                Sa3Dport::Structs::EndianStackReader reader(bytes, scan.size_endian);
                Sa3Dport::ObjectData::NodeReadContext context;
                context.image_base = 0u - modelAddress;
                context.read_attach = false;
                const auto root = Sa3Dport::ObjectData::Node::read(reader, modelAddress, format, context);
                const auto validation = root->validate_graph();
                if (!validation.ok) {
                    result.slice3GraphErrorCount += validation.diagnostics.size();
                }
                ++result.slice3ParsedModelCount;
                fnvUpdateU32(slice3Hash, static_cast<std::uint32_t>(blockIndex));
                fnvUpdateU32(slice3Hash, item.offset);
                fnvUpdateU32(slice4Hash, static_cast<std::uint32_t>(blockIndex));
                fnvUpdateU32(slice4Hash, item.offset);
                fnvUpdateU32(slice5Hash, static_cast<std::uint32_t>(blockIndex));
                fnvUpdateU32(slice5Hash, item.offset);

                for (const auto& node : root->tree_nodes()) {
                    updateSlice3ProbeWithNode(result, slice3Hash, node);
                }
                lastModelNodeCount = static_cast<std::uint32_t>(root->tree_nodes().size());
            } catch (const std::exception& ex) {
                ++result.slice3DiagnosticCount;
                ++result.slice4DiagnosticCount;
                ++result.slice5DiagnosticCount;
                if (result.firstSlice3Diagnostic.empty()) {
                    result.firstSlice3Diagnostic = ex.what();
                }
                continue;
            }

            if (format != Sa3Dport::ObjectData::Enums::ModelFormat::SA2) {
                continue;
            }

            try {
                Sa3Dport::Structs::EndianStackReader reader(bytes, scan.size_endian);
                Sa3Dport::ObjectData::NodeReadContext context;
                context.image_base = 0u - modelAddress;
                context.read_attach = true;
                const auto root = Sa3Dport::ObjectData::Node::read(reader, modelAddress, format, context);
                for (const auto& node : root->tree_nodes()) {
                    updateAttachProbeWithNode(
                        result,
                        slice4Hash,
                        slice5Hash,
                        slice5TypeHash,
                        slice5AttributeHash,
                        slice5ByteSizeHash,
                        slice5StripMetaHash,
                        node);
                }
            } catch (const std::exception& ex) {
                ++result.slice4DiagnosticCount;
                ++result.slice5DiagnosticCount;
                if (result.firstAttachDiagnostic.empty()) {
                    result.firstAttachDiagnostic = ex.what();
                }
            }

            try {
                if (Sa3Dport::Testing::Slice6::CheckIsModelFile(bytes)) {
                    ++result.slice6ModelFileCheckCount;
                }
                const auto modelFile = Sa3Dport::Testing::Slice6::ReadModelFile(bytes);
                ++result.slice6ParsedModelFileCount;
                lastModelNodeCount = static_cast<std::uint32_t>(modelFile.model->tree_nodes().size());
                fnvUpdateU32(slice6Hash, static_cast<std::uint32_t>(blockIndex));
                fnvUpdateU32(slice6Hash, item.offset);
                fnvUpdateU32(slice6Hash, static_cast<std::uint32_t>(modelFile.format));
                for (const auto& node : modelFile.model->tree_nodes()) {
                    updateSlice6ProbeWithNode(result, slice6Hash, node);
                }

                try {
                    const auto normalization = Sa3Dport::Testing::Slice9::SummarizeNodeTree(modelFile.model);
                    fnvUpdateU32(slice9Hash, static_cast<std::uint32_t>(blockIndex));
                    fnvUpdateU32(slice9Hash, item.offset);
                    updateSlice9ProbeWithSummary(result, slice9Hash, normalization);
                } catch (const std::exception& ex) {
                    ++result.slice9DiagnosticCount;
                    if (result.firstSlice9Diagnostic.empty()) {
                        result.firstSlice9Diagnostic = ex.what();
                    }
                }
            } catch (const std::exception& ex) {
                ++result.slice6DiagnosticCount;
                if (result.firstSlice6Diagnostic.empty()) {
                    result.firstSlice6Diagnostic = ex.what();
                }
            }
        }
    }

    result.slice3StructuralHash = hex64(slice3Hash);
    result.slice4StructuralHash = hex64(slice4Hash);
    result.slice5StructuralHash = hex64(slice5Hash);
    result.slice5TypeHash = hex64(slice5TypeHash);
    result.slice5AttributeHash = hex64(slice5AttributeHash);
    result.slice5ByteSizeHash = hex64(slice5ByteSizeHash);
    result.slice5StripMetaHash = hex64(slice5StripMetaHash);
    result.slice6StructuralHash = hex64(slice6Hash);
    result.slice7StructuralHash = hex64(slice7Hash);
    result.slice8StructuralHash = hex64(slice8Hash);
    result.slice9StructuralHash = hex64(slice9Hash);
    return result;
}

struct ReferenceReportProbe {
    bool readable = false;
    bool hasSchema = false;
    bool hasFixture = false;
    bool hasSliceIoPairs = false;
    bool hasComparison = false;
    bool pass = false;
    int mismatchCount = 0;
    std::optional<int> blockCount{};
    std::optional<std::string> blockMapHash{};
    std::optional<int> slice3ModelBlockCount{};
    std::optional<int> slice3ParsedModelCount{};
    std::optional<int> slice3NodeCount{};
    std::optional<int> slice3AttachRefCount{};
    std::optional<int> slice3GraphErrorCount{};
    std::optional<std::string> slice3StructuralHash{};
    std::optional<int> slice4ChunkAttachCount{};
    std::optional<int> slice4VertexChunkCount{};
    std::optional<int> slice4VertexCount{};
    std::optional<int> slice4WeightedVertexChunkCount{};
    std::optional<std::string> slice4StructuralHash{};
    std::optional<int> slice5PolyChunkCount{};
    std::optional<int> slice5NullPolyChunkCount{};
    std::optional<int> slice5BitsChunkCount{};
    std::optional<int> slice5TextureChunkCount{};
    std::optional<int> slice5MaterialChunkCount{};
    std::optional<int> slice5MaterialBumpChunkCount{};
    std::optional<int> slice5StripChunkCount{};
    std::optional<int> slice5PolyCornerCount{};
    std::optional<std::string> slice5StructuralHash{};
    std::optional<std::string> slice5TypeHash{};
    std::optional<std::string> slice5AttributeHash{};
    std::optional<std::string> slice5ByteSizeHash{};
    std::optional<std::string> slice5StripMetaHash{};
    std::optional<int> slice6ModelFileCheckCount{};
    std::optional<int> slice6ParsedModelFileCount{};
    std::optional<int> slice6NodeCount{};
    std::optional<int> slice6AttachRefCount{};
    std::optional<int> slice6ChunkAttachCount{};
    std::optional<int> slice6PolyChunkCount{};
    std::optional<std::string> slice6StructuralHash{};
    std::optional<int> slice7MotionBlockCount{};
    std::optional<int> slice7ParsedMotionCount{};
    std::optional<int> slice7NodeCount{};
    std::optional<int> slice7KeyframeSetCount{};
    std::optional<int> slice7ChannelCount{};
    std::optional<int> slice7KeyframeCount{};
    std::optional<std::string> slice7StructuralHash{};
    std::optional<int> slice8AnimationFileCheckCount{};
    std::optional<int> slice8ParsedAnimationFileCount{};
    std::optional<int> slice8NodeCount{};
    std::optional<int> slice8KeyframeSetCount{};
    std::optional<int> slice8ChannelCount{};
    std::optional<int> slice8KeyframeCount{};
    std::optional<std::string> slice8StructuralHash{};
    std::optional<int> slice9AttachCount{};
    std::optional<int> slice9BufferMeshCount{};
    std::optional<int> slice9BufferVertexCount{};
    std::optional<int> slice9BufferCornerCount{};
    std::optional<int> slice9BufferTriangleCornerCount{};
    std::optional<int> slice9WeightedMeshCount{};
    std::optional<int> slice9WeightedVertexCount{};
    std::optional<int> slice9WeightedTriangleSetCount{};
    std::optional<int> slice9WeightedTriangleCornerCount{};
    std::optional<std::string> slice9StructuralHash{};
};

ReferenceReportProbe probeReferenceReport(const std::filesystem::path& reportPath) {
    const std::string json = readTextFile(reportPath);
    ReferenceReportProbe probe{};
    if (json.empty()) {
        return probe;
    }

    probe.readable = true;
    probe.hasSchema = containsJsonProperty(json, "schema");
    probe.hasFixture = containsJsonProperty(json, "fixture");
    probe.hasSliceIoPairs = containsJsonProperty(json, "slice_io_pairs");
    probe.hasComparison = containsJsonProperty(json, "comparison");
    probe.pass = readJsonBoolProperty(json, "pass").value_or(false);
    probe.mismatchCount = readJsonIntProperty(json, "mismatch_count").value_or(0);
    probe.blockCount = readJsonIntProperty(json, "block_count");
    probe.blockMapHash = readJsonStringProperty(json, "block_map_hash");
    probe.slice3ModelBlockCount = readJsonIntProperty(json, "slice3_model_block_count");
    probe.slice3ParsedModelCount = readJsonIntProperty(json, "slice3_parsed_model_count");
    probe.slice3NodeCount = readJsonIntProperty(json, "slice3_node_count");
    probe.slice3AttachRefCount = readJsonIntProperty(json, "slice3_attach_ref_count");
    probe.slice3GraphErrorCount = readJsonIntProperty(json, "slice3_graph_error_count");
    probe.slice3StructuralHash = readJsonStringProperty(json, "slice3_structural_hash");
    probe.slice4ChunkAttachCount = readJsonIntProperty(json, "slice4_chunk_attach_count");
    probe.slice4VertexChunkCount = readJsonIntProperty(json, "slice4_vertex_chunk_count");
    probe.slice4VertexCount = readJsonIntProperty(json, "slice4_vertex_count");
    probe.slice4WeightedVertexChunkCount = readJsonIntProperty(json, "slice4_weighted_vertex_chunk_count");
    probe.slice4StructuralHash = readJsonStringProperty(json, "slice4_structural_hash");
    probe.slice5PolyChunkCount = readJsonIntProperty(json, "slice5_poly_chunk_count");
    probe.slice5NullPolyChunkCount = readJsonIntProperty(json, "slice5_null_poly_chunk_count");
    probe.slice5BitsChunkCount = readJsonIntProperty(json, "slice5_bits_chunk_count");
    probe.slice5TextureChunkCount = readJsonIntProperty(json, "slice5_texture_chunk_count");
    probe.slice5MaterialChunkCount = readJsonIntProperty(json, "slice5_material_chunk_count");
    probe.slice5MaterialBumpChunkCount = readJsonIntProperty(json, "slice5_material_bump_chunk_count");
    probe.slice5StripChunkCount = readJsonIntProperty(json, "slice5_strip_chunk_count");
    probe.slice5PolyCornerCount = readJsonIntProperty(json, "slice5_poly_corner_count");
    probe.slice5StructuralHash = readJsonStringProperty(json, "slice5_structural_hash");
    probe.slice5TypeHash = readJsonStringProperty(json, "slice5_type_hash");
    probe.slice5AttributeHash = readJsonStringProperty(json, "slice5_attribute_hash");
    probe.slice5ByteSizeHash = readJsonStringProperty(json, "slice5_byte_size_hash");
    probe.slice5StripMetaHash = readJsonStringProperty(json, "slice5_strip_meta_hash");
    probe.slice6ModelFileCheckCount = readJsonIntProperty(json, "slice6_model_file_check_count");
    probe.slice6ParsedModelFileCount = readJsonIntProperty(json, "slice6_parsed_model_file_count");
    probe.slice6NodeCount = readJsonIntProperty(json, "slice6_node_count");
    probe.slice6AttachRefCount = readJsonIntProperty(json, "slice6_attach_ref_count");
    probe.slice6ChunkAttachCount = readJsonIntProperty(json, "slice6_chunk_attach_count");
    probe.slice6PolyChunkCount = readJsonIntProperty(json, "slice6_poly_chunk_count");
    probe.slice6StructuralHash = readJsonStringProperty(json, "slice6_structural_hash");
    probe.slice7MotionBlockCount = readJsonIntProperty(json, "slice7_motion_block_count");
    probe.slice7ParsedMotionCount = readJsonIntProperty(json, "slice7_parsed_motion_count");
    probe.slice7NodeCount = readJsonIntProperty(json, "slice7_node_count");
    probe.slice7KeyframeSetCount = readJsonIntProperty(json, "slice7_keyframe_set_count");
    probe.slice7ChannelCount = readJsonIntProperty(json, "slice7_channel_count");
    probe.slice7KeyframeCount = readJsonIntProperty(json, "slice7_keyframe_count");
    probe.slice7StructuralHash = readJsonStringProperty(json, "slice7_structural_hash");
    probe.slice8AnimationFileCheckCount = readJsonIntProperty(json, "slice8_animation_file_check_count");
    probe.slice8ParsedAnimationFileCount = readJsonIntProperty(json, "slice8_parsed_animation_file_count");
    probe.slice8NodeCount = readJsonIntProperty(json, "slice8_node_count");
    probe.slice8KeyframeSetCount = readJsonIntProperty(json, "slice8_keyframe_set_count");
    probe.slice8ChannelCount = readJsonIntProperty(json, "slice8_channel_count");
    probe.slice8KeyframeCount = readJsonIntProperty(json, "slice8_keyframe_count");
    probe.slice8StructuralHash = readJsonStringProperty(json, "slice8_structural_hash");
    probe.slice9AttachCount = readJsonIntProperty(json, "slice9_attach_count");
    probe.slice9BufferMeshCount = readJsonIntProperty(json, "slice9_buffer_mesh_count");
    probe.slice9BufferVertexCount = readJsonIntProperty(json, "slice9_buffer_vertex_count");
    probe.slice9BufferCornerCount = readJsonIntProperty(json, "slice9_buffer_corner_count");
    probe.slice9BufferTriangleCornerCount = readJsonIntProperty(json, "slice9_buffer_triangle_corner_count");
    probe.slice9WeightedMeshCount = readJsonIntProperty(json, "slice9_weighted_mesh_count");
    probe.slice9WeightedVertexCount = readJsonIntProperty(json, "slice9_weighted_vertex_count");
    probe.slice9WeightedTriangleSetCount = readJsonIntProperty(json, "slice9_weighted_triangle_set_count");
    probe.slice9WeightedTriangleCornerCount = readJsonIntProperty(json, "slice9_weighted_triangle_corner_count");
    probe.slice9StructuralHash = readJsonStringProperty(json, "slice9_structural_hash");
    return probe;
}

void writeBridgeAbComparison(
    const std::filesystem::path& outPath,
    const soasim::mld::parsing::ParseResult& sa3dPortParsed,
    const std::vector<soasim::mld::parsing::ExtractedNjBlock>& parityBlocks,
    const std::vector<std::filesystem::path>& bridgeReportPaths) {
    std::ofstream out(outPath, std::ios::binary);
    out << "mode=sa3d_port_vs_dotnet_sa3d\n";
    out << "sa3d_port.diagnostics=" << sa3dPortParsed.diagnostics.size() << "\n";
    out << "sa3d_port.extracted_nj_blocks=" << sa3dPortParsed.extractedNjBlocks.size() << "\n";
    const auto slice2Probe = buildSlice2Probe(parityBlocks);
    const auto stagedProbe = buildStagedSa3dProbe(parityBlocks);
    out << "sa3d_port.slice2.block_count=" << slice2Probe.blockCount << "\n";
    out << "sa3d_port.slice2.block_map_hash=" << slice2Probe.blockMapHash << "\n";
    out << "sa3d_port.slice2.diagnostic_count=" << slice2Probe.diagnosticCount << "\n";
    out << "sa3d_port.slice3.model_block_count=" << stagedProbe.slice3ModelBlockCount << "\n";
    out << "sa3d_port.slice3.parsed_model_count=" << stagedProbe.slice3ParsedModelCount << "\n";
    out << "sa3d_port.slice3.node_count=" << stagedProbe.slice3NodeCount << "\n";
    out << "sa3d_port.slice3.attach_ref_count=" << stagedProbe.slice3AttachRefCount << "\n";
    out << "sa3d_port.slice3.graph_error_count=" << stagedProbe.slice3GraphErrorCount << "\n";
    out << "sa3d_port.slice3.structural_hash=" << stagedProbe.slice3StructuralHash << "\n";
    out << "sa3d_port.slice3.diagnostic_count=" << stagedProbe.slice3DiagnosticCount << "\n";
    if (!stagedProbe.firstSlice3Diagnostic.empty()) {
        out << "sa3d_port.slice3.first_diagnostic=" << stagedProbe.firstSlice3Diagnostic << "\n";
    }
    out << "sa3d_port.slice4.chunk_attach_count=" << stagedProbe.slice4ChunkAttachCount << "\n";
    out << "sa3d_port.slice4.vertex_chunk_count=" << stagedProbe.slice4VertexChunkCount << "\n";
    out << "sa3d_port.slice4.vertex_count=" << stagedProbe.slice4VertexCount << "\n";
    out << "sa3d_port.slice4.weighted_vertex_chunk_count=" << stagedProbe.slice4WeightedVertexChunkCount << "\n";
    out << "sa3d_port.slice4.structural_hash=" << stagedProbe.slice4StructuralHash << "\n";
    out << "sa3d_port.slice4.diagnostic_count=" << stagedProbe.slice4DiagnosticCount << "\n";
    out << "sa3d_port.slice5.poly_chunk_count=" << stagedProbe.slice5PolyChunkCount << "\n";
    out << "sa3d_port.slice5.null_poly_chunk_count=" << stagedProbe.slice5NullPolyChunkCount << "\n";
    out << "sa3d_port.slice5.bits_chunk_count=" << stagedProbe.slice5BitsChunkCount << "\n";
    out << "sa3d_port.slice5.texture_chunk_count=" << stagedProbe.slice5TextureChunkCount << "\n";
    out << "sa3d_port.slice5.material_chunk_count=" << stagedProbe.slice5MaterialChunkCount << "\n";
    out << "sa3d_port.slice5.material_bump_chunk_count=" << stagedProbe.slice5MaterialBumpChunkCount << "\n";
    out << "sa3d_port.slice5.strip_chunk_count=" << stagedProbe.slice5StripChunkCount << "\n";
    out << "sa3d_port.slice5.poly_corner_count=" << stagedProbe.slice5PolyCornerCount << "\n";
    out << "sa3d_port.slice5.structural_hash=" << stagedProbe.slice5StructuralHash << "\n";
    out << "sa3d_port.slice5.type_hash=" << stagedProbe.slice5TypeHash << "\n";
    out << "sa3d_port.slice5.attribute_hash=" << stagedProbe.slice5AttributeHash << "\n";
    out << "sa3d_port.slice5.byte_size_hash=" << stagedProbe.slice5ByteSizeHash << "\n";
    out << "sa3d_port.slice5.strip_meta_hash=" << stagedProbe.slice5StripMetaHash << "\n";
    out << "sa3d_port.slice5.diagnostic_count=" << stagedProbe.slice5DiagnosticCount << "\n";
    if (!stagedProbe.firstAttachDiagnostic.empty()) {
        out << "sa3d_port.attach.first_diagnostic=" << stagedProbe.firstAttachDiagnostic << "\n";
    }
    out << "sa3d_port.slice6.model_file_check_count=" << stagedProbe.slice6ModelFileCheckCount << "\n";
    out << "sa3d_port.slice6.parsed_model_file_count=" << stagedProbe.slice6ParsedModelFileCount << "\n";
    out << "sa3d_port.slice6.node_count=" << stagedProbe.slice6NodeCount << "\n";
    out << "sa3d_port.slice6.attach_ref_count=" << stagedProbe.slice6AttachRefCount << "\n";
    out << "sa3d_port.slice6.chunk_attach_count=" << stagedProbe.slice6ChunkAttachCount << "\n";
    out << "sa3d_port.slice6.poly_chunk_count=" << stagedProbe.slice6PolyChunkCount << "\n";
    out << "sa3d_port.slice6.structural_hash=" << stagedProbe.slice6StructuralHash << "\n";
    out << "sa3d_port.slice6.diagnostic_count=" << stagedProbe.slice6DiagnosticCount << "\n";
    if (!stagedProbe.firstSlice6Diagnostic.empty()) {
        out << "sa3d_port.slice6.first_diagnostic=" << stagedProbe.firstSlice6Diagnostic << "\n";
    }
    out << "sa3d_port.slice7.motion_block_count=" << stagedProbe.slice7MotionBlockCount << "\n";
    out << "sa3d_port.slice7.parsed_motion_count=" << stagedProbe.slice7ParsedMotionCount << "\n";
    out << "sa3d_port.slice7.node_count=" << stagedProbe.slice7NodeCount << "\n";
    out << "sa3d_port.slice7.keyframe_set_count=" << stagedProbe.slice7KeyframeSetCount << "\n";
    out << "sa3d_port.slice7.channel_count=" << stagedProbe.slice7ChannelCount << "\n";
    out << "sa3d_port.slice7.keyframe_count=" << stagedProbe.slice7KeyframeCount << "\n";
    out << "sa3d_port.slice7.structural_hash=" << stagedProbe.slice7StructuralHash << "\n";
    out << "sa3d_port.slice7.diagnostic_count=" << stagedProbe.slice7DiagnosticCount << "\n";
    if (!stagedProbe.firstSlice7Diagnostic.empty()) {
        out << "sa3d_port.slice7.first_diagnostic=" << stagedProbe.firstSlice7Diagnostic << "\n";
    }
    out << "sa3d_port.slice8.animation_file_check_count=" << stagedProbe.slice8AnimationFileCheckCount << "\n";
    out << "sa3d_port.slice8.parsed_animation_file_count=" << stagedProbe.slice8ParsedAnimationFileCount << "\n";
    out << "sa3d_port.slice8.node_count=" << stagedProbe.slice8NodeCount << "\n";
    out << "sa3d_port.slice8.keyframe_set_count=" << stagedProbe.slice8KeyframeSetCount << "\n";
    out << "sa3d_port.slice8.channel_count=" << stagedProbe.slice8ChannelCount << "\n";
    out << "sa3d_port.slice8.keyframe_count=" << stagedProbe.slice8KeyframeCount << "\n";
    out << "sa3d_port.slice8.structural_hash=" << stagedProbe.slice8StructuralHash << "\n";
    out << "sa3d_port.slice8.diagnostic_count=" << stagedProbe.slice8DiagnosticCount << "\n";
    if (!stagedProbe.firstSlice8Diagnostic.empty()) {
        out << "sa3d_port.slice8.first_diagnostic=" << stagedProbe.firstSlice8Diagnostic << "\n";
    }
    out << "sa3d_port.slice9.attach_count=" << stagedProbe.slice9AttachCount << "\n";
    out << "sa3d_port.slice9.buffer_mesh_count=" << stagedProbe.slice9BufferMeshCount << "\n";
    out << "sa3d_port.slice9.buffer_vertex_count=" << stagedProbe.slice9BufferVertexCount << "\n";
    out << "sa3d_port.slice9.buffer_corner_count=" << stagedProbe.slice9BufferCornerCount << "\n";
    out << "sa3d_port.slice9.buffer_triangle_corner_count=" << stagedProbe.slice9BufferTriangleCornerCount << "\n";
    out << "sa3d_port.slice9.weighted_mesh_count=" << stagedProbe.slice9WeightedMeshCount << "\n";
    out << "sa3d_port.slice9.weighted_vertex_count=" << stagedProbe.slice9WeightedVertexCount << "\n";
    out << "sa3d_port.slice9.weighted_triangle_set_count=" << stagedProbe.slice9WeightedTriangleSetCount << "\n";
    out << "sa3d_port.slice9.weighted_triangle_corner_count=" << stagedProbe.slice9WeightedTriangleCornerCount << "\n";
    out << "sa3d_port.slice9.structural_hash=" << stagedProbe.slice9StructuralHash << "\n";
    out << "sa3d_port.slice9.diagnostic_count=" << stagedProbe.slice9DiagnosticCount << "\n";
    if (!stagedProbe.firstSlice9Diagnostic.empty()) {
        out << "sa3d_port.slice9.first_diagnostic=" << stagedProbe.firstSlice9Diagnostic << "\n";
    }
    out.flush();

    if (bridgeReportPaths.empty()) {
        out << "reference.present=false\n";
        out << "comparison.status=missing_reference_output\n";
        return;
    }

    out << "reference.present=true\n";
    out << "reference.reports=" << bridgeReportPaths.size() << "\n";
    bool allReportsSchemaReady = true;
    bool allReportsPass = true;
    std::size_t schemaReadyCount = 0;
    std::size_t readableCount = 0;
    std::size_t sliceIoReadyCount = 0;
    int totalReferenceMismatches = 0;
    std::optional<int> referenceBlockCount{};
    std::optional<int> referenceSlice2BlockCount{};
    std::optional<std::string> referenceSlice2BlockMapHash{};
    std::optional<ReferenceReportProbe> referenceSlice3{};
    std::optional<ReferenceReportProbe> referenceSlice4{};
    std::optional<ReferenceReportProbe> referenceSlice5{};
    std::optional<ReferenceReportProbe> referenceSlice6{};
    std::optional<ReferenceReportProbe> referenceSlice7{};
    std::optional<ReferenceReportProbe> referenceSlice8{};
    std::optional<ReferenceReportProbe> referenceSlice9{};
    for (std::size_t i = 0; i < bridgeReportPaths.size(); ++i) {
        out << "reference.path[" << i << "]=" << bridgeReportPaths[i].string() << "\n";

        const auto probe = probeReferenceReport(bridgeReportPaths[i]);
        if (probe.readable) {
            ++readableCount;
        }
        if (probe.hasSliceIoPairs) {
            ++sliceIoReadyCount;
        }
        totalReferenceMismatches += probe.mismatchCount;
        allReportsPass = allReportsPass && probe.pass;

        const bool reportReady = probe.readable
            && probe.hasSchema
            && probe.hasFixture
            && probe.hasSliceIoPairs
            && probe.hasComparison;
        if (reportReady) {
            ++schemaReadyCount;
        } else {
            allReportsSchemaReady = false;
        }
        if (!referenceBlockCount.has_value() && probe.blockCount.has_value()) {
            referenceBlockCount = probe.blockCount;
        }
        if (bridgeReportPaths[i].filename().string().find(".slice_2.") != std::string::npos) {
            referenceSlice2BlockCount = probe.blockCount;
            referenceSlice2BlockMapHash = probe.blockMapHash;
        }
        if (bridgeReportPaths[i].filename().string().find(".slice_3.") != std::string::npos) {
            referenceSlice3 = probe;
        }
        if (bridgeReportPaths[i].filename().string().find(".slice_4.") != std::string::npos) {
            referenceSlice4 = probe;
        }
        if (bridgeReportPaths[i].filename().string().find(".slice_5.") != std::string::npos) {
            referenceSlice5 = probe;
        }
        if (bridgeReportPaths[i].filename().string().find(".slice_6.") != std::string::npos) {
            referenceSlice6 = probe;
        }
        if (bridgeReportPaths[i].filename().string().find(".slice_7.") != std::string::npos) {
            referenceSlice7 = probe;
        }
        if (bridgeReportPaths[i].filename().string().find(".slice_8.") != std::string::npos) {
            referenceSlice8 = probe;
        }
        if (bridgeReportPaths[i].filename().string().find(".slice_9.") != std::string::npos) {
            referenceSlice9 = probe;
        }

        out << "reference.report[" << i << "].readable=" << (probe.readable ? "true" : "false") << "\n";
        out << "reference.report[" << i << "].schema_ready=" << (reportReady ? "true" : "false") << "\n";
        out << "reference.report[" << i << "].pass=" << (probe.pass ? "true" : "false") << "\n";
        out << "reference.report[" << i << "].mismatch_count=" << probe.mismatchCount << "\n";
        if (probe.blockMapHash.has_value()) {
            out << "reference.report[" << i << "].block_map_hash=" << *probe.blockMapHash << "\n";
        }
        out.flush();
    }
    out << "reference.readable=" << readableCount << "/" << bridgeReportPaths.size() << "\n";
    out << "reference.schema_ready=" << schemaReadyCount << "/" << bridgeReportPaths.size() << "\n";
    out << "reference.slice_io_pairs_ready=" << sliceIoReadyCount << "/" << bridgeReportPaths.size() << "\n";
    out << "reference.pass=" << (allReportsPass ? "true" : "false") << "\n";
    out << "reference.mismatch_count=" << totalReferenceMismatches << "\n";
    if (referenceBlockCount.has_value()) {
        out << "reference.block_count=" << *referenceBlockCount << "\n";
    }
    if (referenceSlice2BlockCount.has_value()) {
        out << "reference.slice2.block_count=" << *referenceSlice2BlockCount << "\n";
    }
    if (referenceSlice2BlockMapHash.has_value()) {
        out << "reference.slice2.block_map_hash=" << *referenceSlice2BlockMapHash << "\n";
    }
    if (referenceSlice3.has_value()) {
        out << "reference.slice3.model_block_count=" << referenceSlice3->slice3ModelBlockCount.value_or(-1) << "\n";
        out << "reference.slice3.parsed_model_count=" << referenceSlice3->slice3ParsedModelCount.value_or(-1) << "\n";
        out << "reference.slice3.node_count=" << referenceSlice3->slice3NodeCount.value_or(-1) << "\n";
        out << "reference.slice3.attach_ref_count=" << referenceSlice3->slice3AttachRefCount.value_or(-1) << "\n";
        out << "reference.slice3.graph_error_count=" << referenceSlice3->slice3GraphErrorCount.value_or(-1) << "\n";
        out << "reference.slice3.structural_hash=" << referenceSlice3->slice3StructuralHash.value_or("") << "\n";
    }
    if (referenceSlice4.has_value()) {
        out << "reference.slice4.chunk_attach_count=" << referenceSlice4->slice4ChunkAttachCount.value_or(-1) << "\n";
        out << "reference.slice4.vertex_chunk_count=" << referenceSlice4->slice4VertexChunkCount.value_or(-1) << "\n";
        out << "reference.slice4.vertex_count=" << referenceSlice4->slice4VertexCount.value_or(-1) << "\n";
        out << "reference.slice4.weighted_vertex_chunk_count=" << referenceSlice4->slice4WeightedVertexChunkCount.value_or(-1) << "\n";
        out << "reference.slice4.structural_hash=" << referenceSlice4->slice4StructuralHash.value_or("") << "\n";
    }
    if (referenceSlice5.has_value()) {
        out << "reference.slice5.poly_chunk_count=" << referenceSlice5->slice5PolyChunkCount.value_or(-1) << "\n";
        out << "reference.slice5.null_poly_chunk_count=" << referenceSlice5->slice5NullPolyChunkCount.value_or(-1) << "\n";
        out << "reference.slice5.bits_chunk_count=" << referenceSlice5->slice5BitsChunkCount.value_or(-1) << "\n";
        out << "reference.slice5.texture_chunk_count=" << referenceSlice5->slice5TextureChunkCount.value_or(-1) << "\n";
        out << "reference.slice5.material_chunk_count=" << referenceSlice5->slice5MaterialChunkCount.value_or(-1) << "\n";
        out << "reference.slice5.material_bump_chunk_count=" << referenceSlice5->slice5MaterialBumpChunkCount.value_or(-1) << "\n";
        out << "reference.slice5.strip_chunk_count=" << referenceSlice5->slice5StripChunkCount.value_or(-1) << "\n";
        out << "reference.slice5.poly_corner_count=" << referenceSlice5->slice5PolyCornerCount.value_or(-1) << "\n";
        out << "reference.slice5.structural_hash=" << referenceSlice5->slice5StructuralHash.value_or("") << "\n";
        out << "reference.slice5.type_hash=" << referenceSlice5->slice5TypeHash.value_or("") << "\n";
        out << "reference.slice5.attribute_hash=" << referenceSlice5->slice5AttributeHash.value_or("") << "\n";
        out << "reference.slice5.byte_size_hash=" << referenceSlice5->slice5ByteSizeHash.value_or("") << "\n";
        out << "reference.slice5.strip_meta_hash=" << referenceSlice5->slice5StripMetaHash.value_or("") << "\n";
    }
    if (referenceSlice6.has_value()) {
        out << "reference.slice6.model_file_check_count=" << referenceSlice6->slice6ModelFileCheckCount.value_or(-1) << "\n";
        out << "reference.slice6.parsed_model_file_count=" << referenceSlice6->slice6ParsedModelFileCount.value_or(-1) << "\n";
        out << "reference.slice6.node_count=" << referenceSlice6->slice6NodeCount.value_or(-1) << "\n";
        out << "reference.slice6.attach_ref_count=" << referenceSlice6->slice6AttachRefCount.value_or(-1) << "\n";
        out << "reference.slice6.chunk_attach_count=" << referenceSlice6->slice6ChunkAttachCount.value_or(-1) << "\n";
        out << "reference.slice6.poly_chunk_count=" << referenceSlice6->slice6PolyChunkCount.value_or(-1) << "\n";
        out << "reference.slice6.structural_hash=" << referenceSlice6->slice6StructuralHash.value_or("") << "\n";
    }
    if (referenceSlice7.has_value()) {
        out << "reference.slice7.motion_block_count=" << referenceSlice7->slice7MotionBlockCount.value_or(-1) << "\n";
        out << "reference.slice7.parsed_motion_count=" << referenceSlice7->slice7ParsedMotionCount.value_or(-1) << "\n";
        out << "reference.slice7.node_count=" << referenceSlice7->slice7NodeCount.value_or(-1) << "\n";
        out << "reference.slice7.keyframe_set_count=" << referenceSlice7->slice7KeyframeSetCount.value_or(-1) << "\n";
        out << "reference.slice7.channel_count=" << referenceSlice7->slice7ChannelCount.value_or(-1) << "\n";
        out << "reference.slice7.keyframe_count=" << referenceSlice7->slice7KeyframeCount.value_or(-1) << "\n";
        out << "reference.slice7.structural_hash=" << referenceSlice7->slice7StructuralHash.value_or("") << "\n";
    }
    if (referenceSlice8.has_value()) {
        out << "reference.slice8.animation_file_check_count=" << referenceSlice8->slice8AnimationFileCheckCount.value_or(-1) << "\n";
        out << "reference.slice8.parsed_animation_file_count=" << referenceSlice8->slice8ParsedAnimationFileCount.value_or(-1) << "\n";
        out << "reference.slice8.node_count=" << referenceSlice8->slice8NodeCount.value_or(-1) << "\n";
        out << "reference.slice8.keyframe_set_count=" << referenceSlice8->slice8KeyframeSetCount.value_or(-1) << "\n";
        out << "reference.slice8.channel_count=" << referenceSlice8->slice8ChannelCount.value_or(-1) << "\n";
        out << "reference.slice8.keyframe_count=" << referenceSlice8->slice8KeyframeCount.value_or(-1) << "\n";
        out << "reference.slice8.structural_hash=" << referenceSlice8->slice8StructuralHash.value_or("") << "\n";
    }
    if (referenceSlice9.has_value()) {
        out << "reference.slice9.attach_count=" << referenceSlice9->slice9AttachCount.value_or(-1) << "\n";
        out << "reference.slice9.buffer_mesh_count=" << referenceSlice9->slice9BufferMeshCount.value_or(-1) << "\n";
        out << "reference.slice9.buffer_vertex_count=" << referenceSlice9->slice9BufferVertexCount.value_or(-1) << "\n";
        out << "reference.slice9.buffer_corner_count=" << referenceSlice9->slice9BufferCornerCount.value_or(-1) << "\n";
        out << "reference.slice9.buffer_triangle_corner_count=" << referenceSlice9->slice9BufferTriangleCornerCount.value_or(-1) << "\n";
        out << "reference.slice9.weighted_mesh_count=" << referenceSlice9->slice9WeightedMeshCount.value_or(-1) << "\n";
        out << "reference.slice9.weighted_vertex_count=" << referenceSlice9->slice9WeightedVertexCount.value_or(-1) << "\n";
        out << "reference.slice9.weighted_triangle_set_count=" << referenceSlice9->slice9WeightedTriangleSetCount.value_or(-1) << "\n";
        out << "reference.slice9.weighted_triangle_corner_count=" << referenceSlice9->slice9WeightedTriangleCornerCount.value_or(-1) << "\n";
        out << "reference.slice9.structural_hash=" << referenceSlice9->slice9StructuralHash.value_or("") << "\n";
    }
    out.flush();

    const bool blockCountMatches = !referenceBlockCount.has_value()
        || static_cast<std::size_t>(*referenceBlockCount) == parityBlocks.size();
    out << "comparison.block_count_matches=" << (blockCountMatches ? "true" : "false") << "\n";

    const bool slice2BlockCountMatches = referenceSlice2BlockCount.has_value()
        && static_cast<std::size_t>(*referenceSlice2BlockCount) == slice2Probe.blockCount;
    const bool slice2BlockMapHashMatches = referenceSlice2BlockMapHash.has_value()
        && *referenceSlice2BlockMapHash == slice2Probe.blockMapHash;
    out << "comparison.slice2.block_count_matches=" << (slice2BlockCountMatches ? "true" : "false") << "\n";
    out << "comparison.slice2.block_map_hash_matches=" << (slice2BlockMapHashMatches ? "true" : "false") << "\n";

    const bool slice3Matches = referenceSlice3.has_value()
        && referenceSlice3->slice3ModelBlockCount == static_cast<int>(stagedProbe.slice3ModelBlockCount)
        && referenceSlice3->slice3ParsedModelCount == static_cast<int>(stagedProbe.slice3ParsedModelCount)
        && referenceSlice3->slice3NodeCount == static_cast<int>(stagedProbe.slice3NodeCount)
        && referenceSlice3->slice3AttachRefCount == static_cast<int>(stagedProbe.slice3AttachRefCount)
        && referenceSlice3->slice3GraphErrorCount == static_cast<int>(stagedProbe.slice3GraphErrorCount)
        && referenceSlice3->slice3StructuralHash == stagedProbe.slice3StructuralHash;
    const bool slice4Matches = referenceSlice4.has_value()
        && referenceSlice4->slice4ChunkAttachCount == static_cast<int>(stagedProbe.slice4ChunkAttachCount)
        && referenceSlice4->slice4VertexChunkCount == static_cast<int>(stagedProbe.slice4VertexChunkCount)
        && referenceSlice4->slice4VertexCount == static_cast<int>(stagedProbe.slice4VertexCount)
        && referenceSlice4->slice4WeightedVertexChunkCount == static_cast<int>(stagedProbe.slice4WeightedVertexChunkCount)
        && referenceSlice4->slice4StructuralHash == stagedProbe.slice4StructuralHash;
    const bool slice5Matches = referenceSlice5.has_value()
        && referenceSlice5->slice5PolyChunkCount == static_cast<int>(stagedProbe.slice5PolyChunkCount)
        && referenceSlice5->slice5NullPolyChunkCount == static_cast<int>(stagedProbe.slice5NullPolyChunkCount)
        && referenceSlice5->slice5BitsChunkCount == static_cast<int>(stagedProbe.slice5BitsChunkCount)
        && referenceSlice5->slice5TextureChunkCount == static_cast<int>(stagedProbe.slice5TextureChunkCount)
        && referenceSlice5->slice5MaterialChunkCount == static_cast<int>(stagedProbe.slice5MaterialChunkCount)
        && referenceSlice5->slice5MaterialBumpChunkCount == static_cast<int>(stagedProbe.slice5MaterialBumpChunkCount)
        && referenceSlice5->slice5StripChunkCount == static_cast<int>(stagedProbe.slice5StripChunkCount)
        && referenceSlice5->slice5PolyCornerCount == static_cast<int>(stagedProbe.slice5PolyCornerCount)
        && referenceSlice5->slice5StructuralHash == stagedProbe.slice5StructuralHash;
    const bool slice6Matches = referenceSlice6.has_value()
        && referenceSlice6->slice6ModelFileCheckCount == static_cast<int>(stagedProbe.slice6ModelFileCheckCount)
        && referenceSlice6->slice6ParsedModelFileCount == static_cast<int>(stagedProbe.slice6ParsedModelFileCount)
        && referenceSlice6->slice6NodeCount == static_cast<int>(stagedProbe.slice6NodeCount)
        && referenceSlice6->slice6AttachRefCount == static_cast<int>(stagedProbe.slice6AttachRefCount)
        && referenceSlice6->slice6ChunkAttachCount == static_cast<int>(stagedProbe.slice6ChunkAttachCount)
        && referenceSlice6->slice6PolyChunkCount == static_cast<int>(stagedProbe.slice6PolyChunkCount)
        && referenceSlice6->slice6StructuralHash == stagedProbe.slice6StructuralHash;
    const bool slice7Matches = referenceSlice7.has_value()
        && referenceSlice7->slice7MotionBlockCount == static_cast<int>(stagedProbe.slice7MotionBlockCount)
        && referenceSlice7->slice7ParsedMotionCount == static_cast<int>(stagedProbe.slice7ParsedMotionCount)
        && referenceSlice7->slice7NodeCount == static_cast<int>(stagedProbe.slice7NodeCount)
        && referenceSlice7->slice7KeyframeSetCount == static_cast<int>(stagedProbe.slice7KeyframeSetCount)
        && referenceSlice7->slice7ChannelCount == static_cast<int>(stagedProbe.slice7ChannelCount)
        && referenceSlice7->slice7KeyframeCount == static_cast<int>(stagedProbe.slice7KeyframeCount)
        && referenceSlice7->slice7StructuralHash == stagedProbe.slice7StructuralHash;
    const bool slice8Matches = referenceSlice8.has_value()
        && referenceSlice8->slice8AnimationFileCheckCount == static_cast<int>(stagedProbe.slice8AnimationFileCheckCount)
        && referenceSlice8->slice8ParsedAnimationFileCount == static_cast<int>(stagedProbe.slice8ParsedAnimationFileCount)
        && referenceSlice8->slice8NodeCount == static_cast<int>(stagedProbe.slice8NodeCount)
        && referenceSlice8->slice8KeyframeSetCount == static_cast<int>(stagedProbe.slice8KeyframeSetCount)
        && referenceSlice8->slice8ChannelCount == static_cast<int>(stagedProbe.slice8ChannelCount)
        && referenceSlice8->slice8KeyframeCount == static_cast<int>(stagedProbe.slice8KeyframeCount)
        && referenceSlice8->slice8StructuralHash == stagedProbe.slice8StructuralHash;
    const bool slice9Matches = referenceSlice9.has_value()
        && referenceSlice9->slice9AttachCount == static_cast<int>(stagedProbe.slice9AttachCount)
        && referenceSlice9->slice9BufferMeshCount == static_cast<int>(stagedProbe.slice9BufferMeshCount)
        && referenceSlice9->slice9BufferVertexCount == static_cast<int>(stagedProbe.slice9BufferVertexCount)
        && referenceSlice9->slice9BufferCornerCount == static_cast<int>(stagedProbe.slice9BufferCornerCount)
        && referenceSlice9->slice9BufferTriangleCornerCount == static_cast<int>(stagedProbe.slice9BufferTriangleCornerCount)
        && referenceSlice9->slice9WeightedMeshCount == static_cast<int>(stagedProbe.slice9WeightedMeshCount)
        && referenceSlice9->slice9WeightedVertexCount == static_cast<int>(stagedProbe.slice9WeightedVertexCount)
        && referenceSlice9->slice9WeightedTriangleSetCount == static_cast<int>(stagedProbe.slice9WeightedTriangleSetCount)
        && referenceSlice9->slice9WeightedTriangleCornerCount == static_cast<int>(stagedProbe.slice9WeightedTriangleCornerCount)
        && referenceSlice9->slice9StructuralHash == stagedProbe.slice9StructuralHash;
    out << "comparison.slice3.covered=" << (stagedProbe.slice3ModelBlockCount > 0 ? "true" : "false") << "\n";
    out << "comparison.slice4.covered=" << (stagedProbe.slice4ChunkAttachCount > 0 ? "true" : "false") << "\n";
    out << "comparison.slice5.covered=" << (stagedProbe.slice5PolyChunkCount > 0 ? "true" : "false") << "\n";
    out << "comparison.slice6.covered=" << (stagedProbe.slice6ModelFileCheckCount > 0 ? "true" : "false") << "\n";
    out << "comparison.slice7.covered=" << (stagedProbe.slice7MotionBlockCount > 0 ? "true" : "false") << "\n";
    out << "comparison.slice8.covered=" << (stagedProbe.slice8AnimationFileCheckCount > 0 ? "true" : "false") << "\n";
    out << "comparison.slice9.covered=" << (stagedProbe.slice9AttachCount > 0 ? "true" : "false") << "\n";
    out << "comparison.slice3.matches=" << (slice3Matches ? "true" : "false") << "\n";
    out << "comparison.slice4.matches=" << (slice4Matches ? "true" : "false") << "\n";
    out << "comparison.slice5.matches=" << (slice5Matches ? "true" : "false") << "\n";
    out << "comparison.slice6.matches=" << (slice6Matches ? "true" : "false") << "\n";
    out << "comparison.slice7.matches=" << (slice7Matches ? "true" : "false") << "\n";
    out << "comparison.slice8.matches=" << (slice8Matches ? "true" : "false") << "\n";
    out << "comparison.slice9.matches=" << (slice9Matches ? "true" : "false") << "\n";

    if (!allReportsSchemaReady) {
        out << "comparison.status=reference_schema_incomplete\n";
    } else if (!slice2BlockCountMatches || !slice2BlockMapHashMatches) {
        out << "comparison.status=slice2_block_map_mismatch\n";
    } else if (!slice3Matches) {
        out << "comparison.status=slice3_model_graph_mismatch\n";
    } else if (!slice4Matches) {
        out << "comparison.status=slice4_vertex_attach_mismatch\n";
    } else if (!slice5Matches) {
        out << "comparison.status=slice5_poly_chunk_mismatch\n";
    } else if (!slice6Matches) {
        out << "comparison.status=slice6_model_file_mismatch\n";
    } else if (!slice7Matches) {
        out << "comparison.status=slice7_motion_mismatch\n";
    } else if (!slice8Matches) {
        out << "comparison.status=slice8_animation_file_mismatch\n";
    } else if (!slice9Matches) {
        out << "comparison.status=slice9_normalization_mismatch\n";
    } else if (!blockCountMatches) {
        out << "comparison.status=block_count_mismatch\n";
    } else if (!allReportsPass || totalReferenceMismatches != 0) {
        out << "comparison.status=reference_report_failed\n";
    } else {
        out << "comparison.status=pass\n";
    }
}

void writeSctReport(const std::filesystem::path& outPath, const soasim::sct::SctParseResult& result) {
    std::ofstream out(outPath, std::ios::binary);
    out << "source=" << result.file.sourcePath << "\n";
    out << "parseOk=" << (result.parseOk ? "true" : "false") << "\n";
    out << "sections=" << result.file.sections.size() << "\n\n";

    for (const auto& section : result.file.sections) {
        out << "[section] index=" << section.id.index << " name=" << section.id.name << "\n";
        out << "  startOffset=" << section.startOffset << " endOffset=" << section.endOffset << "\n";
        out << "  instructions=" << section.instructions.size()
            << " blocks=" << section.blocks.size()
            << " unknownRegions=" << section.unknownRegions.size() << "\n";
        out << "  heuristics: trigger=" << section.heuristicEvidence.likelyTrigger
            << " cutscene=" << section.heuristicEvidence.likelyCutscene
            << " switch=" << section.heuristicEvidence.hasSwitch
            << " flagsTouched=" << section.heuristicEvidence.touchesFlags << "\n";
    }

    if (!result.diagnostics.empty()) {
        out << "\n[diagnostics]\n";
        for (const auto& diagnostic : result.diagnostics) {
            out << "- @" << diagnostic.offset << " " << diagnostic.message << "\n";
        }
    }
}



} // namespace

int main(int argc, char** argv) {
    std::cout << "[SoaSimFileParsing] Step 1/4: Initializing parser run...\n";
    const std::filesystem::path source_file = __FILE__;
    const std::filesystem::path source_dir = source_file.parent_path();

    const std::filesystem::path processPath = std::filesystem::absolute(std::filesystem::path(argv[0]));
    const std::filesystem::path processDir = processPath.parent_path();

    const auto cliOptions = parseCliOptions(argc, argv, source_dir);
    if (!cliOptions.has_value()) {
        return 1;
    }
    const std::filesystem::path inputDir = cliOptions->inputDir;
    const std::filesystem::path outputDir = cliOptions->outputDir;
    const std::filesystem::path decompressedDir = source_dir / "decompressed_inputs";

    std::filesystem::create_directories(outputDir);
    std::filesystem::create_directories(decompressedDir);
    writeFixtureManifestFromInputDir(inputDir, outputDir);
    std::cout << "[SoaSimFileParsing] Step 2/4: Prepared directories.\n";

    if (!std::filesystem::exists(inputDir) || !std::filesystem::is_directory(inputDir)) {
        std::cerr << "Input directory not found: " << inputDir << "\n";
        return 1;
    }

    soasim::sct::SctParser sctParser{};
    soasim::mld::parsing::MldParser mldParser{};
    soasim::mld::exporting::BlenderIrJsonExporter exporter{};
    std::cout << "[SoaSimFileParsing] Step 3/4: Parsing input files...\n";

    std::size_t filesProcessed = 0;
    constexpr int kAbStartSlice = 1;
    constexpr int kAbEndSlice = 9;

    for (const auto& entry : std::filesystem::directory_iterator(inputDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const auto extension = toLowerCopy(entry.path().extension().string());
        const auto bytes = readAllBytes(entry.path());
        if (bytes.empty()) {
            continue;
        }

        if (cliOptions->runAbSa3dPortVsSa3dBridge && extension != ".mld") {
            continue;
        }

        const bool isSupportedExtension = extension == ".sct" || extension == ".mld";
        if (isSupportedExtension && soasim::compression::aklz::isAklz(bytes)) {
            auto decodedResult = soasim::compression::aklz::decompress(bytes);
            if (!decodedResult.ok()) {
                std::cerr << "AKLZ decompression failed for " << entry.path().string()
                          << ": " << soasim::compression::aklz::errorToString(decodedResult.error) << "\n";
            } else {
                const auto decompressedPath = decompressedDir / entry.path().filename();
                if (!writeAllBytes(decompressedPath, std::span<const std::uint8_t>(decodedResult.bytes.data(), decodedResult.bytes.size()))) {
                    std::cerr << "Failed to write decompressed file: " << decompressedPath.string() << "\n";
                }
            }
        }

        if (extension == ".sct") {
            std::cout << "[SoaSimFileParsing]   - Parsing SCT: " << entry.path().filename().string() << "\n";
            auto parsed = sctParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()), entry.path().string());
            const auto outPath = outputDir / (entry.path().stem().string() + ".sct.txt");
            std::string summary = soasim::sct::formatParseSummary(parsed);
            std::ofstream out(outPath, std::ios::binary);
            out << summary.c_str();
            ++filesProcessed;
            continue;
        }

        if (extension == ".mld") {
            std::cout << "[SoaSimFileParsing]   - Parsing MLD: " << entry.path().filename().string() << "\n";
            if (cliOptions->runAbSa3dPortVsSa3dBridge) {
                soasim::mld::parsing::ParseOptions sa3dPortOptions{};
                auto sa3dPortParsed = mldParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()), sa3dPortOptions);

                const auto sa3dPortOutPath = outputDir / (entry.path().stem().string() + ".mld.sa3d_port.txt");
                std::ofstream sa3dPortOut(sa3dPortOutPath, std::ios::binary);
                sa3dPortOut << soasim::mld::parsing::formatParseSummary(sa3dPortParsed);

                const auto jsonOutPath = outputDir / (entry.path().stem().string() + ".sa3d_port.json");
                std::ofstream jsonOut(jsonOutPath, std::ios::binary);
                if (sa3dPortParsed.blenderIrScene.has_value()) {
                    jsonOut << exporter.toJson(*sa3dPortParsed.blenderIrScene).c_str();
                }

                std::vector<std::filesystem::path> bridgeReportPaths{};
                std::vector<std::filesystem::path> blockInputPaths{};
                std::vector<soasim::mld::parsing::ExtractedNjBlock> validBlocks{};
                for (const auto& block : sa3dPortParsed.extractedNjBlocks) {
                    const auto normalizedBlock = normalizeBlockForSa3dBridge(block);
                    if (!normalizedBlock.has_value()) {
                        continue;
                    }

                    const auto kindLabel = toBlockKindLabel(normalizedBlock->kind);
                    const auto pairLabel = normalizedBlock->includesNjtlPrefix ? "_njtl_njcm" : "";
                    const auto blockStem = entry.path().stem().string() + ".block_" + std::to_string(normalizedBlock->offset) + "_" + kindLabel + pairLabel;
                    const auto blockInputPath = outputDir / (blockStem + ".njblk.bin");
                    if (!writeAllBytes(blockInputPath, std::span<const std::uint8_t>(normalizedBlock->bytes.data(), normalizedBlock->bytes.size()))) {
                        std::cerr << "[SoaSimFileParsing] WARNING: failed to write extracted NJ block input: "
                                  << blockInputPath.string() << "\n";
                        continue;
                    }
                    blockInputPaths.push_back(blockInputPath);
                    validBlocks.push_back(*normalizedBlock);
                }

                const auto blockManifestPath = outputDir / (entry.path().stem().string() + ".block_manifest.json");
                writeFixtureBlockManifest(blockManifestPath, entry.path().stem().string(), blockInputPaths, validBlocks);

                for (int slice = kAbStartSlice; slice <= kAbEndSlice; ++slice) {
                    const auto bridgeReportPath = maybeInvokeDotnetBridge(
                        processDir,
                        entry.path(),
                        outputDir,
                        outputDir / "FIXTURE_MANIFEST.generated.json",
                        blockManifestPath,
                        slice);
                    if (bridgeReportPath.has_value()) {
                        bridgeReportPaths.push_back(*bridgeReportPath);
                    }
                }

                const auto compareOutPath = outputDir / (entry.path().stem().string() + ".mld.ab.compare.txt");
                writeBridgeAbComparison(compareOutPath, sa3dPortParsed, validBlocks, bridgeReportPaths);
            } else {
                soasim::mld::parsing::ParseOptions parityOptions{};
                auto parityParsed = mldParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()), parityOptions);

                const auto parityOutPath = outputDir / (entry.path().stem().string() + ".mld.parity.txt");
                std::ofstream parityOut(parityOutPath, std::ios::binary);
                parityOut << soasim::mld::parsing::formatParseSummary(parityParsed);

                const auto jsonOutPath = outputDir / (entry.path().stem().string() + ".json");
                std::ofstream jsonOut(jsonOutPath, std::ios::binary);
                if (parityParsed.blenderIrScene.has_value()) {
                    jsonOut << exporter.toJson(*parityParsed.blenderIrScene).c_str();
                }
            }
            ++filesProcessed;
            continue;
        }
    }

    std::cout << "[SoaSimFileParsing] Step 4/4: Finalizing summary.\n";
    std::cout << "SoaSimFileParsing finished.\nFilesProcessed=" << filesProcessed
              << "\ninputDir=" << inputDir.string()
              << "\noutputDir=" << outputDir.string() << "\n";

    return 0;
}
