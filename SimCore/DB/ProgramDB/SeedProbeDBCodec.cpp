// SimCore/DB/ProgramDB/SeedProbeDBCodec.cpp
#include "SeedProbeDBCodec.h"
#include "IniKV.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../Scheduling/SeedProbeWinnersRepo.h"
#include "../SeedProbeRepo.h"
#include "../DeltaSeedRepo.h"
#include "../../Phases/Programs/SeedProbe/SeedProbePayload.h"
#include "../../Runner/IPC/Wire.h"

#include <cctype>
#include <sstream>
#include <iomanip>
#include <cstring>

using simcore::db::DbResult;
using simcore::db::JobRow;
using simcore::db::JobSetsRepo;
using simcore::db::JobsRepo;
using simcore::db::JobEventsRepo;
using simcore::db::SeedProbeRepo;
using simcore::db::SeedProbeRow;

static constexpr int PK = simcore::PK_SeedProbe;
static constexpr int PV = 1;

static inline std::string trim(const std::string& s) {
    size_t a = 0; while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size(); while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static inline std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    std::string h; h.reserve(hex.size());
    for (char c : hex) if (!std::isspace(static_cast<unsigned char>(c))) h.push_back(c);
    if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) h = h.substr(2);
    if (h.size() % 2) h.insert(h.begin(), '0');
    std::vector<uint8_t> out; out.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        unsigned int byte = 0;
        std::stringstream ss; ss << std::hex << h.substr(i, 2);
        ss >> byte;
        out.push_back(static_cast<uint8_t>(byte));
    }
    return out;
}

static inline std::string bytes_to_hex(const uint8_t* data, size_t n) {
    std::ostringstream oss;
    for (size_t i = 0; i < n; ++i) { oss << std::hex << std::setw(2) << std::setfill('0') << (unsigned)(data[i]); }
    return oss.str();
}

static inline std::string fingerprint_for(int64_t probe_id, const std::string& frame_hex, uint32_t run_ms, uint32_t vi_stall_ms) {
    std::ostringstream oss;
    oss << "PK=" << PK << ";PV=" << PV << ";probe_id=" << probe_id
        << ";frame=" << frame_hex << ";run_ms=" << run_ms << ";vi=" << vi_stall_ms;
    return oss.str();
}

simcore::db::DbResult<int64_t> SeedProbeDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) {
    auto kv = IniKV::parse(blueprint_ini);
    const int64_t probe_id = kv.get_i64("probe_id", -1);
    const uint32_t run_ms = kv.get_u32("run_ms", 0);
    const uint32_t vi_stall_ms = kv.get_u32("vi_stall_ms", 0);
    const bool is_neutral = kv.get_bool("is_neutral", false);
    const bool is_grid = kv.get_bool("is_grid", false);
    const bool is_unique = kv.get_bool("is_unique", false);

    if (probe_id <= 0) return simcore::db::DbResult<int64_t>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "probe_id required" });

    auto inputs = kv.get_list("inputs");
    if (inputs.empty()) return simcore::db::DbResult<int64_t>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "inputs empty" });

    if (is_neutral) {
        if (inputs.size() != 1) {
            return simcore::db::DbResult<int64_t>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "neutral job requires exactly one input" });
        }
    }
    else {
        auto pr = simcore::db::SeedProbeRepo::Get(probe_id);
        if (!pr.ok) return simcore::db::DbResult<int64_t>::Err(pr.error);
        if (pr.value.neutral_seed == 0) {
            return simcore::db::DbResult<int64_t>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "neutral_seed not set; enqueue neutral job first" });
        }
    }

    auto expected_delta_strs = kv.get_list("expected_deltas");
    auto expected_tag_strs = kv.get_list("expected_tags");

    int64_t enqueued = 0;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const std::string frame_hex = trim(inputs[i]);

        IniKV vmkv;
        vmkv.add("probe_id", std::to_string(probe_id));
        vmkv.add("frame_hex", frame_hex);
        vmkv.add("run_ms", std::to_string(run_ms));
        vmkv.add("vi_stall_ms", std::to_string(vi_stall_ms));
        if (is_neutral && i == 0) vmkv.add("is_neutral", "1");
        if (is_grid) vmkv.add("is_grid", "1");
        if (is_unique) {
            vmkv.add("is_unique", "1");
            if (i < expected_delta_strs.size()) vmkv.add("expected_delta_i32", expected_delta_strs[i]);
            if (i < expected_tag_strs.size())   vmkv.add("expected_tag", expected_tag_strs[i]);
        }

        const std::string vm_kv_text = vmkv.to_string_sorted();
        const std::string fp = fingerprint_for(probe_id, frame_hex, run_ms, vi_stall_ms);

        auto ins = JobsRepo::CreateOrGetByFingerprint(job_set_id, PK, PV, probe_id, fp, 0, vm_kv_text);
        if (!ins.ok) return simcore::db::DbResult<int64_t>::Err(ins.error);

        const int64_t job_id = ins.value;
        const std::string short_line = "probe=" + std::to_string(probe_id) + " input=" + frame_hex
            + " neutral=" + std::string((is_neutral && i == 0) ? "1" : "0")
            + " grid=" + std::string(is_grid ? "1" : "0")
            + " unique=" + std::string(is_unique ? "1" : "0")
            + " prio=0 run_ms=" + std::to_string(run_ms)
            + " vi=" + std::to_string(vi_stall_ms);
        JobEventsRepo::Append(job_id, "ENQUEUED", short_line);

        ++enqueued;
    }

    return simcore::db::DbResult<int64_t>::Ok(enqueued);
}

