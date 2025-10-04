#include "RngSeedDeltaMap.h"
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <optional>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <array>
#include <tuple>
#include <numeric>
#include <random>

#include "../Utils/Log.h"
#include "../Runner/Script/PhaseScriptVM.h"                // PSJob/PSResult
#include "Programs/SeedProbe/SeedProbeScript.h"     // MakeSeedProbeProgram
#include "Programs/SeedProbe/SeedProbePayload.h"
#include "../Runner/IPC/Wire.h"
#include "../Utils/MultiProgress.h"

using simcore::utils::MultiProgress;
using simcore::utils::MPBarSpec;

namespace simcore {

    

    static inline uint64_t mix64(uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33; return x;
    }
    static inline size_t coprime_stride(size_t n, uint64_t seed) {
        if (n <= 1) return 1;
        size_t cand = size_t(seed % (n - 1)) + 1;
        for (size_t k = 0; k < n; ++k) {
            size_t s = (cand + k) % n; if (s == 0) continue;
            if (std::gcd(s, n) == 1) return s;
        }
        return 1;
    }

    namespace detail {

        struct TripleKey { uint8_t jx, jy, cx, cy, tl, tr; };
        struct TKHash {
            size_t operator()(const TripleKey& k) const noexcept {
                uint64_t v = 0;
                std::memcpy(&v, &k, sizeof(k)); // sizeof(TripleKey)==6, safe pack to low bytes
                // mix64 is already defined above
                return static_cast<size_t>(mix64(v ^ 0xD1CEB00BULL));
            }
        };
        struct TKEq {
            bool operator()(const TripleKey& a, const TripleKey& b) const noexcept {
                return std::memcmp(&a, &b, sizeof(TripleKey)) == 0;
            }
        };

        struct TripleState {
            // deltas
            int32_t jd = 0, cd = 0, td = 0;
            // vectors realizing the deltas
            const std::vector<std::array<uint8_t, 2>>* J = nullptr;
            const std::vector<std::array<uint8_t, 2>>* C = nullptr;
            const std::vector<std::array<uint8_t, 2>>* T = nullptr;
            // sizes
            size_t nJ = 0, nC = 0, nT = 0;
            // indices (start offsets)
            size_t iJ = 0, iC = 0, iT = 0;
            // strides (coprime)
            size_t sJ = 1, sC = 1, sT = 1;
            // axis cursor: 0=J,1=C,2=T
            uint8_t axis = 0;
            // accounting
            uint64_t emitted = 0, total = 0;
            bool exhausted = false;

            inline void advance_axis() {
                for (int step = 0; step < 3; ++step) {
                    if (axis == 0) { if (nJ > 1) { iJ = (iJ + sJ) % nJ; axis = 1; break; } axis = 1; }
                    else if (axis == 1) { if (nC > 1) { iC = (iC + sC) % nC; axis = 2; break; } axis = 2; }
                    else { if (nT > 1) { iT = (iT + sT) % nT; axis = 0; break; } axis = 0; }
                }
            }
        };

        struct FairComboIterator {
            int32_t target = 0;
            std::vector<TripleState> triples; // all (jd,cd,td) that sum to target
            size_t cur = 0;

            bool next(std::array<uint8_t, 2>& J, std::array<uint8_t, 2>& C, std::array<uint8_t, 2>& T,
                std::unordered_set<TripleKey, TKHash, TKEq>& tried, uint32_t inner_try_cap)
            {
                if (triples.empty()) return false;
                const size_t N = triples.size();
                size_t checked = 0;

                while (checked < N) {
                    TripleState& ts = triples[cur];
                    if (!ts.exhausted) {
                        uint32_t inner = 0;
                        while (inner < inner_try_cap && !ts.exhausted) {
                            const auto& jv = (*ts.J)[ts.iJ];
                            const auto& cv = (*ts.C)[ts.iC];
                            const auto& tv = (*ts.T)[ts.iT];
                            TripleKey key{ jv[0], jv[1], cv[0], cv[1], tv[0], tv[1] };

                            ts.advance_axis();
                            ++ts.emitted;
                            if (ts.emitted >= ts.total) ts.exhausted = true;

                            ++inner;
                            if (tried.insert(key).second) {
                                J = jv; C = cv; T = tv;
                                cur = (cur + 1) % N;
                                return true;
                            }
                        }
                    }
                    cur = (cur + 1) % N;
                    ++checked;
                }
                return false;
            }
        };

