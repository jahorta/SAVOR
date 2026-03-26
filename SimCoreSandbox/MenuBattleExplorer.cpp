#define NOMINMAX
#include "MenuBattleExplorer.h"
#include "utils.h"
#include <iostream>
#include <iomanip>
#include <limits>
#include <unordered_set>

#include "Core/Memory/Soa/SoaConstants.h"
#include "Core/Memory/Soa/SoaAddrCatalog.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Phases/Programs/BattleRunner/BattleOutcome.h"
#include "Phases/BattleExplorer.h"
#include "Phases/Programs/BattleRunner/BattleRunnerPayload.h"
#include "Core/Input/InputPlanFmt.h"
#include "Core/Input/SoaBattle/PlanWriter.h"


namespace {
    using soa::battle::actions::BattleAction;
    using simcore::battleexplorer::TargetBindingKind;

    static const char* action_name(BattleAction a) {
        switch (a) {
        case BattleAction::Attack: return "Attack";
        case BattleAction::Defend: return "Defend";
        case BattleAction::Focus:  return "Focus";
        case BattleAction::UseItem:return "Item";
        default:                   return "Action";
        }
    }

    static std::string pc_label(const soa::battle::ctx::BattleContext& bc, int pc_slot) {
        const auto pid = static_cast<std::size_t>(bc.slots_[pc_slot].id);
        std::string_view name = (pid < soa::text::PCNames.size())
            ? soa::text::PCNames[pid] : std::string_view{ "PC" };
        std::string s;
        s.reserve(32);
        s += '['; s += char('0' + pc_slot); s += ']'; s += ' ';
        s.append(name.data(), name.size());
        return s;
    }

    static int first_set_bit_index(uint32_t m) {
        if (!m) return -1;
        for (int i = 0; i < 32; ++i) if (m & (1u << i)) return i;
        return -1;
    }

    static std::string slots_from_mask(uint32_t m) {
        std::string out;
        bool first = true;
        for (int s = 0; s < 32; ++s) {
            if (m & (1u << s)) {
                if (!first) out += ",";
                out += std::to_string(s);
                first = false;
            }
        }
        return out;
    }

    static std::string format_params(const simcore::battleexplorer::UI_Action& ua) {
        if (ua.macro == BattleAction::UseItem && ua.params.item_id != 0xFFFF)
            return std::string("itemid=") + std::to_string(ua.params.item_id);
        return {};
    }

    static std::string format_target(const simcore::battleexplorer::TargetBinding& tb) {
        switch (tb.kind) {
        case TargetBindingKind::SingleEnemy: {
            int s = first_set_bit_index(tb.mask);
            return (s >= 0) ? ("slot=" + std::to_string(s)) : std::string{};
        }
        case TargetBindingKind::MultipleEnemies:
            return tb.mask ? ("slots=" + slots_from_mask(tb.mask)) : std::string{};
        case TargetBindingKind::AnyEnemy:
            return "any";
        case TargetBindingKind::SameAsOtherPC:
            return (tb.var_id != 0xFF) ? ("sameas=" + std::to_string(tb.var_id)) : std::string{};
        }
        return {};
    }

    static const simcore::battleexplorer::UI_Action* find_action_for_actor(
        const simcore::battleexplorer::UI_Turn& turn, uint8_t actor_slot)
    {
        for (const auto& ua : turn)
            if (ua.actor_slot == actor_slot) return &ua;
        return nullptr;
    }
}


namespace {
    using simcore::battleexplorer::TargetBindingKind;
    using soa::battle::actions::BattleAction;
    using soa::battle::actions::ActionParameters;

    static bool is_enemy_slot(const soa::battle::ctx::BattleContext& bc, int slot) {
        return slot >= 4 && slot < 12 && bc.slots_[slot].present && !bc.slots_[slot].is_player;
    }

