// SimCore/DB/ProgramDB/SeedProbeDBCodec.cpp
#include "SeedProbeDBCodec.h"
#include "../../Utils/Hex.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../Scheduling/SeedProbeWinnersRepo.h"
#include "../Scheduling/TriggersRepo.h"
#include "../SeedProbeRepo.h"
#include "../TasMovieRepo.h"
#include "../SavestateRepo.h"
#include "../DeltaSeedRepo.h"
#include "../../Phases/Programs/SeedProbe/SeedProbePayload.h"
#include "../../Phases/RNGSeedDeltaMap.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Runner/Parallel/DB/DBTriggerEngine.h"
#include "../../Core/Input/InputPlan.h"

#include <cctype>
#include <sstream>
#include <iomanip>
#include <cstring>
#include "../Querying/DataService.h"

using simcore::db::DbResult;
using simcore::db::JobRow;
using simcore::db::JobSetsRepo;
using simcore::db::JobsRepo;
using simcore::db::TriggersRepo;
using simcore::db::JobEventsRepo;
using simcore::db::SeedProbeRepo;
using simcore::db::SeedProbeRow;
using simcore::db::codec::seedprobe::GridIni;
using simcore::db::codec::seedprobe::UniqueIni;
using simcore::db::codec::seedprobe::BlueprintIni;
using simcore::db::codec::seedprobe::JobIni;
using simcore::db::codec::seedprobe::ResultsIni;
using simcore::db::codec::seedprobe::CleanupIni;
using simcore::db::codec::seedprobe::SeedProbePhase;

static constexpr int PK = simcore::PK_SeedProbe;
static constexpr int PV = 1;
using simcore::TriggerCtx;

static inline std::string fingerprint_for(int64_t probe_id, const std::string& frame_hex, uint32_t run_ms, uint32_t vi_stall_ms) {
    std::ostringstream oss;
    oss << "PK=" << PK << ";PV=" << PV << ";probe_id=" << probe_id
        << ";frame=" << frame_hex << ";run_ms=" << run_ms << ";vi=" << vi_stall_ms;
    return oss.str();
}

static simcore::db::DbResult<int64_t> encode_neutral(int64_t job_set_id, const std::string& blueprint_ini)
{   
    IniDoc ini = IniDoc::parse(blueprint_ini);
    
    BlueprintIni bp_ini = BlueprintIni::from_section(ini);
    
    const std::string frame_hex = simcore::GCInputFrame().to_frame_hex();

    
    JobIni jb{};
    jb.frame_hex = frame_hex;

    const std::string fp = fingerprint_for(bp_ini.probe_id, frame_hex, bp_ini.run_ms, bp_ini.vi_stall_ms);

    auto ins = JobsRepo::CreateOrGetByFingerprint(job_set_id, PK, PV, bp_ini.probe_id, fp, 0, jb.append_section(ini).to_string_sorted());
    if (!ins.ok) return simcore::db::DbResult<int64_t>::Err(ins.error);

    const int64_t job_id = ins.value;
    const std::string short_line = "probe=" + std::to_string(bp_ini.probe_id) + " input=" + frame_hex
        + " neutral=1 grid=0 unique=0"
        + " prio=0 run_ms=" + std::to_string(bp_ini.run_ms)
        + " vi=" + std::to_string(bp_ini.vi_stall_ms);
    JobEventsRepo::Append(job_id, "ENQUEUED", short_line);

    IniKV cond;
    cond.add("type", "EACH_JOB_TERMINAL");
    cond.add("success_only", std::to_string(1));

    auto tr = TriggersRepo::AddForJob(job_id, simcore::PK_SeedProbe, cond.to_string_sorted(), blueprint_ini);
    if (!tr.ok) return simcore::db::DbResult<int64_t>::Err(tr.error);

    return simcore::db::DbResult<int64_t>::Ok(1);
}

