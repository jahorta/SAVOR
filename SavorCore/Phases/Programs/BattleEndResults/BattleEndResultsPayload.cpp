#include "BattleEndResultsPayload.h"

#include <limits>

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/ScriptProgress.h"
#include "../BattleCompletion/BattleCompletionManifest.h"

namespace phase::battle::endresults {
namespace {

constexpr std::size_t kMaxPathBytes = 32 * 1024;
constexpr std::size_t kMaxManifestBytes = 1024 * 1024;

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

bool GetU32(
    const std::uint8_t*& cursor,
    const std::uint8_t* end,
    std::uint32_t& value)
{
    if (static_cast<std::size_t>(end - cursor) < sizeof(std::uint32_t)) return false;
    value = static_cast<std::uint32_t>(cursor[0])
        | (static_cast<std::uint32_t>(cursor[1]) << 8)
        | (static_cast<std::uint32_t>(cursor[2]) << 16)
        | (static_cast<std::uint32_t>(cursor[3]) << 24);
    cursor += sizeof(std::uint32_t);
    return true;
}

void PutString(std::vector<std::uint8_t>& out, const std::string& value)
{
    PutU32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

bool GetString(
    const std::uint8_t*& cursor,
    const std::uint8_t* end,
    std::string& value)
{
    std::uint32_t size = 0;
    if (!GetU32(cursor, end, size)
        || size > kMaxPathBytes
        || static_cast<std::size_t>(end - cursor) < size) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(cursor), size);
    cursor += size;
    return true;
}

bool GetBlob(
    const std::uint8_t*& cursor,
    const std::uint8_t* end,
    std::string& value)
{
    std::uint32_t size = 0;
    if (!GetU32(cursor, end, size)
        || size > kMaxManifestBytes
        || static_cast<std::size_t>(end - cursor) < size) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(cursor), size);
    cursor += size;
    return true;
}

} // namespace

bool encode_payload(const EncodeSpec& spec, std::vector<std::uint8_t>& out)
{
    phase::battle::completion::Manifest manifest{};
    if (spec.run_timeout_ms == 0
        || !IsValidPolicy(spec.acceleration_policy)
        || spec.completion_manifest_blob.empty()
        || spec.completion_manifest_blob.size() > kMaxManifestBytes
        || !phase::battle::completion::DecodeManifest(
            spec.completion_manifest_blob, manifest)
        || spec.output_savestate_path.empty()
        || spec.output_savestate_path.size() > kMaxPathBytes
        || spec.output_savestate_path.size() > std::numeric_limits<std::uint32_t>::max()) {
        out.clear();
        return false;
    }

    out.clear();
    out.reserve(1 + 4 + 4 + 4 + 4 + spec.completion_manifest_blob.size()
        + 4 + spec.output_savestate_path.size());
    out.push_back(savor::PK_BattleResultsScreenRunner);
    PutU32(out, PayloadVersion);
    PutU32(out, spec.run_timeout_ms);
    PutU32(out, static_cast<std::uint32_t>(spec.acceleration_policy));
    PutString(out, spec.completion_manifest_blob);
    PutString(out, spec.output_savestate_path);
    return true;
}