    static std::vector<int> list_present_pcs(const soa::battle::ctx::BattleContext& bc) {
        std::vector<int> out;
        for (int i = 0; i < 4; ++i) if (bc.slots_[i].present && bc.slots_[i].is_player) out.push_back(i);
        return out;
    }

    static std::vector<int> list_present_enemies(const soa::battle::ctx::BattleContext& bc) {
        std::vector<int> out;
        for (int i = 4; i < 12; ++i) if (is_enemy_slot(bc, i)) out.push_back(i);
        return out;
    }

    static void print_enemy_listing(const soa::battle::ctx::BattleContext& bc) {
        auto es = list_present_enemies(bc);
        for (int s : es) {
            const auto type_id = static_cast<std::size_t>(bc.slots_[s].id);
            std::string_view name = (type_id < soa::text::EnemyNames.size()) ? soa::text::EnemyNames[type_id] : std::string_view{ "<Unknown>" };
            std::cout << "  [" << s << "] " << name << " (type " << type_id << ")\n";
        }
    }

    static uint32_t mask_from_enemy_slots(const soa::battle::ctx::BattleContext& bc, const std::vector<int>& slots_) {
        uint32_t m = 0;
        for (int s : slots_) if (is_enemy_slot(bc, s)) m |= (1u << s);
        return m;
    }

    static uint32_t mask_from_enemy_type(const soa::battle::ctx::BattleContext& bc, uint16_t enemy_type_id) {
        uint32_t m = 0;
        for (int s = 4; s < 12; ++s) {
            if (is_enemy_slot(bc, s) && bc.slots_[s].id == enemy_type_id) m |= (1u << s);
        }
        return m;
    }

    static bool prompt_yesno(const char* label, bool defno = true) {
        std::cout << label << (defno ? " [y/N]: " : " [Y/n]: ");
        std::string s; std::getline(std::cin, s);
        if (s.empty()) return !defno;
        return (s[0] == 'y' || s[0] == 'Y');
    }

    static int prompt_choice(const char* label, int minv, int maxv, int defv) {
        for (;;) {
            std::cout << label << " [" << defv << "]: ";
            std::string s; std::getline(std::cin, s);
            int v = defv;
            if (!s.empty()) {
                try { v = std::stoi(s); }
                catch (...) { continue; }
                }
            if (v >= minv && v <= maxv) return v;
        }
    }

