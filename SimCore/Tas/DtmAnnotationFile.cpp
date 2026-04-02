#include "DtmAnnotationFile.h"

#include "Utils/IniDoc.h"

#include <algorithm>

using namespace simcore::tas;

namespace {
    static constexpr const char* kSecMeta = "dtm_annotation";
    static constexpr const char* kSecBinding = "dtm_binding";
    static constexpr const char* kSecBookmarks = "bookmarks";

    static inline std::string bm_key(int idx, const char* field)
    {
        return std::string("bookmark_") + std::to_string(idx) + "_" + field;
    }
}

bool DtmAnnotationIo::save_ini(const std::string& path, const DtmAnnotationDoc& doc)
{
    IniDoc ini;
    ini.ensure_section(kSecMeta);
    ini.set(kSecMeta, "schema_version", std::to_string(doc.schema_version));

    ini.ensure_section(kSecBinding);
    ini.set(kSecBinding, "dtm_sha256", doc.dtm_sha256);
    ini.set(kSecBinding, "dtm_byte_length", std::to_string(doc.dtm_byte_length));
    ini.set(kSecBinding, "sav_required", doc.sav_required ? "1" : "0");
    ini.set(kSecBinding, "sav_sha256", doc.sav_sha256);
    ini.set(kSecBinding, "sav_byte_length", std::to_string(doc.sav_byte_length));

    ini.ensure_section(kSecBookmarks);
    ini.set(kSecBookmarks, "count", std::to_string(doc.bookmarks.size()));

    std::vector<DtmBookmark> sorted = doc.bookmarks;
    std::sort(sorted.begin(), sorted.end(), [](const DtmBookmark& a, const DtmBookmark& b) {
        if (a.input_byte_offset != b.input_byte_offset) return a.input_byte_offset < b.input_byte_offset;
        return a.id < b.id;
        });

    for (size_t i = 0; i < sorted.size(); ++i) {
        const int idx = static_cast<int>(i);
        const auto& bm = sorted[i];
        ini.set(kSecBookmarks, bm_key(idx, "id"), bm.id);
        ini.set(kSecBookmarks, bm_key(idx, "label"), bm.label);
        ini.set(kSecBookmarks, bm_key(idx, "note"), bm.note);
        ini.set(kSecBookmarks, bm_key(idx, "offset"), std::to_string(bm.input_byte_offset));

        std::string tags_joined;
        for (size_t t = 0; t < bm.tags.size(); ++t) {
            if (t != 0) tags_joined += ",";
            tags_joined += bm.tags[t];
        }
        ini.set(kSecBookmarks, bm_key(idx, "tags"), tags_joined);
    }

    return ini.save(path);
}

std::optional<DtmAnnotationDoc> DtmAnnotationIo::load_ini(const std::string& path)
{
    std::optional<IniDoc> ini = IniDoc::load(path);
    if (!ini.has_value()) return std::nullopt;

    DtmAnnotationDoc doc;
    doc.schema_version = ini->get_i32(kSecMeta, "schema_version", 1);
    doc.dtm_sha256 = ini->get(kSecBinding, "dtm_sha256", "");
    doc.dtm_byte_length = static_cast<uint64_t>(ini->get_i64(kSecBinding, "dtm_byte_length", 0));
    doc.sav_required = ini->get_bool(kSecBinding, "sav_required", false);
    doc.sav_sha256 = ini->get(kSecBinding, "sav_sha256", "");
    doc.sav_byte_length = static_cast<uint64_t>(ini->get_i64(kSecBinding, "sav_byte_length", 0));

    const int count = ini->get_i32(kSecBookmarks, "count", 0);
    doc.bookmarks.reserve(static_cast<size_t>(std::max(0, count)));
    for (int i = 0; i < count; ++i) {
        DtmBookmark bm;
        bm.id = ini->get(kSecBookmarks, bm_key(i, "id"), "");
        bm.label = ini->get(kSecBookmarks, bm_key(i, "label"), "");
        bm.note = ini->get(kSecBookmarks, bm_key(i, "note"), "");
        bm.input_byte_offset = static_cast<uint64_t>(ini->get_i64(kSecBookmarks, bm_key(i, "offset"), 0));

        const std::string tags = ini->get(kSecBookmarks, bm_key(i, "tags"), "");
        size_t start = 0;
        while (start < tags.size()) {
            size_t comma = tags.find(',', start);
            if (comma == std::string::npos) comma = tags.size();
            std::string token = tags.substr(start, comma - start);
            if (!token.empty()) bm.tags.push_back(token);
            start = comma + 1;
        }

        doc.bookmarks.push_back(std::move(bm));
    }

    return doc;
}
