#pragma once

#include "BattleJobRunOptions.h"

#include <cstdlib>
#include <string>
#include <utility>

namespace savor::predict {

class ScopedProbeCpuCoreEnvironment {
public:
    explicit ScopedProbeCpuCoreEnvironment(ProbeCpuCore core)
    {
        char* previous = nullptr;
        std::size_t previous_size = 0;
        if (_dupenv_s(&previous, &previous_size, kVariable) == 0 && previous != nullptr) {
            previous_ = previous;
            had_previous_ = true;
        }
        std::free(previous);

        const char* value = "";
        if (core == ProbeCpuCore::Jit)
            value = "jit";
        else if (core == ProbeCpuCore::Interpreter)
            value = "interpreter";
        ok_ = _putenv_s(kVariable, value) == 0;
    }

    ~ScopedProbeCpuCoreEnvironment()
    {
        if (had_previous_)
            (void)_putenv_s(kVariable, previous_.c_str());
        else
            (void)_putenv_s(kVariable, "");
    }

    ScopedProbeCpuCoreEnvironment(const ScopedProbeCpuCoreEnvironment&) = delete;
    ScopedProbeCpuCoreEnvironment& operator=(const ScopedProbeCpuCoreEnvironment&) = delete;

    bool ok() const { return ok_; }

private:
    static constexpr const char* kVariable = "SAVOR_PROBE_CPU_CORE";
    std::string previous_;
    bool had_previous_ = false;
    bool ok_ = false;
};

} // namespace savor::predict