    static std::optional<uint16_t> prompt_item_from_useables(const soa::battle::ctx::BattleContext& bc) {
        struct Row { int idx; uint16_t item_id; uint8_t count; std::string_view name; };
        std::vector<Row> rows;
        rows.reserve(80);
        for (int i = 0; i < 80; ++i) {
            const auto& slot = bc.state.useable_items[i];
            if (slot.count == 0) continue;
            const auto id = static_cast<std::size_t>(slot.item_id);
            if (id >= soa::text::ItemNames.size()) continue;
            std::string_view nm = soa::text::ItemNames[id];
            if (nm.empty()) continue;
            rows.push_back(Row{ i, static_cast<uint16_t>(id), slot.count, nm });
        }
        if (rows.empty()) {
            std::cout << "(No usable items)\n";
            return std::nullopt;
        }
        std::cout << "Choose item:\n";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            std::cout << "  (" << (i + 1) << ") " << rows[i].name << "  x" << int(rows[i].count) << "  [id=" << rows[i].item_id << "]\n";
        }
        int sel = prompt_choice("Item", 1, static_cast<int>(rows.size()), 1);
        return rows[static_cast<std::size_t>(sel - 1)].item_id;
    }

    static simcore::battleexplorer::TargetBinding prompt_target_binding(const soa::battle::ctx::BattleContext& bc, uint8_t actor_slot) {
        simcore::battleexplorer::TargetBinding tb{};
        auto enemies = list_present_enemies(bc);
        if (enemies.empty()) {
            tb.kind = TargetBindingKind::AnyEnemy; // degenerate; VM will handle
            tb.mask = 0;
            return tb;
        }

        std::cout << "Targeting:\n";
        std::cout << "  (1) Single enemy slot\n";
        std::cout << "  (2) Multiple by enemy TYPE (selects all of that type)\n";
        std::cout << "  (3) Multiple by explicit SLOTS\n";
        std::cout << "  (4) AnyEnemy (branch over all enemies)\n";
        std::cout << "  (5) Same as other PC\n";
        int tsel = prompt_choice("> ", 1, 5, 1);

        if (tsel == 1) {
            print_enemy_listing(bc);
            int s = prompt_choice("Enemy slot", 4, 11, enemies.front());
            while (!is_enemy_slot(bc, s)) s = prompt_choice("Enemy slot", 4, 11, enemies.front());
            tb.kind = TargetBindingKind::SingleEnemy;
            tb.mask = (1u << s);
            return tb;
        }
        if (tsel == 2) {
            print_enemy_listing(bc);
            int s = prompt_choice("Pick a slot to infer TYPE", 4, 11, enemies.front());
            while (!is_enemy_slot(bc, s)) s = prompt_choice("Pick a slot to infer TYPE", 4, 11, enemies.front());
            uint16_t type_id = bc.slots_[s].id;
            tb.kind = TargetBindingKind::MultipleEnemies; // note: spelled as in header
            tb.mask = mask_from_enemy_type(bc, type_id);
            return tb;
        }
        if (tsel == 3) {
            print_enemy_listing(bc);
            std::cout << "Enter slots separated by spaces (end with blank): ";
            uint32_t m = 0;
            for (;;) {
                std::string tok;
                if (!(std::cin >> tok)) break;
                if (tok.empty()) break;
                bool done_line = tok.find('\n') != std::string::npos;
                int v = -1;
                try { v = std::stoi(tok); }
                catch (...) { v = -1; }
                if (v >= 4 && v <= 11 && is_enemy_slot(bc, v)) m |= (1u << v);
                if (done_line) break;
                if (std::cin.peek() == '\n') { std::cin.get(); break; }
            }
            std::cin.clear();
            if (m == 0) {
                // fallback to first enemy
                m = (1u << enemies.front());
            }
            tb.kind = TargetBindingKind::MultipleEnemies;
            tb.mask = m;
            return tb;
        }
        if (tsel == 4) {
            tb.kind = TargetBindingKind::AnyEnemy;
            tb.mask = 0;
            return tb;
        }
        // Same as other PC
        {
            auto pcs = list_present_pcs(bc);
            // remove self if present in set
            pcs.erase(std::remove(pcs.begin(), pcs.end(), int(actor_slot)), pcs.end());
            if (pcs.empty()) {
                tb.kind = TargetBindingKind::AnyEnemy;
                tb.mask = 0;
                return tb;
            }
            std::cout << "Bind to which PC slot? ";
            for (int p : pcs) {
                const auto pid = static_cast<std::size_t>(bc.slots_[p].id);
                std::string_view nm = (pid < soa::text::PCNames.size()) ? soa::text::PCNames[pid] : std::string_view{ "PC" };
                std::cout << "[" << p << ":" << nm << "] ";
            }
            std::cout << "\n";
            int pick = prompt_choice("> ", 0, 3, pcs.front());
            while (pick == actor_slot || std::find(pcs.begin(), pcs.end(), pick) == pcs.end()) {
                pick = prompt_choice("> ", 0, 3, pcs.front());
            }
            tb.kind = TargetBindingKind::SameAsOtherPC;
            tb.var_id = static_cast<uint8_t>(pick);
            return tb;
        }
    }

    static bool action_needs_target(BattleAction a) {
        return (a == BattleAction::Attack || a == BattleAction::UseItem);
    }

    static BattleAction prompt_action_kind() {
        std::cout << "Action:\n";
        std::cout << "  (1) Attack\n";
        std::cout << "  (2) Defend\n";
        std::cout << "  (3) Focus\n";
        std::cout << "  (4) Use Item\n";
        int sel = prompt_choice("> ", 1, 4, 1);
        switch (sel) {
        case 1: return BattleAction::Attack;
        case 2: return BattleAction::Defend;
        case 3: return BattleAction::Focus;
        default: return BattleAction::UseItem;
        }
    }

    static bool build_ui_action_for_actor(const soa::battle::ctx::BattleContext& bc,
        uint8_t actor_slot,
        simcore::battleexplorer::UI_Action& out) {
        const auto pid = static_cast<std::size_t>(bc.slots_[actor_slot].id);
        std::string_view nm = (pid < soa::text::PCNames.size()) ? soa::text::PCNames[pid] : std::string_view{ "PC" };
        std::cout << "\n-- Actor [" << int(actor_slot) << "] " << nm << " --\n";

        const auto a = prompt_action_kind();
        out.actor_slot = actor_slot;
        out.macro = a;
        out.params = ActionParameters{};
        out.target = {};

        if (a == BattleAction::UseItem) {
            auto item_opt = prompt_item_from_useables(bc);
            if (!item_opt) {
                std::cout << "Skipping action (no items).\n";
                return false;
            }
            out.params.item_id = *item_opt;
        }

        if (action_needs_target(a)) {
            out.target = prompt_target_binding(bc, actor_slot);
        }
        else {
            out.target.kind = TargetBindingKind::SingleEnemy;
            out.target.mask = 0;
        }

        return true;
    }
} // anon namespace


