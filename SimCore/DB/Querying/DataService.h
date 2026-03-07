#pragma once
#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "Paging.h"
#include "PagedQuery.h"
#include "JobListDTO.h"
#include "JobEventListDTO.h"
#include "JobSetListDTO.h"
#include "IdRepoListDTO.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../../Utils/IniDoc.h"
#include "../ProgramDB/IProgramDBCodec.h"
#include "../DBCore/ObjectStore.h"
#include "../BattleContextRepo.h"
#include "../AuthoringTemplatesRepo.h"
#include "../TurnActionPresetRepo.h"
#include "../UiConfigRowRepo.h"
#include "../PredicateSpecRepo.h"
#include "SnapshotMailbox.h"
#include <future>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

namespace simcore::db {

    struct ProgramKindKV { int id; std::string name; };

    struct UiConfigRowDTO { int64_t id{}, preset_id{}; int32_t turn_index{}, actor_slot{}; int64_t created_at{}; };

    struct ArtifactRefLite {
        int64_t artifact_id{};
        std::string role;
        // optional enrich
        std::string filename;
        uint64_t size_bytes{};
        int compression{};
        int64_t created_at{};
    };

    struct ArtifactIniBuilder {
        static constexpr const char* SECTION_NAME = "artifacts";

        std::vector<std::pair<std::string, int64_t>> artifact_list{};

        static inline std::vector<ArtifactRefLite> parse_from_ini(const std::string ini_str) {
            std::vector<ArtifactRefLite> out;
            IniDoc ini = IniDoc::parse(ini_str);
            if (!ini.has_section("artifacts")) return out;
            const IniKV& kv = ini.section_kv("artifacts");
            uint32_t count = kv.get_u32("count", 0);
            if (count > 0) {
                out.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    std::string rk = "role." + std::to_string(i);
                    std::string ik = "id." + std::to_string(i);
                    ArtifactRefLite a{};
                    a.role = kv.get(rk, "");
                    a.artifact_id = kv.get_i64(ik, -1);
                    if (a.artifact_id > 0) out.push_back(std::move(a));
                }
            }
            return out;
        }

        inline void add_artifact(const std::string role, const int64_t id) {
            artifact_list.emplace_back(role, id);
        }

