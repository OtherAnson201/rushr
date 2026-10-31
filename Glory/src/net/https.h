#pragma once
#include <windows.h>
#include <string>

namespace net
{

	DWORD https_get(const wchar_t* host, const std::wstring& path, std::string& body);

	std::wstring url_encode(const std::string& text);
}