namespace {
    using simcore::battleexplorer::UI_Config;
    using simcore::battleexplorer::UI_Turn;
    using simcore::battleexplorer::UI_Action;
    using simcore::pred::Spec;
    using simcore::pred::PredKind;
    using simcore::pred::PredFlag;
    using simcore::pred::CmpOp;

    static bool get_first_battle_defaults(UI_Config& out) {

        UI_Action vyse{};
        vyse.actor_slot = 0;
        vyse.macro = BattleAction::Attack;
        vyse.target.kind = TargetBindingKind::AnyEnemy;

        UI_Action aika{};
        aika.actor_slot = 1;
        aika.macro = BattleAction::Attack;
        aika.target.kind = TargetBindingKind::SameAsOtherPC;
        aika.target.var_id = 0;

        UI_Turn turn{vyse, aika};

        out.turns = {turn, turn };

        std::vector<Spec> specs{};
        specs.reserve(4);

        // Check that both players go first
        Spec first{};
        first.id = 0;
        first.kind = PredKind::ABS;
        first.required_bp = bp::battle::TurnIsReady;
        first.set_flag(PredFlag::Active);
        first.set_flag(PredFlag::RhsIsKey);
        first.width = 1;
        first.set_every_turn();
        first.lhs_key = addr::derived::battle::TurnOrderPcMax;
        first.cmp = CmpOp::LT;
        first.rhs_key = addr::derived::battle::TurnOrderEcMin;
        first.desc = "Players act before enemies. (max PC turn index is less than min EC turn index)";
        specs.push_back(first);


        
        // Check that we have 1 electribox after first turn
        std::string eb_count_addrprog_desc;
        addrprog::Builder eb_count_addrprog;
        addrprog::catalog::item_drop_amt(eb_count_addrprog, soa::itemid::Electri_Box, eb_count_addrprog_desc);

        Spec eb{};
        eb.id = 1;
        eb.kind = PredKind::ABS;
        eb.required_bp = bp::battle::EndTurn;
        eb.required_bps = { (uint16_t)bp::battle::EndTurn, (uint16_t)bp::battle::Victory };
        eb.set_flag(PredFlag::Active);
        eb.width = 1;
        eb.set_every_turn();

        eb.lhs_prog = eb_count_addrprog.blob();
        eb.lhs_prog_desc = eb_count_addrprog_desc;
        eb.set_flag(PredFlag::LhsIsProg);

        eb.cmp = CmpOp::EQ;

        eb.rhs_key = addr::derived::battle::CurrentTurn;
        eb.set_flag(PredFlag::RhsIsKey);

        eb.desc = "Everyturn, we drop an electribox. (Electribox count equals turn number)";
        specs.push_back(eb);

        out.predicates = std::move(specs);

        return true;
    }