static simcore::db::DbResult<int64_t> encode_grid(int64_t job_set_id, const std::string& blueprint_ini)
{
    IniDoc ini = IniDoc::parse(blueprint_ini);

    BlueprintIni bp_ini = BlueprintIni::from_section(ini);

    GridIni grid_ini = GridIni::from_section(ini);

    auto pr = simcore::db::SeedProbeRepo::Get(bp_ini.probe_id);
    if (!pr.ok) return simcore::db::DbResult<int64_t>::Err(pr.error);
    if (pr.value.neutral_seed == 0) {
        return simcore::db::DbResult<int64_t>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "neutral_seed not set; enqueue neutral job first" });
    }
    
    std::vector<simcore::GCInputFrame> frames;
    frames = simcore::build_grid_main(grid_ini.samples_per_axis, grid_ini.min_value, grid_ini.max_value);

    std::vector<simcore::GCInputFrame> cstick = simcore::build_grid_cstick(grid_ini.samples_per_axis, grid_ini.min_value, grid_ini.max_value);
    frames.insert(frames.end(), cstick.begin(), cstick.end());

    std::vector<simcore::GCInputFrame> triggers = simcore::build_grid_trig(grid_ini.samples_per_axis, 
        grid_ini.ignore_trigger_minmax ? 0 : grid_ini.min_value, 
        grid_ini.ignore_trigger_minmax ? 255 : grid_ini.max_value,
        grid_ini.cap_trigger_top);
    frames.insert(frames.end(), triggers.begin(), triggers.end());

    std::vector<std::string> inputs;
    for (auto f : frames) inputs.push_back(f.to_frame_hex());

    int64_t enqueued = 0;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const std::string frame_hex = inputs[i];

        IniKV vmkv;
        vmkv.add("probe_id", std::to_string(bp_ini.probe_id));
        vmkv.add("frame_hex", frame_hex);
        vmkv.add("run_ms", std::to_string(bp_ini.run_ms));
        vmkv.add("vi_stall_ms", std::to_string(bp_ini.vi_stall_ms));

        const std::string vm_kv_text = vmkv.to_string_sorted();
        const std::string fp = fingerprint_for(bp_ini.probe_id, frame_hex, bp_ini.run_ms, bp_ini.vi_stall_ms);

        auto ins = JobsRepo::CreateOrGetByFingerprint(job_set_id, PK, PV, bp_ini.probe_id, fp, 0, vm_kv_text);
        if (!ins.ok) return simcore::db::DbResult<int64_t>::Err(ins.error);

        const int64_t job_id = ins.value;
        const std::string short_line = "probe=" + std::to_string(bp_ini.probe_id) + " input=" + frame_hex
            + " neutral=0 grid=1 unique=0"
            + " prio=0 run_ms=" + std::to_string(bp_ini.run_ms)
            + " vi=" + std::to_string(bp_ini.vi_stall_ms);
        JobEventsRepo::Append(job_id, "ENQUEUED", short_line);

        ++enqueued;
    }

    IniKV cond;
    cond.add("type", "ALL_SUCCEEDED");
    bp_ini.set_section(ini);

    auto tr = TriggersRepo::AddForJobSet(job_set_id, simcore::PK_SeedProbe, cond.to_string_sorted(), ini.to_string_sorted());
    if (!tr.ok) return simcore::db::DbResult<int64_t>::Err(tr.error);

    return simcore::db::DbResult<int64_t>::Ok(enqueued);
}