DbResult<simcore::PSJob> SeedProbeDBCodec::decode_job_from_db(int64_t job_id) {
    auto jr = JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);
    if (!jr.value.vm_kv.has_value()) return DbResult<simcore::PSJob>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "vm_kv missing" });

    auto kv = IniKV::parse(*jr.value.vm_kv);
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
    auto jr = JobsRepo::Get(job_id);
    if (!jr.ok) return simcore::db::DbResult<void>::Err(jr.error);
    const int64_t probe_id = IniKV::parse(jr.value.vm_kv.value_or("")).get_i64("probe_id", -1);

    auto kv = IniKV::parse(results_ini);
    const uint64_t rng_seed = static_cast<uint64_t>(kv.get_i64("rng_seed", -1));
    const std::string frame_hex = kv.get("frame_hex", "");
    JobEventsRepo::Append(job_id, "RESULTS", "rng_seed=" + std::to_string(rng_seed) + " input=" + frame_hex);

    if (!success) {
        JobsRepo::SetState(job_id, "FAILED");
        return simcore::db::DbResult<void>::Ok();
    }

    auto vmkv = IniKV::parse(jr.value.vm_kv.value_or(""));
    const bool is_neutral = vmkv.get_bool("is_neutral", false);
    if (is_neutral) {
        auto s = simcore::db::SeedProbeRepo::SetNeutralSeed(probe_id, rng_seed);
        if (!s.ok) return simcore::db::DbResult<void>::Err(s.error);
        JobsRepo::SetState(job_id, "SUCCEEDED");
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

    const bool is_unique = vmkv.get_bool("is_unique", false);
    if (is_unique) {
        const std::string expected_delta_s = vmkv.get("expected_delta_i32", "");
        const std::string expected_tag = vmkv.get("expected_tag", "");
        const int64_t job_set_id = jr.value.job_set_id;

        auto win = simcore::db::SeedProbeWinnersRepo::TryInsertWinner(
            job_set_id,
            seed_delta,
            job_id,
            (expected_delta_s.empty() ? std::optional<int32_t>{} : std::optional<int32_t>{ static_cast<int32_t>(std::stoi(expected_delta_s)) }),
            (expected_tag.empty() ? std::nullopt : std::optional<std::string>{ expected_tag }),
            std::optional<std::string>{ "UNIQUE" },
            frame_hex,
            std::nullopt
        );
        if (!win.ok) return simcore::db::DbResult<void>::Err(win.error);

        const bool is_winner = win.value.inserted;

        auto ins = simcore::db::DeltaSeedRepo::InsertOne(probe_id, row, /*is_grid=*/false, /*is_unique=*/is_winner);
        if (!ins.ok) return simcore::db::DbResult<void>::Err(ins.error);

        JobsRepo::SetState(job_id, is_winner ? "SUCCEEDED_WINNER" : "SUCCEEDED_DUPLICATE");
        JobEventsRepo::Append(job_id, "RESULTS", std::string("dedupe=") + (is_winner ? "winner" : "duplicate") + " delta=" + std::to_string(seed_delta));
        return simcore::db::DbResult<void>::Ok();
    }

    std::vector<simcore::db::DeltaSeedRow> rows{ row };
    auto ins = simcore::db::DeltaSeedRepo::BulkQueue(probe_id, rows);
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

DbResult<std::optional<int64_t>> SeedProbeDBCodec::get_required_savestate_id(int64_t /*job_id*/) {
    return DbResult<std::optional<int64_t>>::Ok(std::nullopt);
}

DbResult<simcore::PSInit> SeedProbeDBCodec::build_psinit_for_job(int64_t /*job_id*/) {
    simcore::PSInit init{};
    init.savestate_path.clear();
    init.default_timeout_ms = 10000;
    init.derived_buffer_type = simcore::DBuf::DK_None;
    return DbResult<simcore::PSInit>::Ok(init);
}

DbResult<std::string> SeedProbeDBCodec::build_results_ini_from_prresult(int64_t /*job_id*/, const simcore::PRResult& r) {
    IniKV kv;
    kv.add("ok", r.ps.ok ? "1" : "0");
    kv.add("w_err", std::to_string(static_cast<unsigned>(r.ps.w_err)));
    return DbResult<std::string>::Ok(kv.to_string_sorted());
}
