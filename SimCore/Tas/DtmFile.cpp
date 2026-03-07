#include "DtmFile.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string.h>

using namespace simcore::tas;

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

bool DtmFile::load(const std::string& path)
{
	m_bytes.clear(); m_valid = false;
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
	return i;
}

void DtmFile::set_recording_start_time(uint64_t unix_time, bool is_delta)
{
	if (!m_valid) return;
	if (!is_delta) unix_time += base_sec;
	write_le<uint64_t>(m_bytes.data() + kOffRecordingStartTime, unix_time);
}
