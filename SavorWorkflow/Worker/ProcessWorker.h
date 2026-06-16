#pragma once
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <windows.h>
#include "Core/Input/InputPlan.h"
#include "Runner/Script/PhaseScriptVM.h"  // for PSResult
#include "TSQueue.h"
#include "Runner/Parallel/PRTypes.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "WorkerTelemetry.h"


namespace savor {

		struct ProcStartParams {
			size_t worker_id{ 0 };
			std::string exe_path;     // path to SavorCoreSandbox.exe
			std::string iso_path;
			std::string dolphin_base_dir; // optional; worker derives its base from exe_dir when empty
			std::string user_dir;     // unique per worker
			bool vm_control{ false };
			bool visual{ false };
			bool visual_debug{ false };
			uint64_t render_widget_handle{ 0 };
			std::string visual_control_pipe_name;
			std::string visual_host_events_pipe_name;
			std::string visual_screenshot_dir;
		};

	struct AckWait
	{
		std::mutex m;
		std::condition_variable cv;
		bool pending = false;
		char expect_code = 0;  // 'S','I','A'
		bool have = false;
		bool ok = false;
		void request(char code) {
			std::lock_guard<std::mutex> lk(m);
			expect_code = code;
			pending = true;
			have = false;
			ok = false;
		}
		bool wait_for(uint32_t timeout_ms) {
			std::unique_lock<std::mutex> lk(m);
			if (!pending) return false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms ? timeout_ms : 30000);
			cv.wait_until(lk, deadline, [&] { return have == true; });
			bool r = have && ok;
			pending = false;
			return r;
		}
		void fulfill(char code, bool ok_flag) {
			std::lock_guard<std::mutex> lk(m);
			if (pending && code == expect_code) {
				ok = ok_flag;
				have = true;
				cv.notify_all();
			}
		}
		void cancel_all() {
			std::lock_guard<std::mutex> lk(m);
			pending = false;
			have = true;
			ok = false;
			cv.notify_all();
		}
	};

	class ProcessWorker {
	public:
		ProcessWorker() = default;
		~ProcessWorker();

		bool start(ProcStartParams& p, TSQueue<PRResult>* out_queue);
		bool send_job(uint64_t job_id, uint64_t epoch, const PSJob& job);
		void stop();

		bool ctl_set_program(uint8_t init_kind, uint8_t main_kind, const PSInit& init);
		bool ctl_run_init_once();
		bool ctl_activate_main();
		bool visual_pause_emulation();
		bool visual_resume_emulation();
		bool visual_step_vm();

		bool is_ready()  const { return ready_received_.load() && ready_ok_.load(); }
		bool is_failed() const { return ready_received_.load() && !ready_ok_.load(); }
		bool is_running() const { return running_.load(); }
		uint32_t ready_error() const { return ready_error_.load(); }
		HANDLE process_handle() const { return hProcess; }

		bool try_acquire_slot() {
			bool expected = false;
			return busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
		}
		void release_slot() { busy_.store(false, std::memory_order_release); }
		bool has_slot() const { return !busy_.load(std::memory_order_acquire); }

		bool wait_ready(uint32_t timeout_ms);

		void set_progress_queue(TSQueue<PRProgress>* q) { progress_out_ = q; }

		bool try_get_last_progress(PRProgress& out) const
		{
			std::lock_guard<std::mutex> lk(progress_m_);
			if (!have_progress_) return false;
			out = last_progress_;
			return true;
		}

		int64_t GetPid() const {
			return (int64_t)dwProcessId;
		}

	private:
		enum class VisualControlCommand : uint8_t {
			Pause,
			Resume,
			VmStep
		};

		bool send_visual_control_command(VisualControlCommand command);
		void reader_thread();

		HANDLE hChildStd_IN_Wr{ NULL };  // parent writes jobs here
		HANDLE hChildStd_OUT_Rd{ NULL }; // parent reads results here
		HANDLE hProcess{ NULL };
		HANDLE hThread{ NULL };
		unsigned long dwProcessId{ 0 };
		HANDLE hJob{ 0 };

		std::thread reader_;
		TSQueue<PRResult>* out_{ nullptr };
		size_t id_{ 0 };
		std::atomic<bool> running_{ false };
		
		std::atomic<bool> busy_{ false };           // true if a job is in-flight
		std::atomic<bool> ready_received_{ false }; // we saw MSG_READY
		std::atomic<bool> ready_ok_{ false };       // MSG_READY.ok
		std::atomic<uint32_t> ready_error_{ 0 };    // MSG_READY.error

		AckWait ack_;

		// Progress storage (per worker, last only)
		mutable std::mutex progress_m_;
		PRProgress last_progress_{};
		bool have_progress_{ false };
		TSQueue<PRProgress>* progress_out_{ nullptr };
		std::string visual_control_pipe_name_{};
		std::string visual_host_events_pipe_name_{};
		mutable std::mutex visual_pipe_m_;
		HANDLE hVisualControlPipe_{ INVALID_HANDLE_VALUE };

	};

} // namespace savor
