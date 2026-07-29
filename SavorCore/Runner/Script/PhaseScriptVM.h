#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <optional>
#include <cstdint>
#include <atomic>

#include "../Breakpoints/BpRegistry.h"    // BreakpointMap, BPKey
#include "../Breakpoints/Predicate.h"
#include "../../Core/DolphinWrapper.h"
#include "../../Core/Input/InputPlan.h" // GCInputFrame
#include "../../Core/Input/SoaBattle/Actiontypes.h"
#include "../../Core/Memory/DerivedBase.h"
#include "../../Core/Memory/KeyHostRouter.h"
#include "CtxRegistry.h"
#include "PSContext.h"
#include "PhaseScriptProgram.h"
#include "../InputMacro/IInputMacroHost.h"
#include "../InputMacro/IInputMacroPlanDriver.h"
#include "../InputMacro/Providers/BattleCommandInputMacroProvider.h"
#include "../InputMacro/Providers/BattleCompletionInputMacroProvider.h"
#include "../InputMacro/Providers/BattleResultsScreenInputMacroProvider.h"

namespace savor {
	namespace inputmacro {
		class InputMacroRuntime;
		struct InputMacroStepResult;
	}


	// ----- VM -----
	class PhaseScriptVM :
		private inputmacro::IInputMacroHost,
		private inputmacro::IInputMacroDriverHost,
		private inputmacro::IBattleCommandInputMacroProviderHost {
	public:
		PhaseScriptVM(savor::DolphinWrapper& host, const BreakpointMap& bpmap);
		~PhaseScriptVM();

		// Retained only as a hard-cut legacy entry point while existing
		// PhaseScript builders remain translation evidence.
		bool init(const PSInit& init, const PhaseScript& program);

		// Run the program once for a given job
		PSResult run(const PSJob& job);
		void SetVisualDebugMode(bool enabled);
		void SetVisualDebugPaused(bool paused);
		void StepVisualDebugVmOnce();
		bool IsVisualDebugVmPaused() const;
		bool IsRunUntilBpActive() const;

	private:
		enum class DispatchResult {
			Continue,
			Returned,
			Failed,
		};

		savor::DolphinWrapper& host_;
		const BreakpointMap& bpmap_;
		std::vector<BPKey> canonical_bp_keys_;
		std::vector<BPKey> gated_bp_keys_;
		std::vector<BPKey> predicate_bp_keys_;
		PhaseScript prog_;
		PSInit init_;
		std::vector<uint32_t> armed_pcs_;
		bool visual_debug_mode_{ false };
		std::atomic<bool> visual_debug_paused_{ false };
		std::atomic<uint32_t> visual_debug_vm_step_budget_{ 0 };
		std::atomic<bool> run_until_bp_active_{ false };
		bool input_macro_session_active_{ false };
		std::vector<BPKey> input_macro_enabled_bp_keys_;
		std::vector<BPKey> input_macro_provider_keys_;
		bool armed_{ false };
		struct RunUntilBpSpec {
			std::vector<BPKey> expected_bp_keys;
			GCInputFrame input{};
			bool apply_input{ false };
			bool release_input{ false };
			bool hold_input_through_hit_opcode{ false };
			bool step_off_current_bp{ false };
			bool expected_only_scope{ false };
			bool watch_movie{ true };
			bool include_gated_hit_lookup{ false };
			bool update_derived{ true };
			bool track_input_poll{ false };
		};

		struct RunUntilBpCoreResult {
			DolphinWrapper::RunUntilHitResult run{};
			RunToBpOutcome outcome{ RunToBpOutcome::Unknown };
			uint32_t hit_bp_key{ 0 };
			bool expected_match{ false };
			uint64_t input_epoch{ 0 };
			GCInputFrame requested_input{};
			uint32_t input_poll_count{ 0 };
			bool input_acknowledged{ false };
		};

		std::unique_ptr<inputmacro::InputMacroRuntime> input_macro_runtime_;
		std::unique_ptr<inputmacro::IInputMacroPlanDriver> input_macro_plan_driver_;
		enum class InputMacroContextSink : std::uint8_t {
			None,
			BattleCompletion,
			BattleResultsScreen,
		};
		InputMacroContextSink input_macro_context_sink_{InputMacroContextSink::None};
		PSContext* active_input_macro_context_{ nullptr };
		uint64_t input_macro_stop_sequence_{ 0 };
		inputmacro::InputMacroStopInfo current_input_macro_stop_{};

		// helpers
		void arm_bps_once();
		void restore_canonical_breakpoint_scope();
		bool configure_capture_from_context(PSContext& ctx, PSResult& result);
		void cancel_input_macro();
		bool fail_legacy_service(
			PSResult& result,
			PSContext& ctx,
			const char* diagnostic) const;


		bool compare_u32(uint32_t lhs, PSCmp cmp, uint32_t rhs) const;
		void jump_to_label_if_exists(const std::string& label, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const;
		void wait_for_visual_debug_gate();
		RunUntilBpCoreResult run_until_bp_core(PSContext& ctx, const RunUntilBpSpec& spec);
		DispatchResult dispatch_op(
			const PSOp& op,
			PSContext& ctx,
			PSResult& result,
			KeyHostRouter& router,
			const std::unordered_map<std::string, size_t>& label_vm_pc_map,
			size_t& vm_pc,
			std::string& section);

		bool op_arm_phase_bps_once();
		bool op_load_snapshot(PSResult& result, PSContext& ctx);
		bool op_capture_snapshot(PSResult& result, PSContext& ctx);
		bool op_reboot_core(PSResult& result, PSContext& ctx);
		void op_label() const;
		void op_goto(const PSOp& op, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const;
		void op_goto_if(const PSOp& op, PSContext& ctx, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const;
		void op_goto_if_keys(const PSOp& op, PSContext& ctx, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const;
		void op_set_u32(const PSOp& op, PSContext& ctx) const;
		void op_add_u32(const PSOp& op, PSContext& ctx) const;
		void op_build_turn_inputplan_from_battle_path(PSContext& ctx) const;
		void op_materialize_battle_macro_steps(PSContext& ctx);
		void op_materialize_battle_turn_macro_steps(PSContext& ctx);
		void op_materialize_battle_results_screen_macro_steps(PSContext& ctx);
		void op_materialize_battle_completion_macro_steps(PSContext& ctx);
		void op_execute_battle_macro_step(PSContext& ctx);
		void start_prepared_battle_macro(
			inputmacro::BattleCommandInputMacroProvider::PrepareResult prepared,
			PSContext& ctx,
			bool authored_turn);
		void apply_input_macro_step_result(
			const inputmacro::InputMacroStepResult& step_result,
			PSContext& ctx);
		void sync_input_macro_driver_context(PSContext& ctx) const;
		void sync_battle_results_screen_context(PSContext& ctx) const;
		void sync_battle_completion_context(PSContext& ctx) const;
		void op_apply_battle_inputplan_frames(PSContext& ctx);
		void op_step_frames(const PSOp& op);
		void op_step_opcode(const PSOp& op);
		bool op_start_deterministic_run(PSResult& result, PSContext& ctx) const;
		bool op_end_deterministic_run(PSResult& result, PSContext& ctx) const;
		void op_run_until_bp(PSContext& ctx);
		bool op_run_until_bp_key(const PSOp& op, PSResult& result, PSContext& ctx);
		void op_run_until_debug_stop(PSContext& ctx);
		bool op_arm_memory_watchpoint(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_clear_memory_watchpoints(PSResult& result, PSContext& ctx) const;
		bool op_arm_capture_memory_watchpoints(PSResult& result, PSContext& ctx);
		void op_record_current_bp(PSContext& ctx);
		bool op_record_current_pc_to(
			const PSOp& op,
			PSResult& result,
			PSContext& ctx) const;
		void op_record_tas_input_sample(PSContext& ctx);
		bool op_read_u8(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_read_u16(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_read_u32(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_write_u32(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_read_f32(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_read_f64(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_capture_seed_override(PSResult& result, PSContext& ctx);
		void op_get_battle_context(PSResult& result, PSContext& ctx) const;
		bool op_get_navigation_context(PSResult& result, PSContext& ctx) const;
		void op_emit_result(const PSOp& op, PSResult& result, PSContext& ctx) const;
		bool op_return_result(const PSOp& op, PSResult& result, PSContext& ctx) const;
		bool op_apply_input_from(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_movie_play_from(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_movie_stop(PSResult& result, PSContext& ctx);
		bool op_save_savestate_from(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_require_disc_gameid_from(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_arm_bps_from_pred_table(PSResult& result, PSContext& ctx);
		void op_capture_pred_baselines(PSContext& ctx, KeyHostRouter& router);
		void op_eval_predicates_at_hit_bp(PSContext& ctx, KeyHostRouter& router);

		// IInputMacroHost
		bool acquire_exclusive_session(std::span<const BPKey> provider_keys) override;
		void release_exclusive_session() override;
		inputmacro::BreakpointWaitResult run_to_breakpoints(
			const inputmacro::BreakpointWaitAction& action) override;
		inputmacro::InputMacroHostStatus step_neutral_frames(uint32_t frame_count) override;
		bool read_u32(uint32_t address, uint32_t& value) override;
		inputmacro::MemoryChangeResult wait_for_u32_change(
			uint32_t address,
			uint32_t baseline) override;
		void set_neutral_input() override;
		void clear_macro_memory_watchpoints() override;
		void restore_breakpoint_state() override;

		// IInputMacroDriverHost
		inputmacro::InputMacroStopInfo current_stop() const override;
		bool read_guest_memory(
			uint32_t address,
			std::span<std::byte> output) const override;
		std::uint64_t current_vi() const override;

		// IBattleCommandInputMacroProviderHost
		BPKey current_breakpoint_key() const override;
		inputmacro::BattleCommandProviderWaitResult wait_for_breakpoints(
			std::span<const BPKey> expected_keys) override;
		bool capture_mem1(std::string& out_mem1) override;

		// typed reads
		bool read_u8(uint32_t a, uint8_t& v)  const { return host_.readU8(a, v); }
		bool read_u16(uint32_t a, uint16_t& v) const { return host_.readU16(a, v); }
		bool read_u32(uint32_t a, uint32_t& v) const { return host_.readU32(a, v); }
		bool read_f32(uint32_t a, float& v)    const { return host_.readF32(a, v); }
		bool read_f64(uint32_t a, double& v)   const { return host_.readF64(a, v); }

		// ---- Key-based reads (prefer keys; raw-address helpers remain available) ----
		bool read_u8(addr::AddrKey k, uint8_t& out) { return host_.readByKey(k, out); }
		bool read_u16(addr::AddrKey k, uint16_t& out) { return host_.readByKey(k, out); }
		bool read_u32(addr::AddrKey k, uint32_t& out) { return host_.readByKey(k, out); }
		bool read_u64(addr::AddrKey k, uint64_t& out) { return host_.readByKey(k, out); }
		// Convenience for width-aware fetch (1,2,4,8) into 64-bit; returns width in out_width.
		bool read_any(addr::AddrKey k, uint8_t width, uint64_t& out, uint8_t& out_width) { return host_.readByKeyAny(k, width, out, out_width); }

		struct PlanView { const GCInputFrame* frames{ nullptr }; uint32_t count{ 0 }; };
		std::vector<PlanView> plan_table_;
		uint32_t last_plan_id_{ UINT32_MAX };

		std::unique_ptr<savor::IDerivedBuffer> derived_; // active derived buffer provider for this program
	};

} // namespace savor
