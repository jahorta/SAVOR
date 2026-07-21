#include "SeedProbePayload.h"

#include <cstring>
#include <limits>

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/ScriptProgress.h"

namespace savor::seedprobe {
namespace {

constexpr std::size_t MaxPathBytes = 32 * 1024;

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}
bool GetU16(const std::uint8_t*& cursor, const std::uint8_t* end,
            std::uint16_t& value)
{
    if (end - cursor < 2) return false;
    value = static_cast<std::uint16_t>(cursor[0])
        | (static_cast<std::uint16_t>(cursor[1]) << 8);
    cursor += 2;
    return true;
}
bool GetU32(const std::uint8_t*& cursor, const std::uint8_t* end,
            std::uint32_t& value)
{
    if (end - cursor < 4) return false;
    value = static_cast<std::uint32_t>(cursor[0])
        | (static_cast<std::uint32_t>(cursor[1]) << 8)
        | (static_cast<std::uint32_t>(cursor[2]) << 16)
        | (static_cast<std::uint32_t>(cursor[3]) << 24);
    cursor += 4;
    return true;
}
void PutString(std::vector<std::uint8_t>& out, const std::string& value)
{
    PutU32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
bool GetString(const std::uint8_t*& cursor, const std::uint8_t* end,
               std::string& value)
{
    std::uint32_t size = 0;
    if (!GetU32(cursor, end, size) || size > MaxPathBytes
        || static_cast<std::size_t>(end - cursor) < size) return false;
    value.assign(reinterpret_cast<const char*>(cursor), size);
    cursor += size;
    return true;
}
bool ValidTarget(SeedProbeTarget target)
{
    return target == SeedProbeTarget::PreBattle
        || target == SeedProbeTarget::FieldReturn;
}
bool ValidMode(SeedProbeMode mode)
{
    return mode == SeedProbeMode::Observe
        || mode == SeedProbeMode::Materialize;
}

void SetContext(PSContext& out_ctx, const GCInputFrame& frame,
                std::uint32_t run_ms, std::uint32_t vi_stall_ms,
                SeedProbeTarget target, SeedProbeMode mode,
                std::optional<std::uint32_t> expected_seed,
                std::string output_path)
{
    namespace key = savor::context::key;
    out_ctx[key::seed::INPUT] = frame;
    out_ctx[key::core::RUN_MS] = run_ms;
    out_ctx[key::core::VI_STALL_MS] = vi_stall_ms;
    out_ctx[key::seed::TARGET] = static_cast<std::uint32_t>(target);
    out_ctx[key::seed::MODE] = static_cast<std::uint32_t>(mode);
    out_ctx[key::seed::HAS_EXPECTED_SEED] = expected_seed.has_value() ? 1u : 0u;
    out_ctx[key::seed::EXPECTED_SEED] = expected_seed.value_or(0u);
    out_ctx[key::seed::OUTPUT_SAVESTATE_PATH] = std::move(output_path);
    savor::progress::ProgressDeets progress{.poll_rate = 5000};
    progress.set_flag(CoreProgressFlags::DontRecordHeartbeat);
    out_ctx[key::core::PROGRESS_RATE] = progress.poll_rate;
    out_ctx[key::core::PROGRESS_CORE_FLAGS] = progress.flags;
}

} // namespace

bool encode_payload(const EncodeSpec& spec, std::vector<std::uint8_t>& out)
{
    if (!ValidTarget(spec.target) || !ValidMode(spec.mode)
        || spec.output_savestate_path.size() > MaxPathBytes
        || spec.output_savestate_path.size()
            > (std::numeric_limits<std::uint32_t>::max)()
        || (spec.mode == SeedProbeMode::Materialize
            && (!spec.expected_seed.has_value()
                || spec.output_savestate_path.empty()))) {
        out.clear();
        return false;
    }
    out.clear();
    out.reserve(1 + 2 + 4 + 4 + sizeof(GCInputFrame) + 4 * 4 + 4
        + spec.output_savestate_path.size());
    out.push_back(PK_SeedProbe);
    PutU16(out, PayloadVersion);
    PutU32(out, spec.run_ms);
    PutU32(out, spec.vi_stall_ms);
    const auto* frame = reinterpret_cast<const std::uint8_t*>(&spec.frame);
    out.insert(out.end(), frame, frame + sizeof(GCInputFrame));
    PutU32(out, static_cast<std::uint32_t>(spec.target));
    PutU32(out, static_cast<std::uint32_t>(spec.mode));
    PutU32(out, spec.expected_seed.has_value() ? 1u : 0u);
    PutU32(out, spec.expected_seed.value_or(0u));
    PutString(out, spec.output_savestate_path);
    return true;
}

bool decode_payload(const std::vector<std::uint8_t>& in, PSContext& out_ctx)
{
    constexpr std::size_t V1Size = 1 + 2 + 4 + 4 + sizeof(GCInputFrame);
    if (in.size() < V1Size) return false;
    const auto* cursor = in.data();
    const auto* end = cursor + in.size();
    if (*cursor++ != PK_SeedProbe) return false;
    std::uint16_t version = 0;
    std::uint32_t run_ms = 0, vi_stall_ms = 0;
    if (!GetU16(cursor, end, version)
        || !GetU32(cursor, end, run_ms)
        || !GetU32(cursor, end, vi_stall_ms)
        || static_cast<std::size_t>(end - cursor) < sizeof(GCInputFrame)) return false;
    GCInputFrame frame{};
    std::memcpy(&frame, cursor, sizeof(frame));
    cursor += sizeof(frame);

    if (version == LegacyPayloadVersion) {
        if (cursor != end) return false;
        SetContext(out_ctx, frame, run_ms, vi_stall_ms,
                   SeedProbeTarget::PreBattle, SeedProbeMode::Observe,
                   std::nullopt, {});
        return true;
    }
    if (version != PayloadVersion) return false;

    std::uint32_t raw_target = 0, raw_mode = 0, has_expected = 0, expected = 0;
    std::string output_path;
    if (!GetU32(cursor, end, raw_target) || !GetU32(cursor, end, raw_mode)
        || !GetU32(cursor, end, has_expected) || has_expected > 1
        || !GetU32(cursor, end, expected)
        || !GetString(cursor, end, output_path) || cursor != end) return false;
    const auto target = static_cast<SeedProbeTarget>(raw_target);
    const auto mode = static_cast<SeedProbeMode>(raw_mode);
    const std::optional<std::uint32_t> expected_seed =
        has_expected != 0 ? std::optional<std::uint32_t>(expected) : std::nullopt;
    if (!ValidTarget(target) || !ValidMode(mode)
        || (mode == SeedProbeMode::Materialize
            && (!expected_seed.has_value() || output_path.empty()))) return false;
    SetContext(out_ctx, frame, run_ms, vi_stall_ms, target, mode,
               expected_seed, std::move(output_path));
    return true;
}

} // namespace savor::seedprobe
