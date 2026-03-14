#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <iostream>

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
		static constexpr size_t kMinHeader = 0x100;

		bool load(const std::string& path);
		bool save(const std::string& path) const;

		bool valid() const { return m_valid; }
		const std::vector<uint8_t>& bytes() const { return m_bytes; }

		DtmInfo info() const;
		void set_recording_start_time(uint64_t unix_time, bool is_delta = true);

	private:
		template <class T> static T read_le(const uint8_t* p);
		template <class T> static void write_le(uint8_t* p, T v);

		std::vector<uint8_t> m_bytes;
		bool m_valid{ false };
	};

} // namespace simcore::tas