static simcore::db::DbResult<int64_t> encode_unique(int64_t job_set_id, const std::string& blueprint_ini)
{
    IniDoc ini = IniDoc::parse(blueprint_ini);

    BlueprintIni bp_ini = BlueprintIni::from_section(ini);

    UniqueIni unique_ini = UniqueIni::from_section(ini);
    
    auto pr = simcore::db::SeedProbeRepo::Get(bp_ini.probe_id);
    if (!pr.ok) return simcore::db::DbResult<int64_t>::Err(pr.error);
    if (pr.value.neutral_seed == 0) {
        return simcore::db::DbResult<int64_t>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "neutral_seed not set; enqueue neutral job first" });
    }
    
    auto dr = simcore::db::DeltaSeedRepo::ListGridForProbe(bp_ini.probe_id);
    if (!dr.ok) return simcore::db::DbResult<int64_t>::Err(dr.error);

    simcore::RandSeedProbeResult result{.base_seed=(uint32_t)pr.value.neutral_seed};
    for (auto ds : dr.value) {
        auto family = (simcore::SeedFamily)ds.input.get_family();
        uint8_t x = 0, y = 0;
        switch (family) {
        case simcore::SeedFamily::Main: x = ds.input.main_x; y = ds.input.main_y; break;
        case simcore::SeedFamily::CStick: x = ds.input.c_x; y = ds.input.c_y; break;
        case simcore::SeedFamily::Triggers: x = ds.input.trig_l; y = ds.input.trig_r; break;
        }
        
        simcore::RandSeedProbeEntry entry{
            .family = family,
            .x = x,
            .y = y,
            .seed = (uint32_t)pr.value.neutral_seed + ds.seed_delta,
            .delta = ds.seed_delta,
            .ok = true
        };
        result.entries.push_back(entry);
    }
    
    auto samples = simcore::PlanJCTComboSamples(result, unique_ini.combo_attempts_per_target, unique_ini.combo_sampler_tries);

    for (auto s : samples.singletons) {
        for (auto d : dr.value) {
            if (s == d.input) DeltaSeedRepo::SetUnique(d.id);
        }
    }

    IniDoc t_ini{};
    bp_ini.set_section(t_ini);
    
    int64_t enqueued = 0;
    for (auto sample : samples.samples) {

        int32_t expected_delta = sample.target_delta;
        auto ds = JobSetsRepo::Create(
            "delta_set", simcore::PK_SeedProbe, std::nullopt, std::nullopt, std::nullopt, 
            "expected_delta=" + std::to_string(expected_delta), std::nullopt);

        if (!ds.ok) return simcore::db::DbResult<int64_t>::Err(ds.error);

        for (auto f : sample.frames) {
            const std::string frame_hex = f.to_frame_hex();

            JobIni job_ini{};
            job_ini.expected_delta = expected_delta;
            job_ini.delta_set_id = ds.value;
            job_ini.frame_hex = frame_hex;
            
            const std::string vm_kv_text = job_ini.append_section(t_ini).to_string_sorted();
            const std::string fp = fingerprint_for(bp_ini.probe_id, frame_hex, bp_ini.run_ms, bp_ini.vi_stall_ms);

            auto ins = JobsRepo::CreateOrGetByFingerprint(job_set_id, PK, PV, bp_ini.probe_id, fp, 0, vm_kv_text);
            if (!ins.ok) return simcore::db::DbResult<int64_t>::Err(ins.error);

            const int64_t job_id = ins.value;
            const std::string short_line = "probe=" + std::to_string(bp_ini.probe_id) + " input=" + frame_hex
                + " neutral=0 grid=0 unique=1"
                + " prio=0 run_ms=" + std::to_string(bp_ini.run_ms)
                + " vi=" + std::to_string(bp_ini.vi_stall_ms);
            JobEventsRepo::Append(job_id, "ENQUEUED", short_line);

            ++enqueued;
        }
    }

    JobSetsRepo::SetExpectedTotal(job_set_id, enqueued);

    IniKV cond;
    cond.add("type", "ALL_FINISHED");
    CleanupIni cleanup{};
    cleanup.set_section(ini);

    auto tr = TriggersRepo::AddForJobSet(job_set_id, simcore::PK_SeedProbe, cond.to_string_sorted(), ini.to_string_sorted());
    if (!tr.ok) return simcore::db::DbResult<int64_t>::Err(tr.error);

    if (bp_ini.auto_schedule_battle_run) {
        IniKV cond;
        cond.add("type", "ALL_FINISHED");

        auto tr = TriggersRepo::AddForJobSet(job_set_id, simcore::PK_BattleTurnRunner, cond.to_string_sorted(), blueprint_ini);
        if (!tr.ok) return simcore::db::DbResult<int64_t>::Err(tr.error);
    }
}

simcore::db::DbResult<int64_t> SeedProbeDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) {
    
    IniDoc ini = IniDoc::parse(blueprint_ini);

    BlueprintIni bp = BlueprintIni::from_section(ini);

    if (bp.savestate_id <= 0)
        return DbResult<int64_t>::Err({ DbErrorKind::InvalidArgument, 0, 
            "Require a value for SeedProbe.BlueprintIni.savestate_id"});

    auto ss = SavestateRepo::Get(bp.savestate_id);
    if (!ss.ok) return simcore::db::DbResult<int64_t>::Err(ss.error);
    if (!ss.value.has_value())
        return DbResult<int64_t>::Err({ DbErrorKind::NotFound, 0,
            "Savestate with id" + std::to_string(bp.savestate_id) + " not found" });

    if (bp.probe_id <= 0) {
        auto sp = SeedProbeRepo::Create(bp.savestate_id, PV);
        if (!sp.ok) return simcore::db::DbResult<int64_t>::Err(sp.error);

        bp.probe_id = sp.value;

        bp.set_section(ini);
    }

    simcore::db::DbResult<int64_t> enc;
    switch (bp.cur_phase) {
    case SeedProbePhase::Neutral: encode_neutral(job_set_id, ini.to_string_sorted()); break;
    case SeedProbePhase::Grid: encode_grid(job_set_id, ini.to_string_sorted()); break;
    case SeedProbePhase::Unique: encode_unique(job_set_id, ini.to_string_sorted()); break;
    default: return DbResult<int64_t>::Err({ DbErrorKind::InvalidState, 0, "Invalid SeedProbe Phase: " + std::to_string(bp.cur_phase) });
    }
    return enc;
}

