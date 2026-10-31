#include "https.h"

#include <memory>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace
{
	struct http_closer
	{
		// close the winhttp handle
		void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); }
	};
	using http_handle = std::unique_ptr<void, http_closer>;
}

// do an https GET and return the body
DWORD net::https_get(const wchar_t* host, const std::wstring& path, std::string& body)
{
	body.clear();

	http_handle session{ WinHttpOpen(L"RobloxOverlay/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
	if (!session)
		return 0;

	WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 5000);

	http_handle connection{ WinHttpConnect(session.get(), host, INTERNET_DEFAULT_HTTPS_PORT, 0) };
	if (!connection)
		return 0;

	http_handle request{ WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
	if (!request)
		return 0;

	if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(request.get(), nullptr))
		return 0;

	DWORD status = 0;
	DWORD status_size = sizeof(status);
	if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
		return 0;

	constexpr std::size_t max_body = 1024 * 1024;

	char buffer[8192];
	DWORD received = 0;
	while (body.size() < max_body && WinHttpReadData(request.get(), buffer, sizeof(buffer), &received) && received > 0)
	{
		body.append(buffer, received);
	}

	return status;
}

// percent-encode a string for a url
std::wstring net::url_encode(const std::string& text)
{
	static const wchar_t hex[] = L"0123456789ABCDEF";

	std::wstring out;
	for (unsigned char c : text)
	{
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~')
		{
			out += static_cast<wchar_t>(c);
		}
		else
		{
			out += L'%';
			out += hex[c >> 4];
			out += hex[c & 0xF];
		}
	}
	return out;
}
