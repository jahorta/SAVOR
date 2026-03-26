#include "BattleExplorer.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <functional>
#include <optional>

namespace simcore::battleexplorer {

    using soa::battle::actions::ActionPlan;
    using soa::battle::actions::TurnPlan;
    using soa::battle::actions::TurnPlanSpec;
    using soa::battle::actions::BattlePath;

    // --- Combinatorics helpers ---

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

} // namespace simcore::battleexplorer