DbResult<simcore::PSJob> SeedProbeDBCodec::decode_job_from_db(int64_t job_id) {
    auto jr = JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);
    if (!jr.value.vm_kv.has_value()) return DbResult<simcore::PSJob>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "vm_kv missing" });

    auto kv = IniDoc::parse(*jr.value.vm_kv).section_kv(IniDoc::GLOBAL);
    const uint32_t run_ms = kv.get_u32("run_ms", 0);
    const uint32_t vi_stall_ms = kv.get_u32("vi_stall_ms", 0);
    const std::string frame_hex = kv.get("frame_hex", "");
    if (frame_hex.empty()) return DbResult<simcore::PSJob>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "frame_hex missing" });

    auto bytes = hex_to_bytes(frame_hex);
    simcore::GCInputFrame frame{};
    if (bytes.size() == sizeof(simcore::GCInputFrame)) {
        std::memcpy(&frame, bytes.data(), sizeof(simcore::GCInputFrame));
    }
    else {
        return DbResult<simcore::PSJob>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "frame_hex wrong size" });
    }

    simcore::seedprobe::EncodeSpec spec{};
    spec.frame = frame;
    spec.run_ms = run_ms;
    spec.vi_stall_ms = vi_stall_ms;

    std::vector<uint8_t> payload;
    if (!simcore::seedprobe::encode_payload(spec, payload)) {
        return DbResult<simcore::PSJob>::Err({ simcore::db::DbErrorKind::Unknown, 0, "encode_payload failed" });
    }

    simcore::PSJob out{};
    out.payload = std::move(payload);
    return DbResult<simcore::PSJob>::Ok(std::move(out));
}

DbResult<void> SeedProbeDBCodec::encode_progress_into_db(int64_t job_id, const std::string& progress_line) {
    auto res = JobEventsRepo::Append(job_id, "PROGRESS", progress_line);
    if (!res.ok) return DbResult<void>(false);
    return DbResult<void>(true);
}

