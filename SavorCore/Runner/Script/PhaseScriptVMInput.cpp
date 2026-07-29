#include "PhaseScriptVM.h"

#include "ScriptProgress.h"
#include "../IPC/Wire.h"
#include "../../Core/Input/BattleInputTraceBlob.h"
#include "../../Core/Input/InputPlanFmt.h"
#include "../../Core/Input/SoaBattle/ActionLibrary.h"
#include "../../Core/Input/SoaBattle/PlanWriter.h"
#include "../../Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../Utils/IniDoc.h"

#include <cstring>

namespace savor {
    void PhaseScriptVM::op_step_frames(const PSOp& op)
    {
        (void)op;
        SCLOGE(
            "[VM] frame stepping is disconnected; use the canonical ExecutionEngine");
    }

    void PhaseScriptVM::op_step_opcode(const PSOp& op)
    {
        (void)op;
        SCLOGE(
            "[VM] guest-instruction stepping is unsupported; use router "
            "source-stop suppression or a declared semantic successor");
    }
    bool PhaseScriptVM::op_start_deterministic_run(
        PSResult& result,
        PSContext& ctx) const
    {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] movie recording is disconnected; use MovieService");
    }

    bool PhaseScriptVM::op_end_deterministic_run(
        PSResult& result,
        PSContext& ctx) const
    {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] movie recording is disconnected; use MovieService");
    }

    bool PhaseScriptVM::op_apply_input_from(
        const PSOp& op,
        PSResult& result,
        PSContext& ctx)
    {
        (void)op;
        return fail_legacy_service(
            result,
            ctx,
            "[VM] direct input publication is disconnected; use InputArbiter");
    }

    bool PhaseScriptVM::op_movie_play_from(
        const PSOp& op,
        PSResult& result,
        PSContext& ctx)
    {
        (void)op;
        return fail_legacy_service(
            result,
            ctx,
            "[VM] movie playback is disconnected; use MovieService");
    }

    bool PhaseScriptVM::op_save_savestate_from(
        const PSOp& op,
        PSResult& result,
        PSContext& ctx)
    {
        (void)op;
        return fail_legacy_service(
            result,
            ctx,
            "[VM] savestate persistence is disconnected; use StateService");
    }

    bool PhaseScriptVM::op_require_disc_gameid_from(const PSOp& op, PSResult&, PSContext& ctx) { std::string tmp; ctx.get<std::string>(op.key.id, tmp); if (tmp.size() < 6) return false; auto di = host_.getDiscInfo(); return di.has_value() && di->game_id.size() >= 6 && std::memcmp(di->game_id.data(), tmp.c_str(), 6) == 0; }

    bool PhaseScriptVM::op_movie_stop(
        PSResult& result,
        PSContext& ctx)
    {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] movie playback is disconnected; use MovieService");
    }

    void PhaseScriptVM::op_build_turn_inputplan_from_battle_path(PSContext& ctx) const {
        uint32_t turn = 0;
        ctx.get<uint32_t>(savor::context::key::battle::ACTIVE_TURN, turn);
        if (turn == 0) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::InvalidTurnIdxZero;
            ctx[savor::context::key::core::PLAN_DONE] = (uint32_t)1;
        }
        soa::battle::actions::BattlePath bp;
        if (!ctx.get<soa::battle::actions::BattlePath>(savor::context::key::battle::TURN_PLANS, bp)) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::BadBlob;
            ctx[savor::context::key::core::PLAN_DONE] = (uint32_t)1;
            return;
        }
        if (turn > bp.size()) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::OutOfTurns;
            ctx[savor::context::key::core::PLAN_DONE] = (uint32_t)1;
            return;
        }
        soa::battle::ctx::BattleContext bc{};
        std::string blob;
        if (ctx.get<std::string>(savor::context::key::battle::CTX_BLOB, blob)) soa::battle::ctx::codec::decode(blob, bc);
        const auto& turn_plan = bp[turn - 1];
        savor::ControllerInputSequence plan;
        auto err = soa::battle::actions::MaterializeErr::OK;
        if (!soa::battle::actions::MaterializeBattleTurnInputs(bc, turn_plan, plan, err)) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)err;
            ctx[savor::context::key::core::PLAN_DONE] = (uint32_t)1;
            return;
        }
        const uint32_t n = static_cast<uint32_t>(plan.size());
        std::string counts; counts.resize(sizeof(uint32_t)); std::memcpy(counts.data(), &n, sizeof(uint32_t));
        std::string frames; frames.resize(n * sizeof(savor::GCInputFrame)); if (n) std::memcpy(frames.data(), plan.data(), frames.size());
        ctx[savor::context::key::battle::NUM_TURN_PLANS] = uint32_t(1);
        ctx[savor::context::key::battle::INPUTPLAN_FRAME_COUNT] = counts;
        ctx[savor::context::key::battle::INPUTPLAN] = frames;
        ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = uint32_t((uint32_t)soa::battle::actions::MaterializeErr::OK);
        ctx[savor::context::key::core::PLAN_DONE] = uint32_t(0);
    }

    void PhaseScriptVM::op_apply_battle_inputplan_frames(PSContext& ctx) {
        ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(4);
        ctx[savor::context::key::battle::INPUT_PLAYBACK_UNACKED] = uint32_t(0);
        ctx[savor::context::key::core::PLAN_DONE] = uint32_t(0);
        SCLOGE(
            "[VM] input-tape advancement is disconnected; use InputArbiter and ExecutionEngine");
    }

    void PhaseScriptVM::op_record_tas_input_sample(PSContext& ctx) {
        uint32_t sample_count = 0;
        ctx.get<uint32_t>(savor::context::key::tasframedetector::SAMPLE_COUNT, sample_count);

        const uint32_t vi = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        const uint64_t input_count = host_.getCurrentMovieInputCount();
        const uint32_t movie_ended = host_.isMoviePlaybackEnded() ? 1u : 0u;

        std::string stream_ini;
        (void)ctx.get<std::string>(savor::context::key::tasframedetector::STREAM_INI, stream_ini);
        IniDoc doc = stream_ini.empty() ? IniDoc{} : IniDoc::parse(stream_ini);
        doc.ensure_section("FrameSamples");
        const std::string idx = std::to_string(sample_count);
        doc.set("FrameSamples", "frame." + idx, idx);
        doc.set("FrameSamples", "vi." + idx, std::to_string(vi));
        doc.set("FrameSamples", "input_count." + idx, std::to_string(input_count));
        doc.set("FrameSamples", "movie_ended." + idx, std::to_string(movie_ended));
        doc.set("FrameSamples", "count", std::to_string(sample_count + 1));

        ctx[savor::context::key::tasframedetector::STREAM_INI] = doc.to_string_sorted();
        ctx[savor::context::key::tasframedetector::SAMPLE_COUNT] = sample_count + 1;
        ctx[savor::context::key::tasframedetector::MOVIE_ENDED] = movie_ended;
        ctx[savor::context::key::tasframedetector::INPUT_COUNT] = static_cast<uint32_t>(input_count & 0xFFFFFFFFu);

        SCLOGT("[TasInputStreamDetector] sample=%u vi=%u input_count=%llu movie_ended=%u",
            sample_count,
            vi,
            static_cast<unsigned long long>(input_count),
            movie_ended);
    }
} // namespace savor
