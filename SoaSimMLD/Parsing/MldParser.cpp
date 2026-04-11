#include "MldParser.h"

#include "../../Compression/Aklz.h"

namespace soasim::mld::parsing {

ParseResult MldParser::parse(std::span<const std::uint8_t> mldBytes, const ParseOptions& options) const {
    (void)options;

    ParseResult result{};

    std::vector<std::uint8_t> decoded;
    std::span<const std::uint8_t> payload = mldBytes;
    if (soasim::compression::aklz::isAklz(mldBytes)) {
        auto decodedResult = soasim::compression::aklz::decompress(mldBytes);
        if (!decodedResult.ok()) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = ParseDiagnostic::Severity::Error,
                .message = "AKLZ decompression failed: " + std::string(soasim::compression::aklz::errorToString(decodedResult.error)),
            });
            return result;
        }

        decoded = std::move(decodedResult.bytes);
        payload = std::span<const std::uint8_t>(decoded.data(), decoded.size());
    }
    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Warning,
        .message = "SoaSimMLD parser skeleton only: MLD binary parsing is not implemented yet.",
    });

    if (payload.empty()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "Input buffer is empty.",
        });
    }

    return result;
}

} // namespace soasim::mld::parsing
