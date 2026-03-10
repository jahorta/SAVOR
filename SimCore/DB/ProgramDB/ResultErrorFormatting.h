#pragma once

#include "../../Utils/IniDoc.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Runner/Script/PhaseScriptVM.h"

#include <string>

namespace simcore::db::codec {

inline std::string HumanizeResultIniErrors(const std::string& ini_text)
{
    if (ini_text.empty()) return ini_text;

    IniDoc ini;
    try {
        ini = IniDoc::parse(ini_text);
    }
    catch (...) {
        return ini_text;
    }

    for (const auto& section : ini.list_sections(false)) {
        if (ini.has(section, "w_err")) {
            const auto w_err = ini.get_u32(section, "w_err", simcore::WERR_UnknownError);
            ini.set(section, "w_err", simcore::WErrToString(w_err));
        }

        if (ini.has(section, "dw_err")) {
            const auto dw_err = ini.get_u32(section, "dw_err", static_cast<uint32_t>(simcore::RunToBpOutcome::Unknown));
            ini.set(section, "dw_err", simcore::RunToBpOutcomeToString(dw_err));
        }
    }

    return ini.to_string_preserve_order();
}

} // namespace simcore::db::codec
