#include "DbSnapshotService.h"

#include "DbService.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include <zlib.h>

namespace simcore::db {
    namespace fs = std::filesystem;

    namespace {
        constexpr char kSnapshotMagic[] = "SOASNAP1";
        constexpr uint32_t kSnapshotVersion = 1;

        fs::path canonicalish(const fs::path& path)
        {
            std::error_code ec;
            const fs::path canon = fs::weakly_canonical(path, ec);
            return ec ? path.lexically_normal() : canon;
        }

        bool path_is_within(const fs::path& child, const fs::path& parent)
        {
            const fs::path childNorm = canonicalish(child);
            const fs::path parentNorm = canonicalish(parent);
            auto pit = parentNorm.begin();
            auto cit = childNorm.begin();
            for (; pit != parentNorm.end(); ++pit, ++cit) {
                if (cit == childNorm.end() || *cit != *pit) {
                    return false;
                }
            }
            return true;
        }

        bool remove_path_if_exists(const fs::path& target, std::string& error)
        {
            std::error_code ec;
            if (!fs::exists(target, ec)) {
                return true;
            }
            fs::remove_all(target, ec);
            if (ec) {
                error = "Failed to remove existing path: " + target.string();
                return false;
            }
            return true;
        }

        bool clear_directory_contents(const fs::path& root, std::string& error)
        {
            std::error_code ec;
            fs::create_directories(root, ec);
            if (ec) {
                error = "Failed to create target directory";
                return false;
            }
            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (ec) {
                    error = "Failed to enumerate target directory";
                    return false;
                }
                fs::remove_all(entry.path(), ec);
                if (ec) {
                    error = "Failed to clear target directory";
                    return false;
                }
            }
            return true;
        }

        bool copy_directory_contents(const fs::path& source, const fs::path& target, std::string& error)
        {
            std::error_code ec;
            fs::create_directories(target, ec);
            if (ec) {
                error = "Failed to create target directory";
                return false;
            }
            for (const auto& entry : fs::directory_iterator(source, ec)) {
                if (ec) {
                    error = "Failed to enumerate source directory";
                    return false;
                }
                fs::copy(entry.path(), target / entry.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    error = "Failed to copy existing database files";
                    return false;
                }
            }
            return true;
        }

        std::string build_snapshot_manifest(const fs::path& root, size_t artifactCount)
        {
            std::ostringstream out;
            out << "{\n"
                << "  \"version\": 1,\n"
                << "  \"database\": \"SoaSimDB.sqlite3\",\n"
                << "  \"artifacts_root\": \"objects\",\n"
                << "  \"includes_tmp\": false,\n"
                << "  \"artifact_files\": " << artifactCount << ",\n"
                << "  \"source_root\": \"" << root.string() << "\"\n"
                << "}\n";
            return out.str();
        }

        bool write_u32(std::ofstream& out, uint32_t value)
        {
            out.write(reinterpret_cast<const char*>(&value), sizeof(value));
            return out.good();
        }

        bool write_u64(std::ofstream& out, uint64_t value)
        {
            out.write(reinterpret_cast<const char*>(&value), sizeof(value));
            return out.good();
        }

        bool read_u32(std::ifstream& in, uint32_t& value)
        {
            in.read(reinterpret_cast<char*>(&value), sizeof(value));
            return in.good();
        }

        bool read_u64(std::ifstream& in, uint64_t& value)
        {
            in.read(reinterpret_cast<char*>(&value), sizeof(value));
            return in.good();
        }

