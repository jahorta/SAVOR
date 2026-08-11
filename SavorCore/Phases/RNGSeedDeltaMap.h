#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../Core/Input/GCInputFrame.h"

namespace savor {

    enum class SeedFamily : uint8_t { Neutral = 0, Main, CStick, Triggers };

    struct RandSeedProbeEntry {
        int grid_n = 0;
        SeedFamily family = SeedFamily::Neutral;
        uint8_t x = 0;
        uint8_t y = 0;
        uint32_t seed = 0;
        std::int32_t delta = 0;
        bool ok = false;
        std::string label;
    };

    struct RandSeedProbeResult {
        uint32_t base_seed = 0;
        std::vector<RandSeedProbeEntry> entries;
    };

    std::vector<savor::GCInputFrame> build_grid_main(int n, int minv, int maxv);
    std::vector<savor::GCInputFrame> build_grid_cstick(int n, int minv, int maxv);
    std::vector<savor::GCInputFrame> build_grid_trig(int n, int minv, int maxv, bool cap_top);

    /**
     * Subtract two uint32 RNG states with uint32 wraparound, then interpret
     * the resulting 32 bits as a signed delta.
     */
    std::int32_t wrapped_seed_delta(
        std::uint32_t observed,
        std::uint32_t neutral) noexcept;

    struct ComboSampleSet {
        int32_t target_delta = 0;
        std::vector<GCInputFrame> frames;
    };

    struct SingletonSample {
        int32_t target_delta = 0;
        uint32_t seed = 0;
        GCInputFrame frame;
    };

    struct JCTComboSamples {
        std::vector<SingletonSample> singletons; // unique singleton frames used (includes neutral)
        std::vector<int32_t> expected;           // sorted unique (non-singleton) combo targets
        std::vector<ComboSampleSet> samples;     // samples[i].target_delta == expected[i]
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

} // namespace savor
