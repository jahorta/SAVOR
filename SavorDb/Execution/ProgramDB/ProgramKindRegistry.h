#pragma once

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ProgramKindDescriptor.h"

namespace savor::db::execution::programdb {

class ProgramKindRegistry {
public:
    ProgramKindRegistry() = default;
    ProgramKindRegistry(const ProgramKindRegistry&) = delete;
    ProgramKindRegistry& operator=(const ProgramKindRegistry&) = delete;

    ProgramKindRegistry(ProgramKindRegistry&& other) noexcept {
        std::unique_lock lock(other.mutex_);
        descriptors_ = std::move(other.descriptors_);
        step_kind_descriptors_ = std::move(other.step_kind_descriptors_);
        generation_.store(
            other.generation_.load(std::memory_order_acquire),
            std::memory_order_release);
    }

    ProgramKindRegistry& operator=(ProgramKindRegistry&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        std::scoped_lock lock(mutex_, other.mutex_);
        descriptors_ = std::move(other.descriptors_);
        step_kind_descriptors_ = std::move(other.step_kind_descriptors_);
        generation_.store(
            other.generation_.load(std::memory_order_acquire),
            std::memory_order_release);
        return *this;
    }

    bool Register(const ProgramKindDescriptor& descriptor) {
        if (!descriptor.default_progress_library_ids.has_value() ||
            !descriptor.default_derived_state_block_ids.has_value()) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const bool inserted =
            descriptors_
                .emplace(
                    descriptor.program_kind,
                    std::make_shared<const ProgramKindDescriptor>(
                        descriptor))
                .second;
        if (inserted) {
            generation_.fetch_add(1, std::memory_order_release);
        }
        return inserted;
    }

    const ProgramKindDescriptor* Find(std::int32_t program_kind) const {
        std::shared_lock lock(mutex_);
        const auto it = descriptors_.find(program_kind);
        if (it == descriptors_.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    bool HasRequiredAdapters(std::int32_t program_kind) const {
        const auto* descriptor = Find(program_kind);
        return descriptor != nullptr
            && descriptor->workset_reconstruction != nullptr
            && descriptor->result_handler != nullptr;
    }

    bool RegisterForStepKind(std::string step_kind, const ProgramKindDescriptor& descriptor) {
        if (!descriptor.default_progress_library_ids.has_value() ||
            !descriptor.default_derived_state_block_ids.has_value()) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const bool inserted =
            step_kind_descriptors_
                .emplace(
                    std::move(step_kind),
                    std::make_shared<const ProgramKindDescriptor>(
                        descriptor))
                .second;
        if (inserted) {
            generation_.fetch_add(1, std::memory_order_release);
        }
        return inserted;
    }

    const ProgramKindDescriptor* FindForStepKind(std::string_view step_kind) const {
        std::shared_lock lock(mutex_);
        const auto it = step_kind_descriptors_.find(std::string(step_kind));
        if (it == step_kind_descriptors_.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    bool HasRequiredAdaptersForStepKind(std::string_view step_kind) const {
        const auto* descriptor = FindForStepKind(step_kind);
        return descriptor != nullptr
            && descriptor->job_materializer != nullptr
            && descriptor->workset_reconstruction != nullptr
            && descriptor->result_handler != nullptr;
    }

    [[nodiscard]] std::uint64_t Generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t Size() const {
        std::shared_lock lock(mutex_);
        return descriptors_.size();
    }

    [[nodiscard]] std::vector<std::int32_t> RegisteredProgramKinds() const {
        std::shared_lock lock(mutex_);
        std::vector<std::int32_t> kinds;
        kinds.reserve(descriptors_.size());
        for (const auto& [program_kind, descriptor] : descriptors_) {
            (void)descriptor;
            kinds.push_back(program_kind);
        }
        std::sort(kinds.begin(), kinds.end());
        return kinds;
    }

private:
    mutable std::shared_mutex mutex_;
    std::atomic<std::uint64_t> generation_{ 1 };
    std::unordered_map<
        std::int32_t,
        std::shared_ptr<const ProgramKindDescriptor>>
        descriptors_;
    std::unordered_map<
        std::string,
        std::shared_ptr<const ProgramKindDescriptor>>
        step_kind_descriptors_;
};

} // namespace savor::db::execution::programdb
