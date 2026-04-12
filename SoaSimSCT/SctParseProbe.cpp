#include "SctParseProbe.h"

#include "SctParser.h"

#include <sstream>

namespace soasim::sct {

SctParseResult runSctParseProbe(const std::string& sctPath) {
    SctParser parser{};
    return parser.parseFile(sctPath);
}

std::string formatParseSummary(const SctParseResult& parseResult) {
    std::ostringstream out;
    out << "source=" << parseResult.file.sourcePath << '\n';
    out << "parseOk=" << (parseResult.parseOk ? "true" : "false") << '\n';
    out << "sectionCount=" << parseResult.file.sections.size() << '\n';

    for (const auto& section : parseResult.file.sections) {
        std::size_t scptRecordCount = 0;
        for (const auto& inst : section.instructions) {
            scptRecordCount += inst.scptParameterValueRecords.size();
        }
        out << "- [" << section.id.index << "] " << section.id.name
            << " isStringSection=" << (section.isStringSection ? "true" : "false")
            << " instructions=" << section.instructions.size()
            << " blocks=" << section.blocks.size()
            << " unknownRegions=" << section.unknownRegions.size()
            << " scptParamRecords=" << scptRecordCount << '\n';
    }

    if (!parseResult.diagnostics.empty()) {
        out << "diagnostics:" << '\n';
        for (const auto& diagnostic : parseResult.diagnostics) {
            if (!diagnostic.section.empty())
                out << diagnostic.section;
            out << "  - @" << diagnostic.offset << ": " << diagnostic.message << '\n';
        }
    }

    return out.str();
}

} // namespace soasim::sct