        struct JCTComboPlanner {
            // inverse maps
            std::unordered_map<int32_t, std::vector<std::array<uint8_t, 2>>> j_map, c_map, t_map;
            // singletons (observed per-element deltas, plus 0)
            std::unordered_set<int32_t> singletons;
            // expected targets (non-singleton sums)
            std::vector<int32_t> targets;
            // per-target iterators
            std::unordered_map<int32_t, FairComboIterator> iters;
            // global dedupe across all targets
            std::unordered_set<TripleKey, TKHash, TKEq> tried;

            static inline GCInputFrame make_frame(std::array<uint8_t, 2> j, std::array<uint8_t, 2> c, std::array<uint8_t, 2> t) {
                GCInputFrame f{};
                f.main_x = j[0]; f.main_y = j[1];
                f.c_x = c[0]; f.c_y = c[1];
                f.trig_l = t[0]; f.trig_r = t[1];
                return f;
            }

            JCTComboPlanner(const RandSeedProbeResult& grid) {
                singletons.insert(0);
                for (const auto& e : grid.entries) {
                    singletons.insert(static_cast<int32_t>(e.delta));
                    if (e.family == SeedFamily::Main)       j_map[static_cast<int32_t>(e.delta)].push_back({ e.x, e.y });
                    else if (e.family == SeedFamily::CStick) c_map[static_cast<int32_t>(e.delta)].push_back({ e.x, e.y });
                    else if (e.family == SeedFamily::Triggers) t_map[static_cast<int32_t>(e.delta)].push_back({ e.x, e.y });
                }
                if (j_map.empty() || c_map.empty() || t_map.empty()) return;

                std::vector<int32_t> j_keys, c_keys, t_keys;
                j_keys.reserve(j_map.size()); c_keys.reserve(c_map.size()); t_keys.reserve(t_map.size());
                for (auto& kv : j_map) j_keys.push_back(kv.first);
                for (auto& kv : c_map) c_keys.push_back(kv.first);
                for (auto& kv : t_map) t_keys.push_back(kv.first);
                std::sort(j_keys.begin(), j_keys.end());
                std::sort(c_keys.begin(), c_keys.end());
                std::sort(t_keys.begin(), t_keys.end());

                {
                    std::unordered_set<int32_t> tset;
                    for (int32_t jd : j_keys)
                        for (int32_t cd : c_keys)
                            for (int32_t td : t_keys) {
                                const int32_t s = jd + cd + td;
                                if (!singletons.count(s)) tset.insert(s);
                            }
                    targets.assign(tset.begin(), tset.end());
                    std::sort(targets.begin(), targets.end());
                }

                for (int32_t t : targets) {
                    FairComboIterator it{};
                    it.target = t;

                    for (int32_t jd : j_keys) {
                        for (int32_t cd : c_keys) {
                            const int32_t need = t - jd - cd;
                            auto itT = t_map.find(need);
                            if (itT == t_map.end()) continue;

                            TripleState ts{};
                            ts.jd = jd; ts.cd = cd; ts.td = need;
                            ts.J = &j_map[jd]; ts.C = &c_map[cd]; ts.T = &itT->second;
                            ts.nJ = ts.J->size(); ts.nC = ts.C->size(); ts.nT = ts.T->size();
                            ts.total = uint64_t(ts.nJ) * uint64_t(ts.nC) * uint64_t(ts.nT);
                            if (ts.total == 0) { ts.exhausted = true; }

                            const uint64_t seed = mix64((uint64_t(uint32_t(t)) << 32) ^
                                (uint64_t(uint32_t(jd)) << 21) ^
                                (uint64_t(uint32_t(cd)) << 10) ^
                                uint64_t(uint32_t(need)));

                            if (ts.nJ > 0) { ts.iJ = size_t(seed % ts.nJ); ts.sJ = coprime_stride(ts.nJ, mix64(seed ^ 0x9E3779B185EBCA87ULL)); }
                            if (ts.nC > 0) { ts.iC = size_t((seed >> 7) % ts.nC); ts.sC = coprime_stride(ts.nC, mix64(seed ^ 0xC2B2AE3D27D4EB4FULL)); }
                            if (ts.nT > 0) { ts.iT = size_t((seed >> 13) % ts.nT); ts.sT = coprime_stride(ts.nT, mix64(seed ^ 0x165667B19E3779F9ULL)); }

                            ts.axis = uint8_t(seed & 0x03) % 3;

                            it.triples.push_back(ts);
                        }
                    }
                    iters.emplace(t, std::move(it));
                }
            }

