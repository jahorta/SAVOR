#include "BattleExplorer.h"
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <functional>
#include <optional>
#include "../Core/Input/SoaBattle/ActionPlanSerializer.h"
#include "../Runner/IPC/Wire.h"
#include "../Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../Phases/Programs/BattleContext/BattleContextPayload.h"
#include "../Phases/Programs/BattleRunner/BattleRunnerPayload.h"

namespace simcore::battleexplorer {

    using soa::battle::actions::ActionPlan;
    using soa::battle::actions::TurnPlan;
    using soa::battle::actions::TurnPlanSpec;
    using soa::battle::actions::BattlePath;

    // --- Combinatorics helpers ---

    static uint64_t binom(uint64_t n, uint64_t k) {
        if (k > n) return 0;
        if (k == 0 || k == n) return 1;
        if (k > n - k) k = n - k;
        uint64_t res = 1;
        for (uint64_t i = 1; i <= k; ++i) {
            res = (res * (n - k + i)) / i;
        }
        return static_cast<uint64_t>(res);
    }

    std::vector<std::vector<uint32_t>>
        BattleExplorer::enumerate_fakeattack_vectors(std::size_t N, uint32_t B) {
        // Enumerate all non-negative integer N-tuples (f1..fN) with sum <= B.
        // Simple recursive stars-and-bars with sum cap; N is typically small.
        std::vector<std::vector<uint32_t>> out;
        std::vector<uint32_t> cur(N, 0);
        std::function<void(std::size_t, uint32_t)> dfs = [&](std::size_t idx, uint32_t remain) {
            if (idx + 1 == N) {
                cur[idx] = remain;
                out.push_back(cur);
                return;
            }
            for (uint32_t v = 0; v <= remain; ++v) {
                cur[idx] = v;
                dfs(idx + 1, remain - v);
            }
            };
        if (N == 0) return out;
        for (uint32_t s = 0; s <= B; ++s) {
            dfs(0, s);
        }
        return out;
    }

    // --- Public API ---

    BattleExplorer::BattleExplorer(std::string savestate_path)
    {
        m_savestate_path = savestate_path;
    }

    soa::battle::ctx::BattleContext BattleExplorer::gather_context(ParallelPhaseScriptRunner& runner) {
        // 1) Broadcast program: BattleContext probe (no "main" loop needed but we can set both)
        PSInit init{};
        init.savestate_path = m_savestate_path;
        init.default_timeout_ms = 10000; // or your default
        
        SCLOGI("[runner] setting and activating context program.");
        // Worker program kinds: PK_BattleContextProbe
        if (!runner.set_program(PK_BattleContextProbe, PK_BattleContextProbe, init)) {
            throw std::runtime_error("BattleExplorer.gather_context: set_program failed");
        }
        if (!runner.activate_main()) {
            throw std::runtime_error("BattleExplorer.gather_context: activate_main failed");
        }
        
        SCLOGI("[runner] submitting context job.");
        // 2) Submit an empty job; the probe script reads memory and emits the result in PSContext.
        PSJob job{};
        std::vector<uint8_t> payload;
        phase::battle::ctx::encode_payload({ 100000, 2000 }, payload);
        job.payload = std::move(payload);
        const uint64_t jid = runner.submit(job);

        // 3) Drain results until our job arrives (runner is multi-worker)
        PRResult rr{};
        soa::battle::ctx::BattleContext bc{};
        for (;;) {
            if (!runner.try_get_result(rr)) {
                // busy-spin very lightly; in a real UI loop you might pump events / sleep(1)
                continue;
            }
            if (rr.job_id != jid) {
                // Some other job in the queue - ignore it here; caller may have a global collector
                continue;
            }
            if (!rr.accepted || !rr.ps.ok) {
                throw std::runtime_error("BattleExplorer.gather_context: worker reported failure");
            }

            // 4) Decode from PSContext.
            //
            //    Below, we handle a blob in std::string under keys::battle::CTX_BLOB (replace with your real key).
            //    If your script uses per-field keys instead, replace this block with those reads.
            {
                std::string blob;
                constexpr auto CTX_BLOB_KEY = simcore::keys::battle::CTX_BLOB;
                if (!rr.ps.ctx.get(CTX_BLOB_KEY, blob)) {
                    throw std::runtime_error("BattleExplorer.gather_context: context blob not present");
                }
                if (!soa::battle::ctx::codec::decode(blob, bc)) {
                    throw std::runtime_error("BattleExplorer.gather_context: decode_from_blob failed");
                }
            }
            break;
        }

        return bc;
    }

