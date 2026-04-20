#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace Sa3Dport::Structs {

template <typename T>
class PointerLUT {
public:
    [[nodiscard]] bool Contains(std::uint32_t address) const {
        return entries_.contains(address);
    }

    [[nodiscard]] std::shared_ptr<T> TryGet(std::uint32_t address) const {
        const auto it = entries_.find(address);
        return it == entries_.end() ? nullptr : it->second;
    }

    std::shared_ptr<T> GetOrAdd(std::uint32_t address, std::shared_ptr<T> value) {
        auto [it, inserted] = entries_.try_emplace(address, std::move(value));
        return it->second;
    }

    void Clear() {
        entries_.clear();
    }

    [[nodiscard]] std::size_t Size() const {
        return entries_.size();
    }

private:
    std::unordered_map<std::uint32_t, std::shared_ptr<T>> entries_;
};

} // namespace Sa3Dport::Structs