            inline std::vector<int32_t> singletons_sorted() const {
                std::vector<int32_t> v; v.reserve(singletons.size());
                for (auto d : singletons) v.push_back(d);
                std::sort(v.begin(), v.end());
                return v;
            }

            inline const std::vector<int32_t>& expected() const { return targets; }

            bool next_for_target(int32_t target, uint32_t sampler_tries, GCInputFrame& out) {
                auto it = iters.find(target);
                if (it == iters.end()) return false;
                std::array<uint8_t, 2> J{}, C{}, T{};
                if (!it->second.next(J, C, T, tried, sampler_tries)) return false;
                out = make_frame(J, C, T);
                return true;
            }
        };

    }

    static GCInputFrame neutral_frame() { return GCInputFrame{}; }

    static std::vector<uint8_t> linspace_u8(int n, int minv, int maxv) {
        std::vector<uint8_t> v; v.reserve((size_t)n);
        if (n <= 1) { v.push_back(uint8_t((minv + maxv) / 2)); return v; }
        const float step = float(maxv - minv) / float(n - 1);
        for (int i = 0; i < n; ++i) v.push_back(uint8_t(std::clamp(int(minv + i * step), 0, 255)));
        return v;
    }

    static std::vector<GCInputFrame> build_grid_main(int n, int minv, int maxv) {
        auto xs = linspace_u8(n, minv, maxv);
        auto ys = linspace_u8(n, minv, maxv);
        std::vector<GCInputFrame> out; out.reserve((size_t)n * (size_t)n);
        for (auto y : ys) for (auto x : xs) { GCInputFrame f{}; f.main_x = x; f.main_y = y; out.push_back(f); }
        return out;
    }

    static std::vector<GCInputFrame> build_grid_cstick(int n, int minv, int maxv) {
        auto xs = linspace_u8(n, minv, maxv);
        auto ys = linspace_u8(n, minv, maxv);
        std::vector<GCInputFrame> out; out.reserve((size_t)n * (size_t)n);
        for (auto y : ys) for (auto x : xs) { GCInputFrame f{}; f.c_x = x; f.c_y = y; out.push_back(f); }
        return out;
    }

    static std::vector<GCInputFrame> build_grid_trig(int n, int minv, int maxv, bool cap_top) {
        auto ls = linspace_u8(n, minv, maxv);
        auto rs = linspace_u8(n, minv, maxv);
        if (cap_top) {
            if (!ls.empty()) ls.back() = std::min<uint8_t>(ls.back(), 0xFF);
            if (!rs.empty()) rs.back() = std::min<uint8_t>(rs.back(), 0xFF);
        }
        std::vector<GCInputFrame> out; out.reserve((size_t)n * (size_t)n);
        for (auto r : rs) for (auto l : ls) { GCInputFrame f{}; f.trig_l = l; f.trig_r = r; out.push_back(f); }
        return out;
    }

    static inline long long signed_delta(uint32_t a, uint32_t b) {
        return (long long)(int32_t)a - (long long)(int32_t)b;
    }

    static std::string make_label(const char* title, uint8_t x, uint8_t y) {
        char buf[64]; std::snprintf(buf, sizeof(buf), "%s(%02X,%02X)", title, int(x), int(y));
        return std::string(buf);
    }

