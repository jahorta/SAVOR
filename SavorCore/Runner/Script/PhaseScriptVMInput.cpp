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
    void PhaseScriptVM::op_step_frames(const PSOp& op) { SCLOGD("[VM] phase=run_inputs begin frames=%zu", op.step.n); if (op.imm.v == 1) host_.setEnableAllBreakpoints(false); for (uint32_t i = 0; i < op.step.n; ++i) host_.stepOneFrameBlocking(); if (op.imm.v == 1) restore_canonical_breakpoint_scope(); SCLOGD("[VM] phase=run_inputs end"); }
    void PhaseScriptVM::op_step_opcode(const PSOp& op) { SCLOGD("[VM] phase=run_inputs begin opcode"); if (op.imm.v == 1) host_.setEnableAllBreakpoints(false); host_.stepOneOpcodeBlocking(); if (op.imm.v == 1) restore_canonical_breakpoint_scope(); SCLOGD("[VM] phase=run_inputs end opcode"); }
    void PhaseScriptVM::op_start_deterministic_run() const { if (!host_.startMovieRecording()) SCLOGE("[VM] Unable to start recording for deterministic run"); }
    void PhaseScriptVM::op_end_deterministic_run() const { host_.endMovieRecording(); }

    bool PhaseScriptVM::op_apply_input_from(const PSOp& op, PSResult&, PSContext& ctx) { auto it = ctx.find(op.key.id); if (it == ctx.end()) return false; if (auto p = std::get_if<GCInputFrame>(&it->second)) { host_.setInput(*p); return true; } return false; }

    bool PhaseScriptVM::op_movie_play_from(const PSOp& op, PSResult&, PSContext& ctx) {
        std::string path;
        ctx.get<std::string>(op.key.id, path);

        host_.clearAllPcBreakpoints();
        armed_ = false;
        armed_pcs_.clear();

        SCLOGI("[VM] MOVIE_PLAY arm-before-start path=%s", path.c_str());
        arm_bps_once();

        if (!host_.startMoviePlayback(path))
            return false;

        SCLOGI("[VM] MOVIE_PLAY arm-after-boot path=%s movie=%d input=%llu",
            path.c_str(),
            host_.isMoviePlaying() ? 1 : 0,
            static_cast<unsigned long long>(host_.getCurrentMovieInputCount()));
        armed_ = false;
        armed_pcs_.clear();
        arm_bps_once();
        host_.emitProbeMarker("movie.start");
        return true;
    }
    bool PhaseScriptVM::op_save_savestate_from(const PSOp& op, PSResult& result, PSContext& ctx) {
        std::string path;
        ctx.get<std::string>(op.key.id, path);
        if (path.empty()) {
            SCLOGW("[VM] SAVE_SAVESTATE skipped empty path key=%s", savor::context::key::name_for_id(op.key.id).data());
            result.ctx = ctx;
            return true;
        }
        SCLOGI("[VM] SAVE_SAVESTATE begin path=%s", path.c_str());
        if (!host_.saveSavestateBlocking(path)) {
            SCLOGW("[VM] SAVE_SAVESTATE failed path=%s", path.c_str());
            result.ctx = ctx;
            return false;
        }
        ctx[savor::context::key::core::LAST_SAVESTATE_PATH] = path;
        SCLOGI("[VM] SAVE_SAVESTATE end path=%s", path.c_str());
        return true;
    }
    bool PhaseScriptVM::op_require_disc_gameid_from(const PSOp& op, PSResult&, PSContext& ctx) { std::string tmp; ctx.get<std::string>(op.key.id, tmp); if (tmp.size() < 6) return false; auto di = host_.getDiscInfo(); return di.has_value() && di->game_id.size() >= 6 && std::memcmp(di->game_id.data(), tmp.c_str(), 6) == 0; }

    void PhaseScriptVM::op_movie_stop() {
        host_.endMoviePlaybackBlocking();
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
        auto itC = ctx.find(savor::context::key::battle::INPUTPLAN_FRAME_COUNT);
        auto itT = ctx.find(savor::context::key::battle::INPUTPLAN);
        ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(0);
        ctx[savor::context::key::battle::INPUT_PLAYBACK_UNACKED] = uint32_t(0);
        if (itC == ctx.end() || itT == ctx.end()) {
            ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(1);
            return;
        }
        const auto* counts_s = std::get_if<std::string>(&itC->second);
        const auto* table_s = std::get_if<std::string>(&itT->second);
        if (!counts_s || !table_s || counts_s->size() < sizeof(uint32_t)) {
            ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(2);
            return;
        }
        const uint8_t* counts = (const uint8_t*)counts_s->data();
        const uint8_t* frames = (const uint8_t*)table_s->data();
        if (counts == 0) { ctx[savor::context::key::core::PLAN_DONE] = uint32_t(1); return; }
        const uint32_t apply_vi_start = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        host_.setEnableAllBreakpoints(false);
        uint32_t count = 0;
        std::memcpy(&count, counts, sizeof(uint32_t));
        if (table_s->size() < static_cast<size_t>(count) * sizeof(GCInputFrame)) {
            restore_canonical_breakpoint_scope();
            ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(2);
            return;
        }
        savor::ControllerInputSequence plan{}; plan.reserve(count);
        uint32_t rand{ 0 };
        host_.readU32(addr::AddrRegistry::base(addr::core::RNG_SEED), rand);
        SCLOGTX(SC_TAGS("vm", "input", "rng"), "RNG before inputs %X", rand);
        for (uint32_t idx = 0; idx < count; idx++) {
            GCInputFrame f{};
            std::memcpy(&f, frames + (idx * sizeof(GCInputFrame)), sizeof(GCInputFrame));
            plan.push_back(f);
        }
        uint32_t retry_count = 0;
        ctx.get(savor::context::key::battle::INPUT_RETRY_COUNT, retry_count);
        std::string playback_label = std::format("battle_turn_attempt_{}", retry_count);
        const auto playback = host_.playInputTapeBlocking(
            plan,
            DolphinWrapper::InputTapePlaybackOptions{
                .max_unacked_replays = 2,
                .safe_mode = retry_count > 0,
                .label = playback_label.c_str(),
            });
        host_.readU32(addr::AddrRegistry::base(addr::core::RNG_SEED), rand);
        SCLOGTX(SC_TAGS("vm", "input", "rng"), "RNG after inputs %X", rand);
        restore_canonical_breakpoint_scope();
        ctx[savor::context::key::battle::INPUT_PLAYBACK_UNACKED] = playback.unacked_count;
        if (!playback.ok) {
            ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(3);
            ctx[savor::context::key::core::PLAN_DONE] = uint32_t(0);
            SCLOGDX(SC_TAGS("vm", "input"),
                "[VM] input playback failed attempt=%u failed_index=%u unacked=%u",
                retry_count,
                playback.failed_index,
                playback.unacked_count);
            return;
        }
        const uint32_t apply_vi_end = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        uint32_t turn_number = 0;
        if (!ctx.get(savor::context::key::battle::TURN_OUTPUT_INDEX, turn_number)) (void)ctx.get(savor::context::key::battle::ACTIVE_TURN, turn_number);
        std::string turn_blob; (void)ctx.get(savor::context::key::battle::APPLIED_INPUTPLAN_TURN_BLOB, turn_blob);
        savor::inputtrace::BattleTurnInputTrace chunk{};
        chunk.turn_number = turn_number;
        chunk.vi_start = apply_vi_start;
        chunk.vi_end = apply_vi_end;
        chunk.frames = playback.attempted_frames;
        chunk.vi_durations = playback.vi_durations;
        (void)savor::inputtrace::append_turn_input_trace(turn_blob, chunk);
        ctx[savor::context::key::battle::APPLIED_INPUTPLAN_TURN_BLOB] = std::move(turn_blob);
        ctx[savor::context::key::battle::APPLIED_INPUTPLAN_COUNT] = static_cast<uint32_t>(playback.attempted_frames.size());
        ctx[savor::context::key::core::PLAN_DONE] = uint32_t(1);
        uint32_t cur_turn_plans = 0;
        ctx.get(savor::context::key::battle::NUM_TURN_PLANS, cur_turn_plans);
        ctx[savor::context::key::battle::NUM_TURN_PLANS] = cur_turn_plans > 0 ? cur_turn_plans - 1 : 0;
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
