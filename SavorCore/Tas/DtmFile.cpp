#include "DtmFile.h"

#include "../Utils/Hash.h"

#include <algorithm>
#include <cstring>
#include <fstream>

using namespace savor::tas;

template <class T>
T DtmFile::read_le(const uint8_t* p)
{
    T v{};
    for (size_t i = 0; i < sizeof(T); ++i) v |= (T)p[i] << (8 * i);
    return v;
}

template <class T>
void DtmFile::write_le(uint8_t* p, T v)
{
    for (size_t i = 0; i < sizeof(T); ++i) p[i] = uint8_t((v >> (8 * i)) & 0xFF);
}

bool DtmValidationReport::has_error() const
{
    return std::any_of(issues.begin(), issues.end(), [](const DtmValidationIssue& issue) {
        return issue.severity == DtmValidationIssue::Severity::Error;
        });
}

bool DtmFile::load(const std::string& path)
{
    m_bytes.clear();
    m_valid = false;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const auto sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    m_bytes.resize(sz);
    if (!f.read((char*)m_bytes.data(), m_bytes.size())) return false;
    if (m_bytes.size() < kMinHeader) return false;

    const uint8_t sig[4]{ 'D','T','M',0x1A };
    if (memcmp(m_bytes.data() + kOffSignature, sig, 4) != 0) return false;

    m_valid = true;
    return true;
}

bool DtmFile::save(const std::string& path) const
{
    if (!m_valid) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)m_bytes.data(), m_bytes.size());
    return (bool)f;
}

DtmInfo DtmFile::info() const
{
    DtmInfo i{};
    if (!m_valid) return i;
    memcpy(i.game_id.data(), m_bytes.data() + kOffGameID, 6);
    i.is_wii = m_bytes[kOffIsWii] != 0;
    i.controllers = m_bytes[kOffControllers];
    i.starts_from_savestate = m_bytes[kOffStartsFromSavestate] != 0;
    i.vi_count = read_le<uint64_t>(m_bytes.data() + kOffVICount);
    i.input_count = read_le<uint64_t>(m_bytes.data() + kOffInputCount);
    i.lag_count = read_le<uint64_t>(m_bytes.data() + kOffLagCount);
    i.rerecord_count = read_le<uint32_t>(m_bytes.data() + kOffRerecordCount);
    memcpy(i.game_md5.data(), m_bytes.data() + kOffMD5, 16);
    i.recording_start_time = read_le<uint64_t>(m_bytes.data() + kOffRecordingStartTime);
    i.memcard_bits = m_bytes[kOffMemcardBits];
    i.memcard_blank = m_bytes[kOffMemcardBlank] != 0;
    i.tick_count = read_le<uint64_t>(m_bytes.data() + kOffTickCount);
    return i;
}

DtmValidationReport DtmFile::validate() const
{
    DtmValidationReport report;
    if (!m_valid) {
        report.issues.push_back({ DtmValidationIssue::Severity::Error, "invalid_state", "File was not successfully loaded as a DTM." });
        return report;
    }

    if (m_bytes.size() < kMinHeader) {
        report.issues.push_back({ DtmValidationIssue::Severity::Error, "header_too_small", "DTM file is smaller than the 256-byte header." });
        return report;
    }

    const uint8_t sig[4]{ 'D','T','M',0x1A };
    if (memcmp(m_bytes.data() + kOffSignature, sig, 4) != 0) {
        report.issues.push_back({ DtmValidationIssue::Severity::Error, "bad_signature", "Header signature does not match DTM magic." });
    }

    const bool has_gc = (m_bytes[kOffControllers] & 0x0F) != 0;
    const bool has_wii = (m_bytes[kOffControllers] & 0xF0) != 0;
    if (!has_gc && !has_wii) {
        report.issues.push_back({ DtmValidationIssue::Severity::Warning, "no_controllers", "No controller bits are set in the DTM header." });
    }

    if (has_gc && !has_wii) {
        const size_t payload = m_bytes.size() - kMinHeader;
        if ((payload % 8) != 0) {
            report.issues.push_back({ DtmValidationIssue::Severity::Error, "payload_alignment", "GC-only payload must be divisible by 8 bytes per poll record." });
        }
        const uint64_t input_count = read_le<uint64_t>(m_bytes.data() + kOffInputCount);
        if (input_count != static_cast<uint64_t>(payload / 8)) {
            report.issues.push_back({ DtmValidationIssue::Severity::Warning, "input_count_mismatch", "Header input_count differs from GC payload derived poll count." });
        }
    }

    if (has_wii) {
        report.issues.push_back({ DtmValidationIssue::Severity::Warning, "wii_payload_opaque", "Wii input records are present; editor currently treats Wii poll payload as opaque." });
    }

    return report;
}

std::string DtmFile::compute_sha256() const
{
    if (!m_valid) return {};
    return hash::sha256(m_bytes.data(), m_bytes.size());
}

