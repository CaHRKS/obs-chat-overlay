#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "net.hpp"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <random>

namespace {

struct Url {
	bool tls = true;
	std::wstring host;
	int port = 443;
	std::wstring path = L"/";
};

std::wstring widen(const std::string &text)
{
	if (text.empty())
		return std::wstring();
	const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
	if (length <= 0)
		return std::wstring();
	std::wstring out((size_t)length, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], length);
	return out;
}

std::string narrow(const std::wstring &text)
{
	if (text.empty())
		return std::string();
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
					       nullptr, nullptr);
	if (length <= 0)
		return std::string();
	std::string out((size_t)length, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], length, nullptr, nullptr);
	return out;
}

bool parse_url(const std::string &url, Url &out)
{
	size_t pos = 0;
	if (url.compare(0, 8, "https://") == 0) {
		out.tls = true;
		out.port = 443;
		pos = 8;
	} else if (url.compare(0, 7, "http://") == 0) {
		out.tls = false;
		out.port = 80;
		pos = 7;
	} else if (url.compare(0, 6, "wss://") == 0) {
		out.tls = true;
		out.port = 443;
		pos = 6;
	} else if (url.compare(0, 5, "ws://") == 0) {
		out.tls = false;
		out.port = 80;
		pos = 5;
	} else {
		return false;
	}

	const size_t authority_end = url.find_first_of("/?#", pos);
	const std::string authority =
		url.substr(pos, authority_end == std::string::npos ? std::string::npos : authority_end - pos);
	if (authority.empty())
		return false;

	std::string host = authority;
	const size_t colon = authority.find_last_of(':');
	if (colon != std::string::npos) {
		host = authority.substr(0, colon);
		const int port = atoi(authority.c_str() + colon + 1);
		if (port > 0 && port < 65536)
			out.port = port;
	}
	out.host = widen(host);
	if (authority_end == std::string::npos) {
		out.path = L"/";
	} else {
		std::string path = url.substr(authority_end);
		if (path.empty() || path[0] != '/')
			path.insert(path.begin(), '/');
		out.path = widen(path);
	}
	return !out.host.empty();
}

[[maybe_unused]] std::string base64(const unsigned char *data, size_t length)
{
	static const char *kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((length + 2) / 3 * 4);
	size_t i = 0;
	while (i + 2 < length) {
		const unsigned int chunk = ((unsigned int)data[i] << 16) | ((unsigned int)data[i + 1] << 8) |
					   data[i + 2];
		out.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
		out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
		out.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
		out.push_back(kAlphabet[chunk & 0x3F]);
		i += 3;
	}
	const size_t remaining = length - i;
	if (remaining == 1) {
		const unsigned int chunk = (unsigned int)data[i] << 16;
		out.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
		out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
		out.push_back('=');
		out.push_back('=');
	} else if (remaining == 2) {
		const unsigned int chunk = ((unsigned int)data[i] << 16) | ((unsigned int)data[i + 1] << 8);
		out.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
		out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
		out.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
		out.push_back('=');
	}
	return out;
}

void close_handle(void *&handle)
{
	if (handle) {
		WinHttpCloseHandle((HINTERNET)handle);
		handle = nullptr;
	}
}

bool add_headers(HINTERNET request, const std::vector<std::pair<std::string, std::string>> &headers)
{
	for (const auto &header : headers) {
		std::wstring line = widen(header.first) + L": " + widen(header.second) + L"\r\n";
		if (!WinHttpAddRequestHeaders(request, line.c_str(), (DWORD)-1,
					      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
			return false;
	}
	return true;
}

// Applies an optional "host:port" HTTP proxy to a WinHTTP session.
void apply_proxy(HINTERNET session, const std::string &proxy)
{
	std::string value = proxy;
	const size_t scheme = value.find("://");
	if (scheme != std::string::npos)
		value = value.substr(scheme + 3);
	while (!value.empty() && (value.back() == '/' || value.back() == ' '))
		value.pop_back();
	if (value.empty())
		return;

	const std::wstring wide_value = widen(value);
	WINHTTP_PROXY_INFO info = {};
	info.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
	info.lpszProxy = const_cast<wchar_t *>(wide_value.c_str());
	info.lpszProxyBypass = nullptr;
	WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &info, sizeof(info));
}

} // namespace

HttpResult http_get(const std::string &url,
		    const std::vector<std::pair<std::string, std::string>> &headers,
		    const std::string &proxy)
{
	HttpResult result;
	Url parts;
	if (!parse_url(url, parts)) {
		result.error = "invalid url: " + url;
		return result;
	}

	HINTERNET session = WinHttpOpen(L"obs-chat-overlay/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
					WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		result.error = "WinHttpOpen failed";
		return result;
	}
	apply_proxy(session, proxy);
	WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);

	HINTERNET connection = WinHttpConnect(session, parts.host.c_str(), (INTERNET_PORT)parts.port, 0);
	if (!connection) {
		result.error = "WinHttpConnect failed";
		WinHttpCloseHandle(session);
		return result;
	}

	HINTERNET request = WinHttpOpenRequest(connection, L"GET", parts.path.c_str(), nullptr,
					       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
					       parts.tls ? WINHTTP_FLAG_SECURE : 0);
	if (!request) {
		result.error = "WinHttpOpenRequest failed";
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return result;
	}

	DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
			 sizeof(redirect_policy));

	const char *user_agent = "User-Agent: obs-chat-overlay/1.0\r\n";
	WinHttpAddRequestHeaders(request, widen(user_agent).c_str(), (DWORD)-1,
				 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
	add_headers(request, headers);

	bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
				     0, 0, 0) &&
		  WinHttpReceiveResponse(request, nullptr);
	if (!ok) {
		result.error = "request failed, error " + std::to_string((int)GetLastError());
		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return result;
	}

	DWORD status = 0;
	DWORD status_size = sizeof(status);
	WinHttpQueryHeaders(request,
			    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			    WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
	result.status = (long)status;

	std::string body;
	for (;;) {
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request, &available))
			break;
		if (available == 0)
			break;
		std::string chunk(available, '\0');
		DWORD read = 0;
		if (!WinHttpReadData(request, &chunk[0], available, &read))
			break;
		if (read == 0)
			break;
		chunk.resize(read);
		body += chunk;
		if (body.size() > 32u * 1024u * 1024u)
			break;
	}

	result.body = std::move(body);
	result.ok = result.status >= 200 && result.status < 300;

	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connection);
	WinHttpCloseHandle(session);
	return result;
}