        inline std::string to_string() {
            IniDoc ini{};
            ini.ensure_section(SECTION_NAME);
            ini.set(SECTION_NAME, "count", std::to_string(artifact_list.size()));
            for (int i = 0; i < artifact_list.size(); i++) {
                auto& [role, id] = artifact_list[i];
                std::string rk = "role." + std::to_string(i);
                ini.set(SECTION_NAME, rk, role);
                std::string ik = "id." + std::to_string(i);
                ini.set(SECTION_NAME, ik, std::to_string(id));
            }
            return ini.to_string_sorted();
        }
    };

    class DataService {
    public:
        static std::future<DbResult<Page<JobLite>>>      FetchJobsPage(const JobsListScope& scope, const PagedQuery<>& q, RetryPolicy rp = {});
        static std::future<DbResult<Page<JobEventLite>>> FetchJobEventsPage(const JobEventsListScope& scope, const PagedQuery<>& q, RetryPolicy rp = {});
        static std::future<DbResult<Page<JobSetLite>>>   FetchJobSetsPage(const JobSetsListScope& scope, const PagedQuery<>& q, RetryPolicy rp = {});

        class PollHandle {
        public:
            virtual ~PollHandle() = default;
            virtual void stop() = 0;
            virtual void nudge() = 0;
        };

        static std::unique_ptr<PollHandle> StartJobsPolling(
            JobsListScope scope,
            std::chrono::milliseconds interval,
            int page_limit,
            SnapshotMailbox<Page<JobLite>>& out,
            RetryPolicy rp = {}
        );
        static std::unique_ptr<PollHandle> StartJobEventsPolling(
            JobEventsListScope scope,
            std::chrono::milliseconds interval,
            int page_limit,
            SnapshotMailbox<Page<JobEventLite>>& out,
            RetryPolicy rp = {}
        );

        static std::unique_ptr<PollHandle> StartJobSetsPolling(
            JobSetsListScope scope,
            std::chrono::milliseconds interval,
            int page_limit,
            SnapshotMailbox<Page<JobSetLite>>& out,
            RetryPolicy rp = {}
        );
        static std::future<DbResult<Page<JobLite>>> FetchJobsPageAsync(
            const JobsListScope& scope,
            std::optional<KeysetCursor> before,
            std::optional<KeysetCursor> after,
            int limit,
            RetryPolicy rp = {}
        );
        static std::future<DbResult<std::vector<JobEventsRepo::JobIdPayload>>> BulkLatestProgressByJobsAsync(
            const std::vector<int64_t>& job_ids, RetryPolicy rp = {});


        
        static std::future<DbResult<std::vector<ProgramKindKV>>> ListProgramKindsAsync(RetryPolicy rp = {});
        static std::future<DbResult<IniDoc>> FetchJobVmKvIniAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<IniDoc>> FetchJobResultsIniAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<std::string>> FetchDecodedProgressAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<ArtifactRefLite>>> FetchJobArtifactRefsAsync(int64_t job_id, RetryPolicy rp = {});

        static std::future<DbResult<void>> RequeueJobAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> CancelJobAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> BumpPriorityAsync(int64_t job_id, int delta, RetryPolicy rp = {});

        static std::future<DbResult<int64_t>> CreateJobSetAsync(
            std::optional<std::string> purpose,
            int program_kind,
            std::optional<std::string> created_by = {},
            std::optional<std::string> domain_ref_kind = {},
            std::optional<int64_t>    domain_ref_id = {},
            std::optional<std::string> meta_text = {},
            std::optional<int64_t>    expected_total = {},
            RetryPolicy rp = {});

        static std::future<DbResult<int64_t>> EncodeJobSetWithCodecAsync(
            int program_kind,
            int64_t job_set_id,
            const std::string& ini_sorted,
            RetryPolicy rp = {});

        static inline std::future<DbResult<int64_t>> EncodeJobSetWithCodecAsync(
            int program_kind,
            int64_t job_set_id,
            const IniDoc& ini,
            RetryPolicy rp = {})
        {
            return EncodeJobSetWithCodecAsync(program_kind, job_set_id, ini.to_string_sorted(), rp);
        }

        static std::future<DbResult<void>> SetJobSetExpectedTotalAsync(
            int64_t job_set_id,
            std::optional<int64_t> expected_total,
            RetryPolicy rp = {});

        static std::future<DbResult<Page<SavestateLite>>>       FetchSavestatesPage(const PagedQuery<>& q, const std::string& search, RetryPolicy rp = {});
        static std::future<DbResult<Page<SeedProbeLite>>>       FetchSeedProbesPage(const PagedQuery<>& q, const std::string& search, bool only_done, std::optional<int64_t> filter_savestate_id = std::nullopt, RetryPolicy rp = {});
        static std::future<DbResult<Page<TasMovieLite>>>        FetchTasMoviesPage(const PagedQuery<>& q, const std::string& search, bool only_done, RetryPolicy rp = {});
        static std::future<DbResult<Page<ExplorerSettingsLite>>> FetchExplorerSettingsPage(const PagedQuery<>& q, const std::string& search, RetryPolicy rp = {});
        static std::future<DbResult<Page<ObjectRefLite>>>       FetchObjectRefsPage(const PagedQuery<>& q, const std::string& search, const std::string& ext_filter, RetryPolicy rp = {});

        // SeedProbe -> Savestate
        static std::future<DbResult<int64_t>> GetSavestateForSeedProbeAsync(int64_t seed_probe_id, RetryPolicy rp = {});
        static inline DbResult<int64_t> GetSavestateForSeedProbe(int64_t seed_probe_id) { return GetSavestateForSeedProbeAsync(seed_probe_id).get(); }


        // Latest context for savestate
        static std::future<DbResult<std::optional<simcore::db::BattleContextRow>>> GetLatestBattleContextForSavestateAsync(int64_t savestate_id, RetryPolicy rp = {});
        static inline DbResult<std::optional<simcore::db::BattleContextRow>> GetLatestBattleContextForSavestate(int64_t savestate_id) { return GetLatestBattleContextForSavestateAsync(savestate_id).get(); }
        static std::future<DbResult<int64_t>>GetNewBattleContextAsync(const std::string& ini_string, RetryPolicy rp = {});

        // Authoring templates
        static std::future<DbResult<int64_t>> SaveAuthoringTemplateAsync(const simcore::db::AuthoringTemplateRow& r, bool is_update, RetryPolicy rp = {});
        static std::future<DbResult<simcore::db::AuthoringTemplateRow>> LoadAuthoringTemplateAsync(int64_t template_id, RetryPolicy rp = {});

        // Presets
        static std::future<DbResult<std::vector<simcore::db::TurnActionPresetLite>>> ListActionPresetsAsync(const std::string& search, int32_t limit, RetryPolicy rp = {});
        static std::future<DbResult<int64_t>> SaveActionPresetAsync(const simcore::db::TurnActionPresetRow& r, bool is_update, RetryPolicy rp = {});

        static std::future<DbResult<std::vector<int64_t>>> InsertUiConfigRowsAsync(const std::vector<UiConfigRow>& rows, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<UiConfigRow>>> GetUiConfigRowsByIdsAsync(const std::vector<int64_t>& ids, RetryPolicy rp = {});

        static std::future<DbResult<std::vector<simcore::db::PredicateSpecLite>>>
            ListPredicateSpecsAsync(const std::string& search, int32_t limit, RetryPolicy rp = {});

        static std::future<DbResult<void>> SetJobVmKvAsync(int64_t job_id, std::optional<std::string> vm_kv, RetryPolicy rp = {});


    private:
        class JobsPoller;
        class JobSetsPoller;
        class JobEventsPoller;
    };

} // namespace simcore::db
