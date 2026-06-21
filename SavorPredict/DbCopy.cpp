#include "DbCopy.h"

#include "DbRootCopy.h"

namespace savor::predict {

int run_prepare_db(const PrepareDbOptions& options, std::ostream& out, std::ostream& err) {
    return savor::dbutils::CopyDbRootFull(
        {
            .source_root = options.source,
            .dest_root = options.dest,
            .overwrite = options.overwrite,
        },
        out,
        err);
}

} // namespace savor::predict
