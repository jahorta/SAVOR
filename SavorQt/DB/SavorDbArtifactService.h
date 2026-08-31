#pragma once

#include <atomic>
#include <cstdint>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include "SavorDbRuntime.h"
#include "DB/SavorDbServiceResult.h"
#include "Common/Types/UtcTimestamp.h"
#include "State/IStateDb.h"
#include "UIRead/IUiReadDb.h"
#include "Utils/Hash.h"

namespace savorqt::db {

struct ArtifactImportRequest {
    std::filesystem::path source_path;
    std::string filename;
    std::string artifact_kind;
    int compression_kind = 0;
};

class SavorDbArtifactService {
public:
    static bool StorageReady() {
        return savorqt::SavorDbRuntime::instance().isRunning()
            && !ObjectStoreRoot().empty();
    }

    static ServiceResult<savor::db::UiReadPage<savor::db::UiArtifactSummary>> ListArtifacts(
        const savor::db::UiReadArtifactListQuery& query) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<savor::db::UiReadPage<savor::db::UiArtifactSummary>>(kSavorDbRuntimeUnavailableMessage);
        }
        return ServiceResult<savor::db::UiReadPage<savor::db::UiArtifactSummary>>::Ok(db->ListArtifacts(query));
    }

    static ServiceResult<savor::db::UiArtifactSummary> ImportArtifact(const ArtifactImportRequest& request) {
        auto* state_db = StateDb();
        if (state_db == nullptr) {
            return Unavailable<savor::db::UiArtifactSummary>(kSavorDbRuntimeUnavailableMessage);
        }
        if (request.source_path.empty()) {
            return Invalid<savor::db::UiArtifactSummary>("source path is required");
        }
        if (!std::filesystem::exists(request.source_path) || !std::filesystem::is_regular_file(request.source_path)) {
            return Invalid<savor::db::UiArtifactSummary>("source path is not a readable file");
        }

        const std::string display_filename = SanitizeFilename(
            request.filename.empty()
                ? request.source_path.filename().string()
                : request.filename);
        if (display_filename.empty()) {
            return Invalid<savor::db::UiArtifactSummary>("filename is required");
        }

        std::string sha;
        try {
            sha = hash::sha256_of_file(request.source_path.string());
        } catch (const std::exception& ex) {
            return Failed<savor::db::UiArtifactSummary>(ex.what());
        }
        if (sha.empty()) {
            return Failed<savor::db::UiArtifactSummary>("failed computing artifact SHA-256");
        }

        std::error_code ec;
        const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(request.source_path, ec));
        if (ec) {
            return Failed<savor::db::UiArtifactSummary>("failed reading artifact size: " + ec.message());
        }

        const auto now = savor::db::types::UtcNow();
        savor::db::ImportExternalArtifactCommand command{};
        command.absolute_source_path = std::filesystem::absolute(request.source_path);
        command.artifact.sha256 = sha;
        command.artifact.size_bytes = static_cast<std::int64_t>(size);
        command.artifact.compression_kind = request.compression_kind;
        command.artifact.display_filename = display_filename;
        command.artifact.file_ext = NormalizeFileExt(request.source_path.extension().string());
        command.artifact.artifact_kind = NormalizeArtifactKind(
            request.artifact_kind, command.artifact.file_ext);
        command.artifact.created_at_utc = now;
        command.artifact.correlation_id = NextEventId("State.ArtifactStored.Qt2");
        command.artifact.causation_id = "SavorQt";

        std::int64_t artifact_id = 0;
        std::string error;
        if (!state_db->ImportExternalArtifact(command, &artifact_id, &error)) {
            return Failed<savor::db::UiArtifactSummary>(error);
        }

        savor::db::UiArtifactSummary summary{};
        summary.artifact_id = artifact_id;
        summary.sha256 = command.artifact.sha256;
        summary.size_bytes = size;
        summary.artifact_kind = command.artifact.artifact_kind;
        summary.filename = display_filename;
        summary.created_at_utc = now.time_since_epoch().count();
        return ServiceResult<savor::db::UiArtifactSummary>::Ok(std::move(summary));
    }

    static ServiceResult<void> MaterializeArtifactToPath(
        std::int64_t artifact_id,
        const std::filesystem::path& output_path) {
        auto* db = StateDb();
        if (db == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage });
        }
        if (artifact_id <= 0 || output_path.empty()) {
            return ServiceResult<void>::Err({ ServiceErrorKind::InvalidInput, "artifact_id and output path are required" });
        }

        std::string error;
        const auto materialized = db->MaterializeArtifactToPath(artifact_id, output_path.string(), &error);
        if (!materialized.has_value()) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<std::string> ReadArtifactText(std::int64_t artifact_id) {
        auto* db = StateDb();
        if (db == nullptr) {
            return Unavailable<std::string>(kSavorDbRuntimeUnavailableMessage);
        }
        if (artifact_id <= 0) {
            return Invalid<std::string>("artifact_id is required");
        }

        const auto temp_path = std::filesystem::temp_directory_path()
            / ("savorqt-artifact-" + NextEventId("read") + ".tmp");
        std::string error;
        const auto materialized = db->MaterializeArtifactToPath(artifact_id, temp_path.string(), &error);
        if (!materialized.has_value()) {
            return Failed<std::string>(std::move(error));
        }

        std::ifstream in(temp_path, std::ios::binary);
        if (!in.is_open()) {
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);
            return Failed<std::string>("failed opening materialized artifact text");
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        if (!in.good() && !in.eof()) {
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);
            return Failed<std::string>("failed reading materialized artifact text");
        }

        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
        return ServiceResult<std::string>::Ok(buffer.str());
    }