    static bool load_unique_input_frames_sample(UI_Config& out) {
        out.initial_frames = {
            GCInputFrame(),
            GCInputFrame().JStick(48, 48),
            GCInputFrame().JStick(190, 48),
            GCInputFrame().JStick(89, 64),
            GCInputFrame().JStick(64, 89),
            GCInputFrame().JStick(89, 89),
            GCInputFrame().JStick(73, 173),
            GCInputFrame().CStick(48, 48),
            GCInputFrame().JStick(198, 56).CStick(56, 198).Triggers(148, 48),
            GCInputFrame().JStick(89, 144).CStick(81, 56).Triggers(173, 190),
            GCInputFrame().JStick(48, 144).CStick(48, 48).Triggers(56, 165),
        };
        return true;
    }

}


// Include what you need to print StructuredBattleContext nicely.
// This is a thin shell you can expand as you hook real I/O helpers.
namespace sandbox {

    using simcore::battleexplorer::BattleExplorer;
    using simcore::battleexplorer::UI_Config;
    using simcore::battleexplorer::UI_Turn;

    static void render_battle_context(const soa::battle::ctx::BattleContext& bc) {
        // 1) Structured BattleContext (placeholder one-liners)
        std::cout << "\n== Battle Context ==\n";
        std::cout << "Party Members: ";
        for (int i = 0; i < 4; i++) {
            if (!bc.slots_[i].present) continue;
            auto element = soa::text::get_element_name(bc.slots_[i].instance.current_weapon_element);
            auto name = soa::text::PCNames[bc.slots_[i].id];
            std::cout << "\n  [" << i << "] " << name << "  (element=" << element << ")";
        }
        std::cout << "\nEnemies: ";
        for (int i = 4; i < 12; i++) {
            if (!bc.slots_[i].present) continue;
            auto name = soa::text::get_enemy_name(bc.slots_[i].id);
            std::cout << "\n  [" << i << "] " << name << "  ";
        }
        
        std::cout << "\nItemDrops: ";
        std::unordered_set<uint8_t> unique_enemy_types;
        std::unordered_set<uint8_t> unique_slot_by_enemy_types;
        for (int i = 4; i < 12; i++) {
            if (!bc.slots_[i].present) continue;
            if (unique_enemy_types.contains((uint8_t)bc.slots_[i].id)) continue;
            unique_enemy_types.emplace(bc.slots_[i].id);
            unique_slot_by_enemy_types.emplace(i);
        }
        
        for (auto i : unique_slot_by_enemy_types) {
            auto name = soa::text::get_enemy_name(bc.slots_[i].id);
            std::cout << "\n  " << name;
            for (auto item : bc.slots_[i].enemy_def.items)
            {
                if (item.itemId < 0) continue;
                auto item_name = soa::text::get_item_name((size_t)item.itemId);
                auto amt = (int)item.amount;
                auto chance = (int)item.chance;
                std::cout << "\n    (" << chance << "%) " << item_name << " x" << amt;
            }
        }
        std::cout << "\n";
    }
    
