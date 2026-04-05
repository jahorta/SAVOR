#pragma once

#include <cstdint>
#include <unordered_map>

#include "ProgramKindDescriptor.h"

namespace simcore::db::execution::programdb {

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
            && descriptor->job_persistence != nullptr
            && descriptor->runtime_init != nullptr
            && descriptor->result_mapper != nullptr;
    }

private:
    std::unordered_map<std::int32_t, ProgramKindDescriptor> descriptors_;
};

} // namespace simcore::db::execution::programdb
