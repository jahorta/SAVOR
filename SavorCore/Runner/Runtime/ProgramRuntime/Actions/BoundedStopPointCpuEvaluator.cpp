#include "BoundedStopPointCpuEvaluator.h"

#include "../Capabilities/SourceCapabilityPacks.h"

#include <algorithm>
#include <optional>

#include "Core/HW/Memmap.h"
#include "Core/Movie.h"
#include "Core/System.h"

namespace savor::runtime::program {
namespace {

std::optional<CpuSampleWidth> WidthFor(
    const CpuEvaluatorDescriptor& descriptor) noexcept
{
    if (descriptor.canonical_id == "soa.tasmovie.sample.MovieInputCount"
        && descriptor.operations.empty() && descriptor.address_dependency.empty()
        && descriptor.maximum_reads == 0)
        return CpuSampleWidth::U64;
    if (descriptor.canonical_id == "soa.tasmovie.sample.ReceivedPadStatus" &&
        descriptor.operations == std::vector{
            CpuEvaluatorOperation::ReadU32,
            CpuEvaluatorOperation::ReadU64} &&
        descriptor.maximum_reads == 2)
    {
        return CpuSampleWidth::U64;
    }
    if (descriptor.operations.size() != 1 ||
        descriptor.address_dependency.empty() ||
        descriptor.maximum_reads != 1)
    {
        return std::nullopt;
    }
    switch (descriptor.operations.front())
    {
    case CpuEvaluatorOperation::ReadU8:
        return CpuSampleWidth::U8;
    case CpuEvaluatorOperation::ReadU16:
        return CpuSampleWidth::U16;
    case CpuEvaluatorOperation::ReadU32:
        return CpuSampleWidth::U32;
    case CpuEvaluatorOperation::ReadU64:
        return CpuSampleWidth::U64;
    case CpuEvaluatorOperation::CompareEqual:
    case CpuEvaluatorOperation::CompareMaskedEqual:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] bool ValidWidth(CpuSampleWidth width) noexcept
{
    switch (width)
    {
    case CpuSampleWidth::U8:
    case CpuSampleWidth::U16:
    case CpuSampleWidth::U32:
    case CpuSampleWidth::U64:
        return true;
    }
    return false;
}

[[nodiscard]] bool Compare(
    CpuQualificationComparison comparison,
    std::uint64_t actual,
    std::uint64_t expected) noexcept
{
    switch (comparison)
    {
    case CpuQualificationComparison::Equal:
        return actual == expected;
    case CpuQualificationComparison::NotEqual:
        return actual != expected;
    case CpuQualificationComparison::Less:
        return actual < expected;
    case CpuQualificationComparison::LessEqual:
        return actual <= expected;
    case CpuQualificationComparison::Greater:
        return actual > expected;
    case CpuQualificationComparison::GreaterEqual:
        return actual >= expected;
    }
    return false;
}

} // namespace

BoundedStopPointCpuEvaluator::BoundedStopPointCpuEvaluator(
    std::vector<CpuSampleDescriptor> samples,
    std::vector<CpuQualificationDescriptor> qualifications) noexcept
    : samples_(std::move(samples)),
      qualifications_(std::move(qualifications))
{
    if (samples_.size() > kMaxRoutedHitSamples ||
        qualifications_.size() > kMaxLogicalStopSubscriptions)
    {
        return;
    }
    for (std::size_t index = 0; index < samples_.size(); ++index)
    {
        const CpuSampleDescriptor& sample = samples_[index];
        if (sample.id == 0 || !ValidWidth(sample.width) ||
            ((sample.source == CpuSampleSource::GuestMemoryAbsolute ||
                 sample.source == CpuSampleSource::GuestMemoryIndirectU32) &&
                sample.address == 0))
        {
            return;
        }
        if (std::any_of(
                samples_.begin(),
                samples_.begin() + index,
                [&sample](const CpuSampleDescriptor& candidate) {
                    return candidate.id == sample.id;
                }))
        {
            return;
        }
    }
    for (std::size_t index = 0;
        index < qualifications_.size();
        ++index)
    {
        const CpuQualificationDescriptor& qualification =
            qualifications_[index];
        if (qualification.id == 0 ||
            !FindSample(qualification.sample_descriptor_id))
        {
            return;
        }
        if (std::any_of(
                qualifications_.begin(),
                qualifications_.begin() + index,
                [&qualification](
                    const CpuQualificationDescriptor& candidate) {
                    return candidate.id == qualification.id;
                }))
        {
            return;
        }
    }
    valid_ = true;
}

bool BoundedStopPointCpuEvaluator::Qualify(
    std::uint32_t qualification_id,
    const StopPointCpuContext& context) noexcept
{
    if (!valid_ || qualification_id == 0)
        return false;
    const auto found = std::find_if(
        qualifications_.begin(),
        qualifications_.end(),
        [qualification_id](
            const CpuQualificationDescriptor& candidate) {
            return candidate.id == qualification_id;
        });
    if (found == qualifications_.end())
        return false;

    const RoutedHitSample sample =
        Sample(found->sample_descriptor_id, context);
    if (!sample.available)
        return false;
    return Compare(
        found->comparison,
        sample.value & found->mask,
        found->expected & found->mask);
}

RoutedHitSample BoundedStopPointCpuEvaluator::Sample(
    std::uint32_t descriptor_id,
    const StopPointCpuContext& context) noexcept
{
    RoutedHitSample result{descriptor_id, 0, false};
    if (!valid_)
        return result;
    const CpuSampleDescriptor* descriptor = FindSample(descriptor_id);
    if (!descriptor)
        return result;

    switch (descriptor->source)
    {
    case CpuSampleSource::HitPc:
        result.value = context.pc;
        result.available = true;
        return result;
    case CpuSampleSource::HitAddress:
        result.value = context.address;
        result.available = true;
        return result;
    case CpuSampleSource::HitValue:
        result.value = context.value;
        result.available = true;
        return result;
    case CpuSampleSource::GuestMemoryAbsolute:
        break;
    case CpuSampleSource::GuestMemoryIndirectU32:
        if (!context.system)
            return result;
        {
            auto& memory = context.system->GetMemory();
            if (!memory.GetPointerForRange(descriptor->address, 4))
                return result;
            const std::uint32_t target = memory.Read_U32(descriptor->address);
            if (!memory.GetPointerForRange(target, 8))
                return result;
            result.value = memory.Read_U64(target);
            result.available = true;
            return result;
        }
    case CpuSampleSource::HostMovieInputCount:
        if (!context.system)
            return result;
        result.value = context.system->GetMovie().GetCurrentInputCount();
        result.available = true;
        return result;
    }

    if (!context.system)
        return result;
    auto& memory = context.system->GetMemory();
    const std::size_t width =
        static_cast<std::size_t>(descriptor->width);
    if (!memory.GetPointerForRange(descriptor->address, width))
        return result;
    switch (descriptor->width)
    {
    case CpuSampleWidth::U8:
        result.value = memory.Read_U8(descriptor->address);
        break;
    case CpuSampleWidth::U16:
        result.value = memory.Read_U16(descriptor->address);
        break;
    case CpuSampleWidth::U32:
        result.value = memory.Read_U32(descriptor->address);
        break;
    case CpuSampleWidth::U64:
        result.value = memory.Read_U64(descriptor->address);
        break;
    }
    result.available = true;
    return result;
}

const CpuSampleDescriptor* BoundedStopPointCpuEvaluator::FindSample(
    std::uint32_t descriptor_id) const noexcept
{
    const auto found = std::find_if(
        samples_.begin(),
        samples_.end(),
        [descriptor_id](const CpuSampleDescriptor& candidate) {
            return candidate.id == descriptor_id;
        });
    return found == samples_.end() ? nullptr : &*found;
}

std::unique_ptr<BoundedStopPointCpuEvaluator>
BuildCanonicalStopPointCpuEvaluator()
{
    const capabilities::SourceCapabilityPackCatalog catalog =
        capabilities::BuildSourceCapabilityPackCatalog();
    std::vector<CpuSampleDescriptor> samples;
    for (const CapabilityPackManifest& manifest : catalog.manifests)
    {
        for (const CpuEvaluatorDescriptor& evaluator :
             manifest.cpu_evaluators)
        {
            const std::optional<CpuSampleWidth> width =
                WidthFor(evaluator);
        const bool host_movie_input_count = evaluator.source ==
            CpuEvaluatorSource::HostMovieInputCount;
        const bool indirect_guest = evaluator.source ==
            CpuEvaluatorSource::GuestMemoryIndirectU32;
            const auto address = std::ranges::find(
                manifest.address_symbols,
                evaluator.address_dependency,
                &AddressSymbolDescriptor::canonical_id);
            if (!width || (!host_movie_input_count
                    && address == manifest.address_symbols.end()) ||
                evaluator.routed_sample_descriptor_id == 0 ||
                evaluator.maximum_output_bytes !=
                    static_cast<std::uint32_t>(*width))
            {
                return nullptr;
            }
            samples.push_back({
                evaluator.routed_sample_descriptor_id,
                host_movie_input_count ? CpuSampleSource::HostMovieInputCount
                : indirect_guest ? CpuSampleSource::GuestMemoryIndirectU32
                                 : CpuSampleSource::GuestMemoryAbsolute,
                *width,
                host_movie_input_count ? 0u : address->address,
            });
        }
    }
    auto result = std::make_unique<BoundedStopPointCpuEvaluator>(
        std::move(samples),
        std::vector<CpuQualificationDescriptor>{});
    return result->valid() ? std::move(result) : nullptr;
}

} // namespace savor::runtime::program
