#pragma once

#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace simcore::db {

    struct TagRecord {
        int64_t tag_id{};
        std::string tag_key;
        std::string namespace_name;
        std::string leaf_name;
        std::optional<int64_t> parent_tag_id;
        std::optional<std::string> description;
        int64_t created_at{};
        int64_t updated_at{};
    };

    struct EntityTagRecord {
        std::string entity_kind;
        int64_t entity_id{};
        int64_t tag_id{};
        int64_t created_at{};
        std::optional<std::string> created_by;
    };

    struct TagRepo {
        static std::future<DbResult<TagRecord>> EnsureTagAsync(
            const std::string& tag_key,
            std::optional<std::string> description = std::nullopt,
            RetryPolicy rp = {});

        static std::future<DbResult<std::vector<TagRecord>>> ListTagsAsync(
            std::optional<std::string> namespace_filter = std::nullopt,
            RetryPolicy rp = {});
        static std::future<DbResult<std::vector<TagRecord>>> ListTagsForEntityKindAsync(
            const std::string& entity_kind,
            RetryPolicy rp = {});

        static std::future<DbResult<void>> AttachTagToEntityAsync(
            const std::string& entity_kind,
            int64_t entity_id,
            const std::string& tag_key,
            std::optional<std::string> created_by = std::nullopt,
            RetryPolicy rp = {});

        static std::future<DbResult<void>> DetachTagFromEntityAsync(
            const std::string& entity_kind,
            int64_t entity_id,
            const std::string& tag_key,
            RetryPolicy rp = {});

        static std::future<DbResult<std::vector<TagRecord>>> ListEntityTagsAsync(
            const std::string& entity_kind,
            int64_t entity_id,
            RetryPolicy rp = {});

        static std::future<DbResult<std::vector<int64_t>>> FindEntityIdsByTagAsync(
            const std::string& entity_kind,
            const std::string& tag_key,
            bool include_children,
            RetryPolicy rp = {});

        static inline DbResult<TagRecord> EnsureTag(const std::string& tag_key, std::optional<std::string> description = std::nullopt) {
            return EnsureTagAsync(tag_key, std::move(description)).get();
        }
        static inline DbResult<std::vector<TagRecord>> ListTags(std::optional<std::string> namespace_filter = std::nullopt) {
            return ListTagsAsync(std::move(namespace_filter)).get();
        }
        static inline DbResult<std::vector<TagRecord>> ListTagsForEntityKind(const std::string& entity_kind) {
            return ListTagsForEntityKindAsync(entity_kind).get();
        }
        static inline DbResult<void> AttachTagToEntity(const std::string& entity_kind, int64_t entity_id, const std::string& tag_key, std::optional<std::string> created_by = std::nullopt) {
            return AttachTagToEntityAsync(entity_kind, entity_id, tag_key, std::move(created_by)).get();
        }
        static inline DbResult<void> DetachTagFromEntity(const std::string& entity_kind, int64_t entity_id, const std::string& tag_key) {
            return DetachTagFromEntityAsync(entity_kind, entity_id, tag_key).get();
        }
        static inline DbResult<std::vector<TagRecord>> ListEntityTags(const std::string& entity_kind, int64_t entity_id) {
            return ListEntityTagsAsync(entity_kind, entity_id).get();
        }
        static inline DbResult<std::vector<int64_t>> FindEntityIdsByTag(const std::string& entity_kind, const std::string& tag_key, bool include_children) {
            return FindEntityIdsByTagAsync(entity_kind, tag_key, include_children).get();
        }
    };

} // namespace simcore::db
