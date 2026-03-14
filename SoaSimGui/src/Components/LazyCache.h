#pragma once
#include <unordered_map>
#include <optional>
#include <functional>
#include <mutex>

template <class K, class V>
class LazyCache {
public:
    bool has(const K& k) const {
        std::scoped_lock lk(mu_);
        return map_.find(k) != map_.end();
    }
    std::optional<V> get(const K& k) const {
        std::scoped_lock lk(mu_);
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }
    void set(const K& k, V v) {
        std::scoped_lock lk(mu_);
        map_[k] = std::move(v);
    }
    void clear() {
        std::scoped_lock lk(mu_);
        map_.clear();
    }
private:
    mutable std::mutex mu_;
    std::unordered_map<K, V> map_;
};
