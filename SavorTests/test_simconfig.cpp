#include <gtest/gtest.h>
#include <system_error>
#include "Core/Config/SimConfig.h"

using namespace savor;

namespace {
    std::filesystem::path NormalizeForCompare(const std::filesystem::path& path) {
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(path, ec);
        if (!ec && !canonical.empty()) {
            return canonical;
        }

        ec.clear();
        auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
        if (ec) {
            absolute = path;
        }
        return absolute.lexically_normal();
    }
}

TEST(SimConfig, RoundTrip) {
    auto tmp = std::filesystem::temp_directory_path() / "savor_cfg_test";
    std::filesystem::create_directories(tmp);
    auto cfgfile = tmp / "simulator.ini";

    SimConfig in{ tmp / "User", tmp / "QtBase" };
    std::filesystem::create_directories(in.user_dir / "DB");
    std::filesystem::create_directories(in.dolphin_base_dir);
    std::filesystem::create_directories(in.user_dir / "ArchiveStore");

    std::string err;
    ASSERT_TRUE(SimConfigIO::Save(in, cfgfile, &err)) << err;

    auto out = SimConfigIO::Load(cfgfile, &err);
    ASSERT_TRUE(out.has_value()) << err;
    EXPECT_EQ(NormalizeForCompare(in.user_dir),
        NormalizeForCompare(out->user_dir));
    EXPECT_EQ(NormalizeForCompare(in.dolphin_base_dir),
        NormalizeForCompare(out->dolphin_base_dir));
    EXPECT_EQ(NormalizeForCompare(tmp / "User" / "DB" / "Execution.sqlite"),
        NormalizeForCompare(out->execution_db_path));
    EXPECT_EQ(NormalizeForCompare(tmp / "User" / "DB" / "Analysis.sqlite"),
        NormalizeForCompare(out->analysis_db_path));
    EXPECT_EQ(NormalizeForCompare(tmp / "User" / "ArchiveStore"),
        NormalizeForCompare(out->archive_store_root));
}