    // Convert set bits in a mask to concrete slot indices (0..31).
    static std::vector<uint8_t> BitsToSlots(uint32_t mask) {
        std::vector<uint8_t> out;
        while (mask) {
            uint32_t lsb = mask & (0u - mask);
            // find index of lsb without compiler intrinsics
            uint8_t bit = 0;
            uint32_t t = lsb;
            while ((t & 1u) == 0u) { ++bit; t >>= 1; }
            out.push_back(bit);
            mask ^= lsb;
        }
        return out;
    }

    // Default "any enemy" domain. If you later want to use bc to filter
    // only present/targetable enemies, do it inside this function.
    static std::vector<uint8_t>
        DomainAnyEnemy(const soa::battle::ctx::BattleContext& bc) {
        std::vector<uint8_t> out;
        for (uint8_t i = 4; i < 12; ++i) {
            if (bc.slots_[i].present == 1) out.push_back(i);
        }
        return out;
    }

    static std::vector<uint8_t>
        DomainOneOf(uint32_t mask /*editor-chosen*/) {
        return BitsToSlots(mask);
    }

    // Per-turn compiler: from a symbolic UI_Turn to all concrete TurnPlanSpec choices.
    // Supports ConcreteMask, AnyEnemy, OneOfMask, SameAsVar (with cycle detection).
    static std::vector<TurnPlanSpec>
        CompileTurnSpecs(const soa::battle::ctx::BattleContext& bc, const UI_Turn& ui_turn) {
        // Skeleton with params except possibly target_slot (0 until assigned).
        TurnPlanSpec base; base.reserve(ui_turn.size());

        // Quick lookup: actor_slot -> index in TurnPlanSpec
        std::unordered_map<uint8_t, std::size_t> actor_to_idx;

        // Variables we must assign (by actor)
        struct Var {
            uint8_t actor;
            TargetBindingKind kind;
            std::vector<uint8_t> domain;       // slots for AnyEnemy / OneOfMask
            std::optional<uint8_t> bind_to;     // actor we mirror, for SameAsVar
        };
        std::vector<Var> vars;

        // Known concretes from UI (actor -> slot)
        std::unordered_map<uint8_t, uint8_t> concrete;

        // helper: single-bit mask -> slot, or -1 if invalid
        auto MaskToSingleSlot = [](uint32_t m) -> int {
            if (m == 0 || (m & (m - 1)) != 0) return -1; // not exactly one bit
            uint8_t bit = 0; uint32_t t = m;
            while ((t & 1u) == 0u) { ++bit; t >>= 1; }
            return static_cast<int>(bit);
            };

        // Pass 1: lay down actions and collect variables
        for (const auto& ua : ui_turn) {
            ActionPlan ap{};
            ap.actor_slot = ua.actor_slot;
            ap.macro = ua.macro;
            ap.params = ua.params;

            switch (ua.target.kind) {
            case TargetBindingKind::SingleEnemy: {
                int s = MaskToSingleSlot(ua.target.mask);
                if (s < 4 || s > 11) return {};        // invalid or out of enemy slot range
                ap.params.target_slot = static_cast<uint8_t>(s);
                concrete[ua.actor_slot] = static_cast<uint8_t>(s);
                break;
            }
            case TargetBindingKind::MultipleEnemies: {
                ap.params.target_slot = 0xFF;          // unassigned sentinel
                auto dom = DomainOneOf(ua.target.mask);
                if (dom.empty()) return {};
                vars.push_back(Var{ ua.actor_slot, ua.target.kind, std::move(dom), std::nullopt });
                break;
            }
            case TargetBindingKind::AnyEnemy:
                ap.params.target_slot = 0xFF;          // unassigned sentinel
                vars.push_back(Var{ ua.actor_slot, ua.target.kind, DomainAnyEnemy(bc), std::nullopt });
                break;
            case TargetBindingKind::SameAsOtherPC:
                ap.params.target_slot = 0xFF;          // unassigned sentinel
                vars.push_back(Var{ ua.actor_slot, ua.target.kind, {}, std::make_optional(ua.target.var_id) });
                break;
            }

            actor_to_idx[ua.actor_slot] = base.size();
            base.push_back(ap);
        }

        // Any empty domain --> no solutions.
        for (const auto& v : vars) {
            if ((v.kind == TargetBindingKind::AnyEnemy || v.kind == TargetBindingKind::MultipleEnemies) &&
                v.domain.empty()) {
                return {};
            }
        }

        // Build SameAs dependency graph (actor -> referenced actor), detect cycles.
        std::unordered_map<uint8_t, std::vector<uint8_t>> deps, rdeps;
        std::unordered_set<uint8_t> sameas_nodes;
        for (const auto& v : vars) {
            if (v.bind_to) {
                sameas_nodes.insert(v.actor);
                sameas_nodes.insert(*v.bind_to);
                deps[v.actor].push_back(*v.bind_to);
                rdeps[*v.bind_to].push_back(v.actor);
            }
        }
        if (!sameas_nodes.empty()) {
            std::unordered_map<uint8_t, int> indeg;
            for (auto a : sameas_nodes) indeg[a] = 0;
            for (auto& [u, adj] : deps) {
                if (!sameas_nodes.count(u)) continue;
                for (auto v : adj) if (sameas_nodes.count(v)) indeg[u]++;
            }
            std::queue<uint8_t> q;
            for (auto& [a, d] : indeg) if (d == 0) q.push(a);
            int seen = 0;
            while (!q.empty()) {
                auto u = q.front(); q.pop(); ++seen;
                for (auto w : rdeps[u]) {
                    if (!sameas_nodes.count(w)) continue;
                    if (--indeg[w] == 0) q.push(w);
                }
            }
            if (seen != (int)sameas_nodes.size()) {
                // Cycle among SameAs bindings => unsatisfiable
                return {};
            }
        }

        // Order variables for assignment:
        // 1) independent vars (non-SameAs), then 2) SameAs that bind to concrete,
        // then 3) SameAs that bind to earlier vars (topologically valid by above check).
        std::vector<uint8_t> order;
        std::unordered_set<uint8_t> var_actors;
        for (auto& v : vars) var_actors.insert(v.actor);

        for (auto& v : vars) {
            if (!v.bind_to) order.push_back(v.actor);
            else if (!var_actors.count(*v.bind_to)) order.push_back(v.actor);
        }
        for (auto& v : vars) {
            if (v.bind_to && var_actors.count(*v.bind_to)) order.push_back(v.actor);
        }
        // dedup
        {
            std::unordered_set<uint8_t> seen;
            std::vector<uint8_t> t; t.reserve(order.size());
            for (auto a : order) if (!seen.count(a)) { seen.insert(a); t.push_back(a); }
            order.swap(t);
        }

        // Map actor -> index in vars[]
        std::unordered_map<uint8_t, std::size_t> var_idx;
        for (std::size_t i = 0; i < vars.size(); ++i) var_idx[vars[i].actor] = i;

        // Backtracking
        std::vector<TurnPlanSpec> out;
        TurnPlanSpec cur = base;

        // Prime cur with known concretes
        for (auto [actor, m] : concrete) {
            auto it = actor_to_idx.find(actor);
            if (it != actor_to_idx.end()) cur[it->second].params.target_slot = m;
        }

        std::function<void(std::size_t)> dfs = [&](std::size_t oi) {
            if (oi == order.size()) {
                out.push_back(cur);
                return;
            }
            uint8_t actor = order[oi];

            // If already concrete at UI-level, skip (safety)
            if (auto itc = concrete.find(actor); itc != concrete.end()) {
                auto itp = actor_to_idx.find(actor);
                if (itp != actor_to_idx.end()) cur[itp->second].params.target_slot = itc->second;
                dfs(oi + 1);
                return;
            }

            const auto itv = var_idx.find(actor);
            if (itv == var_idx.end()) { dfs(oi + 1); return; }
            const Var& v = vars[itv->second];

            // SameAs - copy from referenced actor (either concrete or assigned earlier).
            if (v.bind_to) {
                uint8_t ref_slot = 0xFF;
                auto ref = *v.bind_to;

                if (auto itc = concrete.find(ref); itc != concrete.end()) {
                    ref_slot = itc->second;
                }
                else {
                    auto itr = actor_to_idx.find(ref);
                    if (itr == actor_to_idx.end()) return;
                    ref_slot = cur[itr->second].params.target_slot;
                    if (ref_slot == 0xFF) return; // not yet assigned (should not happen due to ordering)
                }

                auto itp = actor_to_idx.find(actor);
                if (itp == actor_to_idx.end()) return;
                cur[itp->second].params.target_slot = ref_slot;
                dfs(oi + 1);
                return;
            }

            // AnyEnemy / OneOf - branch on domain
            auto itp = actor_to_idx.find(actor);
            if (itp == actor_to_idx.end()) { dfs(oi + 1); return; }

            for (uint8_t s : v.domain) {
                cur[itp->second].params.target_slot = s;
                dfs(oi + 1);
            }
            };

        if (vars.empty()) {
            out.push_back(std::move(cur));
            return out;
        }
        dfs(0);
        return out;
    }

