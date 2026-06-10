// Runner/Script/KeyRegistry.cpp
#include "KeyRegistry.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>

namespace savor::keys {

    // Fixed list of module tables (deterministic order)
    static constexpr const KeyPair* kTablesPtrs[] = {
    core::kKeys, seed::kKeys, tas::kKeys, tasframedetector::kKeys, battle::kKeys,
    };
    static constexpr std::size_t kTablesSizes[] = {
      core::kCount, seed::kCount, tas::kCount, tasframedetector::kCount, battle::kCount
    };

    static constexpr size_t kNumTables = sizeof(kTablesPtrs) / sizeof(kTablesPtrs[0]);

    // FNV-1a 32-bit
    static inline uint32_t fnv1a32(const void* data, size_t n, uint32_t seed = 2166136261u) {
        const auto* p = static_cast<const uint8_t*>(data);
        uint32_t h = seed;
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
        return h;
    }

    struct sv_hash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    struct sv_eq {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    };

    // Lazy state
    static std::once_flag g_once;
    static std::unordered_map<KeyId, std::string_view> g_id2name;
    static std::unordered_map<std::string_view, KeyId, sv_hash, sv_eq> g_name2id;
    static std::vector<KeyPair> g_all;   // stable storage for all_keys()
    static uint32_t g_hash = 0;
    static bool g_valid = false;
    static std::string g_error;

    static void build_registry() {
        g_id2name.clear();
        g_name2id.clear();
        g_all.clear();
        g_hash = 2166136261u;
        g_valid = true;
        g_error.clear();

        std::unordered_set<KeyId> seen_ids;
        std::unordered_set<std::string_view, sv_hash, sv_eq> seen_names;

        for (size_t ti = 0; ti < kNumTables; ++ti) {
            const KeyPair* tbl = kTablesPtrs[ti];
            const size_t    sz = kTablesSizes[ti];
            for (size_t i = 0; i < sz; ++i) {
                const KeyPair kp = tbl[i];

                // Duplicate checks (runtime)
                if (!seen_ids.insert(kp.id).second) {
                    g_valid = false;
                    g_error = "Duplicate key id detected";
                }
                if (!seen_names.insert(kp.name).second) {
                    g_valid = false;
                    g_error = "Duplicate key name detected";
                }

                // Insert into maps
                g_id2name.emplace(kp.id, kp.name);
                g_name2id.emplace(kp.name, kp.id);

                // Aggregate list (stable order: by table, then declaration order)
                g_all.push_back(kp);

                // Contribute to deterministic hash: id (LE) + name bytes
                g_hash = fnv1a32(&kp.id, sizeof(kp.id), g_hash);
                g_hash = fnv1a32(kp.name.data(), kp.name.size(), g_hash);
            }
        }
    }

    static inline void ensure_init() {
        std::call_once(g_once, build_registry);
    }

    std::string_view name_for_id(KeyId id) {
        ensure_init();
        auto it = g_id2name.find(id);
        return (it != g_id2name.end()) ? it->second : std::string_view{};
    }

    bool id_for_name(std::string_view name, KeyId& out) {
        ensure_init();
        auto it = g_name2id.find(name);
        if (it == g_name2id.end()) return false;
        out = it->second;
        return true;
    }

    const KeyPair* all_keys(size_t& out_count) {
        ensure_init();
        out_count = g_all.size();
        return g_all.data();
    }

    uint32_t registry_hash() {
        ensure_init();
        return g_hash;
    }

    bool validate_registry(std::string* err_out) {
        ensure_init();
        if (err_out) *err_out = g_error;
        return g_valid;
    }

} // namespace savor::keys
