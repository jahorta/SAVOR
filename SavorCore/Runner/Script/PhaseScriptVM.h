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
#include "Core/Common/Buffer.h"
#include "CtxRegistry.h"
#include "PSContext.h"
#include "PhaseScriptProgram.h"
#include "../InputMacro/IInputMacroHost.h"
#include "../InputMacro/Providers/BattleCommandInputMacroProvider.h"

namespace savor {
	namespace inputmacro {
		class InputMacroRuntime;
		struct InputMacroStepResult;
	}


	// ----- VM -----
	class PhaseScriptVM :
		private inputmacro::IInputMacroHost,
		private inputmacro::IBattleCommandInputMacroProviderHost {
	public:
		PhaseScriptVM(savor::DolphinWrapper& host, const BreakpointMap& bpmap);
		~PhaseScriptVM();

		// Load state, build phase, arm bps, capture "prebattle" snapshot
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
		Common::UniqueBuffer<u8> snapshot_;

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
			uint32_t poll_ms_override{ 0 };
		};

		struct RunUntilBpCoreResult {
			DolphinWrapper::RunUntilHitResult run{};
			RunToBpOutcome outcome{ RunToBpOutcome::Unknown };
			uint32_t hit_bp_key{ 0 };
			bool expected_match{ false };
			uint32_t elapsed_ms{ 0 };
		};

		std::unique_ptr<inputmacro::InputMacroRuntime> input_macro_runtime_;
		PSContext* active_input_macro_context_{ nullptr };

		// helpers
		void arm_bps_once();
		void restore_canonical_breakpoint_scope();
		bool configure_capture_from_context(const PSContext& ctx, PSResult& result);
		void cancel_input_macro();
		bool save_snapshot();
		bool load_snapshot();


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
		bool op_load_snapshot(PSContext& ctx);
		bool op_capture_snapshot();
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
		void op_execute_battle_macro_step(PSContext& ctx);
		void start_prepared_battle_macro(
			inputmacro::BattleCommandInputMacroProvider::PrepareResult prepared,
			PSContext& ctx,
			bool authored_turn);
		void apply_input_macro_step_result(
			const inputmacro::InputMacroStepResult& step_result,
			PSContext& ctx);
		void op_apply_battle_inputplan_frames(PSContext& ctx);
		void op_step_frames(const PSOp& op);
		void op_step_opcode(const PSOp& op);
		void op_start_deterministic_run() const;
		void op_end_deterministic_run() const;
		void op_run_until_bp(PSContext& ctx);
		bool op_run_until_bp_key(const PSOp& op, PSResult& result, PSContext& ctx);
		void op_run_until_debug_stop(PSContext& ctx);
		bool op_arm_memory_watchpoint(const PSOp& op, PSResult& result, PSContext& ctx);
		void op_clear_memory_watchpoints() const;
		bool op_arm_capture_memory_watchpoints(PSResult& result, PSContext& ctx);
		void op_record_current_bp(PSContext& ctx);
		void op_record_tas_input_sample(PSContext& ctx);
		bool op_read_u8(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_read_u16(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_read_u32(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_write_u32(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_read_f32(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_read_f64(const PSOp& op, PSResult& result, PSContext& ctx);
		bool op_capture_seed_override(PSResult& result, PSContext& ctx);
		void op_get_battle_context(PSResult& result, PSContext& ctx) const;
		void op_emit_result(const PSOp& op, PSResult& result, PSContext& ctx) const;
		bool op_return_result(const PSOp& op, PSResult& result, PSContext& ctx) const;
		bool op_apply_input_from(const PSOp& op, PSResult& result, PSContext& ctx);
		void op_set_timeout(const PSOp& op, PSContext& ctx) const;
		void op_set_timeout_from(const PSOp& op, PSContext& ctx) const;
		bool op_movie_play_from(const PSOp& op, PSResult& result, PSContext& ctx);
		void op_movie_stop();
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
			uint32_t baseline,
			uint32_t timeout_ms) override;
		void set_neutral_input() override;
		void clear_macro_memory_watchpoints() override;
		void restore_breakpoint_state() override;

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