    // Cartesian product over per-turn choices -> base BattlePaths with fake_attack_count=0
    static std::vector<soa::battle::actions::BattlePath>
        ProductTurnsToBasePaths(const std::vector<std::vector<TurnPlanSpec>>& choices_per_turn) {
        using soa::battle::actions::TurnPlan;
        using soa::battle::actions::BattlePath;

        const std::size_t N = choices_per_turn.size();
        std::vector<BattlePath> base_paths;

        if (N == 0) {
            base_paths.emplace_back();
            return base_paths;
        }

        std::vector<std::size_t> idx(N, 0);
        auto bump = [&]() -> bool {
            for (std::size_t i = 0; i < N; ++i) {
                if (++idx[i] < choices_per_turn[i].size()) return true;
                idx[i] = 0;
            }
            return false;
            };

        // Seed
        {
            BattlePath p; p.reserve(N);
            for (std::size_t t = 0; t < N; ++t) {
                TurnPlan tp; tp.fake_attack_count = 0; tp.spec = choices_per_turn[t][0];
                p.push_back(std::move(tp));
            }
            base_paths.push_back(std::move(p));
        }
        while (bump()) {
            BattlePath p; p.reserve(N);
            for (std::size_t t = 0; t < N; ++t) {
                TurnPlan tp; tp.fake_attack_count = 0; tp.spec = choices_per_turn[t][idx[t]];
                p.push_back(std::move(tp));
            }
            base_paths.push_back(std::move(p));
        }
        return base_paths;
    }