bool decode_payload(const std::vector<std::uint8_t>& in, savor::PSContext& out_ctx)
{
    if (in.size() < 1 + 4 + 4 + 4 + 4 + 4) return false;
    const std::uint8_t* cursor = in.data();
    const std::uint8_t* end = cursor + in.size();
    if (*cursor++ != savor::PK_BattleResultsScreenRunner) return false;

    std::uint32_t version = 0;
    std::uint32_t run_timeout_ms = 0;
    std::uint32_t policy_value = 0;
    std::string manifest_blob;
    std::string output_path;
    if (!GetU32(cursor, end, version)
        || version != PayloadVersion
        || !GetU32(cursor, end, run_timeout_ms)
        || run_timeout_ms == 0
        || !GetU32(cursor, end, policy_value)
        || !GetBlob(cursor, end, manifest_blob)
        || manifest_blob.empty()
        || !GetString(cursor, end, output_path)
        || output_path.empty()
        || cursor != end) {
        return false;
    }

    const auto policy = static_cast<AccelerationPolicy>(policy_value);
    phase::battle::completion::Manifest manifest{};
    if (!IsValidPolicy(policy)
        || !phase::battle::completion::DecodeManifest(manifest_blob, manifest)) return false;

    namespace key = savor::context::key;
    out_ctx[key::core::RUN_MS] = run_timeout_ms;
    out_ctx[key::core::RUN_POLL_MS] = std::uint32_t{0};
    out_ctx[key::battleend::RUN_TIMEOUT_MS] = run_timeout_ms;
    out_ctx[key::battleend::ACCELERATION_POLICY] = policy_value;
    out_ctx[key::battleend::COMPLETION_MANIFEST_BLOB] = std::move(manifest_blob);
    out_ctx[key::battleend::OUTPUT_SAVESTATE_PATH] = std::move(output_path);
    out_ctx[key::battleend::OUTCOME] = static_cast<std::uint32_t>(Outcome::Failed);
    out_ctx[key::battleend::PROVIDER_FAILURE] = static_cast<std::uint32_t>(FailureCode::None);
    out_ctx[key::battleend::RUNTIME_FAILURE] = std::uint32_t{0};
    out_ctx[key::battleend::MACRO_RESULT] = std::uint32_t{1};
    out_ctx[key::battleend::ACTION_COUNT] = std::uint32_t{0};
    out_ctx[key::battleend::INPUT_REQUEST_COUNT] = std::uint32_t{0};
    out_ctx[key::battleend::INPUT_OBSERVED_COUNT] = std::uint32_t{0};
    out_ctx[key::battleend::RELEASE_REQUEST_COUNT] = std::uint32_t{0};
    out_ctx[key::battleend::RELEASE_OBSERVED_COUNT] = std::uint32_t{0};
    out_ctx[key::battleend::LAST_EXPECTED_BP] = std::uint32_t{0};
    out_ctx[key::battleend::LAST_HIT_BP] = std::uint32_t{0};
    out_ctx[key::battleend::LAST_HIT_PC] = std::uint32_t{0};
    out_ctx[key::battleend::LAST_STATE] = std::uint32_t{0};
    out_ctx[key::battleend::LAST_SUBSTATE] = std::uint32_t{0};
    out_ctx[key::battleend::LAST_TOKEN_LO] = std::uint32_t{0};
    out_ctx[key::battleend::LAST_TOKEN_HI] = std::uint32_t{0};
    out_ctx[key::battleend::EXPECTED_STAT_WAVES] = std::uint32_t{0};
    out_ctx[key::battleend::OBSERVED_STAT_WAVES] = std::uint32_t{0};
    out_ctx[key::battleend::EXPECTED_LEARNED_WAVES] = std::uint32_t{0};
    out_ctx[key::battleend::OBSERVED_LEARNED_WAVES] = std::uint32_t{0};
    out_ctx[key::battleend::EXPECTED_ITEM_POPUP] = std::uint32_t{0};
    out_ctx[key::battleend::OBSERVED_ITEM_POPUP] = std::uint32_t{0};
    out_ctx[key::battleend::MISMATCH_FLAGS] = std::uint32_t{0};
    out_ctx[key::battleend::INVARIANT_FLAGS] = std::uint32_t{0};
    out_ctx[key::battleend::SOURCE_INVARIANT_FLAGS] = std::uint32_t{0};
    out_ctx[key::battleend::REWARD_INVARIANT_FLAGS] = std::uint32_t{0};
    out_ctx[key::battleend::LIFECYCLE_INVARIANT_FLAGS] = std::uint32_t{0};
    out_ctx[key::battleend::COMPLETION_INVARIANT_FLAGS] = std::uint32_t{0};
    out_ctx[key::battleend::REPORT_BLOB] = std::string{};
    out_ctx[key::battleend::DIAGNOSTIC] = std::string{};
    out_ctx[key::battleend::ENTRY_RNG_SEED] = std::uint32_t{0};
    out_ctx[key::battleend::FINAL_RNG_SEED] = std::uint32_t{0};
    out_ctx[key::battleend::RNG_EFFECT_KIND] = std::uint32_t{0};
    out_ctx[key::battleend::RNG_ADVANCE_COUNT] = std::uint32_t{0};

    savor::progress::ProgressDeets progress{.poll_rate = 5000};
    progress.set_flag(CoreProgressFlags::ViDelta);
    progress.set_flag(CoreProgressFlags::ScriptSection);
    progress.set_flag(CoreProgressFlags::WarnViStall);
    progress.set_flag(CoreProgressFlags::DontRecordHeartbeat);
    out_ctx[key::core::PROGRESS_RATE] = progress.poll_rate;
    out_ctx[key::core::PROGRESS_CORE_FLAGS] = progress.flags;
    return true;
}

} // namespace phase::battle::endresults
