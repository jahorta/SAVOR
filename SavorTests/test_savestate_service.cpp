#include <gtest/gtest.h>

#include "Runner/Runtime/Services/Savestate/SavestateService.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

using namespace savor::runtime;

ArtifactCompatibilityToken Compatibility()
{
    return {
        .game_id = "GEAE8E",
        .iso_sha256 = std::string(64, 'a'),
        .emulator_build = "dolphin-2506a",
        .runtime_revision = "workset-epoch-test"};
}

class Backend final : public ISavestateBackendPort
{
public:
    ArtifactCompatibilityToken CurrentCompatibility() const override
    {
        return Compatibility();
    }
    SavestateBackendBufferResult SaveStateBuffer() override
    {
        ++save_buffer_count;
        if (throw_save)
            throw std::runtime_error("save exception");
        return {save_result, bytes};
    }
    SavestateBackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>& restored) override
    {
        ++restore_buffer_count;
        if (throw_restore)
            throw std::runtime_error("restore exception");
        last_restored = restored;
        return restore_result;
    }
    SavestateBackendResult SaveStateFile(
        const std::filesystem::path& path) override
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        return output.good() ? SavestateBackendResult::Success()
                             : SavestateBackendResult::Failure("save failed");
    }
    SavestateBackendResult RestoreStateFile(
        const std::filesystem::path&) override
    {
        ++restore_file_count;
        return restore_result;
    }

    SavestateBackendResult save_result = SavestateBackendResult::Success();
    SavestateBackendResult restore_result = SavestateBackendResult::Success();
    std::vector<std::uint8_t> bytes{1, 2, 3, 4};
    std::vector<std::uint8_t> last_restored;
    int save_buffer_count = 0;
    int restore_buffer_count = 0;
    int restore_file_count = 0;
    bool throw_save = false;
    bool throw_restore = false;
};

TEST(SavestateService, MemoryHandleRestoresWithoutChangingWorksetEpoch)
{
    Backend backend;
    SavestateService service(backend, WorksetEpoch(7), Compatibility());

    const SavestateHandleReceipt captured = service.CaptureMemoryHandle({});
    ASSERT_TRUE(captured.result.ok) << captured.result.message;
    EXPECT_EQ(captured.captured_epoch, WorksetEpoch(7));

    const SavestateRestoreReceipt restored =
        service.RestoreMemoryHandle(captured.handle);
    ASSERT_TRUE(restored.result.ok) << restored.result.message;
    EXPECT_EQ(restored.workset_epoch, WorksetEpoch(7));
    EXPECT_EQ(backend.last_restored, backend.bytes);
    EXPECT_EQ(service.workset_epoch(), WorksetEpoch(7));
}

TEST(SavestateService, HandleCapacityAndReleaseAreWorksetLocal)
{
    Backend backend;
    SavestateService service(
        backend,
        WorksetEpoch(3),
        Compatibility(),
        SavestateServiceLimits{
            .maximum_memory_handles = 1,
            .maximum_handle_bytes = 32,
            .maximum_total_memory_bytes = 32});

    const auto first = service.CaptureMemoryHandle({});
    ASSERT_TRUE(first.result.ok);
    const auto rejected = service.CaptureMemoryHandle({});
    EXPECT_FALSE(rejected.result.ok);
    EXPECT_EQ(rejected.result.code,
              SavestateServiceErrorCode::CapacityExceeded);
    ASSERT_TRUE(service.ReleaseMemoryHandle(first.handle).ok);
    EXPECT_TRUE(service.CaptureMemoryHandle({}).result.ok);
}

TEST(SavestateService, BackendFailurePreservesTypedIntegrity)
{
    Backend backend;
    backend.restore_result = SavestateBackendResult::Failure(
        "load unknown", GuestIntegrity::Unknown);
    SavestateService service(backend, WorksetEpoch(11), Compatibility());
    const auto captured = service.CaptureMemoryHandle({});
    ASSERT_TRUE(captured.result.ok);

    const auto restored = service.RestoreMemoryHandle(captured.handle);
    EXPECT_FALSE(restored.result.ok);
    EXPECT_EQ(restored.workset_epoch, WorksetEpoch(11));
    EXPECT_EQ(restored.result.integrity, GuestIntegrity::Unknown);
}

TEST(SavestateService, ConstructionRequiresNonzeroWorksetEpochAtUse)
{
    Backend backend;
    SavestateService service(backend, WorksetEpoch{}, Compatibility());
    const auto captured = service.CaptureMemoryHandle({});
    EXPECT_FALSE(captured.result.ok);
    EXPECT_EQ(captured.result.code, SavestateServiceErrorCode::InvalidState);
}

TEST(SavestateService, BackendExceptionsBecomeTypedUnknownIntegrityFailures)
{
    Backend backend;
    backend.throw_save = true;
    SavestateService service(backend, WorksetEpoch(13), Compatibility());
    const auto save = service.CaptureMemoryHandle({});
    EXPECT_FALSE(save.result.ok);
    EXPECT_EQ(save.result.code, SavestateServiceErrorCode::BackendFailure);
    EXPECT_EQ(save.result.integrity, GuestIntegrity::Unknown);

    backend.throw_save = false;
    const auto captured = service.CaptureMemoryHandle({});
    ASSERT_TRUE(captured.result.ok);
    backend.throw_restore = true;
    const auto restore = service.RestoreMemoryHandle(captured.handle);
    EXPECT_FALSE(restore.result.ok);
    EXPECT_EQ(restore.result.code, SavestateServiceErrorCode::BackendFailure);
    EXPECT_EQ(restore.result.integrity, GuestIntegrity::Unknown);
    EXPECT_EQ(restore.workset_epoch, WorksetEpoch(13));
}

} // namespace
