#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_set>
#include <unordered_map>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include "DB/DBCore/DbResult.h"
#include "DB/DBCore/DbRetryPolicy.h"

#include "DB/Querying/DataService.h"
#include "DB/SeedProbeRepo.h"
#include "DB/BattleContextRepo.h"
#include "DB/BattlePlanRepo.h"
#include "DB/BattlePlanTurnRepo.h"
#include "DB/BattlePlanAtomRepo.h"
#include "DB/TurnActionPresetRepo.h"
#include "DB/PredicateSpecRepo.h"
#include "DB/AuthoringTemplatesRepo.h"
#include "DB/UiConfigRowRepo.h"

#include "Phases/BattleExplorer.h"
#include "Phases/Programs/BattleRunner/BattleRunnerDBSettingsWriter.h"

#include "../Popups/IdPicker.h"
#include "../Popups/IdRepoAdapters.h"
#include "../Popups/UiActionPresetPopup.h"
#include "../Popups/PredicateSpecPopup.h"
#include "../../Components/ToastBus.h"
#include "../../Components/FutureQueue.h"

// NOTE: Keep payload labels stable for DnD interop.
#define BRB_PAYLOAD_UI_ACTION_PRESET   "BRB/UiActionPreset"
#define BRB_PAYLOAD_PREDICATE_SPEC     "BRB/PredicateSpec"
#define BRB_PAYLOAD_TEMPLATE           "BRB/AuthoringTemplate"

namespace soasim::ui {

	struct UiActionInstance {
		uint32_t  actor_slot{ 0 };
		int64_t  preset_id{ 0 };
	};

	struct UiConfigDraft {
		int32_t fake_attack_budget{ 0 };
		std::vector <std::vector< UiActionInstance >> actions;
	};

	struct PredicateDraft {
		int64_t predicate_id{ 0 };
		std::string name;
		std::string description;
	};

	enum BattleContextRequestState : uint8_t {
		None, Queued, Running, Success, Failure, Decoded
	};

	inline std::string GetBCStateString(BattleContextRequestState bcs) {
		switch (bcs) {
		case BattleContextRequestState::Queued:
			return "BattleContext probe is queued...";
		case BattleContextRequestState::Running:
			return "BattleContext probe is running...";
		case BattleContextRequestState::Success:
			return "BattleContext probe successful. Loading...";
		case BattleContextRequestState::Failure:
			return "Failed to get BattleContext";
		}
		return "";
	}

	class BattleRunSettingsPane {
	public:
		BattleRunSettingsPane() = default;

		void OnActivated();
		// Primary entry
		void Draw();


	private:

		void load_ui_actions();
		void load_predicates();

		// ---------- Left: libraries ----------
		void draw_left_library_();
		void draw_left_ui_actions_();
		void draw_left_predicates_();
		void draw_left_templates_();

		// ---------- Middle: editors ----------
		void draw_middle_();
		void draw_ui_config_editor_();
		void draw_predicates_editor_();

		// ---------- Right: context + counts + save ----------
		void draw_right_();
		void draw_context_loader_();
		void draw_counts_and_estimate_();
		void draw_save_actions_();

		// ---------- Helpers ----------
		PredicateSpecLite get_pred_row(int64_t id);
		void on_context_row_found(int64_t artifact_id, int version);
		void on_savestate_row_found(int64_t savestate_id);
		void on_seed_probe_picked_(int64_t seed_probe_id);
		void reconcile_party_size_();
		void wait_for_context(int64_t job_id, uint32_t poll_rate_ms = 200);
		void add_turn_();
		bool all_slots_filled_() const;

		// Row helpers
		UiActionInstance* find_instance_(int t, int s);
		const UiActionInstance* find_instance_(int t, int s) const;

		// Convert draft -> engine UI_Config (implemented in .cpp using existing presets)
		bool build_ui_config_from_draft_(/*out*/simcore::battleexplorer::UI_Config& cfg) const;

		// Template save/load
		void save_authoring_template_();
		void load_authoring_template_(int64_t template_id);

		void validate_grid_against_context_();

		void ensure_preset_cached_async_(int64_t id);

		// ---------- State ----------
		
		bool inited_{ false };
		// 
		// Left library state
		std::string search_ui_actions_;
		std::string search_predicates_;
		std::string search_templates_;

		std::vector<simcore::db::TurnActionPresetLite> ui_action_results_;
		std::vector<simcore::db::PredicateSpecLite> predicate_results_;
		std::vector<simcore::db::AuthoringTemplateLite> template_results_;

		// Middle editor state
		UiConfigDraft ui_config_{};
		std::vector<PredicateDraft> predicates_{};
		std::unordered_set<uint32_t> invalid_cells_;
		std::unordered_map<uint32_t, std::string> invalid_reasons_;

		std::unordered_map<int64_t, simcore::db::TurnActionPresetRow> preset_cache_;
		std::unordered_map<int64_t, simcore::db::PredicateSpecLite> predicate_cache_{};


		// Right context/save state
		int64_t                         seed_probe_id_{ 0 };
		int64_t                         savestate_id_{ 0 };
		bool                            has_context_{ false };
		std::atomic<BattleContextRequestState>                     context_status_{BattleContextRequestState::None};
		int64_t                         bc_request_id_{ 0 };
		soa::battle::ctx::BattleContext bc_{};
		int                             bc_version_{ 0 };
		int                             party_size_{ 4 }; // 4 until loaded; else #PCs

		// Save fields
		std::string settings_name_;
		std::string settings_desc_;
		std::string run_group_name_;
		std::string run_group_desc_;

		// Pickers / modals
		soasim::ui::UiActionPresetPopup ui_preset_popup_;
		bool show_ui_preset_popup_{ false };
		std::optional<std::pair<uint32_t, uint32_t>> ui_preset_edit_loc_ = std::nullopt;
		std::optional<int64_t> preset_edit_id_ = std::nullopt;

		soasim::ui::PredicateSpecPopup  pred_popup_;
		bool show_pred_popup_{ false };
		std::optional<uint32_t> pred_edit_loc_ = std::nullopt;
		std::optional<int64_t> pred_edit_id_ = std::nullopt;


		};

} // namespace soasim::ui