    std::vector<BattlePath>
        BattleExplorer::enumerate_paths(const soa::battle::ctx::BattleContext& bc,
            const UI_Config& ui) const
    {
        using soa::battle::actions::BattlePath;
        const std::size_t N = ui.turns.size();

        // A) compile each turn's symbolic actions into concrete TurnPlanSpec choices
        std::vector<std::vector<TurnPlanSpec>> choices_per_turn;
        choices_per_turn.reserve(N);
        for (const auto& ui_turn : ui.turns) {
            auto choices = CompileTurnSpecs(bc, ui_turn);
            if (choices.empty()) {
                return {}; // no valid instantiations for this turn => no paths overall
            }
            choices_per_turn.push_back(std::move(choices));
        }

        // B) product across turns => base paths
        auto base_paths = ProductTurnsToBasePaths(choices_per_turn);

        // C) FakeAttack expansion (Sum of f_t <= B)
        const uint32_t B = static_cast<uint32_t>(std::max(0, ui.fakeattack_budget));
        auto fvecs = enumerate_fakeattack_vectors(N, B);

        std::vector<BattlePath> out;
        if (N == 0) {
            out.emplace_back(); // zero-turn path
            return out;
        }
        out.reserve(base_paths.size() * std::max<std::size_t>(std::size_t(1), fvecs.size()));

        for (const auto& base : base_paths) {
            for (const auto& fv : fvecs) {
                BattlePath p = base;
                for (std::size_t i = 0; i < N; ++i) p[i].fake_attack_count = fv[i];
                out.push_back(std::move(p));
            }
        }
        return out;
    }

