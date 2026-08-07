#pragma once

#include "../../RuntimeTypes.h"
#include "IGuestMemoryBackendPort.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace savor::runtime {

enum class GuestScalarWidth : std::uint8_t
{
    U8 = 1,
    U16 = 2,
    U32 = 4,
    U64 = 8,
};

struct GuestReadReceipt
{
    bool ok = false;
    WorksetEpoch epoch;
    std::uint32_t address = 0;
    GuestScalarWidth width = GuestScalarWidth::U8;
    std::uint64_t value = 0;
    std::string message;
};

class GuestMemory final
{
public:
    explicit GuestMemory(IGuestMemoryBackendPort& backend);

    void InitializeWorksetEpoch(WorksetEpoch epoch) noexcept;
    [[nodiscard]] GuestReadReceipt ReadScalar(
        std::uint32_t address,
        GuestScalarWidth width,
        WorksetEpoch epoch) const;
    [[nodiscard]] GuestBytesResult ReadBytes(
        std::uint32_t address,
        std::size_t size,
        WorksetEpoch epoch) const;

private:
    [[nodiscard]] bool OnOwnerThread() const noexcept;

    IGuestMemoryBackendPort& backend_;
    std::thread::id owner_thread_;
    WorksetEpoch epoch_;
};

} // namespace savor::runtime
