#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simcore::tas {

    struct DtmBookmark {
        std::string id;
        std::string label;
        std::string note;
        uint64_t input_byte_offset{ 0 };
        std::vector<std::string> tags;
    };

    struct DtmAnnotationDoc {
        int schema_version{ 1 };
        std::string dtm_sha256;
        uint64_t dtm_byte_length{ 0 };
        bool sav_required{ false };
        std::string sav_sha256;
        uint64_t sav_byte_length{ 0 };
        std::vector<DtmBookmark> bookmarks;
    };

    struct DtmAnnotationIo {
        static bool save_ini(const std::string& path, const DtmAnnotationDoc& doc);
        static std::optional<DtmAnnotationDoc> load_ini(const std::string& path);
    };

} // namespace simcore::tas