    RunResultSummary BattleExplorer::run_paths(const UI_Config& ui,
            const std::vector<soa::battle::actions::BattlePath>& paths,
            ParallelPhaseScriptRunner& runner)
    {
        RunResultSummary sum{};
        uint64_t total_jobs = paths.size() * ui.initial_frames.size();
        sum.jobs_total = total_jobs;

        // 1) Broadcast BattleRunner program to all workers
        PSInit init{};
        init.savestate_path = m_savestate_path;
        init.default_timeout_ms = 10000; // or your default; can be overridden per job via ctx if needed
        init.derived_buffer_type = DK_Battle;

        SCLOGI("[explorer] Setting up workers");

        if (!runner.set_program(PK_BattleTurnRunner, PK_BattleTurnRunner, init)) {
            throw std::runtime_error("BattleExplorer.run_paths: set_program failed");
        }
        if (!runner.run_init_once()) {
            throw std::runtime_error("BattleExplorer.run_paths: run_init_once failed");
        }
        if (!runner.activate_main()) {
            throw std::runtime_error("BattleExplorer.run_paths: activate_main failed");
        }

        SCLOGI("[explorer] Creating Jobs");
        // 2) Submit one job per terminal BattlePath
        struct Pending {
            uint64_t path_id;
            int retry_count = -1;
            phase::battle::runner::EncodeSpec spec;
        };
        std::unordered_map<uint64_t, Pending> pendings;
        pendings.reserve(total_jobs);

        uint64_t path_id = 0;
        SCLOGI("[explorer] Submitting Jobs");
        for (const auto& initial : ui.initial_frames)
        {
            for (const auto& path : paths) {
                phase::battle::runner::EncodeSpec spec{};
                spec.run_ms = 60000;
                spec.vi_stall_ms = 2000;
                spec.initial = initial;
                spec.predicates = ui.predicates;
                spec.path = path;

                std::vector<uint8_t> buf;
                phase::battle::runner::encode_payload(spec, buf);

                PSJob job{};
                job.payload = std::move(buf);

                const uint64_t jid = runner.submit(job);
                Pending p{ path_id++, ui.max_retry_count, spec };
                pendings.emplace(jid, p);
            }
        }

        // 3) Collect results for all submitted jobs
        size_t remaining = pendings.size();
        while (remaining > 0) {
            PRResult rr{};
            if (!runner.try_get_result(rr)) {
                // In a real UI loop, you could also poll progress here via runner.try_get_progress(...)
                continue;
            }

            // Only count results that correspond to our epoch; runner handles epochs internally.
            // Validate transport OK + VM OK
            if (!rr.accepted) {
                // Transport or VM failure; treat as non-success and continue
                SCLOGW("[explorer] Job was not accepted (probably wrong epoch): worker=%d jobid=%d", rr.worker_id, rr.job_id);
                --remaining;
                continue;
            }

            if (!rr.ps.ok) {
                auto p = pendings.find(rr.job_id)->second;
                pendings.erase(rr.job_id);
                uint32_t outcome; rr.ps.ctx.get(keys::core::DW_RUN_OUTCOME_CODE, outcome);
                uint32_t timeout_ms; rr.ps.ctx.get(keys::core::RUN_MS, timeout_ms);
                if (outcome != (uint32_t)RunToBpOutcome::Hit)
                {
                    bool do_retry = false;
                    if (p.retry_count < 0) do_retry = true;
                    else if (p.retry_count > 0) {
                        p.retry_count--;
                        do_retry = true;
                    }
                    SCLOGW("[explorer] Job VM run not ok (%d) %sattempting to resubmit (%s retries): worker=%d jobid=%d, outcome=%d%s%s", 
                        p.path_id, 
                        do_retry ? "" : "not ", 
                        p.retry_count < 0 ? "inf" : std::to_string(p.retry_count).c_str(), 
                        rr.worker_id, 
                        rr.job_id, 
                        outcome,
                        outcome == (uint32_t)RunToBpOutcome::Timeout ? " timeout_ms=" : "",
                        outcome == (uint32_t)RunToBpOutcome::Timeout ? std::to_string(timeout_ms).c_str() : ""
                    );

                    if (do_retry) 
                    {
                        std::vector<uint8_t> buf;
                        phase::battle::runner::encode_payload(p.spec, buf);

                        PSJob job{};
                        job.payload = std::move(buf);
                        uint64_t jid = runner.submit(job);
                        pendings.emplace(jid, p);
                        
                    }
                    else {
                        --remaining;
                    }

                }
                else {
                    SCLOGW("[explorer] Job VM failed (%d) due to unknown reason, not resubmiting: worker=%d jobid=%d, outcome=%d", p.path_id, rr.worker_id, rr.job_id, outcome);
                    --remaining;
                }
                continue;
            }

            bool is_success = false;
            //Example A: outcome code
            uint32_t oc = 0;
            if (rr.ps.ctx.get<uint32_t>(keys::battle::BATTLE_OUTCOME, oc)) {
                is_success = (oc == static_cast<uint32_t>(battle::Outcome::Victory));
            }
            --remaining;

            SCLOGI("[explorer] Received results (%d/%d): workerid=%d jobid=%d success=%s%s", total_jobs - remaining, total_jobs, rr.worker_id, rr.job_id, is_success ? "true" : "false ", oc == 0 ? "" : battle::get_outcome_string((battle::Outcome)oc).c_str());

            auto p = pendings.find(rr.job_id)->second;

            if (is_success) 
            {
                sum.successes.emplace_back((battle::Outcome)oc, p.path_id, p.spec, rr);
                ++sum.jobs_success;
            }
            else {
                sum.fails.emplace_back((battle::Outcome)oc, p.path_id, p.spec, rr);
            }
        }

        return sum;
    }

