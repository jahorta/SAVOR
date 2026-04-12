#include "../SoaSimMLD/SoaSimMLD.h"
#include "../SoaSimSCT/SoaSimSCT.h"
#include "../Compression/Aklz.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
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

template <typename K, typename V>
std::string formatHistogram(const std::map<K, V>& histogram) {
    if (histogram.empty()) {
        return "{}";
    }
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto& [key, value] : histogram) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << key << ":" << value;
    }
    out << "}";
    return out.str();
}

struct DecodedChunkFingerprint {
    std::size_t chunkOffset = 0;
    std::size_t chunkDataSize = 0;
    bool chunkSizeLittleEndian = false;
    bool payloadLittleEndian = false;
    std::uint32_t imageBase = 0;
    bool sawPof0Chunk = false;
    bool usedPof0Fixup = false;
    std::size_t objectCount = 0;
    std::size_t attachCount = 0;
    std::size_t semanticVertexCount = 0;
    std::size_t semanticTriangleCount = 0;
    std::size_t vertexChunkCount = 0;
    std::size_t polyChunkCount = 0;
    std::size_t semanticIndexCount = 0;
    std::size_t outOfRangeIndexCount = 0;
    std::map<int, std::size_t> vertexTypeHistogram{};
    std::map<int, std::size_t> polyTypeHistogram{};
};

DecodedChunkFingerprint fingerprintChunk(const soasim::mld::model::NjcmDecodedChunk& chunk) {
    DecodedChunkFingerprint fp{};
    fp.chunkOffset = chunk.chunkOffset;
    fp.chunkDataSize = chunk.chunkDataSize;
    fp.chunkSizeLittleEndian = chunk.chunkSizeLittleEndian;
    fp.payloadLittleEndian = chunk.payloadLittleEndian;
    fp.imageBase = chunk.imageBase;
    fp.sawPof0Chunk = chunk.sawPof0Chunk;
    fp.usedPof0Fixup = chunk.usedPof0Fixup;
    fp.objectCount = chunk.objects.size();
    fp.attachCount = chunk.attaches.size();

    for (const auto& attach : chunk.attaches) {
        fp.semanticVertexCount += attach.semanticVertices.size();
        fp.semanticTriangleCount += attach.decodedTriangleCount;
        fp.vertexChunkCount += attach.vertexChunks.size();
        fp.polyChunkCount += attach.polyChunks.size();

        for (const auto& vchunk : attach.vertexChunks) {
            ++fp.vertexTypeHistogram[static_cast<int>(vchunk.type)];
        }
        for (const auto& pchunk : attach.polyChunks) {
            ++fp.polyTypeHistogram[static_cast<int>(pchunk.type)];
        }

        for (const auto& sp : attach.semanticPolygons) {
            fp.semanticIndexCount += sp.indices.size();
            for (const auto idx : sp.indices) {
                if (idx >= attach.semanticVertices.size()) {
                    ++fp.outOfRangeIndexCount;
                }
            }
        }
    }

    return fp;
}