    RandSeedProbeResult RunRngSeedDeltaMap(ParallelPhaseScriptRunner& runner, const RngSeedDeltaArgs& args)
    {
        PSInit init{};
        init.savestate_path = args.savestate_path;      // keep using your existing savestate for this phase
        init.default_timeout_ms = args.run_timeout_ms;

        SCLOGI("[seedmap] Setting program ...");

        // No special INIT program for seed probe; main program is SeedProbe
        if (!runner.set_program(/*init_kind=*/PK_None, /*main_kind=*/PK_SeedProbe, init)) {
            SCLOGE("Failed to set program on workers.");
            return {};
        }

        SCLOGI("[seedmap] Activating program ...");

        // No init-once step for seed probe; go straight to main
        if (!runner.activate_main()) {
            SCLOGE("Failed to activate main program.");
            return {};
        }

        SCLOGI("[seedmap] Building Jobs ...");

        RandSeedProbeResult out{};

        std::vector<std::pair<SeedFamily, std::vector<GCInputFrame>>> batches;
        batches.emplace_back(SeedFamily::Neutral, std::vector<GCInputFrame>{ neutral_frame() });
        batches.emplace_back(SeedFamily::Main, build_grid_main(args.samples_per_axis, args.min_value, args.max_value));
        batches.emplace_back(SeedFamily::CStick, build_grid_cstick(args.samples_per_axis, args.min_value, args.max_value));
        batches.emplace_back(SeedFamily::Triggers, build_grid_trig(args.samples_per_axis, args.min_value, args.max_value, args.cap_trigger_top));

        std::vector<MPBarSpec> specs;
        specs.push_back({ "Neutral", 1 });
        specs.push_back({ "Main",    (uint64_t)args.samples_per_axis * args.samples_per_axis });
        specs.push_back({ "CStick",  (uint64_t)args.samples_per_axis * args.samples_per_axis });
        specs.push_back({ "Triggers",(uint64_t)args.samples_per_axis * args.samples_per_axis });


        SCLOGI("[seedmap] Sending Jobs ...");

        MultiProgress::Options mpopt;
        mpopt.use_stdout = true;
        mpopt.use_vt = true;
        mpopt.bar_width = 40;

        MultiProgress mp;
        mp.init(specs, mpopt);
        mp.start();

        // When building and dispatching each batch, keep the family index:
        auto family_index = [&](SeedFamily fam)->size_t {
            switch (fam) {
            case SeedFamily::Neutral:  return 0;
            case SeedFamily::Main:     return 1;
            case SeedFamily::CStick:   return 2;
            case SeedFamily::Triggers: return 3;
            }
            return 0;
            };

        for (auto& [fam, inputs] : batches) {
            
            const size_t fi = family_index(fam);

            // Prepare jobs
            std::vector<PSJob> jobs;
            jobs.reserve(inputs.size());

            // We'll keep a lookup from job_id -> GCInputFrame for labeling later
            std::unordered_map<uint64_t, GCInputFrame> lookup;
            lookup.reserve(inputs.size());

            for (const auto& f : inputs) {
                simcore::seedprobe::EncodeSpec spec{};
                spec.frame = f;
                spec.run_ms = args.run_timeout_ms; // per-job timeout using VMCoreKeys (optional: rely on PSInit otherwise)
                spec.vi_stall_ms = 0;                   // you can set a small stall window if you like (e.g., 500-2000ms)

                PSJob j{};
                simcore::seedprobe::encode_payload(spec, j.payload);

                const uint64_t id = runner.submit(j);
                lookup.emplace(id, f);
            }

            // Drain results for this batch
            size_t done = 0, total = inputs.size();
            while (done < total) {
                PRResult r{};
                if (runner.try_get_result(r)) {
                    if (!r.ps.ok) 
                    {
                        mp.finish();
                        uint32_t run_outcome;
                        r.ps.ctx.get(keys::core::DW_RUN_OUTCOME_CODE, run_outcome);
                        SCLOGE("[seedmap] Result recieved not okay. w_err:%d ps_err:%d", r.ps.w_err, run_outcome);
                        return out;
                    }

                    ++done;
                    mp.tick(fi);

                    // Seed comes back in the ProcessWorker reader under key "seed"
                    // (see ProcessWorker::reader_thread MSG_RESULT handling) :contentReference[oaicite:1]{index=1}
                    uint32_t seed;
                    r.ps.ctx.get(keys::seed::RNG_SEED, seed);

                    if (fam == SeedFamily::Neutral)  out.base_seed = seed;


                    const auto& in = lookup.find(r.job_id)->second;
                    uint8_t vx = 0, vy = 0;
                    const char* title = "";
                    switch (fam) {
                    case SeedFamily::Main:     vx = in.main_x;   vy = in.main_y;   title = "JStick";   break;
                    case SeedFamily::CStick:   vx = in.c_x;      vy = in.c_y;      title = "CStick";   break;
                    case SeedFamily::Triggers: vx = in.trig_l;   vy = in.trig_r;   title = "Triggers"; break;
                    case SeedFamily::Neutral:  vx = 0;           vy = 0;           title = "Neutral";  break;
                    }

                    out.entries.push_back(RandSeedProbeEntry{
                        args.samples_per_axis, fam, vx, vy,
                        seed,
                        signed_delta(seed, out.base_seed),
                        r.ps.ok,
                        make_label(title, uint8_t(vx), uint8_t(vy))
                        });

                    uint32_t last_pc;
                    r.ps.ctx.get(keys::core::RUN_HIT_PC, last_pc);
                    SCLOGT("[Result] job=%llu worker=%zu accepted=%d ok=%d pc=%08X seed=0x%08X",
                        (unsigned long long)r.job_id, r.worker_id,
                        int(r.accepted), int(r.ps.ok), last_pc, seed);
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
            }
        }

        mp.finish();

        std::sort(out.entries.begin(), out.entries.end(), [](const RandSeedProbeEntry& a, const RandSeedProbeEntry& b) {
            if (a.family != b.family) return a.family < b.family;
            if (a.y == b.y) return a.x < b.x;
            return a.y < b.y;
            });

        return out;
    }

    static GCInputFrame make_singleton_frame(SeedFamily fam, uint8_t x, uint8_t y)
    {
        GCInputFrame f{};
        switch (fam) {
        case SeedFamily::Main:     f.main_x = x; f.main_y = y; break;
        case SeedFamily::CStick:   f.c_x = x; f.c_y = y; break;
        case SeedFamily::Triggers: f.trig_l = x; f.trig_r = y; break;
        case SeedFamily::Neutral:  default: /* all neutral */   break;
        }
        return f;
    }



    RandSeedComboResult RunFindSeedDeltaCombos(ParallelPhaseScriptRunner& runner,
        const RngSeedDeltaArgs& args,
        const RandSeedProbeResult& grid)
    {
        PSInit init{};
        init.savestate_path = args.savestate_path;
        init.default_timeout_ms = args.run_timeout_ms;

        SCLOGI("[seedcombos] Setting program ...");
        if (!runner.set_program(/*init_kind=*/PK_None, /*main_kind=*/PK_SeedProbe, init)) {
            SCLOGE("Failed to set program on workers.");
            return {};
        }
        SCLOGI("[seedcombos] Activating program ...");
        if (!runner.activate_main()) {
            SCLOGE("Failed to activate main program.");
            return {};
        }

        RandSeedComboResult out{};
        out.base_seed = grid.base_seed;

        // Pre-catalog: ensure we keep one representative for every observed probe delta
        std::unordered_set<int32_t> have_catalog;
        have_catalog.insert(0);
        for (const auto& e : grid.entries) {
            const int32_t d = static_cast<int32_t>(e.delta);
            if (have_catalog.count(d)) continue;
            if (!e.ok) continue;

            GCInputFrame f = make_singleton_frame(e.family, e.x, e.y);
            RandSeedComboEntry ce{};
            ce.input = f;
            ce.seed = e.seed;
            ce.delta = e.delta;
            ce.ok = true;
            ce.label = e.label;

            out.entries.push_back(ce);
            have_catalog.insert(d);
        }

        // Build planner (maps, singletons, expected non-singleton targets, fair iterators)
        detail::JCTComboPlanner planner(grid);
        const auto& targets = planner.expected();
        if (targets.empty()) {
            SCLOGW("[seedcombos] No non-singleton combo targets; aborting.");
            return out;
        }

        struct TargetState {
            int32_t t = 0;
            bool satisfied = false;
            uint32_t attempts = 0;
            uint32_t in_flight = 0;
        };

        std::vector<TargetState> states;
        states.reserve(targets.size());
        for (int32_t t : targets) {
            TargetState S{}; S.t = t;
            states.push_back(S);
        }

        struct JobMeta { int32_t target; GCInputFrame frame; };
        std::unordered_map<uint64_t, JobMeta> jobs; jobs.reserve(2048);

        auto enqueue_for = [&](TargetState& S)->bool {
            if (S.satisfied) return false;
            if (S.attempts >= args.combos_attempts_per_target) return false;

            GCInputFrame f{};
            if (!planner.next_for_target(S.t, args.combos_sampler_tries, f)) {
                // No more unique concrete combos for this target
                return false;
            }

            seedprobe::EncodeSpec spec{};
            spec.frame = f;
            spec.run_ms = args.run_timeout_ms;
            spec.vi_stall_ms = 0;

            PSJob j{};
            seedprobe::encode_payload(spec, j.payload);

            const uint64_t id = runner.submit(j);
            jobs.emplace(id, JobMeta{ S.t, f });

            ++S.attempts;
            ++S.in_flight;

            SCLOGT("[seedcombos] submit t=%d (attempt %u) J(%02X,%02X) C(%02X,%02X) T(%02X,%02X)",
                S.t, S.attempts,
                int(f.main_x), int(f.main_y), int(f.c_x), int(f.c_y), int(f.trig_l), int(f.trig_r));
            return true;
            };

        auto all_satisfied = [&]() { for (auto& s : states) if (!s.satisfied) return false; return true; };

        // Initial wave: one per target
        for (auto& S : states) (void)enqueue_for(S);

        const uint32_t W = std::max<uint32_t>(1, runner.worker_count());
        auto inflight = [&]() { uint32_t s = 0; for (auto& t : states) s += t.in_flight; return s; };

        while (!all_satisfied() && inflight() > 0) {

            PRResult r{};
            if (!runner.try_get_result(r)) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }

            auto it = jobs.find(r.job_id);
            if (it == jobs.end()) { SCLOGW("[seedcombos] Unknown job id=%llu", (unsigned long long)r.job_id); continue; }

            // mark inflight--
            auto* S_ptr = [&]()->TargetState* {
                for (auto& s : states) if (s.t == it->second.target) return &s; return nullptr;
                }();
            if (S_ptr && S_ptr->in_flight) --S_ptr->in_flight;

            if (!r.ps.ok) {
                uint32_t oc = 0; r.ps.ctx.get(keys::core::DW_RUN_OUTCOME_CODE, oc);
                SCLOGE("[seedcombos] VM error t=%d w_err=%d ps=%u", it->second.target, r.ps.w_err, oc);
                jobs.erase(it);
                continue;
            }

            uint32_t seed = 0; r.ps.ctx.get(keys::seed::RNG_SEED, seed);
            const long long obs = signed_delta(seed, out.base_seed);

            if (!have_catalog.count(int32_t(obs))) {
                RandSeedComboEntry ce{}; ce.input = it->second.frame; ce.seed = seed; ce.delta = obs; ce.ok = true;
                char lab[64]; std::snprintf(lab, sizeof(lab), "obs delta=%lld", obs); ce.label = lab;
                out.entries.push_back(ce);
                have_catalog.insert(int32_t(obs));
                SCLOGI("[seedcombos] Catalog add delta=%lld (job=%llu worker=%zu)", obs, (unsigned long long)r.job_id, r.worker_id);
            }

            if (S_ptr && obs == S_ptr->t) {
                S_ptr->satisfied = true;
                SCLOGI("[seedcombos] Satisfied target delta=%d", S_ptr->t);
            }
            if (S_ptr && !S_ptr->satisfied && S_ptr->attempts >= args.combos_attempts_per_target) {
                SCLOGT("[seedcombos] Target delta=%d hit cap (%u)", S_ptr->t, S_ptr->attempts);
            }

            jobs.erase(it);

            // Top up to W using unsatisfied targets
            while (inflight() < W) {
                bool any = false;
                for (auto& S : states) {
                    if (S.satisfied) continue;
                    if (S.attempts >= args.combos_attempts_per_target) continue;
                    if (enqueue_for(S)) { any = true; break; }
                }
                if (!any) break;
            }
        }

        for (auto s : states) {
            if (!s.satisfied) SCLOGI("[seedcombos] delta not found:%d tries=%d", s.t, s.attempts);
        }

        return out;
    }


