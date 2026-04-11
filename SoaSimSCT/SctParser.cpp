#include "SctParser.h"

namespace soasim::sct {

SctParseResult SctParser::parse(std::span<const std::uint8_t> bytes, std::string sourcePath) const {
    SctParseResult result{};
    result.file.sourcePath = std::move(sourcePath);

    if (bytes.empty()) {
        result.diagnostics.push_back({"SCT parse skipped: input byte buffer is empty.", 0});
        return result;
    }

    result.diagnostics.push_back(
        {"SCT parser skeleton is initialized, but container and instruction decoding are not implemented yet.", 0});
    return result;
}

} // namespace soasim::sct
