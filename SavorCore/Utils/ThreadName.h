#pragma once
#include <windows.h>
#include <string>

inline void set_this_thread_name_utf8(const char* name) {
	if (!name) return;
	int wlen = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
	if (wlen <= 0) return;
	std::wstring wname(static_cast<size_t>(wlen), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, name, -1, wname.data(), wlen);
	SetThreadDescription(GetCurrentThread(), wname.c_str());
}
