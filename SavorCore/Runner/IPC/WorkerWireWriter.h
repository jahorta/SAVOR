#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <mutex>

namespace savor {

struct WorkerWirePart {
    const void* data = nullptr;
    std::size_t size = 0;
};

class WorkerWireWriter {
public:
    using WriteFunction = std::function<bool(const void*, std::size_t)>;

    explicit WorkerWireWriter(WriteFunction write_function);

    bool write_parts(std::initializer_list<WorkerWirePart> parts);

    template <typename T>
    bool write_object(const T& value)
    {
        return write_parts({ WorkerWirePart{ &value, sizeof(value) } });
    }

private:
    WriteFunction write_function_;
    std::mutex mutex_;
};

} // namespace savor