simcore::db::DbResult<void> SeedProbeDBCodec::encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) {
    
    IniDoc ini = IniDoc::parse(results_ini);
    ResultsIni res_ini = ResultsIni::from_section(ini);


    if (!ini.has_section(ResultsIni::SECTION_NAME))
        return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "No ResultsIni sent" });

    
    auto jr = JobsRepo::Get(job_id);
    if (!jr.ok) return simcore::db::DbResult<void>::Err(jr.error);
    if (!jr.value.vm_kv.has_value()) return simcore::db::DbResult<void>::Err({DbErrorKind::NotFound, 0, "No vm_kv was loaded into job"});

    auto job_vmkv_ini = IniDoc::parse(jr.value.vm_kv.value());

    if (!job_vmkv_ini.has_section(BlueprintIni::SECTION_NAME))
        return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "Job has no BlueprintIni" });
    
    if (!job_vmkv_ini.has_section(JobIni::SECTION_NAME))
        return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "Job has no JobIni" });
    
    BlueprintIni bp = BlueprintIni::from_section(job_vmkv_ini);
    JobIni job_ini = JobIni::from_section(job_vmkv_ini);

    const int64_t probe_id = bp.probe_id;
    const std::string frame_hex = job_ini.frame_hex;

    const uint64_t rng_seed = res_ini.rng_seed;
    JobEventsRepo::Append(job_id, "RESULTS", "rng_seed=" + std::to_string(rng_seed) + " input=" + frame_hex);

    if (!success) {
        JobsRepo::SetState(job_id, "FAILED");
        return simcore::db::DbResult<void>::Ok();
    }

    if (bp.cur_phase == SeedProbePhase::Neutral) {
        auto s = simcore::db::SeedProbeRepo::SetNeutralSeed(probe_id, rng_seed);
        if (!s.ok) return simcore::db::DbResult<void>::Err(s.error);
        auto j = JobsRepo::SetState(job_id, "SUCCEEDED");
        if (!j.ok) return simcore::db::DbResult<void>::Err(j.error);
        return simcore::db::DbResult<void>::Ok();
    }

    auto pr = simcore::db::SeedProbeRepo::Get(probe_id);
    if (!pr.ok) return simcore::db::DbResult<void>::Err(pr.error);
    
    const uint32_t neutral = static_cast<uint32_t>(pr.value.neutral_seed & 0xffffffffu);
    int32_t seed_delta = static_cast<int32_t>((int64_t)rng_seed - (int64_t)neutral);

    auto bytes = hex_to_bytes(frame_hex);
    simcore::GCInputFrame frame{};
    if (bytes.size() == sizeof(simcore::GCInputFrame)) {
        std::memcpy(&frame, bytes.data(), sizeof(simcore::GCInputFrame));
    }
    else {
        return simcore::db::DbResult<void>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "bad input frame" });
    }

    simcore::db::DeltaSeedRow row{};
    row.probe_id = probe_id;
    row.seed_delta = seed_delta;
    row.input = frame;
    row.complete = true;

    if (bp.cur_phase = SeedProbePhase::Unique) {

        const uint32_t expected_delta = job_ini.expected_delta;
        const std::string expected_tag = job_ini.unique_tag;
        const int64_t job_set_id = jr.value.job_set_id;

        auto win = simcore::db::SeedProbeWinnersRepo::TryInsertWinner(
            job_set_id,
            seed_delta,
            job_id,
            (std::optional<int32_t>{ expected_delta }),
            (expected_tag.empty() ? std::nullopt : std::optional<std::string>{ expected_tag }),
            std::optional<std::string>{ "UNIQUE" },
            frame_hex,
            std::nullopt
        );
        if (!win.ok) return simcore::db::DbResult<void>::Err(win.error);

        const bool is_winner = win.value.inserted;

        auto ins = simcore::db::DeltaSeedRepo::InsertOne(probe_id, row, /*is_grid=*/false, /*is_unique=*/is_winner);
        if (!ins.ok) return simcore::db::DbResult<void>::Err(ins.error);

        if (is_winner && expected_delta == seed_delta) {
            const int64_t delta_set_id = job_ini.delta_set_id;
            auto others = JobsRepo::GetQueuedByJobSet(delta_set_id);
            for (auto other : others.value) 
                JobsRepo::SetState(other.job_id, "SUPERSEDED");
        }

        JobsRepo::SetState(job_id, is_winner ? "SUCCEEDED_WINNER" : "SUCCEEDED_DUPLICATE");
        JobEventsRepo::Append(job_id, "RESULTS", std::string("dedupe=") + (is_winner ? "winner" : "duplicate") + " delta=" + std::to_string(seed_delta));
        return simcore::db::DbResult<void>::Ok();
    }

    auto ins = simcore::db::DeltaSeedRepo::InsertOne(probe_id, row, /*is_grid=*/true, /*is_unique=*/false);
    if (!ins.ok) return simcore::db::DbResult<void>::Err(ins.error);

    JobsRepo::SetState(job_id, "SUCCEEDED");
    return simcore::db::DbResult<void>::Ok();
}

simcore::db::DbResult<std::string> SeedProbeDBCodec::decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) {
    std::string out;

    if (job_id.has_value()) {
        auto evs = JobEventsRepo::ListByJobAndKind(*job_id, "PROGRESS");
        if (!evs.ok) return simcore::db::DbResult<std::string>::Err(evs.error);
        for (auto& e : evs.value) { out.append(e.payload.value_or("")); out.push_back('\n'); }
        return simcore::db::DbResult<std::string>::Ok(std::move(out));
    }

    if (job_set_id.has_value()) {
        auto evs = JobEventsRepo::ListByJobSetAndKind(*job_set_id, "PROGRESS");
        if (!evs.ok) return simcore::db::DbResult<std::string>::Err(evs.error);
        for (auto& e : evs.value) { out.append(e.payload.value_or("")); out.push_back('\n'); }

        auto wc = simcore::db::SeedProbeWinnersRepo::CountByJobSet(*job_set_id);
        if (wc.ok) { out.append("winners_found="); out.append(std::to_string(wc.value)); out.push_back('\n'); }
    }

    return simcore::db::DbResult<std::string>::Ok(std::move(out));
}

