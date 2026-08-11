#pragma once
#include <mutex>
#include <string_view>
#include <optional>

#include "GCInputFrame.h"
#include "../../Utils/Log.h"

namespace savor {

    // Provides a Dolphin controller override for one GC port.
    // One publication remains held until a later publication replaces it.
    class GCPadOverride {
    public:
        struct PollStats {
            uint64_t publication_epoch = 0;
            uint32_t callback_count = 0;
            GCInputFrame frame{};
        };

        explicit GCPadOverride(int port = 0) : m_port(port) {}
        ~GCPadOverride() = default;

        // Install the override lambda on the Dolphin controller for this port.
        // Safe to call multiple times; it is a no-op after the first successful install.
        void install();

        // Publish one held controller state and reset its poll acknowledgement.
        void publishFrame(uint64_t publication_epoch, const GCInputFrame& f);
        PollStats getPollStats() const;

        // Convenience: centered sticks, no buttons.
        static GCInputFrame NeutralFrame();

        // For diagnostics
        bool isInstalled() const { return m_installed; }
        int  port()        const { return m_port; }

    private:
        int m_port{ 0 };
        bool m_installed{ false };

        GCInputFrame m_cur{}; // guarded by m_mtx
        uint64_t m_publication_epoch{ 0 };
        uint32_t m_callback_count{ 0 };
        mutable std::mutex m_mtx;
    };

} // namespace savor
