#include "BattleCompletionPayload.h"

#include <limits>

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/ScriptProgress.h"
#include "../BattleEndResults/BattleEndResultsReport.h"

namespace phase::battle::completion {
namespace {
constexpr std::size_t MaxPathBytes = 32 * 1024;
void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}
bool GetU32(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint32_t& value)
{
    if (static_cast<std::size_t>(end - cursor) < 4) return false;
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
bool GetString(const std::uint8_t*& cursor, const std::uint8_t* end, std::string& value)
{
    std::uint32_t size = 0;
    if (!GetU32(cursor, end, size) || size > MaxPathBytes
        || static_cast<std::size_t>(end - cursor) < size) return false;
    value.assign(reinterpret_cast<const char*>(cursor), size);
    cursor += size;
    return true;
}
}

bool encode_payload(const EncodeSpec& spec, std::vector<std::uint8_t>& out)
{
    if (spec.output_savestate_path.empty()
        || spec.output_savestate_path.size() > MaxPathBytes
        || spec.output_savestate_path.size()
            > (std::numeric_limits<std::uint32_t>::max)()) {
        out.clear();
        return false;
    }
    out.clear();
    out.push_back(savor::PK_BattleCompletionRunner);
    PutU32(out, PayloadVersion);
    PutString(out, spec.output_savestate_path);
    return true;
}

bool decode_payload(const std::vector<std::uint8_t>& in, savor::PSContext& out_ctx)
{
    if (in.size() < 1 + 4 + 4) return false;
    const auto* cursor = in.data();
    const auto* end = cursor + in.size();
    if (*cursor++ != savor::PK_BattleCompletionRunner) return false;
    std::uint32_t version = 0;
    std::string output_path;
    if (!GetU32(cursor, end, version) || version != PayloadVersion
        || !GetString(cursor, end, output_path) || output_path.empty()
        || cursor != end) return false;
    namespace key = savor::context::key;
    out_ctx[key::battlecompletion::OUTPUT_SAVESTATE_PATH] = std::move(output_path);
    out_ctx[key::battlecompletion::OUTCOME] =
        static_cast<std::uint32_t>(phase::battle::endresults::Outcome::Failed);
    out_ctx[key::battlecompletion::PROVIDER_FAILURE] = std::uint32_t{0};
    out_ctx[key::battlecompletion::RUNTIME_FAILURE] = std::uint32_t{0};
    out_ctx[key::battlecompletion::MACRO_RESULT] = std::uint32_t{1};
    out_ctx[key::battlecompletion::MANIFEST_BLOB] = std::string{};
    out_ctx[key::battlecompletion::DIAGNOSTIC] = std::string{};

    savor::progress::ProgressDeets progress{};
    progress.set_flag(CoreProgressFlags::ViDelta);
    progress.set_flag(CoreProgressFlags::ScriptSection);
    progress.set_flag(CoreProgressFlags::DontRecordHeartbeat);
    out_ctx[key::core::PROGRESS_CORE_FLAGS] = progress.flags;
    return true;
}

} // namespace phase::battle::completion