    static void render_overview(const soa::battle::ctx::BattleContext& bc,
        const UI_Config& ui,
        const BattleExplorer& ex) {
        render_battle_context(bc);
        std::cout << "\n---- Battle Run Setup ----";

        // 2) Number of turn plans set
        const auto N = ui.turns.size();
        std::cout << "\nTurns defined : " << N;

        // 3) Players with T(t):(action) summaries
        std::cout << "\nPlayers (compact per-turn actions):";

        // 3.1) Build labels and find max label width
        int max_label = 0;
        std::array<std::string, 4> labels{};
        std::vector<int> pcs;
        for (int pc = 0; pc < 4; ++pc) {
            if (!bc.slots_[pc].present || !bc.slots_[pc].is_player) continue;
            pcs.push_back(pc);
            labels[pc] = pc_label(bc, pc);
            max_label = std::max<int>(max_label, int(labels[pc].size()));
        }

        // 3.2) Pre-format tokens per PC per turn, and compute column widths per turn
        const std::size_t T = ui.turns.size();
        std::vector<std::vector<std::string>> tokens(4, std::vector<std::string>(T));
        std::vector<int> colw(T, 0);

        auto make_token = [&](const simcore::battleexplorer::UI_Action* ua, std::size_t t) -> std::string {
            if (!ua) return {};
            const char* aname = action_name(ua->macro);
            std::string p = format_params(*ua);   // prints [itemid=<v>] for UseItem
            std::string tgt = format_target(ua->target);

            std::string s;
            s.reserve(32);
            s += "("; s += std::to_string(t + 1); s += ":"; s += aname;
            if (!p.empty()) { s += ":["; s += p; s += "]"; }
            if (!tgt.empty()) { s += ":";  s += tgt; }
            s += ")";
            return s;
            };

        for (int pc : pcs) {
            for (std::size_t t = 0; t < T; ++t) {
                const auto* ua = find_action_for_actor(ui.turns[t], static_cast<uint8_t>(pc));
                tokens[pc][t] = make_token(ua, t);
                colw[t] = std::max<int>(colw[t], int(tokens[pc][t].size()));
            }
        }

        // 3.3) Emit rows with padded label and fixed-width columns per turn
        for (int pc : pcs) {
            std::cout << "\n";
            std::cout << "  " << std::left << std::setw(max_label) << labels[pc] << "  ";
            for (std::size_t t = 0; t < T; ++t) {
                // +2 spaces gap between columns; tweak to taste
                std::cout << std::left << std::setw(colw[t] + 2) << tokens[pc][t];
            }
        }

        // 4) Active Predicates
        std::cout << "\nActive Predicates : " << ui.predicates.size();
        for (int i = 0; i < ui.predicates.size(); i++) {
            auto p = ui.predicates[i];
            std::cout << "\n  [" << i << "]" << "  (" << p.id << ") " << p.desc;
        }

        // 5) Options
        std::cout << "\nOptions:";
        std::cout << "\n  FakeAttack Budget = " << std::max(0, ui.fakeattack_budget);
        std::cout << "\n  Job Retries (-1=inf) = " << ui.max_retry_count;

        // Footer: estimates
        const auto X = ex.estimate_paths_no_fake(ui, bc);
        const auto Y = ex.estimate_paths_with_fake(ui, X);
        std::cout << "\nBase paths: " << X << " | With FakeAttacks ? B: " << Y << "\n";
    }

    static int prompt_int(const char* label, int defval) {
        std::cout << label << " [" << defval << "]: ";
        int v = defval;
        if (!(std::cin >> v)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            v = defval;
        }
        return v;
    }