std::vector<uint8_t> DtmFile::payload_bytes() const
{
    if (!m_valid || m_bytes.size() <= kMinHeader) return {};
    return std::vector<uint8_t>(m_bytes.begin() + static_cast<std::ptrdiff_t>(kMinHeader), m_bytes.end());
}

void DtmFile::set_recording_start_time_unix_seconds(uint64_t unix_seconds)
{
    if (!m_valid) return;
    write_le<uint64_t>(m_bytes.data() + kOffRecordingStartTime, unix_seconds + base_sec);
}

bool DtmFile::supports_gc_poll_editing(std::string* reason) const
{
    if (!m_valid) {
        if (reason) *reason = "DTM is not loaded/valid.";
        return false;
    }
    const uint8_t controllers = m_bytes[kOffControllers];
    const bool has_gc = (controllers & 0x0F) != 0;
    const bool has_wii = (controllers & 0xF0) != 0;
    if (!has_gc) {
        if (reason) *reason = "GC controller bits are not set; GC poll editor is unavailable.";
        return false;
    }
    if (has_wii) {
        if (reason) *reason = "Wii controller bits are set; mixed payload decoding is currently unsupported for safe editing.";
        return false;
    }
    const size_t payload_size = m_bytes.size() - kMinHeader;
    if ((payload_size % 8) != 0) {
        if (reason) *reason = "Payload size is not divisible by 8 bytes (GC poll size).";
        return false;
    }
    return true;
}

size_t DtmFile::gc_poll_count() const
{
    if (!supports_gc_poll_editing(nullptr)) return 0;
    return (m_bytes.size() - kMinHeader) / 8;
}

DtmGCPoll DtmFile::decode_gc_poll(const uint8_t* bytes8)
{
    DtmGCPoll poll;
    poll.button_bits = read_le<uint16_t>(bytes8);
    poll.trigger_l = bytes8[2];
    poll.trigger_r = bytes8[3];
    poll.stick_x = bytes8[4];
    poll.stick_y = bytes8[5];
    poll.cstick_x = bytes8[6];
    poll.cstick_y = bytes8[7];
    return poll;
}

void DtmFile::encode_gc_poll(uint8_t* bytes8, const DtmGCPoll& poll)
{
    write_le<uint16_t>(bytes8, poll.button_bits);
    bytes8[2] = poll.trigger_l;
    bytes8[3] = poll.trigger_r;
    bytes8[4] = poll.stick_x;
    bytes8[5] = poll.stick_y;
    bytes8[6] = poll.cstick_x;
    bytes8[7] = poll.cstick_y;
}

bool DtmFile::read_gc_poll(size_t poll_index, DtmGCPoll& out) const
{
    if (!supports_gc_poll_editing(nullptr)) return false;
    const size_t count = gc_poll_count();
    if (poll_index >= count) return false;
    const size_t offset = kMinHeader + poll_index * 8;
    out = decode_gc_poll(m_bytes.data() + offset);
    return true;
}

bool DtmFile::write_gc_poll(size_t poll_index, const DtmGCPoll& poll)
{
    if (!supports_gc_poll_editing(nullptr)) return false;
    const size_t count = gc_poll_count();
    if (poll_index >= count) return false;
    const size_t offset = kMinHeader + poll_index * 8;
    encode_gc_poll(m_bytes.data() + offset, poll);
    return true;
}

bool DtmFile::insert_gc_polls(size_t poll_index, const std::vector<DtmGCPoll>& polls)
{
    if (!supports_gc_poll_editing(nullptr)) return false;
    const size_t count = gc_poll_count();
    if (poll_index > count) return false;
    if (polls.empty()) return true;

    std::vector<uint8_t> encoded;
    encoded.resize(polls.size() * 8);
    for (size_t i = 0; i < polls.size(); ++i) {
        encode_gc_poll(encoded.data() + (i * 8), polls[i]);
    }

    const auto insert_it = m_bytes.begin() + static_cast<std::ptrdiff_t>(kMinHeader + poll_index * 8);
    m_bytes.insert(insert_it, encoded.begin(), encoded.end());
    sync_input_count_from_gc_polls();
    return true;
}

bool DtmFile::erase_gc_polls(size_t poll_index, size_t count)
{
    if (!supports_gc_poll_editing(nullptr)) return false;
    const size_t total = gc_poll_count();
    if (poll_index >= total) return false;
    const size_t erase_count = std::min(count, total - poll_index);
    if (erase_count == 0) return true;

    const size_t off_begin = kMinHeader + poll_index * 8;
    const size_t off_end = off_begin + erase_count * 8;
    auto it_begin = m_bytes.begin() + static_cast<std::ptrdiff_t>(off_begin);
    auto it_end = m_bytes.begin() + static_cast<std::ptrdiff_t>(off_end);
    m_bytes.erase(it_begin, it_end);
    sync_input_count_from_gc_polls();
    return true;
}

void DtmFile::sync_input_count_from_gc_polls()
{
    if (!supports_gc_poll_editing(nullptr)) return;
    write_le<uint64_t>(m_bytes.data() + kOffInputCount, gc_poll_count());
}
