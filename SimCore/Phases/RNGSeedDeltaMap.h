#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../Runner/Parallel/WorkerBootPlan.h"
#include "../Core/Input/InputPlan.h"

namespace simcore { class ParallelPhaseScriptRunner; }

namespace simcore {

    enum class SeedFamily : uint8_t { Neutral = 0, Main, CStick, Triggers };

    struct RandSeedProbeEntry {
        int grid_n = 0;
        SeedFamily family = SeedFamily::Neutral;
        uint8_t x = 0;
        uint8_t y = 0;
        uint32_t seed = 0;
        long long delta = 0;
        bool ok = false;
        std::string label;
    };

    struct RandSeedProbeResult {
        uint32_t base_seed = 0;
        std::vector<RandSeedProbeEntry> entries;
    };

    struct RandSeedComboEntry {
        GCInputFrame input;
        uint32_t seed = 0;
        long long delta = 0;
        bool ok = false;
        std::string label;
    };

    struct RandSeedComboResult {
        uint32_t base_seed = 0;
        std::vector<RandSeedComboEntry> entries;
    };

    struct RngSeedDeltaArgs {
        BootPlan boot;
        std::string savestate_path;
        int samples_per_axis = 5;
        int min_value = 0;
        int max_value = 255;
        bool cap_trigger_top = true;
        uint32_t run_timeout_ms = 10000;
        uint32_t combos_attempts_per_target = 256;  // max dispatches per target
        uint32_t combos_sampler_tries = 8;   // attempts to construct a triple that sums to target
    };

    std::vector<simcore::GCInputFrame> build_grid_main(int n, int minv, int maxv);
    std::vector<simcore::GCInputFrame> build_grid_cstick(int n, int minv, int maxv);
    std::vector<simcore::GCInputFrame> build_grid_trig(int n, int minv, int maxv, bool cap_top);

    struct ComboSampleSet {
        int32_t target_delta = 0;
        std::vector<GCInputFrame> frames;
    };

    struct JCTComboSamples {
        std::vector<GCInputFrame> singletons; // unique singleton frames used (includes neutral)
        std::vector<int32_t> expected;        // sorted unique (non-singleton) combo targets
        std::vector<ComboSampleSet> samples;  // samples[i].target_delta == expected[i]
    };

    /**
     * Build expected (non-singleton) J/C/T combo deltas from a probe grid and pre-sample
     * up to attempts_per_target GCInputFrames per expected target using the same fair
     * iterator and dedupe rules as RunFindSeedDeltaCombos.
     */
    JCTComboSamples PlanJCTComboSamples(
        const RandSeedProbeResult& grid,
        uint32_t attempts_per_target,
        uint32_t sampler_tries);

    RandSeedProbeResult RunRngSeedDeltaMap(ParallelPhaseScriptRunner& runner, const RngSeedDeltaArgs& args);
    RandSeedComboResult RunFindSeedDeltaCombos(ParallelPhaseScriptRunner& runner, const RngSeedDeltaArgs& args, const RandSeedProbeResult& grid);

} // namespace simcore
