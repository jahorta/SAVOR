// TASMovieDBEnqueue.cpp
#include "TASMovieDBEnqueue.h"
#include <iostream>
#include <filesystem>
#include <string>

#include "utils.h"
#include "DB/DBCore/DbService.h"
#include "DB/DBCore/ObjectStore.h"
#include "Phases/Coordinators/DBTasMovieCoordinator.h"

namespace sandbox {

    static bool try_parse_int(const std::string& s, int& out) {
        try { out = std::stoi(s); return true; }
        catch (...) { return false; }
    }
    static bool try_parse_u32(const std::string& s, uint32_t& out) {
        try {
            unsigned long v = std::stoul(s);
            if (v > 0xFFFFFFFFul) return false;
            out = static_cast<uint32_t>(v);
            return true;
        }
        catch (...) { return false; }
    }

    void menu_tas_movie_db_enqueue(AppState& g)
    {
        using simcore::db::ObjectStore;
        using simcore::db::Compression;
        using simcore::phase::TasMovieCoordinator;
        using simcore::phase::TasJobSpec;

        // Ensure DB is started (idempotent)
        simcore::db::DBService::instance().start();

        std::string dtm_path;
        int rtc_start = 0;
        int rtc_end_exclusive = 0;
        int priority = 0;
        uint32_t run_ms = 0;
        uint32_t vi_stall_ms = 60000; // sensible default
        bool progress_enable = true;

        for (;;) {
            std::cout << "\n--- TAS Movie (DB enqueue) ---\n";
            std::cout << "ObjectStore:  obj_dir=\"" << ObjectStore::ObjDir() << "\"\n";
            std::cout << "             tmp_dir=\"" << ObjectStore::TmpDir() << "\"\n";
            std::cout << "DTM path:           " << (dtm_path.empty() ? std::string("<unset>") : dtm_path) << "\n";
            std::cout << "RTC range:          [" << rtc_start << ", " << rtc_end_exclusive << ")  (end is exclusive)\n";
            std::cout << "Priority:           " << priority << "\n";
            std::cout << "Run duration (ms):  " << run_ms << "\n";
            std::cout << "VI stall (ms):      " << vi_stall_ms << "\n";
            std::cout << "Progress events:    " << (progress_enable ? "enabled" : "disabled") << "\n\n";

            std::cout << "1) Set DTM path\n";
            std::cout << "2) Set RTC start\n";
            std::cout << "3) Set RTC end (exclusive)\n";
            std::cout << "4) Set priority\n";
            std::cout << "5) Set run duration (ms)\n";
            std::cout << "6) Set VI stall (ms)\n";
            std::cout << "7) Toggle progress events\n";
            std::cout << "e) Enqueue jobs\n";
            std::cout << "b) Back\n> ";

            std::string c; if (!std::getline(std::cin, c)) return; c = trim(c);
            if (c == "b" || c == "B") return;

            if (c == "1") {
                dtm_path = prompt_path("DTM path: ", true, false, dtm_path).string();
            }
            else if (c == "2") {
                std::cout << "RTC start: ";
                std::string s; std::getline(std::cin, s);
                int v; if (try_parse_int(trim(s), v)) rtc_start = v; else std::cout << "Invalid integer.\n";
            }
            else if (c == "3") {
                std::cout << "RTC end (exclusive): ";
                std::string s; std::getline(std::cin, s);
                int v; if (try_parse_int(trim(s), v)) rtc_end_exclusive = v; else std::cout << "Invalid integer.\n";
            }
            else if (c == "4") {
                std::cout << "Priority: ";
                std::string s; std::getline(std::cin, s);
                int v; if (try_parse_int(trim(s), v)) priority = v; else std::cout << "Invalid integer.\n";
            }
            else if (c == "5") {
                std::cout << "Run duration (ms): ";
                std::string s; std::getline(std::cin, s);
                uint32_t v; if (try_parse_u32(trim(s), v)) run_ms = v; else std::cout << "Invalid number.\n";
            }
            else if (c == "6") {
                std::cout << "VI stall (ms): ";
                std::string s; std::getline(std::cin, s);
                uint32_t v; if (try_parse_u32(trim(s), v)) vi_stall_ms = v; else std::cout << "Invalid number.\n";
            }
            else if (c == "7") {
                progress_enable = !progress_enable;
            }
            else if (c == "e" || c == "E") {
                if (dtm_path.empty()) {
                    std::cout << "DTM path is required.\n";
                    continue;
                }
                if (!std::filesystem::exists(dtm_path)) {
                    std::cout << "DTM not found.\n";
                    continue;
                }
                if (rtc_end_exclusive < rtc_start) {
                    std::cout << "RTC end must be >= start.\n";
                    continue;
                }

                auto js = TasMovieCoordinator::SetupJobSet();
                if (!js.ok) {
                    std::cout << "[ERR] Failed to create job set: code=" << (int)js.error.kind << " msg=" << js.error.message << "\n";
                    continue;
                }
                const int64_t job_set_id = js.value;

                const std::filesystem::path p = std::filesystem::path(dtm_path);
                const std::string filename = p.filename().string();

                auto fin = ObjectStore::FinalizeFromFile(dtm_path, Compression::None, filename);
                if (!fin.ok) {
                    std::cout << "[ERR] Finalize DTM into ObjectStore failed: code=" << (int)fin.error.kind << " msg=" << fin.error.message << "\n";
                    continue;
                }

                TasJobSpec spec{};
                spec.base_dtm_id = fin.value.id; // artifact id
                spec.new_rtc_min = rtc_start;
                spec.new_rtc_max = rtc_end_exclusive;
                spec.priority = priority;
                spec.run_ms = run_ms;
                spec.vi_stall_ms = vi_stall_ms;
                spec.progress_enable = progress_enable;

                auto q = TasMovieCoordinator::QueueJob(job_set_id, spec);
                if (!q.ok) {
                    std::cout << "[ERR] QueueJob failed: code=" << (int)q.error.kind << " msg=" << q.error.message << "\n";
                    continue;
                }

                const int total = (rtc_end_exclusive > rtc_start) ? (rtc_end_exclusive - rtc_start) : 0;
                std::cout << "Enqueued TasMovie job set " << job_set_id << ": RTC in [" << rtc_start << ", " << rtc_end_exclusive << ") => up to " << total << " jobs.\n";
                std::cout << "(Idempotent: existing jobs by fingerprint will be reused.)\n";
            }
        }
    }
}

// SimCoreSandbox.cpp (patched excerpts)
// Add: #include "TASMovieDBEnqueue.h" near other includes
// In main(): start DBService and set CWD to exe_dir
// In menu_loop(): add a new option that calls sandbox::menu_tas_movie_db_enqueue(g);
