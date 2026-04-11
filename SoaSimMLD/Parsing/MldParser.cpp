#include "MldParser.h"

namespace soasim::mld::parsing {

ParseResult MldParser::parse(std::span<const std::uint8_t> mldBytes, const ParseOptions& options) const {
    (void)options;

    ParseResult result{};
    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Warning,
        .message = "SoaSimMLD parser skeleton only: MLD binary parsing is not implemented yet.",
    });

    if (mldBytes.empty()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "Input buffer is empty.",
        });
    }

    return result;
}

} // namespace soasim::mld::parsing
