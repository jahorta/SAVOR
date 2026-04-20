#include "../SoaSimMLD/SoaSimMLD.h"
#include "../SoaSimSCT/SoaSimSCT.h"
#include "../Compression/Aklz.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
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
    std::optional<std::filesystem::path> dotnetBridgeExe{};
    std::optional<std::string> dotnetBridgeCommand{};
    int dotnetBridgeSlice = 0;
};

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  SoaSimFileParsing [input_dir] [output_dir] [--ab-sa3d-port-vs-sa3d-bridge] [--dotnet-bridge-exe <path>] [--dotnet-bridge-cmd <prefix>] [--dotnet-bridge-slice <n>]\n\n"
        << "Notes:\n"
        << "  - input_dir defaults to SoaSimFileParsing/inputs\n"
        << "  - output_dir defaults to SoaSimFileParsing/parsed\n"
        << "  - --ab-sa3d-port-vs-sa3d-bridge enables A/B mode for .mld files.\n"
        << "  - --dotnet-bridge-exe should point to the .NET bridge runner executable used for SA3D reference output.\n"
        << "    If omitted, SoaSimFileParsing tries: <SoaSimFileParsing.exe_dir>/sa3d_bridge/SA3DRefRunner.exe\n"
        << "  - --dotnet-bridge-cmd supplies a full command prefix (e.g. 'dotnet run --project ... --') used to invoke run-one.\n"
        << "  - --dotnet-bridge-slice sets the --slice value passed to the .NET bridge runner (default 0).\n";
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
        if (arg == "--dotnet-bridge-exe") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --dotnet-bridge-exe.\n";
                return std::nullopt;
            }
            ++i;
            options.dotnetBridgeExe = std::filesystem::path(argv[i]);
            continue;
        }
        if (arg == "--dotnet-bridge-cmd") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --dotnet-bridge-cmd.\n";
                return std::nullopt;
            }
            ++i;
            options.dotnetBridgeCommand = std::string(argv[i]);
            continue;
        }
        if (arg == "--dotnet-bridge-slice") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --dotnet-bridge-slice.\n";
                return std::nullopt;
            }
            ++i;
            try {
                options.dotnetBridgeSlice = std::stoi(argv[i]);
                if (options.dotnetBridgeSlice < 0) {
                    std::cerr << "--dotnet-bridge-slice must be >= 0.\n";
                    return std::nullopt;
                }
            } catch (...) {
                std::cerr << "Invalid integer value for --dotnet-bridge-slice: " << argv[i] << "\n";
                return std::nullopt;
            }
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
    const std::optional<std::filesystem::path>& bridgeExe,
    const std::optional<std::string>& bridgeCommand,
    const std::filesystem::path& processDir,
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& fixtureManifestPath,
    int slice) {
    std::string commandPrefix{};
    if (bridgeCommand.has_value() && !bridgeCommand->empty()) {
        commandPrefix = *bridgeCommand;
    } else {
        const auto bridgePath = bridgeExe.has_value()
            ? *bridgeExe
            : (processDir / "sa3d_bridge" / "SA3DRefRunner.exe");

        if (!std::filesystem::exists(bridgePath)) {
            std::cerr << "[SoaSimFileParsing] WARNING: .NET bridge executable does not exist: "
                      << bridgePath.string() << "\n";
            return std::nullopt;
        }
        commandPrefix = quotePath(bridgePath);
    }

    const auto bridgeOutPath = outputDir / (inputPath.stem().string() + ".sa3d.reference.json");
    const auto command = commandPrefix +
        " run-one" +
        " --input " + quotePath(inputPath) +
        " --out " + quotePath(outputDir) +
        " --output-file " + quotePath(bridgeOutPath) +
        " --manifest " + quotePath(fixtureManifestPath) +
        " --slice " + std::to_string(slice);
    const int exitCode = std::system(command.c_str());
    if (exitCode != 0) {
        std::cerr << "[SoaSimFileParsing] WARNING: .NET bridge run failed with exit code "
                  << exitCode << " for input " << inputPath.string() << "\n";
        return std::nullopt;
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

void writeBridgeAbComparison(
    const std::filesystem::path& outPath,
    const soasim::mld::parsing::ParseResult& sa3dPortParsed,
    const std::vector<std::filesystem::path>& bridgeReportPaths) {
    std::ofstream out(outPath, std::ios::binary);
    out << "mode=sa3d_port_vs_dotnet_sa3d\n";
    out << "sa3d_port.decoded_chunks=" << sa3dPortParsed.decodedNjcmChunks.size() << "\n";
    out << "sa3d_port.object_blocks=" << sa3dPortParsed.decodedNjObjectBlocks.size() << "\n";
    out << "sa3d_port.diagnostics=" << sa3dPortParsed.diagnostics.size() << "\n";
    out << "sa3d_port.extracted_nj_blocks=" << sa3dPortParsed.extractedNjBlocks.size() << "\n";

    if (bridgeReportPaths.empty()) {
        out << "reference.present=false\n";
        out << "comparison.status=missing_reference_output\n";
        return;
    }

    out << "reference.present=true\n";
    out << "reference.reports=" << bridgeReportPaths.size() << "\n";
    bool allReportsSchemaReady = true;
    std::size_t schemaReadyCount = 0;
    for (std::size_t i = 0; i < bridgeReportPaths.size(); ++i) {
        out << "reference.path[" << i << "]=" << bridgeReportPaths[i].string() << "\n";
    }

    const std::string bridgeJson = readTextFile(bridgeReportPaths.front());
    if (bridgeJson.empty()) {
        out << "comparison.status=reference_output_empty\n";
        return;
    }

    const bool hasSchema = containsJsonProperty(bridgeJson, "schema");
    const bool hasFixture = containsJsonProperty(bridgeJson, "fixture");
    const bool hasSliceIoPairs = containsJsonProperty(bridgeJson, "slice_io_pairs");
    const bool hasOutputs = containsJsonProperty(bridgeJson, "outputs");
    const bool hasComparison = containsJsonProperty(bridgeJson, "comparison");
    const bool hasPassTrue = bridgeJson.find("\"pass\": true") != std::string::npos;
    out << "reference.has_schema=" << (hasSchema ? "true" : "false") << "\n";
    out << "reference.has_fixture=" << (hasFixture ? "true" : "false") << "\n";
    out << "reference.has_slice_io_pairs=" << (hasSliceIoPairs ? "true" : "false") << "\n";
    out << "reference.has_outputs=" << (hasOutputs ? "true" : "false") << "\n";
    out << "reference.has_comparison=" << (hasComparison ? "true" : "false") << "\n";
    out << "reference.pass_true=" << (hasPassTrue ? "true" : "false") << "\n";
    for (const auto& reportPath : bridgeReportPaths) {
        const std::string reportJson = readTextFile(reportPath);
        const bool reportReady = !reportJson.empty()
            && containsJsonProperty(reportJson, "schema")
            && containsJsonProperty(reportJson, "fixture")
            && containsJsonProperty(reportJson, "slice_io_pairs")
            && containsJsonProperty(reportJson, "outputs")
            && containsJsonProperty(reportJson, "comparison");
        if (reportReady) {
            ++schemaReadyCount;
        } else {
            allReportsSchemaReady = false;
        }
    }
    out << "reference.schema_ready=" << schemaReadyCount << "/" << bridgeReportPaths.size() << "\n";
    out << "comparison.status=" << (allReportsSchemaReady ? "framework_ready" : "reference_schema_incomplete") << "\n";
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
    soasim::mld::parsing::BlenderIrBuilder builder{};
    soasim::mld::exporting::BlenderIrJsonExporter exporter{};
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
            if (cliOptions->runAbSa3dPortVsSa3dBridge) {
                soasim::mld::parsing::ParseOptions sa3dPortOptions{};
                auto sa3dPortParsed = mldParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()), sa3dPortOptions);

                const auto sa3dPortOutPath = outputDir / (entry.path().stem().string() + ".mld.sa3d_port.txt");
                std::ofstream sa3dPortOut(sa3dPortOutPath, std::ios::binary);
                sa3dPortOut << soasim::mld::parsing::formatParseSummary(sa3dPortParsed);

                const auto jsonOutPath = outputDir / (entry.path().stem().string() + ".sa3d_port.json");
                std::ofstream jsonOut(jsonOutPath, std::ios::binary);
                jsonOut << exporter.toJson(builder.build(sa3dPortParsed)).c_str();

                std::vector<std::filesystem::path> bridgeReportPaths{};
                for (const auto& block : sa3dPortParsed.extractedNjBlocks) {
                    if (block.bytes.empty()) {
                        continue;
                    }

                    const auto kindLabel = toBlockKindLabel(block.kind);
                    const auto pairLabel = block.includesNjtlPrefix ? "_njtl_njcm" : "";
                    const auto blockStem = entry.path().stem().string() + ".block_" + std::to_string(block.offset) + "_" + kindLabel + pairLabel;
                    const auto blockInputPath = outputDir / (blockStem + ".njblk.bin");
                    if (!writeAllBytes(blockInputPath, std::span<const std::uint8_t>(block.bytes.data(), block.bytes.size()))) {
                        std::cerr << "[SoaSimFileParsing] WARNING: failed to write extracted NJ block input: "
                                  << blockInputPath.string() << "\n";
                        continue;
                    }

                    const auto bridgeReportPath = maybeInvokeDotnetBridge(
                        cliOptions->dotnetBridgeExe,
                        cliOptions->dotnetBridgeCommand,
                        processDir,
                        blockInputPath,
                        outputDir,
                        outputDir / "FIXTURE_MANIFEST.generated.json",
                        cliOptions->dotnetBridgeSlice);
                    if (bridgeReportPath.has_value()) {
                        bridgeReportPaths.push_back(*bridgeReportPath);
                    }
                }

                const auto compareOutPath = outputDir / (entry.path().stem().string() + ".mld.ab.compare.txt");
                writeBridgeAbComparison(compareOutPath, sa3dPortParsed, bridgeReportPaths);
            } else {
                soasim::mld::parsing::ParseOptions parityOptions{};
                auto parityParsed = mldParser.parse(std::span<const std::uint8_t>(bytes.data(), bytes.size()), parityOptions);

                const auto parityOutPath = outputDir / (entry.path().stem().string() + ".mld.parity.txt");
                std::ofstream parityOut(parityOutPath, std::ios::binary);
                parityOut << soasim::mld::parsing::formatParseSummary(parityParsed);

                const auto jsonOutPath = outputDir / (entry.path().stem().string() + ".json");
                std::ofstream jsonOut(jsonOutPath, std::ios::binary);
                jsonOut << exporter.toJson(builder.build(parityParsed)).c_str();
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
