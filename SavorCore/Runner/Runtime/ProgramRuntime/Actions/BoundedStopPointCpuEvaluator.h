#pragma once

#include "../../StopPoints/StopPointRouter.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace savor::runtime::program {

enum class CpuSampleSource : std::uint8_t
{
    HitPc,
    HitAddress,
    HitValue,
    GuestMemoryAbsolute,
    HostMovieInputCount,
};

enum class CpuSampleWidth : std::uint8_t
{
    U8 = 1,
    U16 = 2,
    U32 = 4,
    U64 = 8,
};

struct CpuSampleDescriptor
{
    std::uint32_t id = 0;
    CpuSampleSource source = CpuSampleSource::HitValue;
    CpuSampleWidth width = CpuSampleWidth::U64;
    std::uint32_t address = 0;
};

enum class CpuQualificationComparison : std::uint8_t
{
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

struct CpuQualificationDescriptor
{
    std::uint32_t id = 0;
    std::uint32_t sample_descriptor_id = 0;
    CpuQualificationComparison comparison =
        CpuQualificationComparison::Equal;
    std::uint64_t expected = 0;
    std::uint64_t mask = ~std::uint64_t{0};
};

// Immutable after construction and safe for the bounded CPU dispatch path.
// Lookups are linear over a verifier-bounded descriptor set; evaluation does
// not allocate, lock, wait, or invoke arbitrary callbacks.
class BoundedStopPointCpuEvaluator final : public IStopPointCpuEvaluator
{
public:
    BoundedStopPointCpuEvaluator(
        std::vector<CpuSampleDescriptor> samples,
        std::vector<CpuQualificationDescriptor> qualifications) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] bool Qualify(
        std::uint32_t qualification_id,
        const StopPointCpuContext& context) noexcept override;
    [[nodiscard]] RoutedHitSample Sample(
        std::uint32_t descriptor_id,
        const StopPointCpuContext& context) noexcept override;

private:
    [[nodiscard]] const CpuSampleDescriptor* FindSample(
        std::uint32_t descriptor_id) const noexcept;

    std::vector<CpuSampleDescriptor> samples_;
    std::vector<CpuQualificationDescriptor> qualifications_;
    bool valid_ = false;
};

// Constructs the one immutable CPU sampler surface shared by every
// production worker. The implementation is derived from the canonical source
// capability-pack descriptors, so worksets only ever carry exact references.
[[nodiscard]] std::unique_ptr<BoundedStopPointCpuEvaluator>
BuildCanonicalStopPointCpuEvaluator();

} // namespace savor::runtime::program