    // === ADD: helper implementation ===
    JCTComboSamples PlanJCTComboSamples(
        const RandSeedProbeResult& grid,
        uint32_t attempts_per_target,
        uint32_t sampler_tries)
    {
        JCTComboSamples out{};

        // Build the singleton frame list (one representative per observed delta).
        // Always include the neutral frame first.
        std::unordered_set<int32_t> seen_singleton_deltas;
        seen_singleton_deltas.insert(0);
        out.singletons.push_back(neutral_frame());

        for (const auto& e : grid.entries) {
            if (!e.ok) continue;
            const int32_t d = static_cast<int32_t>(e.delta);
            if (seen_singleton_deltas.insert(d).second) {
                // Pick a representative frame for this singleton delta
                out.singletons.push_back(make_singleton_frame(e.family, e.x, e.y));
            }
        }

        detail::JCTComboPlanner planner(grid);
        out.expected = planner.expected();

        out.samples.reserve(out.expected.size());
        for (int32_t t : out.expected) {
            ComboSampleSet set{}; set.target_delta = t;
            for (uint32_t i = 0; i < attempts_per_target; ++i) {
                GCInputFrame f{};
                if (!planner.next_for_target(t, sampler_tries, f)) break;
                set.frames.push_back(f);
            }
            out.samples.push_back(std::move(set));
        }
        return out;
    }

} // namespace simcore
