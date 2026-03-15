#pragma once

#include "../../framework.h"

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace simcore::debug {

    enum class VideoPixelFormat : uint32_t {
        BGRA8 = 1,
        RGBA8 = 2,
    };

    struct VideoFrameDesc {
        uint64_t frame_id{ 0 };
        int64_t timestamp_sec{ 0 };
        uint32_t width{ 0 };
        uint32_t height{ 0 };
        uint32_t stride{ 0 };
        VideoPixelFormat format{ VideoPixelFormat::BGRA8 };
        uint32_t color_space{ 1 }; // 1=sRGB
        uint32_t flags{ 0 };
        uint32_t data_bytes{ 0 };
    };

    struct VideoRingConfig {
        std::string mapping_name;
        uint32_t slot_count{ 4 };
        uint32_t max_frame_bytes{ 1920 * 1080 * 4 };
    };

    class VideoFrameRingProducer {
    public:
        VideoFrameRingProducer() = default;
        ~VideoFrameRingProducer();

        VideoFrameRingProducer(const VideoFrameRingProducer&) = delete;
        VideoFrameRingProducer& operator=(const VideoFrameRingProducer&) = delete;

        bool Open(const VideoRingConfig& cfg);
        void Close();
        bool IsOpen() const { return h_map_ != nullptr; }

        bool WriteFrame(const VideoFrameDesc& desc, const void* pixels, size_t bytes);

    private:
        HANDLE h_map_{ nullptr };
        uint8_t* base_{ nullptr };
        uint32_t slot_count_{ 0 };
        uint32_t max_frame_bytes_{ 0 };
    };

    class VideoFrameRingConsumer {
    public:
        VideoFrameRingConsumer() = default;
        ~VideoFrameRingConsumer();

        VideoFrameRingConsumer(const VideoFrameRingConsumer&) = delete;
        VideoFrameRingConsumer& operator=(const VideoFrameRingConsumer&) = delete;

        bool Open(const std::string& mapping_name);
        void Close();
        bool IsOpen() const { return h_map_ != nullptr; }

        bool ReadLatest(VideoFrameDesc& out_desc, std::vector<uint8_t>& out_pixels);

    private:
        HANDLE h_map_{ nullptr };
        const uint8_t* base_{ nullptr };
        uint32_t slot_count_{ 0 };
        uint32_t max_frame_bytes_{ 0 };
        uint64_t last_seen_frame_id_{ 0 };
    };

    std::string MakeVideoRingMappingName(int64_t session_id, const std::string& token);

} // namespace simcore::debug
