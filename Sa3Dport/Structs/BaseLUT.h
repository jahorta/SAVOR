#pragma once

#include "Structs/LabeledArray.h"

#include <any>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Sa3Dport::Structs {

enum class DedupMode {
    PointerIdentity,
    Value,
};

class BaseLUT {
public:
    explicit BaseLUT(DedupMode mode = DedupMode::PointerIdentity) : mode_(mode) {}
    explicit BaseLUT(std::unordered_map<std::uint32_t, std::string> labels,
                     DedupMode mode = DedupMode::PointerIdentity)
        : mode_(mode), labels_(std::move(labels)) {}

    [[nodiscard]] DedupMode mode() const {
        return mode_;
    }

    template <class Range, class Factory>
    std::uint32_t get_add_address(const Range* values, Factory factory) {
        if (values == nullptr) {
            return 0;
        }

        const auto key = static_cast<const void*>(values);
        const auto it = writeAddresses_.find(key);
        if (it != writeAddresses_.end()) {
            return it->second;
        }

        const std::uint32_t address = factory();
        writeAddresses_.emplace(key, address);
        return address;
    }

    template <class Range, class Hash, class Equal, class Factory>
    std::uint32_t get_add_address_by_value(const Range& values, Hash hash, Equal equal, Factory factory) {
        const std::size_t hashValue = hash(values);
        const auto bucket = valueWriteAddresses_.equal_range(hashValue);
        for (auto it = bucket.first; it != bucket.second; ++it) {
            if (equal(std::any_cast<const Range&>(it->second.values), values)) {
                return it->second.address;
            }
        }

        const std::uint32_t address = factory();
        valueWriteAddresses_.emplace(hashValue, ValueEntry{address, values});
        return address;
    }

    template <class T, class Factory>
    std::vector<T> get_add_value(std::uint32_t address, Factory factory) {
        const CacheKey key {address, std::type_index(typeid(std::vector<T>))};
        const auto it = readValues_.find(key);
        if (it != readValues_.end()) {
            return std::any_cast<std::vector<T>>(it->second);
        }

        auto value = factory();
        readValues_.emplace(key, value);
        return value;
    }

    template <class T, class Factory>
    LabeledArray<T> get_add_labeled_value(std::uint32_t address,
                                          std::string_view generatedPrefix,
                                          Factory factory) {
        const CacheKey key {address, std::type_index(typeid(LabeledArray<T>))};
        const auto it = readValues_.find(key);
        if (it != readValues_.end()) {
            return std::any_cast<LabeledArray<T>>(it->second);
        }

        auto value = LabeledArray<T>(make_label(generatedPrefix, address), factory());
        readValues_.emplace(key, value);
        return value;
    }

    template <class T, class Factory>
    LabeledReadOnlyArray<T> get_add_labeled_read_only_value(std::uint32_t address,
                                                            std::string_view generatedPrefix,
                                                            Factory factory) {
        const CacheKey key {address, std::type_index(typeid(LabeledReadOnlyArray<T>))};
        const auto it = readValues_.find(key);
        if (it != readValues_.end()) {
            return std::any_cast<LabeledReadOnlyArray<T>>(it->second);
        }

        auto value = LabeledReadOnlyArray<T>(make_label(generatedPrefix, address), factory());
        readValues_.emplace(key, value);
        return value;
    }

private:
    struct CacheKey {
        std::uint32_t address = 0;
        std::type_index type = std::type_index(typeid(void));

        [[nodiscard]] bool operator==(const CacheKey& other) const {
            return address == other.address && type == other.type;
        }
    };

    struct CacheKeyHash {
        [[nodiscard]] std::size_t operator()(const CacheKey& key) const noexcept {
            return std::hash<std::uint32_t>{}(key.address) ^ (key.type.hash_code() << 1);
        }
    };

    struct ValueEntry {
        std::uint32_t address = 0;
        std::any values;
    };

    [[nodiscard]] std::string make_label(std::string_view prefix, std::uint32_t address) const {
        const auto existing = labels_.find(address);
        if (existing != labels_.end()) {
            return existing->second;
        }

        std::ostringstream stream;
        stream << prefix << "_0x" << std::hex << address;
        return stream.str();
    }

    DedupMode mode_;
    std::unordered_map<std::uint32_t, std::string> labels_;
    std::unordered_map<const void*, std::uint32_t> writeAddresses_;
    std::unordered_multimap<std::size_t, ValueEntry> valueWriteAddresses_;
    std::unordered_map<CacheKey, std::any, CacheKeyHash> readValues_;
};

} // namespace Sa3Dport::Structs