private:
    static savor::db::IStateDb* StateDb() {
        return savorqt::SavorDbRuntime::instance().stateDb();
    }

    static savor::db::IUiReadDb* UiReadDb() {
        return savorqt::SavorDbRuntime::instance().uiReadDb();
    }

    static std::filesystem::path ObjectStoreRoot() {
        const auto root = savorqt::SavorDbRuntime::instance().root();
        return root.empty() ? std::filesystem::path{} : root / "object_store";
    }

    static std::string SanitizeFilename(std::string filename) {
        for (char& ch : filename) {
            if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
                ch = '_';
            }
        }
        while (!filename.empty() && (filename.back() == ' ' || filename.back() == '.')) {
            filename.pop_back();
        }
        return filename;
    }

    static std::string NormalizeFileExt(std::string ext) {
        if (ext.empty()) return ".bin";
        if (ext.front() != '.') ext = "." + ext;
        for (char& ch : ext) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return ext;
    }

    static std::string NormalizeArtifactKind(std::string artifact_kind, const std::string& file_ext) {
        for (char& ch : artifact_kind) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (artifact_kind == "DTM" || artifact_kind == "DTMINI"
            || artifact_kind == "TAS_MOVIE_ITINERARY"
            || artifact_kind == "SAV" || artifact_kind == "LOG"
            || artifact_kind == "OTHER") {
            return artifact_kind;
        }
        if (file_ext == ".dtm") return "DTM";
        if (file_ext == ".dtmini") return "DTMINI";
        if (file_ext == ".tmi") return "TAS_MOVIE_ITINERARY";
        if (file_ext == ".sav") return "SAV";
        if (file_ext == ".log" || file_ext == ".txt") return "LOG";
        return "OTHER";
    }

    static std::string NextEventId(const char* prefix) {
        static std::atomic<std::uint64_t> counter{ 1 };
        const auto now = savor::db::types::UtcNow().time_since_epoch().count();
        return std::string(prefix) + "." + std::to_string(now) + "." + std::to_string(counter.fetch_add(1));
    }

    template <typename T>
    static ServiceResult<T> Unavailable(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Unavailable, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> Invalid(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::InvalidInput, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> Failed(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Failed, std::move(message) });
    }
};

} // namespace savorqt::db

