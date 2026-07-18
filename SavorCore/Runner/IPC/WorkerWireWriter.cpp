#include "WorkerWireWriter.h"

#include <utility>

namespace savor {

WorkerWireWriter::WorkerWireWriter(WriteFunction write_function)
    : write_function_(std::move(write_function))
{
}

bool WorkerWireWriter::write_parts(std::initializer_list<WorkerWirePart> parts)
{
    std::scoped_lock lock(mutex_);
    if (!write_function_)
        return false;
    for (const auto& part : parts) {
        if (part.size != 0 && (!part.data || !write_function_(part.data, part.size)))
            return false;
    }
    return true;
}

} // namespace savor