        bool write_snapshot_entry(std::ofstream& out, const std::string& relativePath, const std::vector<unsigned char>& input, std::string& error)
        {
            uLongf bound = compressBound(static_cast<uLong>(input.size()));
            std::vector<unsigned char> compressed(bound);
            const int zrc = compress2(compressed.data(), &bound, input.data(), static_cast<uLong>(input.size()), Z_BEST_COMPRESSION);
            const bool useCompressed = (zrc == Z_OK) && (bound < input.size());
            const std::vector<unsigned char>& payload = useCompressed ? compressed : input;
            const uint64_t payloadSize = useCompressed ? static_cast<uint64_t>(bound) : static_cast<uint64_t>(input.size());
            const uint8_t flags = useCompressed ? 1 : 0;
            const uint32_t pathSize = static_cast<uint32_t>(relativePath.size());

            if (!write_u32(out, pathSize) || !write_u64(out, static_cast<uint64_t>(input.size())) || !write_u64(out, payloadSize)) {
                error = "Failed to write snapshot header";
                return false;
            }
            out.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
            out.write(relativePath.data(), static_cast<std::streamsize>(relativePath.size()));
            out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payloadSize));
            if (!out.good()) {
                error = "Failed to write snapshot payload";
                return false;
            }
            return true;
        }

        bool write_snapshot_file_entry(std::ofstream& out, const fs::path& sourcePath, const std::string& relativePath, std::string& error)
        {
            std::ifstream in(sourcePath, std::ios::binary | std::ios::ate);
            if (!in) {
                error = "Failed to open snapshot source file: " + sourcePath.string();
                return false;
            }
            const std::streamsize size = in.tellg();
            if (size < 0) {
                error = "Failed to read snapshot source file size: " + sourcePath.string();
                return false;
            }
            std::vector<unsigned char> bytes(static_cast<size_t>(size));
            in.seekg(0, std::ios::beg);
            if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
                error = "Failed to read snapshot source file: " + sourcePath.string();
                return false;
            }
            return write_snapshot_entry(out, relativePath, bytes, error);
        }
    } // namespace

    DbSnapshotResult DbSnapshotService::SaveSnapshot(const fs::path& snapshot_path)
    {
        DbSnapshotResult result;
        result.snapshot_path = snapshot_path;

        if (snapshot_path.empty()) {
            result.error = "Snapshot path is required";
            return result;
        }

        DBService& dbService = DBService::instance();
        const bool wasRunning = dbService.is_running();
        const fs::path root = dbService.database_root();
        result.active_root = root;

        if (wasRunning) {
            dbService.stop();
        }

        const fs::path dbFile = root / "SoaSimDB.sqlite3";
        const fs::path objectsRoot = root / "objects";

        std::error_code ec;
        if (!fs::exists(dbFile, ec) || !fs::is_regular_file(dbFile, ec)) {
            result.error = "Active database root does not contain SoaSimDB.sqlite3";
            if (wasRunning) {
                dbService.start();
            }
            return result;
        }

        std::vector<fs::path> objectFiles;
        if (fs::exists(objectsRoot, ec) && fs::is_directory(objectsRoot, ec)) {
            for (const auto& entry : fs::recursive_directory_iterator(objectsRoot, ec)) {
                if (ec) {
                    result.error = "Failed to enumerate object-store files";
                    if (wasRunning) {
                        dbService.start();
                    }
                    return result;
                }
                if (entry.is_regular_file()) {
                    objectFiles.push_back(entry.path());
                }
            }
        }

        fs::create_directories(snapshot_path.parent_path(), ec);
        if (ec) {
            result.error = "Failed to create snapshot output directory";
            if (wasRunning) {
                dbService.start();
            }
            return result;
        }

        std::ofstream out(snapshot_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.error = "Failed to open snapshot file for writing";
            if (wasRunning) {
                dbService.start();
            }
            return result;
        }

        out.write(kSnapshotMagic, sizeof(kSnapshotMagic) - 1);
        const uint32_t entryCount = static_cast<uint32_t>(2 + objectFiles.size());
        if (!write_u32(out, kSnapshotVersion) || !write_u32(out, entryCount)) {
            result.error = "Failed to write snapshot header";
            if (wasRunning) {
                dbService.start();
            }
            return result;
        }

        const std::string manifest = build_snapshot_manifest(root, objectFiles.size());
        std::vector<unsigned char> manifestBytes(manifest.begin(), manifest.end());
        if (!write_snapshot_entry(out, "manifest.json", manifestBytes, result.error)
            || !write_snapshot_file_entry(out, dbFile, "SoaSimDB.sqlite3", result.error)) {
            if (wasRunning) {
                dbService.start();
            }
            return result;
        }

        for (const auto& file : objectFiles) {
            const fs::path rel = fs::relative(file, root, ec);
            if (ec) {
                result.error = "Failed to compute snapshot relative path";
                if (wasRunning) {
                    dbService.start();
                }
                return result;
            }
            if (!write_snapshot_file_entry(out, file, rel.generic_string(), result.error)) {
                if (wasRunning) {
                    dbService.start();
                }
                return result;
            }
        }

        result.ok = out.good();
        if (!result.ok && result.error.empty()) {
            result.error = "Failed to finalize snapshot file";
        }

        if (wasRunning) {
            dbService.start();
        }
        return result;
    }

    DbSnapshotResult DbSnapshotService::LoadSnapshot(const fs::path& snapshot_path, const fs::path& target_root, bool switch_to_target)
    {
        DbSnapshotResult result;
        result.snapshot_path = snapshot_path;
        result.target_root = target_root;

        if (snapshot_path.empty() || target_root.empty()) {
            result.error = "Snapshot path and target root are required";
            return result;
        }

        DBService& dbService = DBService::instance();
        const fs::path targetCanon = canonicalish(target_root);
        fs::path snapshotSource = snapshot_path;

        if (!fs::exists(snapshotSource) || !fs::is_regular_file(snapshotSource)) {
            result.error = "Snapshot file does not exist";
            return result;
        }

        std::error_code ec;
        if (path_is_within(canonicalish(snapshotSource), targetCanon)) {
            const fs::path tempCopy = fs::temp_directory_path(ec) / ("soasim_snapshot_import_" + std::to_string(std::time(nullptr)) + ".soasnap");
            if (ec) {
                result.error = "Failed to prepare temporary snapshot staging path";
                return result;
            }
            fs::copy_file(snapshotSource, tempCopy, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                result.error = "Failed to stage snapshot file before restore";
                return result;
            }
            snapshotSource = tempCopy;
        }

        std::ifstream in(snapshotSource, std::ios::binary);
        if (!in) {
            result.error = "Failed to open snapshot file";
            return result;
        }

        char magic[sizeof(kSnapshotMagic) - 1]{};
        in.read(magic, sizeof(magic));
        if (std::string(magic, sizeof(magic)) != std::string(kSnapshotMagic, sizeof(kSnapshotMagic) - 1)) {
            result.error = "Snapshot file header is invalid";
            return result;
        }

        uint32_t version = 0;
        uint32_t entryCount = 0;
        if (!read_u32(in, version) || !read_u32(in, entryCount) || version != kSnapshotVersion) {
            result.error = "Snapshot version is unsupported";
            return result;
        }

        const fs::path stagingRoot = fs::temp_directory_path(ec) / ("soasim_snapshot_stage_" + std::to_string(std::time(nullptr)));
        if (ec) {
            result.error = "Failed to prepare snapshot staging directory";
            return result;
        }
        fs::create_directories(stagingRoot, ec);
        if (ec) {
            result.error = "Failed to create snapshot staging directory";
            return result;
        }

        bool sawDbFile = false;
        for (uint32_t i = 0; i < entryCount; ++i) {
            uint32_t pathSize = 0;
            uint64_t originalSize = 0;
            uint64_t payloadSize = 0;
            uint8_t flags = 0;
            if (!read_u32(in, pathSize) || !read_u64(in, originalSize) || !read_u64(in, payloadSize)) {
                result.error = "Snapshot entry header is truncated";
                remove_path_if_exists(stagingRoot, result.error);
                return result;
            }
            in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            if (!in.good()) {
                result.error = "Snapshot entry flags are truncated";
                remove_path_if_exists(stagingRoot, result.error);
                return result;
            }

            std::string relPath(pathSize, '\0');
            in.read(relPath.data(), static_cast<std::streamsize>(pathSize));
            if (!in.good()) {
                result.error = "Snapshot entry path is truncated";
                remove_path_if_exists(stagingRoot, result.error);
                return result;
            }

            std::vector<unsigned char> payload(static_cast<size_t>(payloadSize));
            if (payloadSize > 0) {
                in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payloadSize));
                if (!in.good()) {
                    result.error = "Snapshot entry payload is truncated";
                    remove_path_if_exists(stagingRoot, result.error);
                    return result;
                }
            }

            std::vector<unsigned char> decoded;
            if ((flags & 1) != 0) {
                decoded.resize(static_cast<size_t>(originalSize));
                uLongf destLen = static_cast<uLongf>(decoded.size());
                const int zrc = uncompress(decoded.data(), &destLen, payload.data(), static_cast<uLong>(payload.size()));
                if (zrc != Z_OK || destLen != decoded.size()) {
                    result.error = "Snapshot entry decompression failed";
                    remove_path_if_exists(stagingRoot, result.error);
                    return result;
                }
            } else {
                decoded = std::move(payload);
                if (decoded.size() != originalSize) {
                    result.error = "Snapshot entry size mismatch";
                    remove_path_if_exists(stagingRoot, result.error);
                    return result;
                }
            }

            const fs::path relFs = fs::path(relPath).lexically_normal();
            if (relFs.is_absolute() || relPath.find("..") != std::string::npos) {
                result.error = "Snapshot entry path is invalid";
                remove_path_if_exists(stagingRoot, result.error);
                return result;
            }

            const fs::path outPath = stagingRoot / relFs;
            fs::create_directories(outPath.parent_path(), ec);
            if (ec) {
                result.error = "Failed to create snapshot restore directories";
                remove_path_if_exists(stagingRoot, result.error);
                return result;
            }
            std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                result.error = "Failed to write restored snapshot file";
                remove_path_if_exists(stagingRoot, result.error);
                return result;
            }
            if (!decoded.empty()) {
                out.write(reinterpret_cast<const char*>(decoded.data()), static_cast<std::streamsize>(decoded.size()));
            }
            if (!out.good()) {
                result.error = "Failed to flush restored snapshot file";
                remove_path_if_exists(stagingRoot, result.error);
                return result;
            }
            if (relFs == fs::path("SoaSimDB.sqlite3")) {
                sawDbFile = true;
            }
        }

        if (!sawDbFile || !fs::exists(stagingRoot / "SoaSimDB.sqlite3")) {
            result.error = "Snapshot does not contain SoaSimDB.sqlite3";
            remove_path_if_exists(stagingRoot, result.error);
            return result;
        }

        const bool shouldSwitch = switch_to_target;
        const bool isActiveTarget = canonicalish(dbService.database_root()) == targetCanon;
        const bool shouldQuiesce = shouldSwitch || isActiveTarget;
        const bool wasRunning = dbService.is_running();
        if (shouldQuiesce && wasRunning) {
            dbService.stop();
        }

        if (!clear_directory_contents(targetCanon, result.error) || !copy_directory_contents(stagingRoot, targetCanon, result.error)) {
            if (shouldQuiesce && wasRunning) {
                dbService.start();
            }
            remove_path_if_exists(stagingRoot, result.error);
            return result;
        }

        std::string ignored;
        (void)remove_path_if_exists(targetCanon / "manifest.json", ignored);

        if (shouldSwitch) {
            dbService.set_database_root(targetCanon);
            result.active_root = targetCanon;
            result.switched_root = true;
            if (wasRunning) {
                dbService.start();
            }
        } else if (shouldQuiesce && wasRunning) {
            dbService.start();
            result.active_root = dbService.database_root();
        } else {
            result.active_root = dbService.database_root();
        }

        remove_path_if_exists(stagingRoot, ignored);
        if (snapshotSource != snapshot_path) {
            remove_path_if_exists(snapshotSource, ignored);
        }

        result.ok = true;
        return result;
    }

} // namespace simcore::db