DbResult<std::string> SeedProbeDBCodec::decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) {
    std::string out;

    if (job_id.has_value()) {
        auto evs = JobEventsRepo::ListByJobAndKind(*job_id, "RESULTS");
        if (!evs.ok) return DbResult<std::string>::Err(evs.error);
        for (auto& e : evs.value) { out.append(e.payload.value_or("")); out.push_back('\n'); }
        return DbResult<std::string>::Ok(std::move(out));
    }

    if (job_set_id.has_value()) {
        auto evs = JobEventsRepo::ListByJobSetAndKind(*job_set_id, "RESULTS");
        if (!evs.ok) return DbResult<std::string>::Err(evs.error);
        for (auto& e : evs.value) { out.append(e.payload.value_or("")); out.push_back('\n'); }
    }

    return DbResult<std::string>::Ok(std::move(out));
}

DbResult<std::optional<int64_t>> SeedProbeDBCodec::get_required_savestate_id(int64_t job_id) {
    auto job = JobsRepo::Get(job_id);
    if (!job.ok) return DbResult<std::optional<int64_t>>::Err(job.error);

    auto probe = SeedProbeRepo::Get(job.value.program_ref_id);
    if (!probe.ok) return DbResult<std::optional<int64_t>>::Err(probe.error);

    return DbResult<std::optional<int64_t>>::Ok(std::optional(probe.value.savestate_id));
}

DbResult<simcore::PSInit> SeedProbeDBCodec::build_psinit_for_job(int64_t job_id) {
    auto job = JobsRepo::Get(job_id);
    if (!job.ok) return DbResult<simcore::PSInit>::Err(job.error);

    auto probe = SeedProbeRepo::Get(job.value.program_ref_id);
    if (!probe.ok) return DbResult<simcore::PSInit>::Err(probe.error);

    auto ss = SavestateRepo::Get(probe.value.savestate_id);
    if (!ss.ok) return DbResult<simcore::PSInit>::Err(ss.error);
    if (!ss.value.has_value()) return DbResult<simcore::PSInit>::Err({ DbErrorKind::NotFound, 0, "Savestate not found by id" });

    auto ss_path = ObjectStore::MaterializeToTemp(ss.value.value().object_ref_id);
    if (!ss_path.ok) return DbResult<simcore::PSInit>::Err(ss_path.error);

    simcore::PSInit init{};
    init.savestate_path = ss_path.value;
    init.default_timeout_ms = 10000;
    init.derived_buffer_type = simcore::DBuf::DK_None;
    return DbResult<simcore::PSInit>::Ok(init);
}

DbResult<std::string> SeedProbeDBCodec::build_results_ini_from_prresult(int64_t /*job_id*/, const simcore::PRResult& r) {
    ResultsIni results{};
    bool success = r.ps.ok ? true : false;
    results.w_err = r.ps.w_err;

    if (results.w_err == 0) r.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, results.dw_err);

    if (success) {
        r.ps.ctx.get(simcore::keys::seed::RNG_SEED, results.rng_seed);
        r.ps.ctx.get(simcore::keys::core::VI_FIRST, results.vi_start);
        r.ps.ctx.get(simcore::keys::core::VI_LAST, results.vi_end);
    }

    IniDoc ini;
    return DbResult<std::string>::Ok(results.append_section(ini).to_string_sorted());
}

