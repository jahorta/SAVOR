#include <gtest/gtest.h>

#include "Utils/Log.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using savor::logger::Level;
using savor::logger::Logger;
using savor::logger::LoggerStatistics;
using savor::logger::FileOpenMode;

[[nodiscard]] std::filesystem::path LogPath(std::string_view name)
{
    return std::filesystem::temp_directory_path() /
        ("savor-async-log-" + std::string(name) + "-" +
         std::to_string(std::chrono::steady_clock::now()
             .time_since_epoch().count()) + ".log");
}

[[nodiscard]] std::uint64_t Dropped(const LoggerStatistics& statistics)
{
    std::uint64_t total = statistics.dropped_formatting;
    for (std::size_t index = 0;
         index < statistics.dropped_queue_busy.size(); ++index)
    {
        total += statistics.dropped_queue_busy[index];
        total += statistics.dropped_queue_full[index];
    }
    return total;
}

TEST(AsyncLogger, FlushPreservesFifoOrderAndShutdownIsIdempotent)
{
    Logger& logger = Logger::get();
    logger.Shutdown();
    logger.set_levels(Level::Off, Level::Debug);
    const auto path = LogPath("fifo");
    ASSERT_TRUE(logger.open_file(path.string().c_str()));

    logger.logf(Level::Info, __FILE__, __LINE__, __func__, "fifo-one");
    logger.logf(Level::Info, __FILE__, __LINE__, __func__, "fifo-two");
    logger.logf(Level::Warn, __FILE__, __LINE__, __func__, "fifo-three");
    logger.Flush();
    logger.close_file();

    std::ifstream input(path, std::ios::binary);
    const std::string content{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const auto first = content.find("fifo-one");
    const auto second = content.find("fifo-two");
    const auto third = content.find("fifo-three");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(second, std::string::npos);
    ASSERT_NE(third, std::string::npos);
    EXPECT_LT(first, second);
    EXPECT_LT(second, third);

    logger.Shutdown();
    logger.Shutdown();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(AsyncLogger, ContendedProducersNeverWaitForSinkAndLossIsAccounted)
{
    Logger& logger = Logger::get();
    logger.Shutdown();
    logger.set_levels(Level::Off, Level::Debug);
    const auto path = LogPath("overflow");
    ASSERT_TRUE(logger.open_file(path.string().c_str()));
    const LoggerStatistics before = logger.statistics();

    constexpr std::size_t kThreads = 12;
    constexpr std::size_t kRecordsPerThread = 20'000;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> begin{false};
    std::vector<std::thread> producers;
    producers.reserve(kThreads);
    for (std::size_t thread = 0; thread < kThreads; ++thread)
    {
        producers.emplace_back([&, thread] {
            ready.fetch_add(1, std::memory_order_release);
            while (!begin.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (std::size_t record = 0;
                 record < kRecordsPerThread; ++record)
            {
                logger.logf(
                    Level::Debug, __FILE__, __LINE__, __func__,
                    "producer=%zu record=%zu", thread, record);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != kThreads)
        std::this_thread::yield();
    const auto started = std::chrono::steady_clock::now();
    begin.store(true, std::memory_order_release);
    for (std::thread& producer : producers)
        producer.join();
    const auto producer_elapsed = std::chrono::steady_clock::now() - started;

    logger.Flush();
    const LoggerStatistics after = logger.statistics();
    const std::uint64_t accepted = after.accepted - before.accepted;
    const std::uint64_t dropped = Dropped(after) - Dropped(before);
    EXPECT_EQ(accepted + dropped, kThreads * kRecordsPerThread);
    EXPECT_GT(dropped, 0u);
    EXPECT_LT(producer_elapsed, std::chrono::seconds(10));
    EXPECT_GE(after.written, after.accepted);

    logger.Shutdown();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(AsyncLogger, ExclusiveCreateNeverModifiesExistingFile)
{
    Logger& logger = Logger::get();
    logger.Shutdown();
    logger.set_levels(Level::Off, Level::Debug);
    const auto path = LogPath("exclusive");
    {
        std::ofstream output(path, std::ios::binary);
        output << "original";
    }

    EXPECT_FALSE(logger.open_file(
        path.string().c_str(),
        FileOpenMode::CreateNew));

    std::ifstream input(path, std::ios::binary);
    const std::string content{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    EXPECT_EQ(content, "original");

    logger.Shutdown();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace
