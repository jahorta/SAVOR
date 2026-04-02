#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace simcore::tas {

    static constexpr std::chrono::year_month_day date{
        std::chrono::year{2000},
        std::chrono::month{1},
        std::chrono::day{1}
    };
    static constexpr std::chrono::hh_mm_ss time{ std::chrono::hours{0} + std::chrono::minutes{0} + std::chrono::seconds{0} };
    static constexpr std::chrono::system_clock::time_point base = std::chrono::sys_days{ date } + time.to_duration();
    static constexpr uint64_t base_sec = static_cast<uint64_t>(base.time_since_epoch().count()) / 10000000;

    struct DtmInfo {
        std::array<char, 6> game_id{};
        bool is_wii{ false };
        uint8_t controllers{ 0 };            // 0x00B
        bool starts_from_savestate{ false }; // 0x00C
        uint64_t vi_count{ 0 };              // 0x00D
        uint64_t input_count{ 0 };           // 0x015
        uint64_t lag_count{ 0 };             // 0x01D
        uint32_t rerecord_count{ 0 };        // 0x02D
        std::array<uint8_t, 16> game_md5{};  // 0x071
        uint64_t recording_start_time{ 0 };  // 0x081
        uint8_t memcard_bits{ 0 };           // 0x097
        bool memcard_blank{ false };         // 0x098
        uint64_t tick_count{ 0 };            // 0x0F0
    };

    struct DtmValidationIssue {
        enum class Severity : uint8_t { Info, Warning, Error };
        Severity severity{ Severity::Info };
        std::string code;
        std::string message;
    };

    struct DtmValidationReport {
        std::vector<DtmValidationIssue> issues;
        bool has_error() const;
    };

    struct DtmGCPoll {
        uint16_t button_bits{ 0 };
        uint8_t trigger_l{ 0 };
        uint8_t trigger_r{ 0 };
        uint8_t stick_x{ 0x80 };
        uint8_t stick_y{ 0x80 };
        uint8_t cstick_x{ 0x80 };
        uint8_t cstick_y{ 0x80 };
    };

    class DtmFile {
    public:
        static constexpr size_t kOffSignature = 0x000; // "DTM\x1A"
        static constexpr size_t kOffGameID = 0x004;
        static constexpr size_t kOffIsWii = 0x00A;
        static constexpr size_t kOffControllers = 0x00B;
        static constexpr size_t kOffStartsFromSavestate = 0x00C;
        static constexpr size_t kOffVICount = 0x00D;
        static constexpr size_t kOffInputCount = 0x015;
        static constexpr size_t kOffLagCount = 0x01D;
        static constexpr size_t kOffRerecordCount = 0x02D;
        static constexpr size_t kOffMD5 = 0x071;
        static constexpr size_t kOffRecordingStartTime = 0x081;
        static constexpr size_t kOffMemcardBits = 0x097;
        static constexpr size_t kOffMemcardBlank = 0x098;
        static constexpr size_t kOffTickCount = 0x0F0;
        static constexpr size_t kMinHeader = 0x100;

        bool load(const std::string& path);
        bool save(const std::string& path) const;

        bool valid() const { return m_valid; }
        const std::vector<uint8_t>& bytes() const { return m_bytes; }

        DtmInfo info() const;
        DtmValidationReport validate() const;

        std::string compute_sha256() const;
        std::vector<uint8_t> payload_bytes() const;

        void set_recording_start_time_unix_seconds(uint64_t unix_seconds);

        bool supports_gc_poll_editing(std::string* reason = nullptr) const;
        size_t gc_poll_count() const;
        bool read_gc_poll(size_t poll_index, DtmGCPoll& out) const;
        bool write_gc_poll(size_t poll_index, const DtmGCPoll& poll);
        bool insert_gc_polls(size_t poll_index, const std::vector<DtmGCPoll>& polls);
        bool erase_gc_polls(size_t poll_index, size_t count);

    private:
        template <class T> static T read_le(const uint8_t* p);
        template <class T> static void write_le(uint8_t* p, T v);
        static DtmGCPoll decode_gc_poll(const uint8_t* bytes8);
        static void encode_gc_poll(uint8_t* bytes8, const DtmGCPoll& poll);
        void sync_input_count_from_gc_polls();

        std::vector<uint8_t> m_bytes;
        bool m_valid{ false };
    };

} // namespace simcore::tas
