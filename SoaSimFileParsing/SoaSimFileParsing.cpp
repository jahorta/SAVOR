#include "../SoaSimMLD/SoaSimMLD.h"
#include "../SoaSimSCT/SoaSimSCT.h"

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

void writeMldReport(const std::filesystem::path& outPath, const soasim::mld::parsing::ParseResult& result) {
    std::ofstream out(outPath, std::ios::binary);
    out << "world.grndSurfaces=" << result.world.grndSurfaces.size() << "\n";
    out << "world.collisions=" << result.world.collisions.size() << "\n";
    out << "world.triggers=" << result.world.triggers.size() << "\n";
    out << "world.unknownEntries=" << result.world.unknownEntries.size() << "\n";
    out << "search.surfaces=" << result.searchWorld.surfaces.size() << "\n";
    out << "search.regions=" << result.searchWorld.regions.size() << "\n";

    if (!result.fxnHistogram.empty()) {
        out << "\n[fxnHistogram]\n";
        for (const auto& [fxn, count] : result.fxnHistogram) {
            out << "- fxn=" << fxn << " count=" << count << "\n";
        }
    }

    if (!result.diagnostics.empty()) {
        out << "\n[diagnostics]\n";
        for (const auto& diagnostic : result.diagnostics) {
            const char* severity = "Info";
            if (diagnostic.severity == soasim::mld::parsing::ParseDiagnostic::Severity::Warning) {
                severity = "Warning";
            } else if (diagnostic.severity == soasim::mld::parsing::ParseDiagnostic::Severity::Error) {
                severity = "Error";
            }
            out << "- [" << severity << "] " << diagnostic.message << "\n";
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path inputDir = argc > 1 ? argv[1] : std::filesystem::path("SoaSimFileParsing/inputs");
    const std::filesystem::path outputDir = argc > 2 ? argv[2] : std::filesystem::path("SoaSimFileParsing/parsed");

    std::filesystem::create_directories(outputDir);

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

        if (extension == ".sct") {
            auto parsed = sctParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()), entry.path().string());
            const auto outPath = outputDir / (entry.path().stem().string() + ".sct.txt");
            writeSctReport(outPath, parsed);
            ++filesProcessed;
            continue;
        }

        if (extension == ".mld") {
            auto parsed = mldParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
            const auto outPath = outputDir / (entry.path().stem().string() + ".mld.txt");
            writeMldReport(outPath, parsed);
            ++filesProcessed;
            continue;
        }
    }

    std::cout << "SoaSimFileParsing finished. filesProcessed=" << filesProcessed
              << " inputDir=" << inputDir.string()
              << " outputDir=" << outputDir.string() << "\n";

    return 0;
}
