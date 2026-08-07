#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Actions/BoundedStopPointCpuEvaluator.h"
#include "Runner/Runtime/ProgramRuntime/Actions/SessionResourceBindingTable.h"

#include <cstdint>
#include <string>

namespace {

using namespace savor::runtime;
using namespace savor::runtime::program;

TEST(BoundedStopPointCpuEvaluator, SamplesNativeEvidenceAndAppliesMasks)
{
    BoundedStopPointCpuEvaluator evaluator(
        {
            {1, CpuSampleSource::HitPc, CpuSampleWidth::U32, 0},
            {2, CpuSampleSource::HitAddress, CpuSampleWidth::U32, 0},
            {3, CpuSampleSource::HitValue, CpuSampleWidth::U64, 0},
        },
        {
            {10, 1, CpuQualificationComparison::Equal,
             0x80001234u, 0xffffffffu},
            {11, 3, CpuQualificationComparison::Equal,
             0x000000a0u, 0x000000f0u},
        });
    ASSERT_TRUE(evaluator.valid());

    const StopPointCpuContext context{
        .pc = 0x80001234u,
        .address = 0x80340000u,
        .value = 0x12a5u,
        .write = true,
        .post_write = true,
    };
    EXPECT_EQ(evaluator.Sample(1, context).value, context.pc);
    EXPECT_EQ(evaluator.Sample(2, context).value, context.address);
    EXPECT_EQ(evaluator.Sample(3, context).value, context.value);
    EXPECT_TRUE(evaluator.Qualify(10, context));
    EXPECT_TRUE(evaluator.Qualify(11, context));
    EXPECT_FALSE(evaluator.Qualify(999, context));
    EXPECT_FALSE(evaluator.Sample(999, context).available);
}

TEST(BoundedStopPointCpuEvaluator, RejectsInvalidDescriptorSetsAtomically)
{
    BoundedStopPointCpuEvaluator duplicate(
        {
            {1, CpuSampleSource::HitPc, CpuSampleWidth::U32, 0},
            {1, CpuSampleSource::HitValue, CpuSampleWidth::U64, 0},
        },
        {});
    EXPECT_FALSE(duplicate.valid());

    BoundedStopPointCpuEvaluator missing_sample(
        {{1, CpuSampleSource::HitPc, CpuSampleWidth::U32, 0}},
        {{10, 99, CpuQualificationComparison::Equal, 0, ~0ull}});
    EXPECT_FALSE(missing_sample.valid());

    BoundedStopPointCpuEvaluator zero_absolute(
        {{1, CpuSampleSource::GuestMemoryAbsolute,
          CpuSampleWidth::U32, 0}},
        {});
    EXPECT_FALSE(zero_absolute.valid());
}

TEST(SessionResourceBindingTable, ReleasesConcreteResourceExactlyOnce)
{
    SessionResourceBindingTable bindings;
    std::uint32_t release_count = 0;
    const auto bound = bindings.Bind({
        .kind = ResourceKind::InputLease,
        .acquisition_epoch = WorksetEpoch(7),
        .release = [&release_count](const ResourceReleaseRequest&) {
            ++release_count;
            return ResourceReleaseResult{
                ResourceReleaseStatus::Released,
                {}};
        },
        .diagnostic_label = "test input lease",
    });
    ASSERT_TRUE(bound.success) << bound.diagnostic;
    ASSERT_TRUE(bindings.Contains(bound.external_id));

    ResourceReceipt receipt;
    receipt.id = ResourceReceiptId(1);
    receipt.release = {
        ResourceKind::InputLease,
        bound.external_id};
    receipt.acquisition_epoch = WorksetEpoch(7);
    ResourceReleaseRequest request{
        .receipt = receipt,
        .reason = ResourceReleaseReason::Explicit,
        .current_epoch = WorksetEpoch(7),
    };
    EXPECT_EQ(
        bindings.Release(request).status,
        ResourceReleaseStatus::Released);
    EXPECT_EQ(release_count, 1u);
    EXPECT_FALSE(bindings.Contains(bound.external_id));

    // A repeated ledger unwind is deliberately idempotent and does not
    // invoke the concrete service twice.
    EXPECT_EQ(
        bindings.Release(request).status,
        ResourceReleaseStatus::Released);
    EXPECT_EQ(release_count, 1u);
}

} // namespace