DbResult<void> SeedProbeDBCodec::phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) {
    IniDoc ini = IniDoc::parse(action_args_ini);

    if (!ini.has_section(BlueprintIni::SECTION_NAME)) return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
        "Seedprobe Trigger has no BlueprintIni" });

    if (!ini.has_section(GridIni::SECTION_NAME)) return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
        "Seedprobe Trigger has no GridIni" });

    if (!ini.has_section(UniqueIni::SECTION_NAME)) return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
        "Seedprobe Trigger has no UniqueIni" });

        BlueprintIni bp = BlueprintIni::from_section(ini);

    if (ctx.prev_program_kind == (int)simcore::PK_TasMovie) {

        if (ctx.scope != "job") return DbResult<void>::Err({DbErrorKind::InvalidState, 0, "scope of trigger from TasMovie to SeedProbe should be a single job"});

        auto jb = JobsRepo::Get(ctx.prev_job_id);
        if (!jb.ok) return DbResult<void>::Err(jb.error);
        if (jb.value.program_kind != (int)simcore::PK_TasMovie) return DbResult<void>::Err({ DbErrorKind::InvalidState, 0, 
            "scope of trigger != previous program kind: " + std::to_string(jb.value.program_kind) });

        auto tm = TasMovieRepo::Get(jb.value.program_ref_id);
        if (!tm.ok) return DbResult<void>::Err(tm.error);

        if (!tm.value.output_savestate_id.has_value()) return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "no savestate id found for tas movie: " + std::to_string(jb.value.program_ref_id)});

        if (bp.cur_phase != SeedProbePhase::None )return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "invalid SeedProbePhase, should be 0, is: " + std::to_string(bp.cur_phase) });
        
        auto sp = SeedProbeRepo::Create(tm.value.output_savestate_id.value(), PV);
        if (!sp.ok) return DbResult<void>::Err(sp.error);

        bp.probe_id = sp.value;

        auto js = JobSetsRepo::Create("seed probe", simcore::PK_SeedProbe, std::nullopt, "SeedProbe", bp.probe_id, "phase=Neutral", 1);
        if (!js.ok) return DbResult<void>::Err(js.error);

        bp.cur_phase = SeedProbePhase::Neutral;
        bp.set_section(ini);
        auto e = encode_job_into_db(js.value, ini.to_string_sorted());
        if (!e.ok) return DbResult<void>::Err(e.error);

        return DbResult<void>::Ok();
    }

    if (ctx.prev_program_kind != (int)simcore::PK_SeedProbe) return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "previous program kind limited to TasMovie and SeedProbe, pk=" + std::to_string(ctx.prev_program_kind) });

    if (bp.cur_phase == SeedProbePhase::Neutral)
    {
        auto js = JobSetsRepo::Create("seed probe", simcore::PK_SeedProbe, std::nullopt, "SeedProbe", bp.probe_id, "phase=Grid", 1);
        if (!js.ok) return DbResult<void>::Err(js.error);

        bp.cur_phase = SeedProbePhase::Grid;
        bp.set_section(ini);

        auto e = encode_job_into_db(js.value, ini.to_string_sorted());
        if (!e.ok) return DbResult<void>::Err(e.error);

        return DbResult<void>::Ok();
    }
    else if (bp.cur_phase == SeedProbePhase::Grid) {
        auto js = JobSetsRepo::Create("seed probe", simcore::PK_SeedProbe, std::nullopt, "SeedProbe", bp.probe_id, "phase=Unique", 1);
        if (!js.ok) return DbResult<void>::Err(js.error);

        bp.cur_phase = SeedProbePhase::Unique;
        bp.set_section(ini);

        auto e = encode_job_into_db(js.value, ini.to_string_sorted());
        if (!e.ok) return DbResult<void>::Err(e.error);

        return DbResult<void>::Ok();
    }
    else if (bp.cur_phase == SeedProbePhase::Unique) {

        if (!ini.has_section(CleanupIni::SECTION_NAME)) return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "Seedprobe Trigger has no CleanupIni after a Unique run" });
        
        CleanupIni cleanup = CleanupIni::from_section(ini);

        if (cleanup.clear_winners) {
            (void)simcore::db::SeedProbeWinnersRepo::DeleteByJobSet(ctx.prev_job_set_id);
        }

        return DbResult<void>::Ok();
    }
    
    return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "Seedprobe Trigger can only be set for phases Neutral(1), Grid(2), and Unique(3), not: " + std::to_string(bp.cur_phase) });

}

DbResult<std::string> SeedProbeDBCodec::build_artifact_ini_from_db(int64_t job_id)
{
    ArtifactIniBuilder artifacts{};

    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<std::string>::Err(jr.error);

    if (!jr.value.vm_kv) return DbResult<std::string>::Err(jr.error);

    IniKV kv = IniDoc::parse(*jr.value.vm_kv).section_kv(BlueprintIni::SECTION_NAME);
    const uint64_t probe_id = kv.get_i64("probe_id", 0);

    auto sp = SeedProbeRepo::Get(probe_id);
    if (!sp.ok) return DbResult<std::string>::Err(sp.error);

    auto ss = SavestateRepo::Get(sp.value.savestate_id);
    if (!ss.ok) return DbResult<std::string>::Err(ss.error);
    if (!ss.value.has_value()) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0,
        "No Savestate found..." });

    artifacts.add_artifact("Savestate", ss.value.value().object_ref_id);

    return DbResult<std::string>::Ok(artifacts.to_string());
}
