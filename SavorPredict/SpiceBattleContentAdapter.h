#pragma once

#include "BattlePredictorResourceBundle.h"

#include <filesystem>
#include <string>
#include <vector>

namespace savor::predict {

struct SpiceBattleContentAdapterOptions {
    std::filesystem::path disc_dump_root;
    std::filesystem::path legacy_std_json_dir;
    std::string spice_revision;
};

struct SpiceBattleContentAdapterResult {
    BattlePredictorResourceBundlePtr bundle;
    BattlePredictorResourceInputStatus status =
        BattlePredictorResourceInputStatus::MissingInput;
    std::vector<BattlePredictorResourceDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return bundle != nullptr
            && status == BattlePredictorResourceInputStatus::Ready;
    }
};

[[nodiscard]] SpiceBattleContentAdapterResult
load_first_battle_spice_resource_bundle(
    const SpiceBattleContentAdapterOptions& options);

[[nodiscard]] const char* spice_battle_content_adapter_rule_detail();

} // namespace savor::predict
