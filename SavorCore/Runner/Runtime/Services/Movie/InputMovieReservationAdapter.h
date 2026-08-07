#pragma once

#include "Runner/Runtime/Services/Input/InputArbiter.h"
#include "Runner/Runtime/Services/Movie/IMovieBackendPort.h"

#include <functional>
#include <optional>

namespace savor::runtime {

class InputMovieReservationAdapter final
    : public IMovieInputReservationPort
{
public:
    InputMovieReservationAdapter(
        InputArbiter& input,
        std::function<WorksetEpoch()> current_epoch);

    MovieInputReservationReceipt
    AcquireUnsuspendableMovieReservation() override;
    MovieServiceResult ReleaseMovieReservation(
        MovieReservationId reservation) noexcept override;

    [[nodiscard]] MovieServiceResult Shutdown() noexcept;

private:
    [[nodiscard]] InputLeaseReceipt AcquireLease(WorksetEpoch epoch);

    InputArbiter& input_;
    std::function<WorksetEpoch()> current_epoch_;
    MovieReservationId reservation_;
    InputLeaseId lease_;
    std::uint64_t next_reservation_ = 1;
};

} // namespace savor::runtime
