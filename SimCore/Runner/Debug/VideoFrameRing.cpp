#include "VideoFrameRing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

namespace simcore::debug {

#pragma pack(push, 1)
    struct RingHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
        uint32_t slot_count;
        uint32_t max_frame_bytes;
        std::atomic<uint64_t> latest_frame_id;
        std::atomic<uint32_t> write_index;
        std::atomic<uint32_t> dropped_frames;
        uint8_t pad[96];
    };

    struct RingSlotHeader {
        std::atomic<uint64_t> frame_id;
        int64_t timestamp_sec;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
        uint32_t format;
        uint32_t color_space;
        uint32_t flags;
        uint32_t data_bytes;
        uint8_t pad[28];
    };
#pragma pack(pop)

    static constexpr uint32_t kRingMagic = 0x56524631; // "VRF1"
    static constexpr uint16_t kRingVersion = 1;

    static size_t ring_total_bytes(uint32_t slots, uint32_t max_frame_bytes) {
        return sizeof(RingHeader) + (sizeof(RingSlotHeader) + static_cast<size_t>(max_frame_bytes)) * static_cast<size_t>(slots);
    }

    static RingHeader* ring_header(uint8_t* base) {
        return reinterpret_cast<RingHeader*>(base);
    }

    static const RingHeader* ring_header(const uint8_t* base) {
        return reinterpret_cast<const RingHeader*>(base);
    }

    static RingSlotHeader* ring_slot(uint8_t* base, uint32_t idx, uint32_t max_frame_bytes) {
        const size_t off = sizeof(RingHeader) + static_cast<size_t>(idx) * (sizeof(RingSlotHeader) + static_cast<size_t>(max_frame_bytes));
        return reinterpret_cast<RingSlotHeader*>(base + off);
    }

    static const RingSlotHeader* ring_slot(const uint8_t* base, uint32_t idx, uint32_t max_frame_bytes) {
        const size_t off = sizeof(RingHeader) + static_cast<size_t>(idx) * (sizeof(RingSlotHeader) + static_cast<size_t>(max_frame_bytes));
        return reinterpret_cast<const RingSlotHeader*>(base + off);
    }

    static uint8_t* ring_slot_pixels(uint8_t* base, uint32_t idx, uint32_t max_frame_bytes) {
        auto* sh = ring_slot(base, idx, max_frame_bytes);
        return reinterpret_cast<uint8_t*>(sh + 1);
    }

    static const uint8_t* ring_slot_pixels(const uint8_t* base, uint32_t idx, uint32_t max_frame_bytes) {
        auto* sh = ring_slot(base, idx, max_frame_bytes);
        return reinterpret_cast<const uint8_t*>(sh + 1);
    }

    VideoFrameRingProducer::~VideoFrameRingProducer() {
        Close();
    }

    bool VideoFrameRingProducer::Open(const VideoRingConfig& cfg) {
        Close();
        if (cfg.mapping_name.empty() || cfg.slot_count == 0 || cfg.max_frame_bytes == 0) return false;

        const size_t total = ring_total_bytes(cfg.slot_count, cfg.max_frame_bytes);
        h_map_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(total >> 32), static_cast<DWORD>(total & 0xFFFFFFFFu), cfg.mapping_name.c_str());
        if (!h_map_) return false;

        base_ = static_cast<uint8_t*>(MapViewOfFile(h_map_, FILE_MAP_ALL_ACCESS, 0, 0, total));
        if (!base_) {
            CloseHandle(h_map_);
            h_map_ = nullptr;
            return false;
        }

        slot_count_ = cfg.slot_count;
        max_frame_bytes_ = cfg.max_frame_bytes;

        auto* h = ring_header(base_);
        std::memset(base_, 0, total);
        h->magic = kRingMagic;
        h->version = kRingVersion;
        h->slot_count = slot_count_;
        h->max_frame_bytes = max_frame_bytes_;
        h->latest_frame_id.store(0, std::memory_order_release);
        h->write_index.store(0, std::memory_order_release);
        h->dropped_frames.store(0, std::memory_order_release);

        return true;
    }

    void VideoFrameRingProducer::Close() {
        if (base_) {
            UnmapViewOfFile(base_);
            base_ = nullptr;
        }
        if (h_map_) {
            CloseHandle(h_map_);
            h_map_ = nullptr;
        }
        slot_count_ = 0;
        max_frame_bytes_ = 0;
    }

    bool VideoFrameRingProducer::WriteFrame(const VideoFrameDesc& desc, const void* pixels, size_t bytes) {
        if (!base_ || !pixels || bytes == 0) return false;
        if (bytes > max_frame_bytes_) return false;

        auto* h = ring_header(base_);
        if (h->magic != kRingMagic || h->version != kRingVersion) return false;

        uint32_t wi = h->write_index.load(std::memory_order_acquire);
        uint32_t idx = wi % slot_count_;

        auto* sh = ring_slot(base_, idx, max_frame_bytes_);
        auto* dst = ring_slot_pixels(base_, idx, max_frame_bytes_);

        std::memcpy(dst, pixels, bytes);

        sh->timestamp_sec = desc.timestamp_sec;
        sh->width = desc.width;
        sh->height = desc.height;
        sh->stride = desc.stride;
        sh->format = static_cast<uint32_t>(desc.format);
        sh->color_space = desc.color_space;
        sh->flags = desc.flags;
        sh->data_bytes = static_cast<uint32_t>(bytes);
        sh->frame_id.store(desc.frame_id, std::memory_order_release);

        h->latest_frame_id.store(desc.frame_id, std::memory_order_release);
        h->write_index.store((wi + 1) % slot_count_, std::memory_order_release);

        return true;
    }

    VideoFrameRingConsumer::~VideoFrameRingConsumer() {
        Close();
    }

    bool VideoFrameRingConsumer::Open(const std::string& mapping_name) {
        Close();
        if (mapping_name.empty()) return false;

        h_map_ = OpenFileMappingA(FILE_MAP_READ, FALSE, mapping_name.c_str());
        if (!h_map_) return false;

        base_ = static_cast<const uint8_t*>(MapViewOfFile(h_map_, FILE_MAP_READ, 0, 0, 0));
        if (!base_) {
            CloseHandle(h_map_);
            h_map_ = nullptr;
            return false;
        }

        const auto* h = ring_header(base_);
        if (h->magic != kRingMagic || h->version != kRingVersion || h->slot_count == 0 || h->max_frame_bytes == 0) {
            Close();
            return false;
        }

        slot_count_ = h->slot_count;
        max_frame_bytes_ = h->max_frame_bytes;
        last_seen_frame_id_ = 0;
        return true;
    }

    void VideoFrameRingConsumer::Close() {
        if (base_) {
            UnmapViewOfFile(base_);
            base_ = nullptr;
        }
        if (h_map_) {
            CloseHandle(h_map_);
            h_map_ = nullptr;
        }
        slot_count_ = 0;
        max_frame_bytes_ = 0;
        last_seen_frame_id_ = 0;
    }

    bool VideoFrameRingConsumer::ReadLatest(VideoFrameDesc& out_desc, std::vector<uint8_t>& out_pixels) {
        if (!base_) return false;

        const auto* h = ring_header(base_);
        const uint64_t latest = h->latest_frame_id.load(std::memory_order_acquire);
        if (latest == 0 || latest == last_seen_frame_id_) return false;

        bool found = false;
        VideoFrameDesc tmp{};
        std::vector<uint8_t> tmp_pixels;

        for (uint32_t i = 0; i < slot_count_; ++i) {
            const auto* sh = ring_slot(base_, i, max_frame_bytes_);
            const uint64_t fid = sh->frame_id.load(std::memory_order_acquire);
            if (fid != latest) continue;
            const uint32_t bytes = std::min(sh->data_bytes, max_frame_bytes_);
            if (bytes == 0) break;

            const auto* src = ring_slot_pixels(base_, i, max_frame_bytes_);
            tmp_pixels.resize(bytes);
            std::memcpy(tmp_pixels.data(), src, bytes);

            tmp.frame_id = fid;
            tmp.timestamp_sec = sh->timestamp_sec;
            tmp.width = sh->width;
            tmp.height = sh->height;
            tmp.stride = sh->stride;
            tmp.format = static_cast<VideoPixelFormat>(sh->format);
            tmp.color_space = sh->color_space;
            tmp.flags = sh->flags;
            tmp.data_bytes = bytes;
            found = true;
            break;
        }

        if (!found) return false;

        out_desc = tmp;
        out_pixels = std::move(tmp_pixels);
        last_seen_frame_id_ = latest;
        return true;
    }

    std::string MakeVideoRingMappingName(int64_t session_id, const std::string& token) {
        return std::string("Local\\SOASim.DebugVideo.") + std::to_string(static_cast<long long>(session_id)) + "." + token;
    }

} // namespace simcore::debug