WebSocket::~WebSocket()
{
	close();
}

bool WebSocket::connect(const std::string &url,
			const std::vector<std::pair<std::string, std::string>> &headers,
			const std::string &proxy, std::string *error)
{
	close();

	Url parts;
	if (!parse_url(url, parts)) {
		if (error)
			*error = "invalid websocket url: " + url;
		return false;
	}

	session_ = WinHttpOpen(L"obs-chat-overlay/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session_) {
		if (error)
			*error = "WinHttpOpen failed";
		return false;
	}
	apply_proxy((HINTERNET)session_, proxy);
	WinHttpSetTimeouts(session_, 10000, 10000, 15000, 15000);

	connection_ = WinHttpConnect(session_, parts.host.c_str(), (INTERNET_PORT)parts.port, 0);
	if (!connection_) {
		if (error)
			*error = "WinHttpConnect failed";
		close();
		return false;
	}

	HINTERNET request = WinHttpOpenRequest(connection_, L"GET", parts.path.c_str(), nullptr,
					       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
					       parts.tls ? WINHTTP_FLAG_SECURE : 0);
	if (!request) {
		if (error)
			*error = "WinHttpOpenRequest failed";
		close();
		return false;
	}

	// This option takes no parameters: it tells WinHTTP to perform the
	// WebSocket handshake (including the Sec-WebSocket-* headers) when the
	// request is sent.
	if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
		if (error)
			*error = "WinHttpSetOption(UPGRADE_TO_WEB_SOCKET) failed";
		WinHttpCloseHandle(request);
		close();
		return false;
	}

	std::vector<std::pair<std::string, std::string>> all_headers = headers;
	all_headers.emplace_back("User-Agent", "obs-chat-overlay/1.0");
	add_headers(request, all_headers);

	bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
				     0, 0, 0) &&
		  WinHttpReceiveResponse(request, nullptr);
	if (!ok) {
		if (error)
			*error = "websocket handshake failed, error " + std::to_string((int)GetLastError());
		WinHttpCloseHandle(request);
		close();
		return false;
	}

	DWORD status = 0;
	DWORD status_size = sizeof(status);
	WinHttpQueryHeaders(request,
			    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			    WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
	if (status != 101) {
		if (error)
			*error = "websocket upgrade rejected with status " + std::to_string((int)status);
		WinHttpCloseHandle(request);
		close();
		return false;
	}

	socket_ = WinHttpWebSocketCompleteUpgrade(request, 0);
	WinHttpCloseHandle(request);
	if (!socket_) {
		if (error)
			*error = "WinHttpWebSocketCompleteUpgrade failed";
		close();
		return false;
	}
	return true;
}

void WebSocket::close()
{
	if (socket_) {
		WinHttpWebSocketClose((HINTERNET)socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
				      nullptr, 0);
		close_handle(socket_);
	}
	close_handle(connection_);
	close_handle(session_);
}

void WebSocket::abort()
{
	// Closing the WebSocket handle cancels a pending synchronous receive with
	// ERROR_WINHTTP_OPERATION_CANCELLED, which unblocks the network thread.
	close_handle(socket_);
	close_handle(connection_);
	close_handle(session_);
}

bool WebSocket::send_text(const std::string &text)
{
	if (!socket_)
		return false;
	const DWORD result = WinHttpWebSocketSend(
		(HINTERNET)socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
		(PVOID)(text.empty() ? "" : text.c_str()), (DWORD)text.size());
	return result == ERROR_SUCCESS;
}

bool WebSocket::receive_text(std::string &out, std::string *error)
{
	out.clear();
	if (!socket_) {
		if (error)
			*error = "not connected";
		return false;
	}

	BYTE buffer[8192];
	std::string accumulated;
	for (;;) {
		DWORD read = 0;
		WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
		const DWORD result = WinHttpWebSocketReceive((HINTERNET)socket_, buffer, sizeof(buffer),
							     &read, &type);
		if (result != ERROR_SUCCESS) {
			if (error)
				*error = "receive failed, error " + std::to_string((int)result);
			return false;
		}

		switch (type) {
		case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE:
			if (error)
				*error = "connection closed by peer";
			return false;
		case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:
		case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
			accumulated.append((const char *)buffer, read);
			break;
		case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
			accumulated.append((const char *)buffer, read);
			out = std::move(accumulated);
			return true;
		case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:
			accumulated.clear();
			break;
		default:
			accumulated.append((const char *)buffer, read);
			break;
		}
	}
}