    // --- Validation & estimates ---

    bool BattleExplorer::validate_action_against_context(const soa::battle::ctx::BattleContext& bc,
        const ActionPlan& ap) const {
        // Minimal legality gate:
        // - target exists/targetable (if Attack)
        // - resources non-negative (lightweight check)
        // Keep conservative; actual impossibilities will be caught by the VM if needed.
        (void)bc;
        (void)ap;
        return true;
    }

    uint64_t BattleExplorer::estimate_paths_no_fake(const UI_Config& ui, const soa::battle::ctx::BattleContext& ctx) const {
        // Build a conservative, exact count using the same per-turn compiler
        // but only counting, not building full BattlePaths.
        // NOTE: If you want a very fast upper-bound without compiling, you could
        //       sum domains directly, but this exact path keeps it simple.
        const std::size_t N = ui.turns.size();
        if (N == 0) return 0;

        uint64_t total = 1;
        for (const auto& ui_turn : ui.turns) {
            auto choices = CompileTurnSpecs(ctx, ui_turn);
            if (choices.empty()) return 0;
            total *= static_cast<uint64_t>(choices.size());
        }
        return total * ui.initial_frames.size();
    }

    uint64_t BattleExplorer::estimate_paths_with_fake(const UI_Config& ui, const uint64_t paths_wo_fake) const {
        if (paths_wo_fake == 0) return 0;
        const uint64_t N = ui.turns.size();
        const uint64_t B = static_cast<uint64_t>(std::max(0, ui.fakeattack_budget));
        // Stars-and-bars: sum_{s=0..B} C(s+N-1, N-1) = C(B+N, N)
        return paths_wo_fake * binom(B + N, N);
    }

} // namespace simcore::battleexplorer
