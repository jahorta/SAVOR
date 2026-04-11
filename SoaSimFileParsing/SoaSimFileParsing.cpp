#include "../SoaSimMLD/SoaSimMLD.h"
#include "../SoaSimSCT/SoaSimSCT.h"
#include "../Compression/Aklz.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
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
    const std::filesystem::path source_file = __FILE__;
    const std::filesystem::path source_dir = source_file.parent_path();

    const std::filesystem::path inputDir = argc > 1 ? argv[1] : std::filesystem::path(source_dir / "inputs");
    const std::filesystem::path outputDir = argc > 2 ? argv[2] : std::filesystem::path(source_dir / "parsed");
    const std::filesystem::path decompressedDir = source_dir / "decompressed_inputs";

    std::filesystem::create_directories(outputDir);
    std::filesystem::create_directories(decompressedDir);

    if (!std::filesystem::exists(inputDir) || !std::filesystem::is_directory(inputDir)) {
        std::cerr << "Input directory not found: " << inputDir << "\n";
        return 1;
    }

    soasim::sct::SctParser sctParser{};
    soasim::mld::parsing::MldParser mldParser{};

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
            auto parsed = sctParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()), entry.path().string());
            const auto outPath = outputDir / (entry.path().stem().string() + ".sct.txt");
            std::string summary = soasim::sct::formatParseSummary(parsed);
            std::ofstream out(outPath, std::ios::binary);
            out << summary.c_str();
            ++filesProcessed;
            continue;
        }

        if (extension == ".mld") {
            auto parsed = mldParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
            const auto outPath = outputDir / (entry.path().stem().string() + ".mld.txt");
            //std::string summary = soasim::mld::parsing(outPath, parsed);
            //std::ofstream out(outPath, std::ios::binary);
            //out << summary.c_str();
            ++filesProcessed;
            continue;
        }
    }

    std::cout << "SoaSimFileParsing finished.\nFilesProcessed=" << filesProcessed
              << "\ninputDir=" << inputDir.string()
              << "\noutputDir=" << outputDir.string() << "\n";

    return 0;
}