    void get_battle_context(AppState& app) {
        std::string savestate_path = app.default_savestate;

        for (;;)
        {
            std::cout << "\n--- BattleExplorer ---\n";
            std::cout << "ISO:              " << (app.iso_path.empty() ? "<unset>" : app.iso_path) << "\n";
            std::cout << "Dolphin base:     " << (app.dolphin_base_dir.empty() ? "<unset>" : app.dolphin_base_dir) << "\n";
            std::cout << "Workers:          " << app.workers << "\n";
            std::cout << "Savestate:        " << (savestate_path.empty() ? "<unset>" : savestate_path) << "\n";
            std::cout << "\n"
                << "1) Set savestate path\n"
                << "r) Run\n"
                << "b) Back\n> ";
            std::string c; if (!std::getline(std::cin, c)) return;

            if (c == "1") savestate_path = prompt_path("Savestate path: ", true, true, savestate_path).string();
            else if (c == "b") return;
            else if (c == "r") break;
        }

        BattleExplorer ex = BattleExplorer(savestate_path);
        UI_Config ui;

        simcore::ParallelPhaseScriptRunner runner{ app.workers };

        // Boot plan: use your Boot module; keep ISO and portable base fixed for the lifetime of the pool.
        simcore::BootPlan boot = make_boot_plan(app);

        // Start runners
        if (!runner.start(boot)) {
            SCLOGE("Failed to boot workers.");
            exit(1); // or propagate error
        }

        // Expect app to already hold a savestate path and a started runner.
        // Also expect a cached last_battle_ctx from Gather Context step or fetch it now.
        soa::battle::ctx::BattleContext bc = ex.gather_context(runner);

        render_battle_context(bc);
    }