std::string formatNjcmParityComparison(const soasim::mld::parsing::ParseResult& legacyResult,
    const soasim::mld::parsing::ParseResult& parityResult) {
    std::ostringstream out;
    out << "legacyDecodedChunks=" << legacyResult.decodedNjcmChunks.size() << '\n';
    out << "parityDecodedChunks=" << parityResult.decodedNjcmChunks.size() << '\n';

    std::unordered_map<std::size_t, DecodedChunkFingerprint> legacyByOffset{};
    std::unordered_map<std::size_t, DecodedChunkFingerprint> parityByOffset{};
    legacyByOffset.reserve(legacyResult.decodedNjcmChunks.size());
    parityByOffset.reserve(parityResult.decodedNjcmChunks.size());

    for (const auto& chunk : legacyResult.decodedNjcmChunks) {
        legacyByOffset[chunk.chunkOffset] = fingerprintChunk(chunk);
    }
    for (const auto& chunk : parityResult.decodedNjcmChunks) {
        parityByOffset[chunk.chunkOffset] = fingerprintChunk(chunk);
    }

    std::map<std::size_t, int> offsets{};
    for (const auto& [off, _] : legacyByOffset) {
        offsets[off] = 1;
    }
    for (const auto& [off, _] : parityByOffset) {
        offsets[off] = 1;
    }

    std::size_t changed = 0;
    std::size_t onlyLegacy = 0;
    std::size_t onlyParity = 0;
    out << "chunkComparisons:" << '\n';
    for (const auto& [off, _] : offsets) {
        const auto legacyIt = legacyByOffset.find(off);
        const auto parityIt = parityByOffset.find(off);
        if (legacyIt == legacyByOffset.end()) {
            ++onlyParity;
            out << "  - offset=" << off << " only=parity" << '\n';
            continue;
        }
        if (parityIt == parityByOffset.end()) {
            ++onlyLegacy;
            out << "  - offset=" << off << " only=legacy" << '\n';
            continue;
        }

        const auto& a = legacyIt->second;
        const auto& b = parityIt->second;
        const bool same = a.imageBase == b.imageBase &&
            a.usedPof0Fixup == b.usedPof0Fixup &&
            a.objectCount == b.objectCount &&
            a.attachCount == b.attachCount &&
            a.semanticVertexCount == b.semanticVertexCount &&
            a.semanticTriangleCount == b.semanticTriangleCount &&
            a.vertexChunkCount == b.vertexChunkCount &&
            a.polyChunkCount == b.polyChunkCount &&
            a.semanticIndexCount == b.semanticIndexCount &&
            a.outOfRangeIndexCount == b.outOfRangeIndexCount &&
            a.vertexTypeHistogram == b.vertexTypeHistogram &&
            a.polyTypeHistogram == b.polyTypeHistogram;

        if (!same) {
            ++changed;
        }

        out << "  - offset=" << off
            << " changed=" << (same ? "no" : "yes")
            << " legacy{imgBase=" << a.imageBase
            << ", pof0Fixup=" << (a.usedPof0Fixup ? "yes" : "no")
            << ", objs=" << a.objectCount
            << ", attaches=" << a.attachCount
            << ", verts=" << a.semanticVertexCount
            << ", tris=" << a.semanticTriangleCount
            << ", vchunks=" << a.vertexChunkCount
            << ", pchunks=" << a.polyChunkCount
            << ", indices=" << a.semanticIndexCount
            << ", oob=" << a.outOfRangeIndexCount
            << ", vtypes=" << formatHistogram(a.vertexTypeHistogram)
            << ", ptypes=" << formatHistogram(a.polyTypeHistogram)
            << "}"
            << " parity{imgBase=" << b.imageBase
            << ", pof0Fixup=" << (b.usedPof0Fixup ? "yes" : "no")
            << ", objs=" << b.objectCount
            << ", attaches=" << b.attachCount
            << ", verts=" << b.semanticVertexCount
            << ", tris=" << b.semanticTriangleCount
            << ", vchunks=" << b.vertexChunkCount
            << ", pchunks=" << b.polyChunkCount
            << ", indices=" << b.semanticIndexCount
            << ", oob=" << b.outOfRangeIndexCount
            << ", vtypes=" << formatHistogram(b.vertexTypeHistogram)
            << ", ptypes=" << formatHistogram(b.polyTypeHistogram)
            << "}"
            << '\n';
    }

    out << "summary: changed=" << changed
        << " onlyLegacy=" << onlyLegacy
        << " onlyParity=" << onlyParity
        << '\n';
    return out.str();
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
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

    const std::filesystem::path inputDir = argc > 1 ? argv[1] : std::filesystem::path(source_dir / "inputs");
    const std::filesystem::path outputDir = argc > 2 ? argv[2] : std::filesystem::path(source_dir / "parsed");
    const std::filesystem::path decompressedDir = source_dir / "decompressed_inputs";

    std::filesystem::create_directories(outputDir);
    std::filesystem::create_directories(decompressedDir);
    std::cout << "[SoaSimFileParsing] Step 2/4: Prepared directories.\n";

    if (!std::filesystem::exists(inputDir) || !std::filesystem::is_directory(inputDir)) {
        std::cerr << "Input directory not found: " << inputDir << "\n";
        return 1;
    }

    soasim::sct::SctParser sctParser{};
    soasim::mld::parsing::MldParser mldParser{};
    std::cout << "[SoaSimFileParsing] Step 3/4: Parsing input files...\n";

    std::size_t filesProcessed = 0;

    for (const auto& entry : std::filesystem::directory_iterator(inputDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const auto extension = toLowerCopy(entry.path().extension().string());
        const auto bytes = readAllBytes(entry.path());
        if (bytes.empty()) {
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
            auto parsed = mldParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
            const auto outPath = outputDir / (entry.path().stem().string() + ".mld.txt");
            std::string summary = soasim::mld::parsing::formatParseSummary(parsed);
            std::ofstream out(outPath, std::ios::binary);
            out << summary.c_str();

            soasim::mld::parsing::ParseOptions parityOptions{};
            parityOptions.njcmPolicy.useSaToolsParityPath = true;
            auto parityParsed = mldParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()), parityOptions);

            const auto parityOutPath = outputDir / (entry.path().stem().string() + ".mld.parity.txt");
            std::ofstream parityOut(parityOutPath, std::ios::binary);
            parityOut << soasim::mld::parsing::formatParseSummary(parityParsed);

            const auto comparisonOutPath = outputDir / (entry.path().stem().string() + ".mld.njcm-compare.txt");
            std::ofstream compareOut(comparisonOutPath, std::ios::binary);
            compareOut << formatNjcmParityComparison(parsed, parityParsed);
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
