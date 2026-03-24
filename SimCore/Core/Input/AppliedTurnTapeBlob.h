#pragma once
#include "InputPlan.h"
#include "InputPlanFmt.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>

namespace simcore::inputtape {

    struct TurnChunk {
        uint32_t turn_number{ 0 };
        uint32_t vi_start{ 0 };
        uint32_t vi_end{ 0 };
        std::vector<uint32_t> vi_durations{};
        simcore::InputPlan frames{};
    };

    inline void write_u32(std::string& out, uint32_t v) {
        char b[4]{
            static_cast<char>(v & 0xFF),
            static_cast<char>((v >> 8) & 0xFF),
            static_cast<char>((v >> 16) & 0xFF),
            static_cast<char>((v >> 24) & 0xFF)
        };
        out.append(b, 4);
    }

    inline bool read_u32(const std::string& in, size_t& off, uint32_t& out) {
        if (off + 4 > in.size()) return false;
        const auto* p = reinterpret_cast<const uint8_t*>(in.data() + off);
        out = static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
        off += 4;
        return true;
    }

    inline bool ensure_header(std::string& blob) {
        if (blob.empty()) {
            blob.append("AITB", 4);
            write_u32(blob, 1u); // version
            write_u32(blob, 0u); // segment_count
            return true;
        }
        if (blob.size() < 12) return false;
        if (std::memcmp(blob.data(), "AITB", 4) != 0) return false;
        size_t off = 4;
        uint32_t ver = 0;
        if (!read_u32(blob, off, ver)) return false;
        return ver == 1u;
    }

    inline bool append_turn_chunk(std::string& blob, const TurnChunk& chunk) {
        if (!ensure_header(blob)) return false;
        if (chunk.vi_durations.size() != chunk.frames.size()) return false;

        size_t count_off = 8; // after magic + version
        uint32_t seg_count = 0;
        {
            size_t o = count_off;
            if (!read_u32(blob, o, seg_count)) return false;
        }

        write_u32(blob, chunk.turn_number);
        write_u32(blob, chunk.vi_start);
        write_u32(blob, chunk.vi_end);
        write_u32(blob, static_cast<uint32_t>(chunk.frames.size()));
        write_u32(blob, static_cast<uint32_t>(chunk.vi_durations.size()));

        if (!chunk.frames.empty()) {
            blob.append(reinterpret_cast<const char*>(chunk.frames.data()), chunk.frames.size() * sizeof(simcore::GCInputFrame));
        }
        if (!chunk.vi_durations.empty()) {
            blob.append(reinterpret_cast<const char*>(chunk.vi_durations.data()), chunk.vi_durations.size() * sizeof(uint32_t));
        }

        ++seg_count;
        blob[count_off + 0] = static_cast<char>(seg_count & 0xFF);
        blob[count_off + 1] = static_cast<char>((seg_count >> 8) & 0xFF);
        blob[count_off + 2] = static_cast<char>((seg_count >> 16) & 0xFF);
        blob[count_off + 3] = static_cast<char>((seg_count >> 24) & 0xFF);
        return true;
    }

    inline bool decode_turn_chunks(const std::string& blob, std::vector<TurnChunk>& out) {
        out.clear();
        if (blob.empty()) return true;
        if (blob.size() < 12) return false;
        if (std::memcmp(blob.data(), "AITB", 4) != 0) return false;

        size_t off = 4;
        uint32_t ver = 0, seg_count = 0;
        if (!read_u32(blob, off, ver)) return false;
        if (!read_u32(blob, off, seg_count)) return false;
        if (ver != 1u) return false;

        out.reserve(seg_count);
        for (uint32_t i = 0; i < seg_count; ++i) {
            TurnChunk c{};
            uint32_t frame_count = 0, dur_count = 0;
            if (!read_u32(blob, off, c.turn_number)) return false;
            if (!read_u32(blob, off, c.vi_start)) return false;
            if (!read_u32(blob, off, c.vi_end)) return false;
            if (!read_u32(blob, off, frame_count)) return false;
            if (!read_u32(blob, off, dur_count)) return false;
            if (frame_count != dur_count) return false;

            const size_t frame_bytes = static_cast<size_t>(frame_count) * sizeof(simcore::GCInputFrame);
            const size_t dur_bytes = static_cast<size_t>(dur_count) * sizeof(uint32_t);
            if (off + frame_bytes + dur_bytes > blob.size()) return false;

            c.frames.resize(frame_count);
            if (frame_count) {
                std::memcpy(c.frames.data(), blob.data() + off, frame_bytes);
                off += frame_bytes;
            }

            c.vi_durations.resize(dur_count);
            if (dur_count) {
                std::memcpy(c.vi_durations.data(), blob.data() + off, dur_bytes);
                off += dur_bytes;
            }

            out.push_back(std::move(c));
        }

        return off == blob.size();
    }

    inline std::string turn_windows_csv(const std::vector<TurnChunk>& chunks) {
        std::ostringstream out;
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (i) out << ";";
            out << "t" << chunks[i].turn_number << ":" << chunks[i].vi_start << "-" << chunks[i].vi_end;
        }
        return out.str();
    }

    inline std::string durations_csv(const std::vector<TurnChunk>& chunks) {
        std::ostringstream out;
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (i) out << ";";
            out << "t" << chunks[i].turn_number << ":";
            for (size_t j = 0; j < chunks[i].vi_durations.size(); ++j) {
                if (j) out << ",";
                out << chunks[i].vi_durations[j];
            }
        }
        return out.str();
    }

    inline std::string render_text(const std::vector<TurnChunk>& chunks) {
        std::ostringstream out;
        for (const auto& c : chunks) {
            out << "Turn " << c.turn_number << " (vi_start=" << c.vi_start << ", vi_end=" << c.vi_end << ")\n";
            uint32_t vi_cursor = c.vi_start;
            for (size_t i = 0; i < c.frames.size(); ++i) {
                const uint32_t vi_dur = (i < c.vi_durations.size()) ? c.vi_durations[i] : 0u;
                out << "  f" << i
                    << " vi_start=" << vi_cursor
                    << " vi_dur=" << vi_dur
                    << " : " << simcore::DescribeFrameCompact(c.frames[i]) << "\n";
                vi_cursor += vi_dur;
            }
        }
        return out.str();
    }

    inline simcore::InputPlan flatten_plan(const std::vector<TurnChunk>& chunks) {
        simcore::InputPlan out;
        size_t n = 0;
        for (const auto& c : chunks) n += c.frames.size();
        out.reserve(n);
        for (const auto& c : chunks) out.insert(out.end(), c.frames.begin(), c.frames.end());
        return out;
    }
}

