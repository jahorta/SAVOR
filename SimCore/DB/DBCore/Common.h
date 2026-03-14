#pragma once

namespace simcore::db {
    enum class Compression { None = 0, Zstd = 1, lz4 = 2 };
}