    void run_battle_explorer_menu(AppState& app) {

        std::string savestate_path = app.default_savestate;

        for (;;)
        {
            std::cout << "\n--- BattleExplorer ---\n";
            std::cout << "ISO:              " << (app.iso_path.empty() ? "<unset>" : app.iso_path) << "\n";
            std::cout << "Dolphin base:     " << (app.dolphin_base_dir.empty() ? "<unset>" : app.dolphin_base_dir) << "\n";
            std::cout << "Workers:          " << app.workers << "\n";
            std::cout << "Savestate:        " << (savestate_path.empty() ? "<unset>" : savestate_path) << "\n";
            std::cout << "\n"
                << "1) Set savestate path\n"
                << "r) Run\n"
                << "b) Back\n> ";
            std::string c; if (!std::getline(std::cin, c)) return;

            if (c == "1") savestate_path = prompt_path("Savestate path: ", true, true, savestate_path).string();
            else if (c == "b") return;
            else if (c == "r") break;
        }

        BattleExplorer ex = BattleExplorer(savestate_path);
        UI_Config ui;
        ui.initial_frames.emplace_back(GCInputFrame{});

        simcore::ParallelPhaseScriptRunner runner{ app.workers };

        // Boot plan: use your Boot module; keep ISO and portable base fixed for the lifetime of the pool.
        simcore::BootPlan boot = make_boot_plan(app);

        // Start runners
        if (!runner.start(boot)) {
            SCLOGE("Failed to boot workers.");
            exit(1); // or propagate error
        }

        // Expect app to already hold a savestate path and a started runner.
        // Also expect a cached last_battle_ctx from Gather Context step or fetch it now.
        soa::battle::ctx::BattleContext bc = ex.gather_context(runner);

        runner.increment_epoch();
        runner.reset_job_ids();

        for (;;) {
            render_overview(bc, ui, ex);
            std::cout << "\n[1] Add turn  [2] Modify turn  [3] Add predicate  [6] Remove predicate  [4] Set options  [5] Load First Battle Defaults [7] Add unique Input Frame Starts [R] Run  [B] Back\n> ";
            std::string c; if (!std::getline(std::cin, c)) return;
            if (c == "B" || c == "b") break;

            if (c == "1") {
                UI_Turn t{};
                for (int s : list_present_pcs(bc)) {
                    simcore::battleexplorer::UI_Action ua{};
                    if (build_ui_action_for_actor(bc, static_cast<uint8_t>(s), ua)) {
                        t.push_back(std::move(ua));
                    }
                }
                ui.turns.push_back(std::move(t));
            }
            else if (c == "2") {
                std::cout << "[Not implemented]";
                continue;

                if (ui.turns.empty()) continue;
                const int idx = prompt_int("Turn index (1..N)", 1) - 1;
                if (idx >= 0 && idx < static_cast<int>(ui.turns.size())) {
                    // TODO: edit ui.turns[idx].spec entries per player
                }
            }
            else if (c == "3") {
                std::cout << "[Not implemented]";
                continue;
                // TODO: predicate catalog ? push into ui.predicates
            }
            else if (c == "4") {
                ui.fakeattack_budget = std::max(0, prompt_int("FakeAttack Budget", ui.fakeattack_budget));
                ui.max_retry_count = std::max(-1, prompt_int("Max Job Retries (-1=inf)", ui.max_retry_count));
            }
            else if (c == "5") {
                get_first_battle_defaults(ui);
                for (uint32_t i = 0; i < ui.predicates.size(); i++) ui.predicates[i].id = i;
            }
            else if (c == "6") {
                if (ui.predicates.empty()) continue;
                std::string prompt = "Predicate Idx [0.." + std::to_string(ui.predicates.size() - 1) + "]";
                const int idx = prompt_int(prompt.c_str(), 0);
                if (idx >= 0 && idx < static_cast<int>(ui.predicates.size())) {
                    ui.predicates.erase(ui.predicates.begin() + idx);
                }
                for (uint32_t i = 0; i < ui.predicates.size(); i++) ui.predicates[i].id = i;
            }
            else if (c == "7") {
                load_unique_input_frames_sample(ui);
            }
            else if (c == "R" || c == "r") {
                auto paths = ex.enumerate_paths(bc, ui);
                auto summary = ex.run_paths(ui, paths, runner);
                std::cout << "Submitted " << summary.jobs_total << " jobs; successes: " << summary.jobs_success << "\n";
                if (summary.successes.size() > 0) std::cout << "\nSuccesses found!";
                for (auto r : summary.successes) {
                    std::cout << "\n  [jid=" << r.job_id << "] " << simcore::battle::get_outcome_string(r.outcome) << ": initframe=(" << simcore::DescribeFrame(r.spec.initial) << ") " << soa::battle::actions::get_battle_path_summary(r.spec.path);
                }
                if (summary.fails.size() > 0) std::cout << "\nFailures:";
                for (auto r : summary.fails) {
                    std::string outcome_s = simcore::battle::get_outcome_string(r.outcome);
                    if (r.outcome == simcore::battle::Outcome::PlanMaterializeFailure) 
                    {
                        uint32_t err; r.pr.ps.ctx.get(keys::battle::PLAN_MATERIALIZE_ERR, err);
                        uint32_t turn; r.pr.ps.ctx.get(keys::battle::ACTIVE_TURN, turn);
                        outcome_s = outcome_s + " [t" + std::to_string(turn) + "]";
                        outcome_s = outcome_s + ": " + soa::battle::actions::get_materialize_err_string((soa::battle::actions::MaterializeErr)err);
                    }
                    else if (r.outcome == simcore::battle::Outcome::PredFailure) {
                        uint32_t perr; r.pr.ps.ctx.get(keys::core::PRED_FIRST_FAILED, perr);
                        uint32_t turn; r.pr.ps.ctx.get(keys::battle::ACTIVE_TURN, turn);
                        std::string pcmp; r.pr.ps.ctx.get(keys::core::PRED_FAILED_CMP_STR, pcmp);
                        outcome_s = outcome_s + " [t" + std::to_string(turn) + "]";
                        if (perr >= 0 && perr < ui.predicates.size()) outcome_s = outcome_s + ": " + ui.predicates[perr].desc + " [" + pcmp + "]";
                        else outcome_s = outcome_s + ": unknown predicate id=" + std::to_string(perr);
                    }
                    std::cout << "\n  [" << r.job_id << "] " << outcome_s << ":\n  initframe=(" << simcore::DescribeFrame(r.spec.initial) << ") " << soa::battle::actions::get_battle_path_summary(r.spec.path);
                }
                std::cout << "\n\n Press Enter to Continue...";
                std::string c; std::getline(std::cin, c);
            }
        }
    }

} // namespace sandbox
