#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "ProgramKindDescriptor.h"

namespace savor::db::execution::programdb {

class ProgramKindRegistry {
public:
    bool Register(const ProgramKindDescriptor& descriptor) {
        return descriptors_.emplace(descriptor.program_kind, descriptor).second;
    }

    const ProgramKindDescriptor* Find(std::int32_t program_kind) const {
        const auto it = descriptors_.find(program_kind);
        if (it == descriptors_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    bool HasRequiredAdapters(std::int32_t program_kind) const {
        const auto* descriptor = Find(program_kind);
        return descriptor != nullptr
            && (descriptor->job_persistence != nullptr || descriptor->graph_job_persistence != nullptr)
            && descriptor->runtime_init != nullptr
            && descriptor->result_mapper != nullptr;
    }

    bool RegisterForStepKind(std::string step_kind, const ProgramKindDescriptor& descriptor) {
        return step_kind_descriptors_.emplace(std::move(step_kind), descriptor).second;
    }

    const ProgramKindDescriptor* FindForStepKind(std::string_view step_kind) const {
        const auto it = step_kind_descriptors_.find(std::string(step_kind));
        if (it == step_kind_descriptors_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    bool HasRequiredAdaptersForStepKind(std::string_view step_kind) const {
        const auto* descriptor = FindForStepKind(step_kind);
        return descriptor != nullptr
            && (descriptor->job_persistence != nullptr || descriptor->graph_job_persistence != nullptr)
            && descriptor->runtime_init != nullptr
            && descriptor->result_mapper != nullptr;
    }

private:
    std::unordered_map<std::int32_t, ProgramKindDescriptor> descriptors_;
    std::unordered_map<std::string, ProgramKindDescriptor> step_kind_descriptors_;
};

} // namespace savor::db::execution::programdb
